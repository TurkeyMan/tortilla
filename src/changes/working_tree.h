// The working tree's changes for the lists of Check for modifications,
// Commit and the like [spec 5.2.2]: what git status says, line counts, and
// on request ignored files and the files flagged to ignore local changes.
// Blocking; for a worker thread.
#pragma once

#include "git/command.h"
#include "git/repo.h"

#include <string>
#include <vector>

namespace tortilla
{

struct working_tree_options
{
    // Repository-relative paths to limit to (a file, or a folder and all
    // under it); empty for the whole repository.
    std::vector<std::string> paths;
    bool unversioned = true;
    // Every untracked file, rather than a folder for a folder of them.
    bool each_untracked_file = false;
    bool ignored = false;
    bool local_changes_ignored = false;  // skip-worktree and assume-unchanged files
    // Staged changes anywhere in the repository too, outside `paths`
    // (TortoiseGit's "Show all staged files").
    bool all_staged = true;
};

struct working_tree_changes
{
    std::vector<file_change> files;
    std::string error;  // git's complaint, if it failed
};

// `canceller`, if given, stops it from another thread (a newer load).
working_tree_changes load_working_tree(const std::string& workdir, const working_tree_options&,
                                       git_canceller* canceller = nullptr);

// Whether repository-relative `path` is one of `paths` or under one.
bool within(const std::string& path, const std::vector<std::string>& paths);

}  // namespace tortilla
