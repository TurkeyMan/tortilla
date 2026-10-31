// The badges on file manager icons (git/badges.h): Clean, Modified,
// Conflicted and Added. Drawn here once: tools/make_menu_icons.cpp renders
// them to the overlay icons Explorer lays over a file's icon
// (data/windows/badges/<kind>.ico), the badge in the bottom-left corner.
#pragma once

#include <rapport/rapport.h>

#include <span>
#include <string_view>

namespace tortilla
{

// The badge kinds, highest priority first, as the shell extension names them.
std::span<const std::string_view> badge_kinds();

// Draws `kind`'s badge filling the square at (x, y) of side `size`.
void paint_badge(rap::painter&, std::string_view kind, float x, float y, float size);

// How big the badge is on an icon of side `icon`: half of it, but never
// too small to read.
float badge_size(float icon);

}  // namespace tortilla
