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

enum class DiffMode
{
    Builtin,
    Git,
    Custom,
};

// The saved mode; with none saved, git's diff.tool when the repository (or
// the user's git config) has one, else the built-in viewer.
DiffMode diffMode(const std::string& repoDir);
std::string diffCommand();
void saveDiffSettings(DiffMode, const std::string& command);

// git's diff.tool for `repoDir`, or "" if unset.
std::string gitDiffTool(const std::string& repoDir);

// Diff tools found on this machine, with command lines ready for the
// Custom setting. Only tools that compare folders, since a commit's
// changes open as one folder comparison.
struct DetectedTool
{
    std::string name, command;
};
std::vector<DetectedTool> detectDiffTools();

// git arguments that run `command` (placeholders and all) as a one-off
// difftool comparing `revisions` (as for git diff): folder to folder, or
// file to file when `paths` names exactly one file.
std::vector<std::string> customDifftoolArgs(const std::string& command,
                                            const std::vector<std::string>& revisions,
                                            const std::string& baseName,
                                            const std::string& mineName,
                                            const std::vector<std::string>& paths = {});

// `command` with its placeholders turned into the shell words git's
// difftool runs it with ("$LOCAL" / "$REMOTE", the names quoted).
std::string expandPlaceholders(const std::string& command, const std::string& baseName,
                               const std::string& mineName);

}  // namespace tortilla
