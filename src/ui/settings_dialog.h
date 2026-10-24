// tortilla's Settings dialog [TortoiseGit: Settings]: pages listed down the
// left, each built the first time it's shown.
#pragma once

#include <rapport/rapport.h>

#include <functional>
#include <string>

namespace tortilla
{

enum class settings_page
{
    // In the order shown.
    general,
    context_menu,
    diff_viewer,
};

// Modal to `parent` (or a window of its own when null). `repo_dir` is the
// repository whose git config the pages describe; `saved` runs after OK.
void show_settings(rap::ui&, rap::window* parent, const std::string& repo_dir,
                  settings_page first = settings_page::general, std::function<void()> saved = {},
                  std::function<void()> closed = {});

}  // namespace tortilla
