#include "pr_watch.h"

#include "git/command.h"
#include "git/remotes.h"
#include "git/repo.h"
#include "i18n.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <ctime>
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
    // Each candidate's commit time, by its full name; <remote>/HEAD as the
    // branch it names. In process (the log asks as it opens); git, in one
    // run, when libgit2 can't open the repository.
    std::map<std::string, std::int64_t, std::less<>> tips;
    if (auto local = repo::open(workdir))
    {
        for (const forge_remote& r : remotes)
            for (std::string_view b : defaults)
            {
                const std::string name = "refs/remotes/" + r.name + std::string(b);
                if (const auto id = local->resolve(name))
                    if (const auto c = local->lookup(*id)) tips.emplace(name, c->commit_time);
            }
    }
    else
    {
        std::vector<std::string> args{"for-each-ref", "--format=%(refname) %(committerdate:unix)"};
        for (const forge_remote& r : remotes)
            for (std::string_view b : defaults)
                args.push_back("refs/remotes/" + r.name + std::string(b));
        const git_result listed = run_git(workdir, args, git_output::stdout_only);
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
    // The quick asks' (one worker at a time uses them): replies by URL with
    // their ETags, and each workflow's last time.
    etag_cache etags;
    std::map<std::int64_t, std::int64_t> durations;
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
                    update_fast_timer();
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
    // With the quick asks watching running checks, the list needn't hurry.
    const bool only_anonymous = std::ranges::all_of(remotes_, anonymous_github);
    const int seconds = poll_seconds(!error_.empty(), running && fast_.empty(), only_anonymous,
                                     anonymous_budget(), now_seconds());
    timer_ = ui_.timer(seconds * 1000, false, [this] { ask(false); });
}

void pr_watch::watch(std::optional<std::string> sha, std::optional<forge_remote> where,
                     bool pending)
{
    watched_ = std::move(sha);
    watched_where_ = std::move(where);
    watched_pending_ = watched_ && watched_where_ && pending;
    update_fast_timer();
}

namespace
{

// The earliest start of the runs still going: when the checks began, for
// how far through they are.
std::int64_t running_since(const std::vector<check_run>& runs)
{
    std::int64_t since = 0;
    for (const check_run& r : runs)
        if (r.state == check_state::pending && r.started_at && (!since || r.started_at < since))
            since = r.started_at;
    return since;
}

}  // namespace

void pr_watch::update_fast_timer()
{
    // GitHub's only: GitLab's and Gitea's checks wait for the list's ask.
    const bool any = (watched_pending_ && watched_where_->account.kind == forge_kind::github) ||
                     std::ranges::any_of(prs_,
                                         [](const open_pr& p)
                                         {
                                             return p.pr.checks == check_state::pending &&
                                                    p.into.account.kind == forge_kind::github;
                                         });
    if (!any)
    {
        fast_timer_.reset();
        fast_.clear();
        return;
    }
    if (!fast_timer_) fast_timer_ = ui_.timer(1000, true, [this] { fast_tick(); });
}

void pr_watch::fast_tick()
{
    const auto now = std::chrono::steady_clock::now();
    const std::int64_t wall = std::int64_t(std::time(nullptr));
    // Who's due: the plan's, in its order.
    std::vector<pending_head> pending;
    std::map<std::string, forge_remote> where;
    // Signed in, every pending PR; as no one, GitHub's allowance is short.
    for (const open_pr& p : prs_)
        if (p.pr.checks == check_state::pending && p.into.account.kind == forge_kind::github &&
            p.into.signed_in() && !where.contains(p.pr.head_sha))
        {
            pending.push_back({.sha = p.pr.head_sha,
                               .started_at = running_since(p.pr.runs),
                               .expected = fast_[p.pr.head_sha].expected});
            where.emplace(p.pr.head_sha, p.into);
        }
    std::optional<pending_head> watched;
    if (watched_pending_ && watched_where_->account.kind == forge_kind::github)
    {
        std::int64_t since = 0;
        for (const open_pr& p : prs_)
            if (p.pr.head_sha == *watched_) since = running_since(p.pr.runs);
        watched = pending_head{
            .sha = *watched_, .started_at = since, .expected = fast_[*watched_].expected};
        where.insert_or_assign(*watched_, *watched_where_);
    }
    // The watched one as no one only while the allowance lasts.
    const bool anonymous = watched && !watched_where_->signed_in();
    auto plan = plan_fast_polls(pending, anonymous ? std::nullopt : watched, wall, true, 0);
    if (anonymous)
    {
        const int left = budget_allows(anonymous_budget(), 1, 31, wall) ? 31 : 0;
        auto first = plan_fast_polls({}, watched, wall, false, left);
        plan.insert(plan.begin(), first.begin(), first.end());
    }
    std::erase_if(fast_,
                  [&](const auto& f)
                  {
                      return std::ranges::none_of(plan, [&](const fast_poll& p)
                                                  { return p.sha == f.first; });
                  });
    if (plan.empty())
    {
        update_fast_timer();
        return;
    }
    if (fast_busy_ || now < backoff_until_) return;

    struct ask
    {
        std::string sha;
        forge_remote where;
        bool expected_known;
    };
    std::vector<ask> due;
    for (const fast_poll& p : plan)
    {
        fast_state& f = fast_[p.sha];
        // Just seen running: its first quick ask after one interval.
        if (f.due == std::chrono::steady_clock::time_point{}) f.due = now + p.interval;
        if (f.due > now) continue;
        f.due = now + p.interval;
        due.push_back({p.sha, where.at(p.sha), f.expected_known});
    }
    if (due.empty()) return;
    fast_busy_ = true;
    std::thread(
        [this, sh = shared_, due = std::move(due), &ui = ui_]
        {
            struct answer
            {
                std::string sha;
                std::optional<commit_runs> runs;
                std::optional<std::int64_t> expected;
            };
            std::vector<answer> answers;
            std::chrono::seconds wait{0};
            for (const ask& a : due)
            {
                std::string token;
                {
                    std::lock_guard lock(sh->mutex);
                    if (!sh->alive) return;
                    token = sh->tokens[a.where.account.host + ' ' + a.where.account.login];
                }
                // Signed in, the list's ask fetches the token first.
                if (token.empty() && a.where.signed_in()) continue;
                answer got{.sha = a.sha};
                // The worker's alone with these: one at a time. As no one,
                // the workflows' times aren't worth the allowance.
                if (!a.expected_known && a.where.signed_in())
                    got.expected = expected_duration(a.where.repo, a.sha, token, sh->durations);
                if (auto r = poll_commit_runs(a.where.repo, a.sha, token, sh->etags))
                {
                    wait = std::max(wait, r->retry_after);
                    if (!r->retry_after.count()) got.runs = std::move(*r);
                }
                answers.push_back(std::move(got));
                if (wait.count()) break;  // asked to slow down: the rest later
            }
            ui.post(
                [this, sh, answers = std::move(answers), wait]
                {
                    {
                        std::lock_guard lock(sh->mutex);
                        if (!sh->alive) return;
                    }
                    fast_busy_ = false;
                    if (wait.count()) backoff_until_ = std::chrono::steady_clock::now() + wait;
                    bool changed = false;
                    for (const answer& a : answers)
                    {
                        if (a.expected)
                        {
                            fast_[a.sha].expected = *a.expected;
                            fast_[a.sha].expected_known = true;
                        }
                        if (!a.runs || !a.runs->changed) continue;
                        // The label's mark and tooltip at once; a finished
                        // head drops out of the plan.
                        for (open_pr& p : prs_)
                            if (p.pr.head_sha == a.sha && !a.runs->runs.empty())
                            {
                                p.pr.runs = a.runs->runs;
                                p.pr.checks = a.runs->state;
                                changed = true;
                            }
                        if (watched_ == a.sha)
                        {
                            if (a.runs->state != check_state::pending) watched_pending_ = false;
                            if (commit_checks_) commit_checks_(a.sha, *a.runs);
                        }
                    }
                    if (changed && changed_) changed_();
                    update_fast_timer();
                });
        })
        .detach();
}

}  // namespace tortilla
