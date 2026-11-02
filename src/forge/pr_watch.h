// A repository's pull requests, kept up to date for the log: every remote on
// a forge Tortilla has an account for, or on github.com, gitlab.com or
// codeberg.org without one (asked as no one, for a public repository's), is
// asked on a worker thread, and again a while later (sooner while checks are
// running). Forges don't call back a desktop program, so this polls: one
// request a repository on GitHub signed in; as no one, its REST API's few,
// every ten minutes, within its hourly allowance.
#pragma once

#include "forge/forge.h"
#include "git/repo.h"

#include <rapport/rapport.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace tortilla
{

// A remote on a forge, and the account Tortilla talks to it as. Without one
// (a public forge's), the account holds only its kind and host, and it's
// asked as no one.
struct forge_remote
{
    std::string name;  // origin
    forge_repo repo;
    forge_account account;
    bool signed_in() const { return !account.login.empty(); }
};
// The repository's remotes that are on a forge with an account, or on one
// known_forge() names; quick git queries, for the UI thread.
std::vector<forge_remote> forge_remotes(const std::string& workdir);
// Of those, one a forge host, leading remote first: the newest tip of its
// default branch (<remote>/HEAD, else main, else master) as last fetched.
// A remote never fetched goes last. A git query: for a worker thread.
std::vector<forge_remote> leading_remotes(const std::string& workdir,
                                          std::vector<forge_remote> remotes);

// A pull request, and where it's open.
struct open_pr
{
    pull_request pr;
    forge_remote into;
};

// Which remote branches are pull requests' heads (the remote being the PR's
// head repository), by full name. Only those: a local branch can be moved
// anywhere until it's pushed, and the PR follows the push, not the branch.
std::unordered_map<std::string, open_pr> match_pull_requests(const std::vector<forge_remote>&,
                                                             const std::vector<open_pr>&);

// A PR's menu as what Tortilla may do there allows (`access`, pr_watch's):
// Merge... and Close..., greyed with why not when they can't be, briefly:
// "sign in to GitHub", "needs write access", "conflicts". Its author may
// close their own.
struct pr_menu_labels
{
    std::string merge, close;
    bool can_merge = true, can_close = true;
};
pr_menu_labels pr_menu_for(const open_pr&, repo_access);
// Create pull request...'s why not: "" when a remote is signed in.
std::string create_pr_blocker(const std::vector<forge_remote>&);
// "sign in to GitHub" (Codeberg, GitLab).
std::string sign_in_to(const forge_remote&);

// Whether an anonymous GitHub repository is asked now, last asked at `last`
// (seconds since 1970; 0: never): every ten minutes, a minute after a
// refresh() (a fetch, say), and not while what's left of the allowance is
// short.
bool anonymous_due(std::int64_t last, std::int64_t now, bool forced, const rate_budget&);
// How long until the next poll: sooner while checks run; rarely when it
// failed, or when only anonymous GitHub is asked (anonymous_due), and not
// before the allowance is full again when it's run out.
int poll_seconds(bool failed, bool running, bool only_anonymous_github, const rate_budget&,
                 std::int64_t now);

// A remote that came back hidden as no one (private, most likely, which a
// forge won't say): "github.com/you/repo: sign in to see its pull requests".
std::string hint_text(const forge_remote&);

class pr_watch
{
public:
    // `changed` after each answer that changes what's known. `branch_tips`:
    // want_branch_tips from the first ask.
    pr_watch(rap::ui&, std::string workdir, std::function<void()> changed,
             bool branch_tips = false);
    ~pr_watch();

    // Asks now: after a push, a fetch, or a PR made or merged here.
    void refresh();
    // Asks now unless it did a moment ago: when the repository's refs change,
    // which a fetch or a push does, but so does much else.
    void nudge();
    // The open pull requests, as last heard; empty before the first answer.
    const std::vector<open_pr>& pull_requests() const { return prs_; }
    // The forge remotes, as last read.
    const std::vector<forge_remote>& remotes() const { return remotes_; }
    // Why the last ask failed; "" when it didn't. A repository that can't be
    // seen isn't a failure: it's hidden (access, hints).
    const std::string& error() const { return error_; }
    // What Tortilla may do with a remote's repository, as last heard.
    repo_access access(const forge_remote&) const;
    // The remotes asked as no one that came back hidden: signing in may show
    // their pull requests (hint_text says so).
    const std::vector<forge_remote>& hints() const { return hints_; }
    // Whether to ask for the remotes' branches too, for the log's sync; from
    // the next ask on. Never as no one on GitHub (its allowance is short).
    void want_branch_tips(bool on) { want_tips_ = on; }
    // The branches of each forge remote that answered the last ask, by
    // remote name.
    const std::map<std::string, remote_tips, std::less<>>& branch_tips() const { return tips_; }

    // Every open watch's hints, each repository once: for Settings >
    // Accounts. The UI thread's.
    static std::vector<forge_remote> all_hints();

private:
    struct shared;
    void ask(bool forced);
    void schedule();

    rap::ui& ui_;
    std::string workdir_;
    std::function<void()> changed_;
    std::vector<forge_remote> remotes_;
    std::vector<open_pr> prs_;
    std::map<std::string, remote_tips, std::less<>> tips_;
    bool want_tips_ = false;
    std::string error_;
    std::map<std::string, repo_access> access_;  // by access_key
    std::vector<forge_remote> hints_;
    std::shared_ptr<shared> shared_;
    rap::subscription timer_;
    bool asking_ = false, again_ = false, again_forced_ = false;
    std::chrono::steady_clock::time_point asked_{};
};

}  // namespace tortilla
