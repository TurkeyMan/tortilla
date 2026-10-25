// PCManFM's and PCManFM-Qt's Tortilla menu: freedesktop "file manager
// actions" (.desktop files in share/file-manager/actions), which libfm and
// libfm-qt read, those file managers having no menu extensions to load.
// The package installs the menu for the default placements (written at
// build time by tools/make_fm_actions.cpp); when Settings saves others, the
// user's own set goes in $XDG_DATA_HOME/file-manager/actions. A packaged
// file can't be overridden there by name, so the user's files have their
// own names (tortilla-user*), and the packaged ones' condition steps aside
// while they exist (data/linux/fm-condition).
#pragma once

#include "shell/menu_table.h"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace tortilla::fm_actions
{

// An entry's name, and its translations by language tag ("de", "pt-BR").
struct name
{
    std::string text;
    std::map<std::string, std::string> translated;
};
using names = std::map<std::string, name, std::less<>>;  // by menu_table.h row id

// The row ids PCManFM's menu offers: libfm's conditions can't see an item's
// state, as the shell extensions do, so a fixed list for each side of a
// working tree.
std::vector<std::string> ids();

// The names from Tortilla's catalogues in `translations` (at build time).
names from_catalogues(const std::filesystem::path& translations);
// The names file the package installs beside the menu, and reading it back.
void write_names(const std::filesystem::path& file, const names&);
names read_names(const std::filesystem::path& file);

struct programs
{
    std::string condition;  // the installed fm-condition
    std::string tortilla;   // the program
};

// Writes the menu for `where` into `out`: a Tortilla submenu for each side
// of a working tree, and "Git <entry>" in the file manager's own menu for
// the entries chosen as top level. Hidden entries are left out; Shift-only
// ones go in the submenu, libfm not seeing Shift. The packaged set's files
// are tortilla*.desktop; the user's (`user`) tortilla-user*.desktop. Each
// file is written beside its final name and renamed into place, so the file
// manager never reads half of one. Returns the files written.
std::vector<std::filesystem::path> write(const std::filesystem::path& out,
                                         const shell_menu::placements& where, const names&,
                                         const programs&, bool user);

// After Settings saves the menu's placements: the user's set when they
// differ from the defaults for PCManFM's entries and its menu is installed,
// else none. Only ever writes or deletes tortilla-user*.desktop. PCManFM-Qt
// notices the change by the next right-click.
void update_user_set(const shell_menu::placements& where);

}  // namespace tortilla::fm_actions
