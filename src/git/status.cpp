#include "status.h"

#include "git/git_install.h"
#include "git/porcelain.h"

#include <git2.h>

#include <algorithm>

#include <charconv>
#include <unordered_map>

namespace tortilla
{
namespace
{

// Splits on NULs; the trailing empty field after the last NUL is dropped.
std::vector<std::string> fields(const std::string& s)
{
    std::vector<std::string> out;
    size_t at = 0;
    while (at < s.size())
    {
        size_t end = s.find('\0', at);
        if (end == std::string::npos) end = s.size();
        out.push_back(s.substr(at, end - at));
        at = end + 1;
    }
    return out;
}

char status_letter(char x, char y)
{
    if (x == '?' && y == '?') return '?';
    if (x == '!' && y == '!') return '!';
    // Unmerged: either side U, or both added/deleted.
    if (x == 'U' || y == 'U' || (x == 'A' && y == 'A') || (x == 'D' && y == 'D')) return 'U';
    char c = x != ' ' ? x : y;
    switch (c)
    {
    case 'A':
    case 'D':
    case 'R':
    case 'C':
    case 'T': return c;
    default: return 'M';
    }
}

}  // namespace

std::vector<file_change> parse_status(const std::string& porcelain, const std::string& numstat)
{
    struct line_counts
    {
        size_t added = 0, removed = 0;
        bool binary = false;
    };
    // numstat -z: "added\tremoved\tpath\0", or for renames
    // "added\tremoved\t\0old\0new\0". Binary files count as "-\t-".
    std::unordered_map<std::string, line_counts> counts;
    auto nf = fields(numstat);
    for (size_t i = 0; i < nf.size(); ++i)
    {
        const std::string& f = nf[i];
        size_t t1 = f.find('\t'), t2 = t1 == std::string::npos ? t1 : f.find('\t', t1 + 1);
        if (t2 == std::string::npos) continue;
        line_counts c;
        std::string a = f.substr(0, t1), r = f.substr(t1 + 1, t2 - t1 - 1);
        c.binary = a == "-";
        // Never throws on odd output: unparsable counts stay 0.
        if (!c.binary)
        {
            std::from_chars(a.data(), a.data() + a.size(), c.added);
            std::from_chars(r.data(), r.data() + r.size(), c.removed);
        }
        std::string path = f.substr(t2 + 1);
        if (path.empty() && i + 2 < nf.size())
        {
            path = nf[i + 2];  // rename: old, new
            i += 2;
        }
        counts[path] = c;
    }

    // porcelain v1 -z: "XY path\0", renames and copies "XY new\0old\0".
    std::vector<file_change> out;
    each_porcelain_entry(porcelain,
                         [&](const porcelain_entry& e)
                         {
                             file_change f;
                             f.status = status_letter(e.x, e.y);
                             f.staged = e.x != ' ' && e.x != '?' && e.x != '!';
                             f.path = e.path;
                             f.old_path = e.old_path;
                             if (auto it = counts.find(f.path); it != counts.end())
                             {
                                 f.added = it->second.added;
                                 f.removed = it->second.removed;
                                 f.binary = it->second.binary;
                             }
                             out.push_back(std::move(f));
                         });
    return out;
}

namespace
{

// A repository-relative path within one of `paths` ("" or none: all).
bool under(std::string_view path, const std::vector<std::string>& paths)
{
    if (paths.empty()) return true;
    for (std::string_view p : paths)
    {
        while (!p.empty() && p.back() == '/') p.remove_suffix(1);
        if (p.empty() || p == "." || path == p ||
            (path.size() > p.size() && path.starts_with(p) && path[p.size()] == '/'))
            return true;
        // An untracked folder ("dir/") holding a path asked for.
        if (path.ends_with('/') && p.starts_with(path)) return true;
    }
    return false;
}

std::string porcelain_in_process(git_repository* r, std::string_view untracked, bool ignored,
                                 const std::vector<std::string>& paths)
{
    git_status_options o = GIT_STATUS_OPTIONS_INIT;
    o.show = GIT_STATUS_SHOW_INDEX_AND_WORKDIR;
    o.flags = GIT_STATUS_OPT_RENAMES_HEAD_TO_INDEX | GIT_STATUS_OPT_SORT_CASE_SENSITIVELY;
    if (untracked != "no") o.flags |= GIT_STATUS_OPT_INCLUDE_UNTRACKED;
    if (untracked == "all") o.flags |= GIT_STATUS_OPT_RECURSE_UNTRACKED_DIRS;
    if (ignored) o.flags |= GIT_STATUS_OPT_INCLUDE_IGNORED;
    git_status_list* list = nullptr;
    if (git_status_list_new(&list, r, &o) != 0) return {};
    git_index* index = nullptr;
    git_repository_index(&index, r);
    std::string out;
    for (size_t i = 0, n = git_status_list_entrycount(list); i < n; ++i)
    {
        const git_status_entry* e = git_status_byindex(list, i);
        const unsigned s = e->status;
        const git_diff_delta* d = e->head_to_index ? e->head_to_index : e->index_to_workdir;
        if (!d) continue;
        const std::string path = e->index_to_workdir ? e->index_to_workdir->new_file.path
                                                     : e->head_to_index->new_file.path;
        if (!under(path, paths)) continue;
        char x = ' ', y = ' ';
        if (s & GIT_STATUS_CONFLICTED)
        {
            const git_index_entry *base = nullptr, *ours = nullptr, *theirs = nullptr;
            if (index) git_index_conflict_get(&base, &ours, &theirs, index, path.c_str());
            // git's codes, by which stages there are: DD AU UD UA DU AA UU.
            x = y = 'U';
            if (base && !ours && !theirs) x = y = 'D';
            if (!base && ours && !theirs) x = 'A', y = 'U';
            if (base && ours && !theirs) x = 'U', y = 'D';
            if (!base && !ours && theirs) x = 'U', y = 'A';
            if (base && !ours && theirs) x = 'D', y = 'U';
            if (!base && ours && theirs) x = y = 'A';
            if (base && ours && theirs) x = y = 'U';
        }
        else if ((s & GIT_STATUS_WT_NEW) &&
                 !(s & (GIT_STATUS_INDEX_NEW | GIT_STATUS_INDEX_MODIFIED)))
            x = y = '?';
        else if (s & GIT_STATUS_IGNORED)
            x = y = '!';
        else
        {
            if (s & GIT_STATUS_INDEX_NEW) x = 'A';
            if (s & GIT_STATUS_INDEX_MODIFIED) x = 'M';
            if (s & GIT_STATUS_INDEX_DELETED) x = 'D';
            if (s & GIT_STATUS_INDEX_RENAMED) x = 'R';
            if (s & GIT_STATUS_INDEX_TYPECHANGE) x = 'T';
            if (s & GIT_STATUS_WT_MODIFIED) y = 'M';
            if (s & GIT_STATUS_WT_DELETED) y = 'D';
            if (s & GIT_STATUS_WT_TYPECHANGE) y = 'T';
            if (s & GIT_STATUS_WT_RENAMED) y = 'R';
        }
        if (x == ' ' && y == ' ') continue;
        out += x;
        out += y;
        out += ' ';
        out += path;
        out += '\0';
        if (x == 'R' && e->head_to_index)
        {
            out += e->head_to_index->old_file.path;
            out += '\0';
        }
    }
    git_index_free(index);
    git_status_list_free(list);
    return out;
}

// git diff HEAD --numstat -z: lines added and removed per file, "-\t-"
// for binary ones; nullopt without a HEAD, as git fails then.
std::optional<std::string> numstat_in_process(git_repository* r,
                                              const std::vector<std::string>& paths)
{
    git_object* head = nullptr;
    if (git_revparse_single(&head, r, "HEAD^{tree}") != 0) return std::nullopt;
    git_diff_options o = GIT_DIFF_OPTIONS_INIT;
    git_diff* diff = nullptr;
    const int made =
        git_diff_tree_to_workdir_with_index(&diff, r, reinterpret_cast<git_tree*>(head), &o);
    git_object_free(head);
    if (made != 0) return std::nullopt;
    git_diff_find_options find = GIT_DIFF_FIND_OPTIONS_INIT;  // diff.renames' default
    git_diff_find_similar(diff, &find);
    std::string out;
    for (size_t i = 0, n = git_diff_num_deltas(diff); i < n; ++i)
    {
        git_patch* patch = nullptr;
        if (git_patch_from_diff(&patch, diff, i) != 0) continue;
        const git_diff_delta* d = git_patch_get_delta(patch);
        if (under(d->new_file.path, paths) || under(d->old_file.path, paths))
        {
            size_t context = 0, added = 0, removed = 0;
            git_patch_line_stats(&context, &added, &removed, patch);
            out += d->flags & GIT_DIFF_FLAG_BINARY
                       ? std::string("-\t-")
                       : std::to_string(added) + '\t' + std::to_string(removed);
            out += '\t';
            if (d->status == GIT_DELTA_RENAMED || d->status == GIT_DELTA_COPIED)
                out += std::string("\0", 1) + d->old_file.path + '\0' + d->new_file.path + '\0';
            else
                out += std::string(d->new_file.path) + '\0';
        }
        git_patch_free(patch);
    }
    git_diff_free(diff);
    return out;
}

std::string ls_files_in_process(git_repository* r, bool tags, const std::vector<std::string>& paths)
{
    git_index* index = nullptr;
    if (git_repository_index(&index, r) != 0) return {};
    std::string out;
    for (size_t i = 0, n = git_index_entrycount(index); i < n; ++i)
    {
        const git_index_entry* e = git_index_get_byindex(index, i);
        if (!under(e->path, paths)) continue;
        if (tags)
        {
            // -v: S skip-worktree, lower case assume-unchanged.
            char tag = (e->flags_extended & GIT_INDEX_ENTRY_SKIP_WORKTREE) ? 'S' : 'H';
            if (e->flags & GIT_INDEX_ENTRY_VALID) tag = char(tag - 'A' + 'a');
            out += tag;
            out += ' ';
        }
        out += e->path;
        out += '\0';
    }
    git_index_free(index);
    return out;
}

}  // namespace

std::optional<git_result> status_in_process(const std::string& workdir,
                                            const std::vector<std::string>& args)
{
    // The command, its options and its paths.
    std::vector<std::string_view> words;
    std::vector<std::string> paths;
    bool after = false;
    for (const std::string& a : args)
    {
        if (after)
            paths.push_back(a.starts_with(":(top)") ? a.substr(6) : a);
        else if (a == "--")
            after = true;
        else if (a != "--no-optional-locks" && a != "--literal-pathspecs")
            words.push_back(a);
    }
    if (words.empty()) return std::nullopt;
    auto has = [&](std::string_view w) { return std::ranges::find(words, w) != words.end(); };
    auto r = repo::open(workdir);
    if (!r) return git_result{.exit_code = 128, .output = r.error()};
    git_result out{.exit_code = 0};
    if (words[0] == "status" && has("--porcelain") && has("-z"))
    {
        std::string_view untracked = "normal";
        for (std::string_view w : words)
            if (w.starts_with("--untracked-files=")) untracked = w.substr(18);
        out.output = porcelain_in_process(r->raw(), untracked, has("--ignored=matching"), paths);
        return out;
    }
    if (words[0] == "diff" && words.size() == 4 && has("HEAD") && has("--numstat") && has("-z"))
    {
        auto counted = numstat_in_process(r->raw(), paths);
        if (!counted) return git_result{.exit_code = 128};
        out.output = std::move(*counted);
        return out;
    }
    if (words[0] == "ls-files" && has("-z") &&
        (words.size() == 2 || (words.size() == 3 && has("-v"))))
    {
        out.output = ls_files_in_process(r->raw(), has("-v"), paths);
        return out;
    }
    return std::nullopt;
}

git_result run_status(const std::string& workdir, const std::vector<std::string>& args,
                      git_canceller* canceller)
{
    if (!git_installed())
        if (auto answered = status_in_process(workdir, args)) return *answered;
    return canceller ? run_git(workdir, args, git_output::stdout_only, *canceller)
                     : run_git(workdir, args, git_output::stdout_only);
}

}  // namespace tortilla
