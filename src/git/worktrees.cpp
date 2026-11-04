#include "worktrees.h"

#include "repo.h"

#include <git2.h>
#include <rapport/sys.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>

namespace tortilla
{
namespace
{

namespace fs = std::filesystem;

std::string trimmed_path(std::string p)
{
    while (p.size() > 1 && (p.back() == '/' || p.back() == '\\')) p.pop_back();
    return p;
}

std::string hex_of(const git_oid* id)
{
    if (!id) return {};
    char buf[GIT_OID_HEXSZ + 1];
    git_oid_tostr(buf, sizeof buf, id);
    return buf;
}

// What a working tree's repository has checked out. An unborn branch (no
// commits yet) still names its branch, from HEAD's symbolic target.
void read_head(git_repository* r, worktree_info& w)
{
    git_reference* head = nullptr;
    const int error = git_repository_head(&head, r);
    if (error == 0)
    {
        if (git_reference_is_branch(head)) w.branch = git_reference_shorthand(head);
        w.head = hex_of(git_reference_target(head));
        git_reference_free(head);
        return;
    }
    if (error != GIT_EUNBORNBRANCH) return;
    if (git_reference_lookup(&head, r, "HEAD") != 0) return;
    if (const char* target = git_reference_symbolic_target(head))
    {
        std::string name = target;
        if (name.starts_with("refs/heads/")) name.erase(0, 11);
        w.branch = std::move(name);
    }
    git_reference_free(head);
}

// A linked worktree's HEAD, from its admin directory (worktrees/<name>/HEAD
// in the common directory), which is there even when its folder isn't, as
// git worktree list reads it. Its branch's commit is the shared refs'.
void read_admin_head(git_repository* main, const std::string& name, worktree_info& w)
{
    const fs::path file = rap::sys::path_from_utf8(git_repository_commondir(main)) / "worktrees" /
                          rap::sys::path_from_utf8(name) / "HEAD";
    std::ifstream in(file, std::ios::binary);
    std::string line;
    if (!std::getline(in, line)) return;
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (!line.starts_with("ref: "))
    {
        w.head = line;
        return;
    }
    const std::string ref = line.substr(5);
    w.branch = ref.starts_with("refs/heads/") ? ref.substr(11) : ref;
    git_oid id;
    if (git_reference_name_to_id(&id, main, ref.c_str()) == 0) w.head = hex_of(&id);
}

// The branches a rebase or bisect in progress in a worktree holds, from
// its git directory, as git's is_worktree_being_rebased and _bisected read
// them: rebase-merge/head-name or rebase-apply/head-name (a full name, or
// "detached HEAD"), and BISECT_START (the branch bisect started on, short,
// or a commit when it started detached).
void read_held(const fs::path& git_dir, worktree_info& w)
{
    auto first_line = [&](const fs::path& file) -> std::string
    {
        std::ifstream in(git_dir / file, std::ios::binary);
        std::string line;
        if (!std::getline(in, line)) return {};
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        return line;
    };
    for (const char* file : {"rebase-merge/head-name", "rebase-apply/head-name"})
        if (std::string name = first_line(file); name.starts_with("refs/heads/"))
            w.held.push_back(std::move(name));
    std::string start = first_line("BISECT_START");
    const bool commit =
        (start.size() == 40 || start.size() == 64) &&
        std::ranges::all_of(start, [](char c) { return std::isxdigit(uint8_t(c)); });
    if (start.empty() || commit || start == "detached HEAD") return;
    w.held.push_back(start.starts_with("refs/heads/") ? std::move(start) : "refs/heads/" + start);
}

}  // namespace

bool worktree_info::holds(std::string_view full_name) const
{
    if (!branch.empty() && full_name.starts_with("refs/heads/") && full_name.substr(11) == branch)
        return true;
    return std::ranges::contains(held, full_name);
}

std::expected<std::vector<worktree_info>, std::string> list_worktrees(const repo& r)
{
    // Opened from a linked worktree, the main one is the common directory's.
    git_repository* main = r.raw();
    std::unique_ptr<git_repository, void (*)(git_repository*)> owned(nullptr, git_repository_free);
    if (git_repository_is_worktree(r.raw()) == 1)
    {
        if (git_repository_open(&main, git_repository_commondir(r.raw())) != 0)
            return std::unexpected(last_git_error());
        owned.reset(main);
    }
    const std::string here = trimmed_path(r.workdir());

    std::vector<worktree_info> out;
    worktree_info first;
    first.main = true;
    first.bare = git_repository_is_bare(main) == 1;
    const char* top = first.bare ? git_repository_path(main) : git_repository_workdir(main);
    first.path = trimmed_path(top ? top : "");
    read_head(main, first);
    read_held(rap::sys::path_from_utf8(git_repository_path(main)), first);
    out.push_back(std::move(first));

    git_strarray names{};
    if (git_worktree_list(&names, main) != 0) return std::unexpected(last_git_error());
    std::vector<std::string> sorted(names.strings, names.strings + names.count);
    git_strarray_dispose(&names);
    std::ranges::sort(sorted);
    for (const std::string& name : sorted)
    {
        git_worktree* wt = nullptr;
        if (git_worktree_lookup(&wt, main, name.c_str()) != 0) continue;
        worktree_info w;
        w.name = name;
        if (const char* p = git_worktree_path(wt)) w.path = trimmed_path(p);
        git_buf reason = GIT_BUF_INIT;
        w.locked = git_worktree_is_locked(&reason, wt) > 0;
        if (w.locked && reason.ptr) w.lock_reason = reason.ptr;
        git_buf_dispose(&reason);
        // The default options: prunable when its directory is gone and it
        // isn't locked, as git worktree prune decides.
        w.prunable = git_worktree_is_prunable(wt, nullptr) > 0;
        read_admin_head(main, name, w);
        read_held(rap::sys::path_from_utf8(git_repository_commondir(main)) / "worktrees" /
                      rap::sys::path_from_utf8(name),
                  w);
        git_worktree_free(wt);
        out.push_back(std::move(w));
    }
    for (worktree_info& w : out) w.current = w.path == here;
    return out;
}

}  // namespace tortilla
