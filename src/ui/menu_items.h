// Building context menus in TortoiseGit's shape: items, placeholders for
// what tortilla doesn't do yet, and separators only between groups that
// produced items [spec 1.11].
#pragma once

#include "shell/menu_icons.h"

#include <rapport/rapport.h>

#include <algorithm>
#include <functional>
#include <string>
#include <string_view>

namespace tortilla
{

inline rap::menu_item item(std::string label, std::function<void()> action, bool enabled = true)
{
    return {.label = std::move(label), .action = std::move(action), .enabled = enabled};
}

// The file manager menu's icon for `group` (menu_icons.h).
inline rap::icon menu_icon(std::string_view group)
{
    return rap::icon::drawn(
        [group](rap::painter& p, rap::size_f s)
        {
            const float side = std::min(s.w, s.h);
            paint_menu_icon(p, group, (s.w - side) / 2, (s.h - side) / 2, side);
        });
}

inline rap::menu_item item(std::string label, std::string_view group, std::function<void()> action,
                           bool enabled = true)
{
    rap::menu_item m = item(std::move(label), std::move(action), enabled);
    m.icon = menu_icon(group);
    return m;
}

// Shown so the menu has TortoiseGit's shape, until the feature exists.
inline rap::menu_item todo(std::string label)
{
    return {.label = std::move(label), .enabled = false};
}

inline void separator(rap::menu& m)
{
    if (!m.empty() && !m.back().is_separator()) m.push_back(rap::menu_item::separator());
}

}  // namespace tortilla
