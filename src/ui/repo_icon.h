// Telling repositories' windows apart on the taskbar: their icon is
// Tortilla's with the taco shell coloured. The colours go to the
// repositories open at the time: the first keeps the icon's own, each other
// one open meanwhile gets a free colour, freed again when it closes.
// Worktrees and submodules count as repositories of their own.
#pragma once

#include <rapport/rapport.h>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace tortilla
{

// The icon at one size, whole and split in two (tools/make_icons.cpp): the
// icon without its shell, and the shell's grey shading. Built in from
// data/icons/layers (CMakeLists.txt).
struct icon_layers
{
    int size;
    std::span<const uint8_t> icon, body, shell;
};
extern const std::vector<icon_layers> k_icon_layers;

// From now on, windows made with create_window carry the icon of the
// repository at `path` (anything inside it; with `outer`, the repository
// around a submodule at `path`). Outside one, they keep Tortilla's.
void use_repo_icon(rap::ui&, const std::string& path, bool outer = false);

// ui.create, with the repository's icon when use_repo_icon chose one, and
// Tortilla's own otherwise: set, not left to the desktop to find by name
// (which an uninstalled build, or WSLg, doesn't).
std::unique_ptr<rap::window> create_window(rap::ui&, const rap::window_spec&,
                                           rap::window* parent = nullptr);

class repo;

// What tells a repository's windows apart: their icon, and the app id that
// groups them on the taskbar ("" for Tortilla's own group). `lease` holds
// the repository's colour for as long as any copy of it is kept.
struct repo_identity
{
    rap::icon icon;
    std::string app_id;
    std::shared_ptr<const void> lease;
};
// The repository's own identity, as use_repo_icon would choose it, without
// changing what create_window gives windows: several repositories' at once
// in one process (a log window's tabs). Repositories open in this process
// keep the colour they have.
repo_identity repo_identity_of(rap::ui&, const repo&);
// The identity use_repo_icon chose, which create_window gives windows; with
// none chosen, Tortilla's own icon.
repo_identity current_repo_identity(rap::ui&);

}  // namespace tortilla
