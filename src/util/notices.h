// What a repository's window wasn't left to say: a history edit that ended
// after its window closed with a warning (local changes left in the stash)
// or a failure (refs left moved). A GUI launch on Windows has no stderr, so
// each goes in a file of its own in config_dir("tortilla")/notices, and the
// next window on that repository shows it once and removes it.
//
// Between processes: a notice is written under a name of its own and
// renamed in whole, so no reader sees part of one; and it's taken by
// removing it, so only the process whose removal succeeds shows it.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace tortilla
{

std::filesystem::path notices_dir();

// A notice for the repository at `repo` (its working tree, or git dir if
// bare), with the time; false if it couldn't be written.
bool save_notice(const std::filesystem::path& dir, const std::string& repo,
                 const std::string& text);

// The notices for `repo`, oldest first, each with its time ("2026-11-07
// 18:04:31 UTC\n..."), removed as they're taken.
std::vector<std::string> take_notices(const std::filesystem::path& dir, const std::string& repo);

}  // namespace tortilla
