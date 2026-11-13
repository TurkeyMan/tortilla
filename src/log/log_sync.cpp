// The log's sync: its Fetch button, whose icon says a sync would bring
// something (or that a remote can't be reached, or there's no network), the
// download ahead of a click, and the hint when the current branch is behind
// its upstream. Nothing here asks the user anything: git runs with
// git_prompts::never, and a remote that wants a password fails, shown on
// the button.
#include "log_window.h"
#include "ref_labels.h"

#include "git/remotes.h"
#include "i18n.h"

#include <algorithm>
#include <numeric>
#include <thread>

namespace tortilla
{
namespace
{

using rap::color;
using rap::paint;

// The desktop's error colour, as a failed check's mark (ref_labels.cpp).
const paint k_failure_red{rap::theme_color::negative};
constexpr color k_white{255, 255, 255};

// A circle `d` across at `c` with `text` in it, in bold, sized to fit.
void paint_disc(rap::painter& p, rap::point_f c, float d, const paint& fill,
                const std::string& text, const paint& fg = paint{k_white})
{
    p.circle(c, d / 2, fill, std::nullopt);
    if (text.empty()) return;
    // Points from the disc's pixels: three quarters of a pixel a point.
    rap::text_style ts{.fg = fg, .bold = true, .size = std::max(5.0f, d * 0.7f * 0.75f)};
    const rap::size_f t = p.measure(text, ts);
    p.text({c.x - t.w / 2, c.y - t.h / 2}, text, ts);
}

// How often remotes with no forge to ask are asked with ls-remote.
constexpr int k_poll_ms = 10 * 60 * 1000;

}  // namespace

rap::icon log_window::fetch_icon(fetch_badge badge, size_t count)
{
    // A down arrow into a tray, in the theme's text colour (grey offline),
    // and the badge top right.
    rap::icon icon = rap::icon::drawn(
        [badge, count](rap::painter& p, rap::size_f s)
        {
            const float w = s.w, h = s.h, cx = w / 2;
            const float line = std::max(1.5f, w / 11);
            const paint ink{badge == fetch_badge::offline ? rap::theme_color::placeholder_text
                                                          : rap::theme_color::window_text};
            p.line({cx, h * 0.1f}, {cx, h * 0.6f}, ink, line, rap::line_cap::round);
            p.line({cx - w * 0.22f, h * 0.4f}, {cx, h * 0.62f}, ink, line, rap::line_cap::round);
            p.line({cx + w * 0.22f, h * 0.4f}, {cx, h * 0.62f}, ink, line, rap::line_cap::round);
            p.line({w * 0.12f, h * 0.62f}, {w * 0.12f, h * 0.88f}, ink, line, rap::line_cap::round);
            p.line({w * 0.12f, h * 0.88f}, {w * 0.88f, h * 0.88f}, ink, line, rap::line_cap::round);
            p.line({w * 0.88f, h * 0.88f}, {w * 0.88f, h * 0.62f}, ink, line, rap::line_cap::round);
            const float d = w * 0.58f;
            const rap::point_f at{w - d / 2, d / 2};
            switch (badge)
            {
            case fetch_badge::none: break;
            case fetch_badge::changes:
                paint_disc(p, at, d, paint{rap::theme_color::accent},
                           count > 9 ? "9+" : std::to_string(count),
                           paint{rap::theme_color::highlight_text});
                break;
            case fetch_badge::failure:
                paint_disc(p, at, d, k_failure_red, "!", paint{ink_on(p.resolve(k_failure_red))});
                break;
            case fetch_badge::offline:
                // A grey disc, struck through.
                p.circle(at, d / 2, ink, std::nullopt);
                p.line({at.x - d * 0.25f, at.y + d * 0.25f}, {at.x + d * 0.25f, at.y - d * 0.25f},
                       paint{k_white}, line);
                break;
            }
        });
    // Unbadged, the desktop's own where it has named icons; a badge needs
    // the drawing (rapport draws no overlay on a button's named icon).
    if (badge == fetch_badge::none) icon.name = "folder-download-symbolic";
    return icon;
}

void log_window::paint_failure_mark(rap::painter& p, rap::size_f s)
{
    // (A canvas has no background of its own on every toolkit.)
    p.rect({0, 0, s.w, s.h}, 0, paint{rap::theme_color::window}, std::nullopt);
    const float d = std::min({s.w, s.h, 16.0f});
    paint_disc(p, {s.w / 2, s.h / 2}, d, k_failure_red, "!",
               paint{ink_on(p.resolve(k_failure_red))});
}

// What git said, the failures' first: the status bar's "!" clicked.
void log_window::show_sync_output()
{
    std::string text, detail;
    for (const auto& [remote, failure] : failures_)
    {
        text += (text.empty() ? "" : "\n") + rap::i18n::format(_("{0}: {1}"), remote, failure.why);
        detail += failure.output + "\n";
    }
    for (const remote_changes& c : changes_) text += (text.empty() ? "" : "\n") + changes_text(c);
    if (failures_.empty()) detail = last_output_;
    if (text.empty()) text = _("Nothing to bring from the remotes.");
    ui_.message(window_.get(), {.icon = failures_.empty() ? rap::message_spec::icon::info
                                                          : rap::message_spec::icon::warning,
                                .title = _("Fetch"),
                                .text = text,
                                .detail = detail});
}

void log_window::update_sync_badge()
{
    if (!page_) return;
    // No network at all is the system's to say: Offline, not the remotes'
    // failures. Connected, any remote's failure is red.
    const size_t count = std::accumulate(
        changes_.begin(), changes_.end(), size_t(0), [](size_t a, const remote_changes& c)
        { return a + c.added.size() + c.moved.size() + c.deleted.size(); });
    const fetch_badge badge = !network_online()    ? fetch_badge::offline
                              : !failures_.empty() ? fetch_badge::failure
                              : count              ? fetch_badge::changes
                                                   : fetch_badge::none;
    if (badge != page_->badge_shown ||
        (badge == fetch_badge::changes && count != page_->badge_count))
    {
        page_->fetch->set_icon(fetch_icon(badge, count));
        page_->badge_shown = badge;
        page_->badge_count = count;
    }

    std::string tip = _("Fetch from every remote, and fast-forward the branches behind them");
    if (badge == fetch_badge::offline) tip += "\n" + std::string(_("Offline"));
    for (const auto& [remote, failure] : failures_)
        tip += "\n" + rap::i18n::format(_("{0}: {1}"), remote, failure.why);
    for (const remote_changes& c : changes_) tip += "\n" + changes_text(c);
    // Repositories the forge hid from no one: signing in may show more.
    if (pr_watch_)
        for (const forge_remote& h : pr_watch_->hints()) tip += "\n" + hint_text(h);
    page_->fetch->set_tooltip(tip);
}

void log_window::set_sync_notice(std::string text, bool failure)
{
    notice_ = std::move(text);
    notice_failure_ = failure;
    show_notice();
    update_status();
    notice_timer_ = ui_.timer(10000, false,
                              [this]
                              {
                                  notice_.clear();
                                  show_notice();
                                  update_status();
                              });
}

void log_window::show_notice()
{
    if (!page_) return;
    page_->status_mark->set_visible(notice_failure_ && !notice_.empty());
    page_->status_mark->invalidate();
}

void log_window::show_sync_state()
{
    if (!page_) return;
    page_->fetch->set_enabled(!syncing_);
}

// Every remote fetched side by side, a git each, the leading one (the hot
// repository) started first: the log reloads when it's done, and again when
// the rest are. Branches behind a remote's upstreams move as soon as that
// remote is fetched.
void log_window::sync(const std::string& only)
{
    if (syncing_) return;
    syncing_ = true;
    show_sync_state();
    if (page_)
        page_->status->set_text(only.empty() ? std::string(_("Fetching from the remotes…"))
                                             : rap::i18n::format(_("Fetching {0}..."), only));
    // The forge's view too, at once (a forge action asks it itself).
    if (only.empty() && pr_watch_) pr_watch_->refresh();
    auto run = sync_run_ = std::make_shared<sync_state>();
    run->only = only;
    std::thread(
        [this, alive = std::weak_ptr<bool>(alive_), dir = repo_location(), only, run,
         others = fast_forward_others_setting(), &ui = ui_]
        {
            std::vector<std::string> remotes =
                only.empty() ? fetch_all_remotes(dir) : std::vector<std::string>{only};
            // The leading remote: the forge's newest, else the current
            // branch's upstream's, else the first.
            std::string leading;
            for (const forge_remote& r : leading_remotes(dir, forge_remotes(dir)))
                if (std::ranges::find(remotes, r.name) != remotes.end())
                {
                    leading = r.name;
                    break;
                }
            if (leading.empty())
                if (const std::string b = current_branch(dir); !b.empty())
                    if (const std::string up = upstream_of(dir, b).remote;
                        std::ranges::find(remotes, up) != remotes.end())
                        leading = up;
            if (leading.empty() && !remotes.empty()) leading = remotes.front();
            std::ranges::stable_partition(remotes,
                                          [&](const std::string& r) { return r == leading; });
            ui.post(
                [this, alive, run, remotes, leading]
                {
                    if (!alive.lock()) return;
                    run->pending = remotes.size();
                    run->leading = leading;
                    if (remotes.empty()) sync_finished(run);
                });
            for (const std::string& r : remotes)
                std::thread(
                    [this, alive, dir, r, run, others, &ui]
                    {
                        remote_result result = fetch_remote(dir, r);
                        std::vector<std::string> moved;
                        if (others && result.failure == fetch_failure::none)
                            for (const fast_forward& f : fast_forward_targets(dir, r))
                                if (run_git(dir, fast_forward_commands({f}).front(),
                                            git_output::stdout_only)
                                        .ok())
                                    moved.push_back(f.branch);
                        ui.post(
                            [this, alive, r, run, result = std::move(result),
                             moved = std::move(moved)]() mutable
                            {
                                if (alive.lock())
                                    remote_synced(run, r, std::move(result), std::move(moved));
                            });
                    })
                    .detach();
        })
        .detach();
}

void log_window::remote_synced(const std::shared_ptr<sync_state>& run, const std::string& remote,
                               remote_result result, std::vector<std::string> moved)
{
    for (std::string& m : moved) run->moved.push_back(std::move(m));
    if (result.failure == fetch_failure::none)
        failures_.erase(remote);
    else
        failures_[remote] = result;
    // What was fetched is here now; the rest waits for the next look.
    if (result.failure == fetch_failure::none)
        std::erase_if(changes_, [&](const remote_changes& c) { return c.remote == remote; });
    run->results[remote] = std::move(result);
    update_sync_badge();
    const bool last = --run->pending == 0;
    // The hot one at once; the rest together (two reloads at most).
    if (remote == run->leading || (last && run->results.size() > 1))
    {
        after_sync_ = true;
        reload();
    }
    if (last) sync_finished(run);
}

void log_window::sync_finished(const std::shared_ptr<sync_state>& run)
{
    if (run == sync_run_) sync_run_.reset();
    syncing_ = false;
    show_sync_state();
    last_output_.clear();
    std::vector<std::string> failed;
    fetch_failure kind = fetch_failure::none;
    for (const auto& [remote, result] : run->results)
    {
        last_output_ += result.output;
        if (result.failure == fetch_failure::none) continue;
        kind = failed.empty() || kind == result.failure ? result.failure : fetch_failure::other;
        failed.push_back(remote);
    }
    // A failure says so for a while, in red; else what moved.
    if (!failed.empty())
    {
        std::string names;
        for (const std::string& f : failed) names += (names.empty() ? "" : ", ") + f;
        std::string text;
        if (!network_online())
            text = _("Offline");
        else if (kind == fetch_failure::unreachable)
            text = rap::i18n::format(
                N_("remote {0} unreachable", "remotes {0} unreachable", failed.size()), names);
        else if (kind == fetch_failure::credentials)
            text = rap::i18n::format(
                N_("remote {0} needs a password", "remotes {0} need a password", failed.size()),
                names);
        else
            text = rap::i18n::format(N_("fetching from remote {0} failed",
                                        "fetching from remotes {0} failed", failed.size()),
                                     names);
        set_sync_notice(text, network_online());
    }
    else if (!run->moved.empty())
    {
        std::string names;
        for (const std::string& m : run->moved) names += (names.empty() ? "" : ", ") + m;
        set_sync_notice(
            rap::i18n::format(N_("Fast-forwarded {0}", "Fast-forwarded {0}", run->moved.size()),
                              names),
            false);
    }
}

// -- before the click ---------------------------------------------------------

void log_window::start_sync_watch()
{
    update_sync_badge();
    if (pr_watch_) pr_watch_->want_branch_tips(allowed_now(check_setting()));
    // A first look a little after opening, once the log has loaded.
    poll_timer_ = ui_.timer(5000, false, [this] { poll_remotes(); });
}

// Remotes the forge didn't answer for: ls-remote, rarely.
void log_window::poll_remotes()
{
    poll_timer_ = ui_.timer(k_poll_ms, false, [this] { poll_remotes(); });
    const bool allowed = allowed_now(check_setting());
    if (pr_watch_) pr_watch_->want_branch_tips(allowed);
    if (!allowed || polling_) return;
    polling_ = true;
    std::vector<std::string> answered;
    if (pr_watch_)
        for (const auto& [remote, tips] : pr_watch_->branch_tips()) answered.push_back(remote);
    std::thread(
        [this, alive = std::weak_ptr<bool>(alive_), dir = repo_location(), answered, &ui = ui_]
        {
            const std::vector<std::string> remotes = fetch_all_remotes(dir);
            std::map<std::string, std::pair<remote_tips, remote_result>, std::less<>> heard;
            for (const std::string& r : remotes)
            {
                if (std::ranges::find(answered, r) != answered.end()) continue;
                const git_result listed =
                    run_git(dir, {"ls-remote", "--heads", r}, git_output::all, git_prompts::never);
                heard[r] = listed.ok() ? std::pair{parse_ls_remote(listed.output), remote_result{}}
                                       : std::pair{remote_tips{}, classify_failure(listed.output)};
            }
            ui.post(
                [this, alive, heard = std::move(heard)]() mutable
                {
                    if (!alive.lock()) return;
                    polling_ = false;
                    for (auto& [remote, answer] : heard)
                    {
                        if (answer.second.failure == fetch_failure::none)
                            known_tips_[remote] = std::move(answer.first);
                        contact_result(remote, std::move(answer.second));
                    }
                    recheck_changes();
                });
        })
        .detach();
}

void log_window::tips_heard(std::map<std::string, remote_tips, std::less<>> tips)
{
    update_sync_badge();  // the watch's hints may have changed
    if (tips.empty() || !allowed_now(check_setting())) return;
    for (auto& [remote, t] : tips) known_tips_[remote] = std::move(t);
    recheck_changes();
}

// The remotes' branches as last heard against the repository's copies of
// them, on a worker; then the ones that differ downloaded ahead of a click.
void log_window::recheck_changes()
{
    if (known_tips_.empty()) return;
    if (rechecking_)
    {
        recheck_again_ = true;
        return;
    }
    rechecking_ = true;
    std::thread(
        [this, alive = std::weak_ptr<bool>(alive_), dir = repo_location(), tips = known_tips_,
         &ui = ui_]
        {
            std::vector<std::string> names;
            const std::vector<std::string> remotes = remote_names(dir);
            for (const auto& [remote, t] : tips)
                if (std::ranges::find(remotes, remote) != remotes.end()) names.push_back(remote);
            const auto local = local_tips(dir, names);
            const auto prefetched = local_tips(dir, names, "refs/prefetch/");
            std::vector<remote_changes> changes;
            std::vector<std::string> to_prefetch;
            for (const std::string& r : names)
            {
                const remote_tips& theirs = tips.find(r)->second;
                remote_changes c = compare_tips(r, theirs, local.find(r)->second);
                if (c.empty()) continue;
                if (!compare_tips(r, theirs, prefetched.find(r)->second).empty())
                    to_prefetch.push_back(r);
                changes.push_back(std::move(c));
            }
            ui.post(
                [this, alive, changes = std::move(changes), to_prefetch]() mutable
                {
                    if (!alive.lock()) return;
                    rechecking_ = false;
                    changes_ = std::move(changes);
                    update_sync_badge();
                    if (std::exchange(recheck_again_, false)) return recheck_changes();
                    if (allowed_now(prefetch_setting()))
                        for (const std::string& r : to_prefetch) prefetch(r);
                });
        })
        .detach();
}

// Only a speed-up: refs/remotes/ stays as it is, and the click still fetches.
void log_window::prefetch(const std::string& remote)
{
    if (syncing_ || std::ranges::find(prefetching_, remote) != prefetching_.end()) return;
    prefetching_.push_back(remote);
    std::thread(
        [this, alive = std::weak_ptr<bool>(alive_), dir = repo_location(), remote, &ui = ui_]
        {
            const git_result fetched =
                run_git(dir, prefetch_args(remote), git_output::all, git_prompts::never);
            remote_result result =
                fetched.ok() ? remote_result{} : classify_failure(fetched.output);
            result.output = command_line(prefetch_args(remote)) + "\n" + fetched.output;
            ui.post(
                [this, alive, remote, result = std::move(result)]() mutable
                {
                    if (!alive.lock()) return;
                    std::erase(prefetching_, remote);
                    contact_result(remote, std::move(result));
                });
        })
        .detach();
}

void log_window::contact_result(const std::string& remote, remote_result result)
{
    if (result.failure == fetch_failure::none)
        failures_.erase(remote);
    else
        failures_[remote] = std::move(result);
    update_sync_badge();
}

// -- the current branch -------------------------------------------------------

void log_window::update_branch_hint()
{
    if (repo_.workdir().empty()) return;  // a bare repository checks nothing out
    const bool after_sync = std::exchange(after_sync_, false);
    std::thread(
        [this, alive = std::weak_ptr<bool>(alive_), dir = repo_.workdir(), after_sync, &ui = ui_]
        {
            current_state state = current_against_upstream(dir);
            ui.post(
                [this, alive, state = std::move(state), after_sync]
                {
                    if (!alive.lock()) return;
                    current_ = state;
                    // Set to: moved forward, when nothing stands in the way.
                    if (after_sync && fast_forward_current_setting() && state.behind > 0 &&
                        state.ahead == 0 && state.clean)
                        return fast_forward_current();
                    show_branch_hint(state);
                });
        })
        .detach();
}

void log_window::show_branch_hint(const current_state& s)
{
    if (!page_) return;
    const std::string key = s.branch + ' ' + s.upstream + ' ' + std::to_string(s.behind);
    if (s.behind <= 0 || s.upstream.empty() || key == hint_hidden_ || picked_)
    {
        page_->hint_bar->set_visible(false);
        return;
    }
    const size_t n = size_t(s.behind);
    if (s.ahead == 0)
        page_->hint_text->set_text(rap::i18n::format(
            N_("{0} is {1} commit behind {2}", "{0} is {1} commits behind {2}", n), s.branch, n,
            s.upstream));
    else
        page_->hint_text->set_text(rap::i18n::format(
            N_("{0} is {1} commit behind {2}, with {3} of its own: they have diverged",
               "{0} is {1} commits behind {2}, with {3} of its own: they have diverged", n),
            s.branch, n, s.upstream, s.ahead));
    page_->hint_ff->set_visible(s.ahead == 0);
    page_->hint_pull->set_visible(s.ahead != 0);
    page_->hint_bar->set_visible(true);
}

// git refuses if uncommitted changes are in the way, and says why.
void log_window::fast_forward_current()
{
    if (page_) page_->hint_bar->set_visible(false);
    run_quietly(_("Fast-forward"), {{"merge", "--ff-only", "@{u}"}},
                [this](bool, const std::string&) { reload(); });
}

}  // namespace tortilla
