// A repository's working trees, for the Worktrees window (TortoiseGit's
// worktreelist [spec 4.2, row 52]): the main one and those `git worktree
// add` made, with what each has checked out. Read through libgit2.
#pragma once

#include <expected>
#include <string>
#include <vector>

namespace tortilla
{

class repo;

struct worktree_info
{
    std::string path;    // no trailing separator
    std::string name;    // git's name for a linked one (its admin dir); "" for the main one
    std::string branch;  // the branch checked out, short; "" when detached or unknown
    std::string head;    // HEAD's commit; "" for an unborn branch or a missing worktree
    bool main = false;   // the repository's own working tree (or a bare repository's directory)
    bool bare = false;
    bool current = false;  // the one the repository was opened from
    bool locked = false;
    std::string lock_reason;
    bool prunable = false;  // its directory is gone: git worktree prune would forget it
};

// The main working tree first, then the linked ones in name order.
std::expected<std::vector<worktree_info>, std::string> list_worktrees(const repo&);

}  // namespace tortilla
