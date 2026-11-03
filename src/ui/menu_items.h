// Building context menus in TortoiseGit's shape: items, and separators
// only between groups that produced items [spec 1.11].
#pragma once

#include "git/git_install.h"
#include "i18n.h"
#include "shell/menu_icons.h"

#include <rapport/rapport.h>

#include <algorithm>
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

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

// Whether the desktop has an "Open with" chooser (rap::sys::open_with),
// asked once on a worker: on Linux it may wait for D-Bus to start the
// desktop portal. Taken as yes until it's answered.
inline bool open_with_available()
{
    static std::atomic<int> known{-1};
    static std::once_flag asked;
    std::call_once(asked,
                   [] { std::thread([] { known = rap::sys::open_with_available(); }).detach(); });
    return known != 0;
}

// "Open with...", greyed with the reason where there's no chooser
// [TortoiseGit: Open with].
inline rap::menu_item open_with_item(std::function<void()> action, bool enabled = true,
                                     bool available = open_with_available())
{
    return item(available ? _("Open with...") : _("Open with... (no desktop portal)"),
                std::move(action), enabled && available);
}

// Without git, the items that need it greyed with the reason, as other
// greyed items give theirs; those `reads` names (labels, or formats up to
// their first "{") run in process and stay, their submenus too.
inline void grey_without_git(rap::menu& m, const std::vector<std::string>& reads)
{
    if (git_installed()) return;
    auto kept = [&](const std::string& label)
    {
        return std::ranges::any_of(reads, [&](const std::string& r)
                                   { return label.starts_with(r.substr(0, r.find('{'))); });
    };
    const std::string why = " (" + git_missing_reason() + ")";
    for (rap::menu_item& i : m)
    {
        if (i.is_separator() || kept(i.label)) continue;
        if (!i.children.empty())
            grey_without_git(i.children, reads);
        else if (i.enabled)
        {
            i.enabled = false;
            i.label += why;
        }
    }
}

inline void separator(rap::menu& m)
{
    if (!m.empty() && !m.back().is_separator()) m.push_back(rap::menu_item::separator());
}

}  // namespace tortilla
