#include "ref_list.h"

#include "git/command.h"
#include "git/repo.h"
#include "util/search_query.h"
#include "util/strings.h"

#include <git2.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <memory>
#include <optional>
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

// A message's subject as git's %(subject): its first paragraph, its lines
// joined by spaces.
std::string subject_of(std::string_view message)
{
    std::string out;
    bool started = false;
    while (!message.empty())
    {
        const size_t nl = message.find('\n');
        std::string_view line = message.substr(0, nl);
        message.remove_prefix(nl == std::string_view::npos ? message.size() : nl + 1);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
            line.remove_suffix(1);
        if (line.empty())
        {
            if (started) break;
            continue;
        }
        if (started) out += ' ';
        out += line;
        started = true;
    }
    return out;
}

// for-each-ref's lines as list_refs asks for them (fields NUL-separated),
// through libgit2: Browse References opens on them. nullopt when libgit2
// can't read the refs, or HEAD for the merged filters.
std::optional<std::string> refs_in_process(const std::string& workdir, merged_filter filter)
{
    auto r = repo::open(workdir);
    if (!r) return std::nullopt;
    const auto refs = r->refs_under({});
    if (!refs) return std::nullopt;
    git_repository* raw = r->raw();
    git_odb* odb = nullptr;
    if (git_repository_odb(&odb, raw) != 0) return std::nullopt;
    std::unique_ptr<git_odb, decltype(&git_odb_free)> owned_odb(odb, git_odb_free);

    // --merged/--no-merged HEAD: the commits HEAD reaches, walked once.
    std::unordered_set<oid, oid_hash> reached;
    if (filter != merged_filter::all)
    {
        const auto head = r->head_id();
        if (!head) return std::nullopt;
        for (const oid& id : r->rev_list({*head}, {})) reached.insert(id);
    }
    auto when = [](const git_signature* s) { return s ? std::to_string(s->when.time) : ""; };
    auto name = [](const git_signature* s) { return std::string(s && s->name ? s->name : ""); };
    std::string out;
    for (const repo::listed_ref& l : *refs)
    {
        const git_oid id = [&]
        {
            git_oid g;
            std::memcpy(g.id, l.target.bytes.data(), l.target.bytes.size());
            return g;
        }();
        size_t size = 0;
        git_object_t type = GIT_OBJECT_INVALID;
        if (git_odb_read_header(&size, &type, odb, &id) != 0) return std::nullopt;
        std::string fields[11];
        fields[0] = l.full_name;
        fields[1] = l.target.hex();
        fields[2] = git_object_type2string(type);
        if (l.full_name.starts_with("refs/heads/"))
            fields[3] = r->upstream_ref(std::string_view(l.full_name).substr(11));
        std::optional<oid> commit;
        if (type == GIT_OBJECT_COMMIT)
        {
            git_commit* c = nullptr;
            if (git_commit_lookup(&c, raw, &id) != 0) return std::nullopt;
            if (const char* s = git_commit_message(c)) fields[4] = subject_of(s);
            fields[5] = name(git_commit_author(c));
            fields[6] = when(git_commit_author(c));
            fields[7] = name(git_commit_committer(c));
            fields[8] = when(git_commit_committer(c));
            git_commit_free(c);
            commit = l.target;
        }
        else if (type == GIT_OBJECT_TAG)
        {
            git_tag* t = nullptr;
            if (git_tag_lookup(&t, raw, &id) != 0) return std::nullopt;
            if (const char* m = git_tag_message(t)) fields[4] = subject_of(m);
            fields[9] = name(git_tag_tagger(t));
            fields[10] = when(git_tag_tagger(t));
            git_object* peeled = nullptr;
            if (git_tag_peel(&peeled, t) == 0)
            {
                if (git_object_type(peeled) == GIT_OBJECT_COMMIT)
                {
                    oid o;
                    std::memcpy(o.bytes.data(), git_object_id(peeled)->id, o.bytes.size());
                    commit = o;
                }
                git_object_free(peeled);
            }
            git_tag_free(t);
        }
        if (filter != merged_filter::all &&
            (!commit || reached.contains(*commit) != (filter == merged_filter::merged)))
            continue;
        for (size_t i = 0; i < 11; ++i)
        {
            if (i) out += '\0';
            out += fields[i];
        }
        out += '\n';
    }
    return out;
}

// git config --null --get-regexp ^branch\..*\.description$'s output,
// through libgit2; nullopt when it can't read the config.
std::optional<std::string> descriptions_in_process(const std::string& workdir)
{
    auto r = repo::open(workdir);
    git_config* config = nullptr;
    if (!r || git_repository_config_snapshot(&config, r->raw()) != 0) return std::nullopt;
    std::string out;
    git_config_iterator* it = nullptr;
    if (git_config_iterator_glob_new(&it, config, "^branch\\..*\\.description$") == 0)
    {
        git_config_entry* e = nullptr;
        while (git_config_next(&e, it) == 0)
            out += std::string(e->name) + '\n' + (e->value ? e->value : "") + '\0';
        git_config_iterator_free(it);
    }
    git_config_free(config);
    return out;
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
    git_result r{.exit_code = 0};
    if (auto listed = refs_in_process(workdir, filter))
        r.output = std::move(*listed);
    else
        r = run_git(workdir, args, git_output::stdout_only);
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
        if (f.size() < 11 || is_prefetch_ref(f[0])) continue;  // the sync's, not the user's
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
    git_result d{.exit_code = 0};
    if (auto found = descriptions_in_process(workdir))
        d.output = std::move(*found);
    else
        d = run_git(workdir, {"config", "--null", "--get-regexp", "^branch\\..*\\.description$"},
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

namespace
{

bool branch(const ref_entry& e)
{
    return e.local_branch() || e.remote_branch();
}

bool under(std::string_view full, std::string_view folder)
{
    return full.size() > folder.size() && full.starts_with(folder) && full[folder.size()] == '/';
}

}  // namespace

std::map<std::string, folder_check, std::less<>> folder_checks(const std::vector<ref_entry>& refs,
                                                               const checked_refs& checked)
{
    // Each folder's branches, and how many of them are checked: one pass,
    // each branch counted in every folder above it.
    std::map<std::string, std::pair<size_t, size_t>, std::less<>> counts;
    for (const std::string& f : ref_folders(refs)) counts[f];
    for (const ref_entry& e : refs)
    {
        if (!branch(e)) continue;
        const bool on = checked.contains(e.full);
        const std::string_view full = e.full;
        for (size_t slash = full.find('/'); slash != std::string_view::npos;
             slash = full.find('/', slash + 1))
            if (auto it = counts.find(full.substr(0, slash)); it != counts.end())
            {
                ++it->second.first;
                it->second.second += on;
            }
    }
    std::map<std::string, folder_check, std::less<>> out;
    for (auto& [folder, n] : counts)
        out.emplace_hint(out.end(), folder,
                         n.second == 0         ? folder_check::unchecked
                         : n.second == n.first ? folder_check::checked
                                               : folder_check::mixed);
    return out;
}

void toggle_folder(const std::vector<ref_entry>& refs, checked_refs& checked,
                   std::string_view folder)
{
    bool all = true;
    for (const ref_entry& e : refs)
        if (branch(e) && under(e.full, folder) && !checked.contains(e.full)) all = false;
    for (const ref_entry& e : refs)
        if (branch(e) && under(e.full, folder))
        {
            if (all)
                checked.erase(e.full);
            else
                checked.insert(e.full);
        }
}

std::vector<std::string> checked_branches(const std::vector<ref_entry>& refs,
                                          const checked_refs& checked)
{
    std::vector<std::string> out;
    for (const ref_entry& e : refs)
        if (branch(e) && checked.contains(e.full)) out.push_back(e.full);
    return out;
}

bool ref_filter_matches(const std::string& filter, const std::string& text)
{
    // Case-insensitive, each term a substring.
    const search_query q = parse_search_query(filter);
    const std::string hay = ascii_lower(text);
    const bool hit = std::ranges::all_of(
        q.terms, [&](const search_term& t)
        { return (hay.find(ascii_lower(t.text)) != std::string::npos) != t.exclude; });
    return hit != q.invert;
}

}  // namespace tortilla
