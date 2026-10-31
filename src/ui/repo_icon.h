// Telling repositories' windows apart on the taskbar: each repository has a
// colour, and its windows' icon is Tortilla's with the taco shell in it.
// The first repository keeps the icon's own colours.
#pragma once

#include <rapport/rapport.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace tortilla
{

// The icon at one size, split in two (tools/make_icons.cpp): the icon
// without its shell, and the shell's grey shading. Built in from
// data/icons/layers (CMakeLists.txt).
struct icon_layers
{
    int size;
    std::span<const uint8_t> body, shell;
};
extern const std::vector<icon_layers> k_icon_layers;

// The repository's colour, from its git config (tortilla.color: "#rrggbb",
// or "plain" for the icon's own). A repository without one is given the
// next of a palette, the first ever "plain", and keeps it. nullopt: plain.
std::optional<rap::color> repo_color(const std::string& workdir);

// From now on, windows made with create_window carry the icon of the
// repository at `path` (anything inside it). Outside one, they keep
// Tortilla's.
void use_repo_icon(rap::ui&, const std::string& path);

// ui.create, with the repository's icon when use_repo_icon chose one.
std::unique_ptr<rap::window> create_window(rap::ui&, const rap::window_spec&,
                                           rap::window* parent = nullptr);

}  // namespace tortilla
