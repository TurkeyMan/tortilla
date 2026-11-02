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

// From the repository's own (worktree's) git directory: a few file checks.
operation_state operation_in_git_dir(const std::string& git_dir);
// For a working tree (runs git rev-parse to find its git directory).
operation_state operation_in_progress(const std::string& workdir);

}  // namespace tortilla
