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
    network,
    remotes,
    accounts,
};

// Modal to `parent` (or a window of its own when null). `repo_dir` is the
// repository whose git config the pages describe; `saved` runs after OK.
void show_settings(rap::ui&, rap::window* parent, const std::string& repo_dir,
                  settings_page first = settings_page::general, std::function<void()> saved = {},
                  std::function<void()> closed = {});

// Says that choices weren't saved, and why (`error`, from settings::apply).
// The settings are as they were; what was chosen still applies to what the
// user asked for now.
// Closing/destruction and operation continuations belong in `dismissed`.
void report_unsaved_settings(rap::ui&, rap::window* parent, const std::string& error,
                              std::function<void()> dismissed = {});
// Called instead of quit(): report pending save errors while the event loop
// still runs, and quit only after the warning is dismissed. Never restarts it.
void quit_after_settings(rap::ui&);
// Keep for the loaded UI's lifetime, and reset before destroying the backend.
// Cancels pending completion continuations without calling a dying UI.
rap::subscription track_settings_warnings(rap::ui&);

}  // namespace tortilla
