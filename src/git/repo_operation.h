// What git (or Tortilla) is in the middle of, and so which side of a
// conflict is the user's own: one answer for the merge tool, the conflict
// menu and the rest, so they never take opposite sides.
//
// In a merge or a cherry-pick (and git am, and revert) HEAD is the user's
// branch: "mine" is stage 2, git's "ours". In a rebase, git's or Tortilla's
// (cherry-picks onto the upstream, with <git dir>/tortilla-rebase-active),
// HEAD is the upstream being built on and the pick is the user's commit:
// "mine" is stage 3, git's "theirs".
#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace tortilla
{

enum class repo_operation
{
    none,
    tortilla_rebase,  // tortilla-rebase-active (a cherry-pick may be going on inside it)
    rebase,           // git's rebase-merge, or rebase-apply for a rebase
    apply_mailbox,    // git am (rebase-apply/applying)
    merge,            // MERGE_HEAD
    cherry_pick,      // CHERRY_PICK_HEAD
    revert,           // REVERT_HEAD
};

struct operation_state
{
    repo_operation operation = repo_operation::none;
    // Tortilla's rebase: what it's onto and which branch, as chosen; empty
    // when not known.
    std::string onto, branch;

    bool rebasing() const
    {
        return operation == repo_operation::tortilla_rebase || operation == repo_operation::rebase;
    }
    // The index stage of the user's own side, and of the other.
    int mine_stage() const { return rebasing() ? 3 : 2; }
    int theirs_stage() const { return rebasing() ? 2 : 3; }
};

// Tortilla's rebase going on in a worktree: its marker, while HEAD is
// detached (a marker left behind by a crash, with a branch checked out
// again, doesn't count). The one description of it: the operation shown,
// and the branch it holds against other worktrees and history edits
// (worktree_info::held), both read it here.
struct tortilla_rebase
{
    std::string onto;    // what it's onto, as chosen; may be empty
    std::string branch;  // the branch being rebased, short; may be empty
    // The branch's full name ("refs/heads/feature"); empty when not known.
    std::string branch_ref() const;
};
std::optional<tortilla_rebase> tortilla_rebase_in(const std::filesystem::path& git_dir);

// From the repository's own (worktree's) git directory: a few file checks.
operation_state operation_in_git_dir(const std::string& git_dir);
// For a working tree (runs git rev-parse to find its git directory).
operation_state operation_in_progress(const std::string& workdir);

}  // namespace tortilla
