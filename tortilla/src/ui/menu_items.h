// Building context menus in TortoiseGit's shape: items, placeholders for
// what tortilla doesn't do yet, and separators only between groups that
// produced items [spec 1.11].
#pragma once

#include <rapport/rapport.h>

#include <functional>
#include <string>

namespace tortilla
{

inline rap::MenuItem item(std::string label, std::function<void()> action, bool enabled = true)
{
    return {.label = std::move(label), .action = std::move(action), .enabled = enabled};
}

// Shown so the menu has TortoiseGit's shape, until the feature exists.
inline rap::MenuItem todo(std::string label)
{
    return {.label = std::move(label), .enabled = false};
}

inline void separator(rap::Menu& m)
{
    if (!m.empty() && !m.back().isSeparator()) m.push_back(rap::MenuItem::separator());
}

}  // namespace tortilla
