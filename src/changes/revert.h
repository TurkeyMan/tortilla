// Revert, one way for every window that offers it (the Revert dialog and
// the file list's menu) [spec 6.3.4]: what it would overwrite or remove is
// moved to the Recycle Bin (the trash) first, so it can be undone.
#pragma once

#include "git/repo.h"

#include <rapport/rapport.h>

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace tortilla
{

// By kind: additions are unstaged (the file stays); a rename's new file goes
// and its old one comes back from HEAD; the rest come back from HEAD, index
// and working tree.
struct revert_plan
{
    std::vector<std::string> unstage, remove, restore;  // repository-relative
    // The working-tree files it overwrites or removes, absolute: to the trash first.
    std::vector<std::filesystem::path> trash;
    std::string error;  // nothing to run when set
};
revert_plan plan_revert(const std::string& workdir, const std::vector<file_change>&);

// The plan's git commands, the paths on the command line (the Revert dialog
// passes its own in a file instead, for any number of them).
std::vector<std::vector<std::string>> revert_commands(const revert_plan&);

// The files of `paths` (repository-relative) that hold what git doesn't:
// changes against HEAD, or not tracked at all. What overwriting them would lose.
std::vector<std::filesystem::path> with_local_changes(const std::string& workdir,
                                                      const std::vector<std::string>& paths);

// Moves `files` to the trash and then runs `go`; if they can't be moved,
// asks `question` (whether to go on anyway, as that can't be undone). `go`
// doesn't run on No.
void trash_first(rap::ui&, rap::window* parent, const std::string& title,
                 const std::string& question, const std::vector<std::filesystem::path>& files,
                 std::function<void()> go);

}  // namespace tortilla
