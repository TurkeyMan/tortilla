#include "pr_watch.h"

#include "git/remotes.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <thread>

namespace tortilla
{

std::vector<forge_remote> forge_remotes(const std::string& workdir)
{
    std::vector<forge_remote> out;
    const auto all = accounts();
    for (const std::string& name : remote_names(workdir))
    {
        const std::string url = config_value(workdir, "remote." + name + ".url");
        auto repo = repo_of_url(url);
        auto account = account_for_url(url, all);
        if (repo && account)
            out.push_back({.name = name, .repo = std::move(*repo), .account = std::move(*account)});
    }
    return out;
}

std::unordered_map<std::string, open_pr>
match_pull_requests(const std::vector<forge_remote>& remotes, const std::vector<open_pr>& prs,
                    const ref_map& refs)
{
    std::unordered_map<std::string, open_pr> out;
    for (const open_pr& p : prs)
        for (const forge_remote& r : remotes)
            if (r.repo == p.pr.head)
                out.emplace("refs/remotes/" + r.name + "/" + p.pr.head_branch, p);
    for (const auto& [id, list] : refs)
        for (const ref& r : list)
            if (r.kind == ref_kind::local_branch && !r.upstream.empty())
                if (auto it = out.find(r.upstream); it != out.end())
                {
                    open_pr p = it->second;  // (a copy: emplace may move the table)
                    out.emplace(r.full_name, std::move(p));
                }
    return out;
}

// What the worker keeps between asks: the tokens, so gh isn't run each time.
struct pr_watch::shared
{
    std::mutex mutex;
    std::map<std::string, std::string> tokens;  // "host login" -> token
    bool alive = true;
};

pr_watch::pr_watch(rap::ui& ui, std::string workdir, std::function<void()> changed)
    : ui_(ui)
    , workdir_(std::move(workdir))
    , changed_(std::move(changed))
    , shared_(std::make_shared<shared>())
{
    refresh();
}

pr_watch::~pr_watch()
{
    std::lock_guard lock(shared_->mutex);
    shared_->alive = false;
}

void pr_watch::refresh()
{
    if (asking_)
    {
        again_ = true;
        return;
    }
    timer_.reset();
    remotes_ = forge_remotes(workdir_);
    if (remotes_.empty())
    {
        if (!prs_.empty())
        {
            prs_.clear();
            if (changed_) changed_();
        }
        return;
    }
    asking_ = true;
    asked_ = std::chrono::steady_clock::now();
    std::thread(
        [this, sh = shared_, remotes = remotes_, &ui = ui_]
        {
            std::vector<open_pr> found;
            std::string error;
            std::vector<forge_repo> asked;
            for (const forge_remote& r : remotes)
            {
                if (std::ranges::find(asked, r.repo) != asked.end()) continue;
                asked.push_back(r.repo);
                const std::string key = r.account.host + ' ' + r.account.login;
                std::string token;
                {
                    std::lock_guard lock(sh->mutex);
                    if (!sh->alive) return;
                    token = sh->tokens[key];
                }
                if (token.empty())
                {
                    std::string why;
                    if (auto t = load_token(r.account, &why))
                        token = t->access;
                    else
                    {
                        error = why;
                        continue;
                    }
                }
                auto prs = open_pull_requests(r.account.kind, r.repo, token);
                std::lock_guard lock(sh->mutex);
                if (!prs)
                {
                    sh->tokens.erase(key);  // it may be what's wrong: asked for again
                    error = prs.error();
                    continue;
                }
                sh->tokens[key] = token;
                for (pull_request& pr : *prs) found.push_back({std::move(pr), r});
            }
            ui.post(
                [this, sh, found = std::move(found), error = std::move(error)]() mutable
                {
                    {
                        std::lock_guard lock(sh->mutex);
                        if (!sh->alive) return;
                    }
                    asking_ = false;
                    error_ = std::move(error);
                    prs_ = std::move(found);
                    if (changed_) changed_();
                    if (std::exchange(again_, false))
                        refresh();
                    else
                        schedule();
                });
        })
        .detach();
}

void pr_watch::nudge()
{
    if (std::chrono::steady_clock::now() - asked_ > std::chrono::seconds(15))
        refresh();
    else if (asking_)
        again_ = true;
}

// Sooner while checks run, as they'll change; rarely when it failed.
void pr_watch::schedule()
{
    const bool running = std::ranges::any_of(prs_, [](const open_pr& p)
                                             { return p.pr.checks == check_state::pending; });
    const int seconds = !error_.empty() ? 300 : running ? 20 : 120;
    timer_ = ui_.timer(seconds * 1000, false, [this] { refresh(); });
}

}  // namespace tortilla
