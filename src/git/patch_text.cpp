#include "patch_text.h"

#include "git/repo.h"
#include "util/calendar.h"

#include <git2.h>

#include <cstdio>
#include <ctime>
#include <memory>
#include <string_view>

namespace tortilla
{
namespace
{

using diff_ptr = std::unique_ptr<git_diff, decltype(&git_diff_free)>;
using object_ptr = std::unique_ptr<git_object, decltype(&git_object_free)>;

object_ptr lookup(git_repository* r, const std::string& rev, git_object_t type)
{
    git_object* o = nullptr;
    git_object* peeled = nullptr;
    if (git_revparse_single(&o, r, rev.c_str()) == 0)
    {
        if (git_object_peel(&peeled, o, type) != 0) peeled = nullptr;
        git_object_free(o);
    }
    return object_ptr(peeled, &git_object_free);
}

std::string text_of(git_diff* diff)
{
    git_diff_find_options find = GIT_DIFF_FIND_OPTIONS_INIT;  // diff.renames' default
    git_diff_find_similar(diff, &find);
    git_buf buf = GIT_BUF_INIT;
    std::string out;
    if (git_diff_to_buf(&buf, diff, GIT_DIFF_FORMAT_PATCH) == 0 && buf.ptr)
        out.assign(buf.ptr, buf.size);
    git_buf_dispose(&buf);
    return out;
}

// git's default date format: "Thu Feb 1 13:45:00 2024 +0100".
std::string git_date(const git_time& t)
{
    std::tm tm = utc_calendar(std::time_t(t.time + int64_t(t.offset) * 60));
    char buf[64];
    std::strftime(buf, sizeof buf, "%a %b %e %H:%M:%S %Y", &tm);
    std::string out = buf;
    // %e pads the day with a space; git doesn't.
    if (const size_t two = out.find("  "); two != std::string::npos) out.erase(two, 1);
    const int off = t.offset < 0 ? -t.offset : t.offset;
    char zone[8];
    std::snprintf(zone, sizeof zone, " %c%02d%02d", t.offset < 0 ? '-' : '+', off / 60, off % 60);
    return out + zone;
}

}  // namespace

std::optional<std::string> patch_in_process(const std::string& dir,
                                            const std::vector<std::string>& args)
{
    std::vector<std::string> words, paths;
    bool after = false;
    for (const std::string& a : args)
    {
        if (after)
            paths.push_back(a.starts_with(":(top)") ? a.substr(6) : a);
        else if (a == "--")
            after = true;
        else
            words.push_back(a);
    }
    auto r = repo::open(dir);
    if (!r || words.empty()) return std::nullopt;
    git_repository* raw = r->raw();
    std::vector<const char*> specs;
    for (const std::string& p : paths) specs.push_back(p.c_str());
    git_diff_options o = GIT_DIFF_OPTIONS_INIT;
    o.pathspec = {const_cast<char**>(specs.data()), specs.size()};
    o.flags |= GIT_DIFF_DISABLE_PATHSPEC_MATCH;

    git_diff* d = nullptr;
    if (words.size() == 2 && words[0] == "diff")
    {
        // The working tree and index against one revision.
        object_ptr tree = lookup(raw, words[1], GIT_OBJECT_TREE);
        if (!tree || git_diff_tree_to_workdir_with_index(
                         &d, raw, reinterpret_cast<git_tree*>(tree.get()), &o) != 0)
            return std::nullopt;
        return text_of(diff_ptr(d, &git_diff_free).get());
    }
    if (words.size() == 3 && words[0] == "diff")
    {
        object_ptr a = lookup(raw, words[1], GIT_OBJECT_TREE),
                   b = lookup(raw, words[2], GIT_OBJECT_TREE);
        if (!a || !b ||
            git_diff_tree_to_tree(&d, raw, reinterpret_cast<git_tree*>(a.get()),
                                  reinterpret_cast<git_tree*>(b.get()), &o) != 0)
            return std::nullopt;
        return text_of(diff_ptr(d, &git_diff_free).get());
    }
    if (words.size() == 3 && words[0] == "show" && words[1] == "--format=fuller")
    {
        object_ptr c = lookup(raw, words[2], GIT_OBJECT_COMMIT);
        if (!c) return std::nullopt;
        git_commit* commit = reinterpret_cast<git_commit*>(c.get());
        char hex[GIT_OID_MAX_HEXSIZE + 1];
        git_oid_tostr(hex, sizeof hex, git_commit_id(commit));
        std::string out = "commit " + std::string(hex) + "\n";
        if (git_commit_parentcount(commit) > 1)
        {
            out += "Merge:";
            for (unsigned i = 0; i < git_commit_parentcount(commit); ++i)
            {
                git_oid_tostr(hex, 8, git_commit_parent_id(commit, i));
                out += " " + std::string(hex);
            }
            out += "\n";
        }
        const git_signature* a = git_commit_author(commit);
        const git_signature* m = git_commit_committer(commit);
        out += "Author:     " + std::string(a->name) + " <" + a->email + ">\n";
        out += "AuthorDate: " + git_date(a->when) + "\n";
        out += "Commit:     " + std::string(m->name) + " <" + m->email + ">\n";
        out += "CommitDate: " + git_date(m->when) + "\n\n";
        std::string_view message = git_commit_message(commit) ? git_commit_message(commit) : "";
        while (message.ends_with('\n')) message.remove_suffix(1);
        while (!message.empty())
        {
            const size_t nl = message.find('\n');
            const std::string_view line = message.substr(0, nl);
            message.remove_prefix(nl == std::string_view::npos ? message.size() : nl + 1);
            out += line.empty() ? "\n" : "    " + std::string(line) + "\n";
        }
        out += "\n";
        // Against the first parent (git shows a merge's combined diff).
        git_tree* tree = nullptr;
        git_tree* parent_tree = nullptr;
        git_commit* parent = nullptr;
        if (git_commit_tree(&tree, commit) != 0) return std::nullopt;
        if (git_commit_parentcount(commit) > 0 && git_commit_parent(&parent, commit, 0) == 0)
            git_commit_tree(&parent_tree, parent);
        const int made = git_diff_tree_to_tree(&d, raw, parent_tree, tree, &o);
        git_tree_free(tree);
        git_tree_free(parent_tree);
        git_commit_free(parent);
        if (made != 0) return std::nullopt;
        return out + text_of(diff_ptr(d, &git_diff_free).get());
    }
    return std::nullopt;
}

}  // namespace tortilla
