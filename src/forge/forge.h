// The forges Tortilla signs in to: GitHub (and GitHub Enterprise), GitLab,
// and Gitea (Forgejo and Codeberg too: Forgejo speaks Gitea's API). An
// account is the forge, its host and the user; its token is in the user's
// keychain (rap::sys secrets), never in Tortilla's settings.
//
// Everything here that talks to a forge or the keychain blocks: for a
// worker thread.
#pragma once

#include <rapport/sys.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tortilla
{

enum class forge_kind
{
    github,
    gitlab,
    gitea,
};

// "GitHub", "GitLab", "Gitea / Forgejo".
std::string forge_name(forge_kind);
// Where an account on it usually is: github.com, gitlab.com, codeberg.org.
std::string_view default_host(forge_kind);

struct forge_account
{
    forge_kind kind = forge_kind::github;
    std::string host;        // github.com, gitlab.example.org: no scheme
    std::string login;       // the user name on it
    std::string name;        // their display name; may be empty
    std::string avatar_url;  // may be empty
    // Where its token is: the keychain, or (true) the GitHub CLI's sign-in,
    // asked each time (`gh auth token`), so gh's logins and renewals hold.
    bool via_gh = false;
};

// What signing in gives. A GitLab sign-in through the browser expires and
// is renewed with its refresh token; a pasted token lasts until revoked.
struct forge_token
{
    std::string access;
    std::string refresh;          // empty: can't be renewed
    std::int64_t expires_at = 0;  // seconds since 1970; 0: doesn't expire
};

// -- accounts -----------------------------------------------------------------

// The signed-in accounts, as saved ("forge.accounts").
std::vector<forge_account> saved_accounts();
void save_accounts(const std::vector<forge_account>&);
// The accounts Tortilla uses: the saved ones, and the GitHub CLI's sign-ins
// (gh_accounts) as they are, with nothing to set up, unless signed out of
// here (gh_account_off).
std::vector<forge_account> accounts();
// Whether Tortilla is told not to use gh's sign-in to that account; signing
// in through gh again lifts it.
void set_gh_account_off(const forge_account&, bool off);
// The account for a remote's URL (https or ssh, scp-style too), by host;
// the first, if several share it.
std::optional<forge_account> account_for_url(std::string_view url,
                                             const std::vector<forge_account>&);

// The keychain's copy of an account's token.
bool store_token(const forge_account&, const forge_token&);
// nullopt when there's none, or the keychain can't be read (then why).
std::optional<forge_token> load_token(const forge_account&, std::string* why = nullptr);
bool erase_token(const forge_account&);

// The GitHub CLI, where it's installed: its token for `host`, or why not
// (not signed in there, say).
bool gh_installed();
// Who the GitHub CLI is signed in as, on each host, from its hosts.yml (no gh
// run, so it's quick): the accounts, via_gh, without a name or avatar.
std::vector<forge_account> gh_accounts();
std::expected<std::string, std::string> gh_token(std::string_view host);

// -- talking to it ------------------------------------------------------------

// The API's root: https://api.github.com, https://host/api/v4, ...
std::string api_root(forge_kind, std::string_view host);
// Authorization and the forge's Accept and User-Agent.
rap::sys::http_headers api_headers(forge_kind, std::string_view token);

// Who the token belongs to (GET /user); why not, in words.
std::expected<forge_account, std::string> who_am_i(forge_kind, std::string_view host,
                                                   std::string_view token);

// -- repositories and their pull requests -------------------------------------

// A repository on a forge, from a remote's URL: its host, and its path there
// as owner (a GitLab group may hold slashes) and name, without ".git".
struct forge_repo
{
    std::string host, owner, name;
    std::string path() const { return owner + "/" + name; }
    bool operator==(const forge_repo&) const = default;
};
std::optional<forge_repo> repo_of_url(std::string_view url);

// The checks on a pull request's head: CI's runs, combined.
enum class check_state
{
    none,  // nothing reported
    pending,
    success,
    failure,
};

struct pull_request
{
    int number = 0;
    std::string title, url;  // url: its web page
    forge_repo head;         // where its branch is: the repository, or a fork
    std::string head_branch, base_branch, head_sha;
    bool draft = false;
    check_state checks = check_state::none;
};

// The repository's open pull requests (merge requests on GitLab), with their
// checks: the newest hundred.
std::expected<std::vector<pull_request>, std::string>
open_pull_requests(forge_kind, const forge_repo&, std::string_view token);

// The branch pull requests go into by default (main, say).
std::expected<std::string, std::string> default_branch(forge_kind, const forge_repo&,
                                                       std::string_view token);

// Where a pull request from `head` can go: itself, and the repositories it's
// a fork of (its parent and, a fork of a fork, the original).
std::expected<std::vector<forge_repo>, std::string>
pull_request_targets(forge_kind, const forge_repo& head, std::string_view token);

struct new_pull_request
{
    forge_repo head;  // where the branch is: `into` itself, or a fork of it
    std::string head_branch, base_branch, title, body;
    bool draft = false;
};
// Opens one on `into`; GitLab's only from a branch in the same repository.
std::expected<pull_request, std::string> create_pull_request(forge_kind, const forge_repo& into,
                                                             const new_pull_request&,
                                                             std::string_view token);

// -- signing in through the browser (OAuth's device flow) ---------------------

// Whether Tortilla can: it needs an application registered on that host,
// which it has only for github.com and gitlab.com.
bool browser_sign_in(forge_kind, std::string_view host);
// Where to make a token by hand, for pasting.
std::string token_page(forge_kind, std::string_view host);
// What a pasted token needs to be allowed, in words.
std::string token_scopes(forge_kind);

struct device_code
{
    std::string device;             // Tortilla's, for asking
    std::string user;               // the user's, to type: WDJB-MJHT
    std::string verification;       // where to type it
    std::string verification_full;  // a page with the code filled in, if the forge gives one
    std::chrono::seconds interval{5};
    std::chrono::steady_clock::time_point expires;
};

std::expected<device_code, std::string> start_sign_in(forge_kind, std::string_view host);
// Asks until the user approves, refuses, the code expires or `cancelled`.
std::expected<forge_token, std::string> finish_sign_in(forge_kind, std::string_view host,
                                                       const device_code&,
                                                       const std::atomic<bool>& cancelled);
// A new token for an expired one, where it can be.
std::expected<forge_token, std::string> renew(forge_kind, std::string_view host,
                                              const forge_token&);

}  // namespace tortilla
