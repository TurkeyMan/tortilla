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

}  // namespace tortilla
