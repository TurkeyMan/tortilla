// A repository's pull requests, kept up to date for the log: every remote on
// a forge Tortilla has an account for is asked on a worker thread, and again
// a while later (sooner while checks are running). Forges don't call back
// a desktop program, so this polls: one request a repository on GitHub.
#pragma once

#include "forge/forge.h"
#include "git/repo.h"

#include <rapport/rapport.h>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace tortilla
{

// A remote on a forge, and the account Tortilla talks to it as.
struct forge_remote
{
    std::string name;  // origin
    forge_repo repo;
    forge_account account;
};
// The repository's remotes that are on a forge with an account; quick git
// queries, for the UI thread.
std::vector<forge_remote> forge_remotes(const std::string& workdir);

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

class pr_watch
{
public:
    // `changed` after each answer that changes what's known.
    pr_watch(rap::ui&, std::string workdir, std::function<void()> changed);
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
    // Why the last ask failed; "" when it didn't.
    const std::string& error() const { return error_; }

private:
    struct shared;
    void schedule();

    rap::ui& ui_;
    std::string workdir_;
    std::function<void()> changed_;
    std::vector<forge_remote> remotes_;
    std::vector<open_pr> prs_;
    std::string error_;
    std::shared_ptr<shared> shared_;
    rap::subscription timer_;
    bool asking_ = false, again_ = false;
    std::chrono::steady_clock::time_point asked_{};
};

}  // namespace tortilla
