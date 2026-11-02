// Whether a remote is on a forge Tortilla can sign in to, and what the
// Remotes page says of it: signed in, or the reason to sign in (a private
// repository needs it to show anything at all; a public one, to merge and
// comment). The forge is asked as anyone, without a token, where there's no
// account: which forge a host is, and whether it shows the repository.
#pragma once

#include "forge/forge.h"

#include <optional>
#include <string>
#include <string_view>

namespace tortilla
{

// The forge a host is: known_forge()'s by name, else asked as anyone (Gitea's
// /api/v1/version, then GitLab's /api/v4/version, a few seconds each). Each
// host is asked once while Tortilla runs; nullopt when it's neither, or
// didn't answer. Blocking: for a worker thread.
std::optional<forge_kind> probe_forge(std::string_view host);

// What the forge says of a repository when asked for it.
enum class repo_seen
{
    unknown,  // not asked yet, or no answer (offline, rate-limited)
    shown,    // there
    hidden,   // 404: private (or gone), as far as this asker can tell
};

// Asks for the repository as `token`'s owner; with no token, as anyone
// (GitHub's only while anonymous_budget() has some to spare). Blocking: for
// a worker thread.
repo_seen look_for_repo(forge_kind, const forge_repo&, std::string_view token = {});

// What the Remotes page shows of a remote.
struct remote_sign_in
{
    enum class offer
    {
        none,    // nothing to sign in to, or signed in already
        link,    // a quiet "Sign in..." link
        button,  // the strong nudge: a button
    };
    // For the list's column: the account, Private, Public, Not signed in;
    // "" for a remote that isn't on a forge.
    std::string summary;
    std::string text;                      // the line under the remote's fields
    check_state mark = check_state::none;  // success: the tick; failure: a cross
    offer sign_in = offer::none;
};

// The decision: `on_forge` (the remote's URL is a repository on a forge
// Tortilla can sign in to), the account for its host if there's one, what
// the forge said of the repository (asked as the account, or as anyone), and
// whether it's still being asked.
remote_sign_in describe_remote(bool on_forge, const forge_account* account, repo_seen, bool asking);

}  // namespace tortilla
