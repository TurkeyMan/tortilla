// Whether git is installed. Tortilla reads repositories through libgit2
// (history, refs, diffs, blame, status), so without git it is still a
// viewer; what writes (commit, sync, merge, rebase, stash...) needs git, and
// is greyed with git_missing_reason() instead of failing when it runs.
#pragma once

#include <string>
#include <string_view>

namespace tortilla
{

// Whether git is on PATH, where run_git starts it. Looked for once and
// kept; recheck_git() looks again (Settings does as it opens).
bool git_installed();
bool recheck_git();

// "git isn't installed": the reason a greyed action gives.
std::string git_missing_reason();

// Where a program git runs as a subcommand (git-lfs for `git lfs`) would
// be found: on PATH, or in git's own folders (its exec path; Git for
// Windows' mingw64/bin). "" if nowhere.
std::string find_git_program(const std::string& name);

// How to install git here, for the notice that says it's missing.
struct git_install_help
{
    std::string text;     // a sentence or two
    std::string command;  // to copy and run; "" when there's none
    std::string url;      // to open in the browser; "" when there's none
};
// On Linux, by the distribution /etc/os-release names (its ID, then
// ID_LIKE); `os_release` is that file's text.
git_install_help linux_git_install_help(std::string_view os_release);
// For this machine.
git_install_help git_install_help_here();

}  // namespace tortilla
