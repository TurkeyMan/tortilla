// The badges' status cache: what tortilla cache serves the file manager
// extensions. It answers at once from what it knows, and works out the rest
// in the background: a repository is loaded when something inside it is
// asked about (its files have no badge until then), then watched and loaded
// again when it changes. A repository's own folder, seen from outside it,
// shows a summary instead: remembered for every repository met, across
// runs, and refreshed with a plain git status when it's a while old, so a
// folder of repositories shows each one's state without loading them all.
// Paths asked about whose badge changed are reported, so the file manager
// can draw them again.
#pragma once

#include "git/badges.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string_view>
#include <vector>

namespace tortilla
{

class badge_cache
{
public:
    // `changed` gets paths asked about whose badge changed, from the
    // cache's own thread.
    // `summaries`: where the repositories' summaries are kept between runs;
    // empty for Tortilla's cache folder.
    explicit badge_cache(std::function<void(const std::vector<std::filesystem::path>&)> changed,
                         std::filesystem::path summaries = {});
    ~badge_cache();

    // Never waits on git.
    badge ask(const std::filesystem::path&);

    // The badge, and whether it can be relied on: `current` when the
    // repository is loaded, watched and not waiting to load a change;
    // `untracked` (then) when the path is untracked or holds untracked files.
    struct state
    {
        badge b = badge::none;
        bool current = false;
        bool untracked = false;
    };
    state look(const std::filesystem::path&);
    // The same for a path in UTF-8, as requests carry it: no path object made.
    state look(std::string_view utf8_path);

    // Since the last question.
    std::chrono::steady_clock::duration idle() const;

    struct impl;

private:
    std::unique_ptr<impl> impl_;
};

}  // namespace tortilla
