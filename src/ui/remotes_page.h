// Settings' Remotes page [TortoiseGit: Settings > Git > Remote]: the
// repository's remotes, each one's URL, push URL, tags fetched and pruning.
// Unlike the other pages it changes the repository at once (Save, Remove),
// as TortoiseGit's does: it's git's config, not Tortilla's settings.
#pragma once

#include <rapport/rapport.h>

#include <functional>
#include <string>

namespace tortilla
{

// `parent` gives the window for its messages.
rap::node remotes_page(rap::ui&, std::function<rap::window*()> parent, const std::string& repo_dir);

}  // namespace tortilla
