// Editing a conflicted file in a three-way merge tool, as TortoiseGit's
// Edit conflicts [spec 9]: the file's sides written beside it as
// <file>.BASE<ext>, <file>.LOCAL<ext> ("mine") and <file>.REMOTE<ext>
// ("theirs"), and the tool started with them and the file to save.
//
// The tool is a command line with TortoiseGit's placeholders: %base %mine
// %theirs %merged (the files), %bname %yname %tname %mname (their titles)
// and %wtroot. Kept in tortilla's settings ("merge.command"); git's
// merge.tool isn't used, as TortoiseGit doesn't.
#pragma once

#include <expected>
#include <string>
#include <vector>

namespace tortilla
{

struct detected_merge_tool
{
    std::string name, command;
};
// Merge tools found on this machine, the preferred first.
std::vector<detected_merge_tool> detect_merge_tools();

// The saved command, else the first tool found; "" if none.
std::string merge_command();
std::string saved_merge_command();
void save_merge_command(const std::string&);

// A conflicted file's sides, written beside it.
struct conflict_sides
{
    std::string path;                // repository-relative
    std::string base, mine, theirs;  // the files written (absolute); base may be empty, as the file
    std::string merged;              // the working-tree file (absolute)
    std::string base_title, mine_title, theirs_title;
    bool mine_deleted = false, theirs_deleted = false;  // a delete/modify conflict: no tool
    bool submodule = false;                             // a gitlink conflict: no tool
};
// Writes the sides of conflicted `path` (repository-relative). During a
// rebase "mine" is the commit being replayed, as the user means it.
std::expected<conflict_sides, std::string> write_conflict_sides(const std::string& workdir,
                                                                const std::string& path);
// Removes the files write_conflict_sides wrote.
void remove_conflict_sides(const conflict_sides&);

// A command line's words: spaces separate them outside quotes, and the
// quotes go (git's tool and editor settings, run without a shell).
std::vector<std::string> command_words(const std::string& command);

// The program and arguments to run: `command` split into words, quotes
// honoured, each placeholder replaced within its word (so no quoting is
// needed for paths with spaces). Without a file placeholder the four files
// go last.
std::vector<std::string> merge_tool_command(const std::string& command, const conflict_sides&,
                                            const std::string& workdir);

// Whether `path` still has unmerged entries in the index.
bool still_conflicted(const std::string& workdir, const std::string& path);

}  // namespace tortilla
