// The forges Tortilla signs in to: GitHub (and GitHub Enterprise), GitLab,
// and Gitea (Forgejo and Codeberg too: Forgejo speaks Gitea's API). An
// account is the forge, its host and the user; its token is in the user's
// keychain (rap::sys secrets), never in Tortilla's settings.
//
// Everything here that talks to a forge or the keychain blocks: for a
// worker thread.
#pragma once

#include <rapport/widgets.h>

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
// A web host, "host" or "host:port", as its origin: https://host:port, the
// host in lower case and the port spelt out (443 when none is given), so
// forge.example and forge.example:443 are one, forge.example:8443 another
// (rap::sys::url_origin's rules); "" for what isn't a host, matching none.
std::string web_origin(std::string_view host);
// The account for a remote's URL (https or ssh, scp-style too), by the web
// origin its repository is asked at (repo_of_url's host): an https URL's
// port is the web's, an ssh one's isn't (ssh://host:2222 is https://host).
// The first, if several share it.
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
// Authorization and the forge's Accept and User-Agent; no Authorization
// for an empty token, to ask as no one (a public repository's reads).
rap::sys::http_headers api_headers(forge_kind, std::string_view token);

// The forge a host is without an account for it: github.com, gitlab.com and
// codeberg.org, which Tortilla asks about public repositories as no one;
// nullopt for any other host, whose forge only an account says.
std::optional<forge_kind> known_forge(std::string_view host);

// GitHub's allowance for asking without signing in: 60 requests an hour
// from an address, whatever asks. As its replies last said, shared by the
// whole program (the watch's polls and the authors' pictures).
struct rate_budget
{
    int remaining = -1;         // -1: not heard yet
    std::int64_t reset_at = 0;  // seconds since 1970, when it's full again
};
rate_budget anonymous_budget();
// Whether `want` more requests leave `reserve` of it, at `now`: yes before
// anything's been heard, and once it's been reset.
bool budget_allows(const rate_budget&, int want, int reserve, std::int64_t now);

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

// One of CI's runs on the head: a check run or a commit status (GitHub,
// Gitea), or a pipeline's job (GitLab).
struct check_run
{
    std::string name;
    check_state state = check_state::none;
    // Seconds since 1970; 0 when the forge doesn't say.
    std::int64_t started_at = 0, completed_at = 0;
    std::string url;  // its page on the forge; may be empty
};

// What stands in the way of merging, besides the checks.
enum class merge_block
{
    conflicts,          // with its base
    review_required,    // an approving review
    changes_requested,  // a reviewer asked for changes
    behind,             // its base has moved on, and the rules want it up to date
    draft,
    discussions,    // threads to resolve (GitLab)
    blocked_other,  // the forge's other rules
};

// A reviewer's say on a pull request, latest first per reviewer.
struct pr_review
{
    enum class verdict
    {
        approved,
        changes_requested,
        commented,
        requested,  // asked to review, not yet done
    };
    std::string who;  // a login, or a team's name
    verdict say = verdict::commented;
};

struct pr_label
{
    std::string name;
    std::optional<rap::color> color;
};

struct pull_request
{
    int number = 0;
    std::string title, url;  // url: its web page
    forge_repo head;         // where its branch is: the repository, or a fork
    std::string head_branch, base_branch, head_sha;
    bool draft = false;
    check_state checks = check_state::none;
    // The forge's own rules stand in the way of merging, whatever the checks
    // say: a review it requires, changes asked for.
    bool blocked = false;
    // It conflicts with its base: it can't be merged at all as it is.
    bool conflicts = false;
    // The checks one by one, where the forge lists them (GitLab's jobs only
    // for a pipeline that failed or is running; else the pipeline itself).
    std::vector<check_run> runs;
    bool pipeline = false;  // GitLab's: the runs are a pipeline's
    // What stands in the way of merging it: nothing, when it can be.
    std::vector<merge_block> blocks;
    std::string author;  // who opened it: their login
    // Asked as no one, GitHub's list doesn't say whether it can be merged
    // (that's a request a PR), and its checks are asked only while the
    // hourly allowance lasts: what's not known isn't "none".
    bool merge_known = true, checks_known = true;
    std::string author_avatar;  // their picture's address; may be empty
    // Its description: GitLab's and Gitea's lists have it; GitHub's is
    // asked for when it's shown (pull_request_details).
    std::string body;
    bool body_known = false;
    // Its reviewers: GitHub's, GitLab's approvals and Gitea's
    // reviews asked for when it's shown (GitLab's and Gitea's list has
    // those asked to review).
    std::vector<pr_review> reviews;
    bool reviews_known = false;
    std::vector<pr_label> labels;
};

// What the signed-in user (or no one) may do with a repository.
enum class repo_access
{
    unknown,  // not asked yet
    hidden,   // it can't be seen: private, or gone (a forge doesn't say which)
    read,     // its pull requests can be seen, not merged or closed
    triage,   // closed too, not merged (GitHub's triage role)
    write,
};

// An ISO 8601 time as the forges give it (2026-10-21T10:44:02Z, with a
// fraction or an offset too), in seconds since 1970; 0 if it isn't one.
std::int64_t parse_iso_time(std::string_view);

// A branch on a remote, and the commit it's at.
struct branch_tip
{
    std::string name, sha;  // name without refs/heads/
};
// A remote's branches as it lists them. A long list comes in pages: then
// only the first, and `complete` is false (a branch missing from it may
// still be there).
struct remote_tips
{
    std::vector<branch_tip> branches;
    bool complete = true;
};

// The repository's open pull requests (merge requests on GitLab), with their
// checks: the newest hundred; and what the token may do there. Hidden (404,
// or 403 signed in) isn't a failure: there's nothing to show. An empty token
// asks as no one; GitHub's GraphQL needs one, so then its REST API, within
// anonymous_budget().
struct pull_requests_reply
{
    std::vector<pull_request> prs;
    repo_access access = repo_access::unknown;
    bool rate_limited = false;  // GitHub's allowance ran out: nothing was asked
    // Its branches, when asked for and the forge said: for telling a fetch
    // has something to bring.
    std::optional<remote_tips> tips;
};
// `ask_access`: GitLab's and Gitea's take a request more (GitHub's comes
// with the list), wanted once. `ask_tips`: the branches too, in the same
// query on GitHub signed in, a request more on GitLab and Gitea; never as
// no one on GitHub (its allowance is short: ls-remote costs none of it).
std::expected<pull_requests_reply, std::string> open_pull_requests(forge_kind, const forge_repo&,
                                                                   std::string_view token,
                                                                   bool ask_access = true,
                                                                   bool ask_tips = false);

// GitHub's REST answers, as no one, for the tests: a /pulls list, and the
// checks from an /actions/runs list (each workflow's latest run on a head)
// or one head's /check-runs.
std::vector<pull_request> parse_github_rest_pulls(std::string_view json, const forge_repo&);
void apply_github_workflow_runs(std::string_view json, std::vector<pull_request>&);
bool apply_github_check_runs(std::string_view json, pull_request&);

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

// -- merging and closing ------------------------------------------------------

enum class merge_method
{
    merge,   // a merge commit
    squash,  // the branch's commits as one
    rebase,  // the branch's commits replayed onto the base
};

// What merging a pull request takes, as the forge's own page would say: the
// methods the repository allows, and whether it can be merged now.
struct merge_state
{
    enum class verdict
    {
        ready,     // it can be merged
        waiting,   // only checks still running stand in the way
        blocked,   // something else does: `why`
        checking,  // the forge is still working it out: ask again shortly
    };
    verdict state = verdict::checking;
    std::string why;  // when it isn't ready, in words
    std::vector<merge_method> methods;
    merge_method preferred = merge_method::merge;
    bool can_merge_when_ready = false;  // the forge merges it itself once checks pass
    bool can_bypass = false;            // an admin may merge it despite the rules
    bool merge_when_ready_set = false;  // already set to merge when ready
    bool forge_deletes_branch = false;  // the repository deletes merged branches itself
    std::string title, head_sha;
    std::string node_id;  // GitHub's id for the PR, for merging when ready
};

std::expected<merge_state, std::string>
pull_request_merge_state(forge_kind, const forge_repo&, int number, std::string_view token);

struct merge_request
{
    merge_method method = merge_method::merge;
    std::string commit_title, commit_message;  // empty: the forge's own
    std::string head_sha;                      // refused if the branch has moved since
    bool when_ready = false;                   // merge once the checks pass, rather than now
    std::string node_id;                       // (GitHub, for when_ready)
};
// Merges it (on the forge, which does the work), or sets it to merge when
// ready. "" when done; else why not.
std::string merge_pull_request(forge_kind, const forge_repo&, int number, const merge_request&,
                               std::string_view token);

// Closes it without merging, `comment` (the reason) posted first if given.
std::string close_pull_request(forge_kind, const forge_repo&, int number, std::string_view comment,
                               std::string_view token);

// Deletes a branch on the forge (after its PR is merged or closed).
std::string delete_branch(forge_kind, const forge_repo&, std::string_view branch,
                          std::string_view token);

// -- authors' pictures --------------------------------------------------------

// A GitHub noreply address's picture, `px` square, without asking the API:
// 12345+login@users.noreply.github.com by its user id, the older
// login@users.noreply.github.com by its login. "" for any other address.
std::string noreply_avatar_url(std::string_view email, int px);

// Whether a commit's author (name, email) is the forge user `login`, as far
// as can be told without asking the forge: their email's user part is the
// login (a noreply address's too, after its id), their name is, or `account`
// is that login and has their name. Unsure is no.
bool author_is(std::string_view login, std::string_view name, std::string_view email,
               const forge_account& account);

// The forge's picture of a commit's author, `px` square (as near as the
// forge has it): GitHub's and Gitea's for the user the commit's author is,
// GitLab's for the user with that public email. The picture's bytes; ""
// when the forge knows the commit but has no picture for its author (a
// made-up one, Gravatar's or an identicon, is none); why not, when it
// couldn't say (offline, or the commit isn't there). An empty token asks as
// no one: GitHub's within anonymous_budget().
std::expected<std::string, std::string>
author_avatar(forge_kind, const forge_repo&, std::string_view commit, std::string_view email,
              int px, std::string_view token, const std::atomic<bool>* cancelled = nullptr);

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
