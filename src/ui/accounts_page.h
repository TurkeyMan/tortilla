// Settings' Accounts page: the forges Tortilla is signed in to (GitHub,
// GitLab, Gitea and Forgejo), for their pull requests and builds. Signing in
// and out take effect at once, as on the Remotes page: the token goes to the
// keychain, not to Tortilla's settings, so there's nothing for OK to save.
#pragma once

#include "forge/forge.h"

#include <rapport/rapport.h>

#include <functional>
#include <string>

namespace tortilla
{

// Signing in to a forge's server: the Sign in group set for it, and the
// keyboard on the way to sign in there (the browser where Tortilla can, else
// the token). For another page's Sign in...; nothing once the page is gone.
using forge_sign_in = std::function<void(forge_kind, const std::string& host)>;

// `parent` gives the window for its messages. `prefill`, if given, gets the
// page's forge_sign_in once the page is made.
rap::node accounts_page(rap::ui&, std::function<rap::window*()> parent,
                        forge_sign_in* prefill = nullptr);

}  // namespace tortilla
