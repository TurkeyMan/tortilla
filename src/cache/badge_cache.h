// The badges' status cache: what tortilla cache serves the file manager
// extensions. It answers at once from what it knows, and works out the rest
// in the background: a repository is loaded on its first question (its
// files have no badge until then), then watched and loaded again when it
// changes. Paths asked about whose badge changed are reported, so the file
// manager can draw them again.
#pragma once

#include "git/badges.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <vector>

namespace tortilla
{

class badge_cache
{
public:
    // `changed` gets paths asked about whose badge changed, from the
    // cache's own thread.
    explicit badge_cache(std::function<void(const std::vector<std::filesystem::path>&)> changed);
    ~badge_cache();

    // Never waits on git.
    badge ask(const std::filesystem::path&);

    // Since the last question.
    std::chrono::steady_clock::duration idle() const;

    struct impl;

private:
    std::unique_ptr<impl> impl_;
};

}  // namespace tortilla
