// tortilla's Settings dialog [TortoiseGit: Settings]: pages listed down the
// left, each built the first time it's shown.
#pragma once

#include <rapport/rapport.h>

#include <functional>
#include <string>

namespace tortilla
{

enum class SettingsPage
{
    General,
    DiffViewer,
};

// Modal to `parent` (or a window of its own when null). `repoDir` is the
// repository whose git config the pages describe; `saved` runs after OK.
void showSettings(rap::Ui&, rap::Window* parent, const std::string& repoDir,
                  SettingsPage first = SettingsPage::General, std::function<void()> saved = {},
                  std::function<void()> closed = {});

}  // namespace tortilla
