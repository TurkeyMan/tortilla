// Icons for the file manager's menu entries, one per entry the Settings page
// lists (shell_menu::group_of). Drawn here once: the Context Menu page paints
// them, and tools/make_menu_icons.cpp renders them to the .ico files the
// shell extension shows (data/windows/menu/<group>.ico).
#pragma once

#include <rapport/rapport.h>

#include <span>
#include <string_view>

namespace tortilla
{

// Draws `group`'s icon in the square at (x, y) of side `size`; nothing for
// a group without one.
void paint_menu_icon(rap::painter&, std::string_view group, float x, float y, float size);

// Every group with an icon.
std::span<const std::string_view> menu_icon_groups();

}  // namespace tortilla
