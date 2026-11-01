// Settings' Accounts page: the forges Tortilla is signed in to (GitHub,
// GitLab, Gitea and Forgejo), for their pull requests and builds. Signing in
// and out take effect at once, as on the Remotes page: the token goes to the
// keychain, not to Tortilla's settings, so there's nothing for OK to save.
#pragma once

#include <rapport/rapport.h>

#include <functional>

namespace tortilla
{

// `parent` gives the window for its messages.
rap::node accounts_page(rap::ui&, std::function<rap::window*()> parent);

}  // namespace tortilla
