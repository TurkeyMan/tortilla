// Which tool compares two revisions, as the Settings dialog's Diff Viewer
// page sets it [TortoiseGit: Settings > Diff Viewer]:
//   builtin  tortilla's unified diff window
//   git      git difftool with the configured diff.tool
//   custom   a command line with TortoiseGit's placeholders:
//            %base %mine (the two sides: folders for a whole commit),
//            %bname %yname (their names, e.g. "abc12345" / "working tree")
// Kept in tortilla's settings ("diff.mode", "diff.command"); git's own
// config is left alone.
#pragma once

#include <string>
#include <vector>

namespace tortilla
{

enum class diff_mode
{
    builtin,
    git,
    custom,
};

// The saved mode; with none saved, git's diff.tool when the repository (or
// the user's git config) has one, else the built-in viewer.
diff_mode saved_diff_mode(const std::string& repo_dir);
std::string diff_command();
// Whether the custom command compares folders (one comparison for a whole
// commit) or only files (git opens it for each changed file in turn).
bool diff_folders();
void save_diff_settings(diff_mode, const std::string& command, bool folders = true);

// git's diff.tool for `repo_dir`, or "" if unset.
std::string git_diff_tool(const std::string& repo_dir);

// Diff tools found on this machine, with command lines ready for the
// Custom setting.
struct detected_tool
{
    std::string name, command;
    bool folders = true;  // compares folders, so a commit opens as one comparison
};
std::vector<detected_tool> detect_diff_tools();

// git arguments that run `command` (placeholders and all) as a one-off
// difftool comparing `revisions` (as for git diff): folder to folder, or
// file to file when `paths` names exactly one file.
std::vector<std::string>
custom_difftool_args(const std::string& command, const std::vector<std::string>& revisions,
                     const std::string& base_name, const std::string& mine_name,
                     const std::vector<std::string>& paths = {}, bool folders = true);

// `command` with its placeholders turned into the shell words git's
// difftool runs it with ("$LOCAL" / "$REMOTE", the names quoted).
std::string expand_placeholders(const std::string& command, const std::string& base_name,
                               const std::string& mine_name);

}  // namespace tortilla
