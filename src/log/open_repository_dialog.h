// Open repository, for a log window's new tab (+ and Ctrl+T): the
// repositories opened lately, and Browse... for any other folder.
#pragma once

#include <rapport/rapport.h>

#include <functional>
#include <string>
#include <vector>

namespace tortilla
{

// The repositories logs were opened on, newest first (log.recentRepositories),
// leaving out those whose folder has gone.
std::vector<std::string> recent_repositories();
// `path` (a repository's folder) first among them.
void remember_repository(const std::string& path);

// The recent repositories in a list, a double click or Open opening one,
// and Browse... a folder picker; with none recent, the folder picker
// alone. `done` gets the folder chosen; nothing if the user cancels.
void open_repository_dialog(rap::ui&, rap::window* parent,
                            std::function<void(const std::string& path)> done);

}  // namespace tortilla
