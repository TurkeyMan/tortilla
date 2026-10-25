#include "ref_list.h"

#include "git/command.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace tortilla
{
namespace
{

std::vector<std::string_view> split(std::string_view s, char sep)
{
    std::vector<std::string_view> out;
    for (size_t at = 0;;)
    {
        const size_t next = s.find(sep, at);
        out.push_back(s.substr(at, next == std::string_view::npos ? std::string_view::npos : next - at));
        if (next == std::string_view::npos) break;
        at = next + 1;
    }
    return out;
}

int64_t number(std::string_view s)
{
    int64_t v = 0;
    for (char c : s)
    {
        if (c < '0' || c > '9') break;
        v = v * 10 + (c - '0');
    }
    return v;
}

std::string lower(std::string s)
{
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

}  // namespace

std::string strip_ref_name(const std::string& full)
{
    if (full.starts_with("refs/heads/")) return full.substr(11);
    if (full.starts_with("refs/")) return full.substr(5);
    return full;
}

ref_list_result list_refs(const std::string& workdir, merged_filter filter)
{
    ref_list_result out;
    // One ref per line, its fields NUL-separated (subjects have no newlines).
    std::vector<std::string> args{"for-each-ref",
                                  "--format=%(refname)%00%(objectname)%00%(objecttype)%00%(upstream)%00%(subject)%00"
                                  "%(authorname)%00%(authordate:unix)%00%(committername)%00%(committerdate:unix)%00"
                                  "%(taggername)%00%(taggerdate:unix)"};
    if (filter == merged_filter::merged) args.insert(args.begin() + 1, {"--merged", "HEAD"});
    if (filter == merged_filter::unmerged) args.insert(args.begin() + 1, {"--no-merged", "HEAD"});
    const git_result r = run_git(workdir, args, git_output::stdout_only);
    if (!r.ok())
    {
        out.error = r.output;
        return out;
    }
    std::unordered_set<std::string> refs;
    for (std::string_view line : split(r.output, '\n'))
    {
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        const auto f = split(line, '\0');
        if (f.size() < 11) continue;
        ref_entry e;
        e.full = f[0];
        e.hash = f[1];
        e.type = f[2];
        e.upstream = f[3];
        e.subject = f[4];
        e.author = f[5];
        e.author_date = number(f[6]);
        e.committer = f[7];
        e.commit_date = number(f[8]);
        if (e.author.empty() && !f[9].empty())
        {
            // An annotated tag: its tagger stands for both.
            e.author = e.committer = f[9];
            e.author_date = e.commit_date = number(f[10]);
        }
        refs.insert(e.full);
        out.refs.push_back(std::move(e));
    }

    // Upstreams as origin/main, or gone when their ref isn't there [spec 10.3, 6].
    for (ref_entry& e : out.refs)
        if (!e.upstream.empty())
        {
            const bool there = refs.contains(e.upstream);
            std::string shown = e.upstream.starts_with("refs/") ? e.upstream.substr(5) : e.upstream;
            if (shown.starts_with("remotes/")) shown = shown.substr(8);
            e.upstream = there ? shown : "(gone: " + shown + ")";
        }

    // Descriptions (branch.<name>.description), newlines as spaces.
    const git_result d = run_git(workdir, {"config", "--null", "--get-regexp", "^branch\\..*\\.description$"},
                                 git_output::stdout_only);
    if (d.ok())
    {
        std::unordered_map<std::string, std::string> desc;
        for (std::string_view entry : split(d.output, '\0'))
        {
            const size_t nl = entry.find('\n');
            if (nl == std::string_view::npos) continue;
            std::string_view key = entry.substr(0, nl);
            key.remove_prefix(7);                             // branch.
            key.remove_suffix(std::string_view(".description").size());
            std::string text(entry.substr(nl + 1));
            std::ranges::replace(text, '\n', ' ');
            desc["refs/heads/" + std::string(key)] = std::move(text);
        }
        for (ref_entry& e : out.refs)
            if (auto it = desc.find(e.full); it != desc.end()) e.description = it->second;
    }
    return out;
}

std::vector<std::string> ref_folders(const std::vector<ref_entry>& refs)
{
    std::set<std::string> folders{"refs"};
    for (const ref_entry& e : refs)
        for (size_t slash = e.full.find('/'); slash != std::string::npos; slash = e.full.find('/', slash + 1))
            folders.insert(e.full.substr(0, slash));
    return {folders.begin(), folders.end()};
}

bool ref_filter_matches(const std::string& filter, const std::string& text)
{
    std::string_view s = filter;
    size_t i = 0;
    while (i < s.size() && s[i] == ' ') ++i;
    bool invert = false;
    if (i < s.size() && s[i] == '!')
    {
        invert = true;
        ++i;
    }
    const std::string hay = lower(text);
    bool hit = true;
    while (i < s.size() && hit)
    {
        while (i < s.size() && s[i] == ' ') ++i;
        if (i >= s.size()) break;
        bool exclude = false;
        if (s[i] == '-' || s[i] == '+')
        {
            exclude = s[i] == '-';
            ++i;
        }
        std::string term;
        if (i < s.size() && s[i] == '"')
        {
            for (++i; i < s.size(); ++i)
            {
                if (s[i] != '"')
                {
                    term += s[i];
                    continue;
                }
                if (i + 1 < s.size() && s[i + 1] == '"')
                {
                    term += '"';
                    ++i;
                    continue;
                }
                ++i;
                break;
            }
        }
        else
        {
            while (i < s.size() && s[i] != ' ') term += s[i++];
        }
        if (!term.empty() && (hay.find(lower(term)) != std::string::npos) == exclude) hit = false;
    }
    return hit != invert;
}

}  // namespace tortilla
