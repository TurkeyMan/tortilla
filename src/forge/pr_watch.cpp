#include "pr_watch.h"

#include "git/command.h"
#include "git/remotes.h"
#include "i18n.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <limits>
#include <map>
#include <mutex>
#include <thread>

namespace tortilla
{
namespace
{

std::int64_t now_seconds()
{
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// A repository as one account (or no one) sees it.
std::string access_key(const forge_remote& r)
{
    return r.repo.host + ' ' + r.repo.path() + ' ' + r.account.login;
}

bool anonymous_github(const forge_remote& r)
{
    return !r.signed_in() && r.account.kind == forge_kind::github;
}

// The open watches, for all_hints(): the UI thread's.
std::vector<const pr_watch*>& watches()
{
    static std::vector<const pr_watch*> all;
    return all;
}

}  // namespace

std::vector<forge_remote> forge_remotes(const std::string& workdir)
{
    std::vector<forge_remote> out;
    const auto all = accounts();
    for (const std::string& name : remote_names(workdir))
    {
        const std::string url = config_value(workdir, "remote." + name + ".url");
        auto repo = repo_of_url(url);
        if (!repo) continue;
        auto account = account_for_url(url, all);
        // A public forge's without one: asked as no one.
        if (!account)
            if (const auto kind = known_forge(repo->host))
                account = forge_account{.kind = *kind, .host = std::string(default_host(*kind))};
        if (account)
            out.push_back({.name = name, .repo = std::move(*repo), .account = std::move(*account)});
    }
    return out;
}

std::vector<forge_remote> leading_remotes(const std::string& workdir,
                                          std::vector<forge_remote> remotes)
{
    if (remotes.empty()) return remotes;
    constexpr std::array<std::string_view, 3> defaults{"/HEAD", "/main", "/master"};
    // Every candidate in one git run: for-each-ref follows <remote>/HEAD to
    // its branch.
    std::vector<std::string> args{"for-each-ref", "--format=%(refname) %(committerdate:unix)"};
    for (const forge_remote& r : remotes)
        for (std::string_view b : defaults)
            args.push_back("refs/remotes/" + r.name + std::string(b));
    const git_result listed = run_git(workdir, args, git_output::stdout_only);
    std::map<std::string_view, std::int64_t, std::less<>> tips;  // views `listed`
    for (std::string_view rest = listed.output; !rest.empty();)
    {
        const size_t end = rest.find('\n');
        const std::string_view line = rest.substr(0, end);
        rest.remove_prefix(end == std::string_view::npos ? rest.size() : end + 1);
        const size_t space = line.rfind(' ');
        if (space == std::string_view::npos) continue;
        std::int64_t when = 0;
        const std::string_view time = line.substr(space + 1);
        std::from_chars(time.data(), time.data() + time.size(), when);
        tips.emplace(line.substr(0, space), when);
    }
    auto tip = [&](const forge_remote& r)
    {
        for (std::string_view b : defaults)
            if (auto it = tips.find("refs/remotes/" + r.name + std::string(b)); it != tips.end())
                return it->second;
        return std::numeric_limits<std::int64_t>::min();
    };
    std::vector<std::pair<std::int64_t, forge_remote>> ranked;
    for (forge_remote& r : remotes) ranked.emplace_back(tip(r), std::move(r));
    std::ranges::stable_sort(ranked, std::greater<>{}, [](const auto& p) { return p.first; });
    std::vector<forge_remote> out;
    for (auto& [when, r] : ranked)
        if (std::ranges::none_of(out, [&](const forge_remote& o)
                                 { return o.account.host == r.account.host; }))
            out.push_back(std::move(r));
    return out;
}

std::unordered_map<std::string, open_pr>
match_pull_requests(const std::vector<forge_remote>& remotes, const std::vector<open_pr>& prs)
{
    std::unordered_map<std::string, open_pr> out;
    for (const open_pr& p : prs)
        for (const forge_remote& r : remotes)
            if (r.repo == p.pr.head)
                out.emplace("refs/remotes/" + r.name + "/" + p.pr.head_branch, p);
    return out;
}

std::string sign_in_to(const forge_remote& r)
{
    const auto kind = known_forge(r.account.host);
    const std::string name = !kind                        ? r.account.host
                             : *kind == forge_kind::gitea ? std::string("Codeberg")
                                                          : forge_name(*kind);
    return rap::i18n::format(_("sign in to {0}"), name);
}

pr_menu_labels pr_menu_for(const open_pr& p, repo_access access)
{
    pr_menu_labels m{.merge = _("_Merge..."), .close = _("_Close...")};
    std::string why;
    if (!p.into.signed_in())
        why = sign_in_to(p.into);
    else if (access == repo_access::read || access == repo_access::triage ||
             access == repo_access::hidden)
        why = _("needs write access");
    // Its author may close it whoever else may; so may a triager.
    const std::string_view login = p.into.account.login, author = p.pr.author;
    const bool own = p.into.signed_in() &&
                     (access == repo_access::triage ||
                      std::ranges::equal(login, author,
                                         [](char x, char y)
                                         {
                                             return std::tolower(static_cast<unsigned char>(x)) ==
                                                    std::tolower(static_cast<unsigned char>(y));
                                         }));
    if (!why.empty())
    {
        m.can_merge = false;
        m.merge = rap::i18n::format(_("_Merge... ({0})"), why);
    }
    // Conflicts can't be merged at all: nothing for the dialog to offer.
    else if (p.pr.conflicts)
    {
        m.can_merge = false;
        m.merge = _("_Merge... (conflicts)");
    }
    if (!why.empty() && !own)
    {
        m.can_close = false;
        m.close = rap::i18n::format(_("_Close... ({0})"), why);
    }
    return m;
}

std::string create_pr_blocker(const std::vector<forge_remote>& remotes)
{
    if (remotes.empty() || std::ranges::any_of(remotes, &forge_remote::signed_in)) return {};
    return sign_in_to(remotes.front());
}

bool anonymous_due(std::int64_t last, std::int64_t now, bool forced, const rate_budget& b)
{
    if (!budget_allows(b, 2, 2, now)) return false;
    return last == 0 || now - last >= (forced ? 60 : 600);
}

int poll_seconds(bool failed, bool running, bool only_anonymous_github, const rate_budget& b,
                 std::int64_t now)
{
    int seconds = failed ? 300 : running ? 20 : 120;
    if (!only_anonymous_github) return seconds;
    seconds = std::max(seconds, 600);
    if (!budget_allows(b, 2, 2, now) && b.reset_at > now)
        seconds = int(std::max<std::int64_t>(seconds, b.reset_at - now + 5));
    return seconds;
}

std::string hint_text(const forge_remote& r)
{
    return rap::i18n::format(_("{0}: sign in to see its pull requests"),
                             r.repo.host + "/" + r.repo.path());
}

// What the worker keeps between asks: the tokens, so gh isn't run each time;
// what each account may do with each repository, asked once; and when
// anonymous GitHub was last asked, to spare its allowance.
struct pr_watch::shared
{
    std::mutex mutex;
    std::map<std::string, std::string> tokens;  // "host login" -> token
    std::map<std::string, repo_access> access;  // by access_key
    std::map<std::string, std::int64_t> anonymous_asked;
    bool alive = true;
};

pr_watch::pr_watch(rap::ui& ui, std::string workdir, std::function<void()> changed,
                   bool branch_tips)
    : ui_(ui)
    , workdir_(std::move(workdir))
    , changed_(std::move(changed))
    , want_tips_(branch_tips)
    , shared_(std::make_shared<shared>())
{
    watches().push_back(this);
    refresh();
}

pr_watch::~pr_watch()
{
    std::erase(watches(), this);
    std::lock_guard lock(shared_->mutex);
    shared_->alive = false;
}

repo_access pr_watch::access(const forge_remote& r) const
{
    auto it = access_.find(access_key(r));
    return it == access_.end() ? repo_access::unknown : it->second;
}

std::vector<forge_remote> pr_watch::all_hints()
{
    std::vector<forge_remote> out;
    for (const pr_watch* w : watches())
        for (const forge_remote& r : w->hints_)
            if (std::ranges::none_of(out, [&](const forge_remote& o) { return o.repo == r.repo; }))
                out.push_back(r);
    return out;
}

void pr_watch::refresh()
{
    ask(true);
}

void pr_watch::ask(bool forced)
{
    if (asking_)
    {
        again_ = true;
        again_forced_ = again_forced_ || forced;
        return;
    }
    timer_.reset();
    remotes_ = forge_remotes(workdir_);
    if (remotes_.empty())
    {
        hints_.clear();
        tips_.clear();
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
        [this, sh = shared_, remotes = remotes_, forced, &ui = ui_, want_tips = want_tips_]
        {
            std::vector<open_pr> found;
            std::map<std::string, remote_tips, std::less<>> tips;
            std::string error;
            std::vector<forge_repo> asked, kept;  // kept: not asked now, as last heard
            for (const forge_remote& r : remotes)
            {
                if (std::ranges::find(asked, r.repo) != asked.end()) continue;
                asked.push_back(r.repo);
                const std::string key = r.account.host + ' ' + r.account.login,
                                  seen = access_key(r);
                std::string token;
                bool ask_access = false;
                std::int64_t last = 0;
                {
                    std::lock_guard lock(sh->mutex);
                    if (!sh->alive) return;
                    if (r.signed_in()) token = sh->tokens[key];
                    ask_access = !sh->access.contains(seen);
                    last = sh->anonymous_asked[seen];
                }
                if (anonymous_github(r) &&
                    !anonymous_due(last, now_seconds(), forced, anonymous_budget()))
                {
                    kept.push_back(r.repo);
                    continue;
                }
                if (r.signed_in() && token.empty())
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
                auto reply = open_pull_requests(r.account.kind, r.repo, token, ask_access,
                                                want_tips && !anonymous_github(r));
                std::lock_guard lock(sh->mutex);
                if (!reply)
                {
                    sh->tokens.erase(key);  // it may be what's wrong: asked for again
                    error = reply.error();
                    continue;
                }
                if (r.signed_in()) sh->tokens[key] = token;
                if (reply->rate_limited)
                {
                    kept.push_back(r.repo);
                    continue;
                }
                if (anonymous_github(r)) sh->anonymous_asked[seen] = now_seconds();
                if (reply->access != repo_access::unknown) sh->access[seen] = reply->access;
                // Every remote that is this repository has its branches.
                if (reply->tips)
                    for (const forge_remote& same : remotes)
                        if (same.repo == r.repo) tips[same.name] = *reply->tips;
                for (pull_request& pr : reply->prs) found.push_back({std::move(pr), r});
            }
            std::map<std::string, repo_access> access;
            {
                std::lock_guard lock(sh->mutex);
                access = sh->access;
            }
            ui.post(
                [this, sh, found = std::move(found), error = std::move(error),
                 kept = std::move(kept), access = std::move(access),
                 tips = std::move(tips)]() mutable
                {
                    {
                        std::lock_guard lock(sh->mutex);
                        if (!sh->alive) return;
                    }
                    asking_ = false;
                    error_ = std::move(error);
                    for (open_pr& p : prs_)
                        if (std::ranges::find(kept, p.into.repo) != kept.end())
                            found.push_back(std::move(p));
                    prs_ = std::move(found);
                    tips_ = std::move(tips);
                    access_ = std::move(access);
                    hints_.clear();
                    for (const forge_remote& r : remotes_)
                        if (!r.signed_in() && this->access(r) == repo_access::hidden &&
                            std::ranges::none_of(hints_, [&](const forge_remote& h)
                                                 { return h.repo == r.repo; }))
                            hints_.push_back(r);
                    if (changed_) changed_();
                    if (std::exchange(again_, false))
                        ask(std::exchange(again_forced_, false));
                    else
                        schedule();
                });
        })
        .detach();
}

void pr_watch::nudge()
{
    if (std::chrono::steady_clock::now() - asked_ > std::chrono::seconds(15))
        ask(false);
    else if (asking_)
        again_ = true;
}

// Sooner while checks run, as they'll change; rarely when it failed, or
// when there's only GitHub's anonymous allowance to spend.
void pr_watch::schedule()
{
    const bool running = std::ranges::any_of(prs_, [](const open_pr& p)
                                             { return p.pr.checks == check_state::pending; });
    const bool only_anonymous = std::ranges::all_of(remotes_, anonymous_github);
    const int seconds =
        poll_seconds(!error_.empty(), running, only_anonymous, anonymous_budget(), now_seconds());
    timer_ = ui_.timer(seconds * 1000, false, [this] { ask(false); });
}

}  // namespace tortilla
