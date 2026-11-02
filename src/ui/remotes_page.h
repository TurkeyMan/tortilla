// Settings' Remotes page [TortoiseGit: Settings > Git > Remote]: the
// repository's remotes, each one's URL, push URL, tags fetched and pruning.
// Unlike the other pages it changes the repository at once (Save, Remove),
// as TortoiseGit's does: it's git's config, not Tortilla's settings.
//
// A remote on a forge says whether Tortilla is signed in there, and if it
// isn't, why to (a private repository shows nothing without it) and offers
// Sign in..., which goes to the Accounts page.
#pragma once

#include "ui/accounts_page.h"

#include <rapport/rapport.h>

#include <functional>
#include <string>

namespace tortilla
{

// `parent` gives the window for its messages. `sign_in` is Sign in... for a
// remote's forge and server: the Accounts page, set for it. `shown`, if
// given, gets what the dialog calls each time the page is shown again: it
// looks at the accounts afresh, as one may have been signed in to since.
rap::node remotes_page(rap::ui&, std::function<rap::window*()> parent, const std::string& repo_dir,
                       sign_in_to sign_in = {}, std::function<void()>* shown = nullptr);

}  // namespace tortilla
