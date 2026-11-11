#include "forge_panel.h"

#include "forge/pr_details.h"
#include "i18n.h"
#include "log/ref_labels.h"
#include "ui/menu_text.h"

#include <rapport/sys.h>

#include <algorithm>
#include <ctime>
#include <mutex>
#include <thread>

namespace tortilla
{
namespace
{

// The author's picture by the PR's title, and the marks by their lines, in
// logical px.
constexpr int k_picture_size = 24;
constexpr int k_mark_size = 16;
// How often the shown build is asked again while its checks run.
constexpr auto k_running_poll = std::chrono::seconds(20);

std::int64_t now_seconds()
{
    return std::int64_t(std::time(nullptr));
}

std::string count(std::int64_t n)
{
    return std::to_string(n);
}

// "850 KB", "12.4 MB".
std::string size_text(std::int64_t bytes)
{
    if (bytes <= 0) return {};
    if (bytes < 1024 * 1024)
        return rap::i18n::format(_("{0} KB"), count(std::max<std::int64_t>(1, bytes / 1024)));
    const std::int64_t tenths = bytes * 10 / (1024 * 1024);
    return rap::i18n::format(_("{0} MB"), count(tenths / 10) + "." + count(tenths % 10));
}

// How long a job took, or has been running.
std::string job_duration(const ci_job& j, std::int64_t now)
{
    if (j.state == check_state::pending)
        return j.started_at && now > j.started_at ? check_duration(now - j.started_at, true)
                                                  : std::string();
    if (j.started_at && j.completed_at > j.started_at)
        return check_duration(j.completed_at - j.started_at);
    return {};
}

struct tally
{
    std::int64_t passed = 0, failed = 0, running = 0, first_start = 0, last_end = 0;
    std::int64_t counted() const { return passed + failed + running; }
    check_state state() const
    {
        return failed    ? check_state::failure
               : running ? check_state::pending
               : passed  ? check_state::success
                         : check_state::none;
    }
    void add(const ci_job& j)
    {
        switch (j.state)
        {
        case check_state::success: ++passed; break;
        case check_state::failure: ++failed; break;
        case check_state::pending: ++running; break;
        case check_state::none: return;
        }
        if (j.started_at && (!first_start || j.started_at < first_start))
            first_start = j.started_at;
        last_end = std::max(last_end, j.completed_at);
    }
};

// "5 passed, 1 failed, 2 running": a group's counts.
std::string tally_text(const tally& t)
{
    std::vector<std::string> parts;
    if (t.passed) parts.push_back(rap::i18n::format(_("{0} passed"), count(t.passed)));
    if (t.failed) parts.push_back(rap::i18n::format(_("{0} failed"), count(t.failed)));
    if (t.running) parts.push_back(rap::i18n::format(_("{0} running"), count(t.running)));
    std::string out;
    for (const std::string& p : parts) out += (out.empty() ? "" : ", ") + p;
    return out;
}

// The commit's checks in a line: "23 checks passed in 6 min", "2 of 23
// checks failed", "5 of 23 checks running".
std::string build_text(const tally& t)
{
    const std::int64_t n = t.counted();
    if (n == 0) return _("No checks ran");
    std::string out;
    if (t.failed)
        out = rap::i18n::format(N_("{0} of {1} check failed", "{0} of {1} checks failed", n),
                                count(t.failed), count(n));
    else if (t.running)
        out = rap::i18n::format(N_("{0} of {1} check running", "{0} of {1} checks running", n),
                                count(t.running), count(n));
    else if (t.first_start && t.last_end > t.first_start)
        return rap::i18n::format(N_("{0} check passed in {1}", "{0} checks passed in {1}", n),
                                 count(n), check_duration(t.last_end - t.first_start));
    else
        return rap::i18n::format(N_("{0} check passed", "{0} checks passed", n), count(n));
    if (t.failed && t.running) out += rap::i18n::format(_(", {0} still running"), count(t.running));
    return out;
}

// A description's start: HTML comments (a template's hints) dropped, and
// blank lines run together.
std::string clean_body(std::string_view body)
{
    std::string text;
    for (size_t at = 0; at < body.size();)
    {
        const size_t open = body.find("<!--", at);
        if (open == std::string_view::npos)
        {
            text.append(body.substr(at));
            break;
        }
        text.append(body.substr(at, open - at));
        const size_t close = body.find("-->", open + 4);
        at = close == std::string_view::npos ? body.size() : close + 3;
    }
    std::string out;
    bool blank = true;
    for (std::string_view rest = text; !rest.empty();)
    {
        const size_t end = rest.find('\n');
        std::string_view line = rest.substr(0, end);
        rest.remove_prefix(end == std::string_view::npos ? rest.size() : end + 1);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
        if (line.empty())
        {
            if (!blank) out += '\n';
            blank = true;
            continue;
        }
        out.append(line).append("\n");
        blank = false;
    }
    while (!out.empty() && out.back() == '\n') out.pop_back();
    return out;
}

std::string names(const std::vector<std::string>& who)
{
    std::string out;
    for (const std::string& w : who) out += (out.empty() ? "" : ", ") + w;
    return out;
}

void paint_mark(rap::painter& p, rap::size_f size, check_state s, mergeable m = mergeable::yes)
{
    if (s == check_state::none) return;
    const float d = std::min(size.w, size.h) - 2;
    paint_check_mark(p, {size.w / 2, size.h / 2}, d, s, m);
}

}  // namespace

// -- the lists' models ---------------------------------------------------------

// The build's matrix: each workflow a line with its counts, a matrix's jobs
// under a line of their own ("build: 5 passed, 1 failed"), then the jobs.
class forge_panel::matrix_model : public rap::list_model
{
public:
    struct row
    {
        enum kind
        {
            workflow,
            group,
            job,
        } kind = job;
        check_state state = check_state::none;
        std::string text, duration;
        size_t w = 0, j = 0;  // the workflow and job, in the build's ci
    };
    std::vector<row> rows;

    static std::vector<rap::column> columns()
    {
        using namespace rap::literals;
        // The time in the name's column: a column after a stretching one
        // can end up past the list's edge.
        return {{.title = "", .width = 3_ch, .painted = true},
                {.title = _("Check"), .stretch = true}};
    }
    size_t row_count() const override { return rows.size(); }
    std::string text(size_t r, size_t c) const override
    {
        if (r >= rows.size()) return {};
        if (c != 1) return {};
        return rows[r].duration.empty() ? rows[r].text : rows[r].text + "  ·  " + rows[r].duration;
    }
    rap::text_style style(size_t r, size_t) const override
    {
        return {.bold = r < rows.size() && rows[r].kind == row::workflow};
    }
    void paint(size_t r, size_t c, rap::painter& p, const rap::cell_info& cell) const override
    {
        if (c == 0 && r < rows.size()) paint_mark(p, cell.size, rows[r].state);
    }
    void set(const commit_ci& ci, std::int64_t now)
    {
        rows.clear();
        for (size_t w = 0; w < ci.workflows.size(); ++w)
        {
            const ci_workflow& flow = ci.workflows[w];
            tally all;
            for (const ci_job& j : flow.jobs) all.add(j);
            std::string title = flow.name.empty() ? std::string(_("Statuses")) : flow.name;
            if (flow.run_number) title += " #" + count(flow.run_number);
            const bool alone = ci.workflows.size() == 1 && flow.name.empty();
            if (!alone)
                rows.push_back(
                    {.kind = row::workflow,
                     .state = all.state(),
                     .text = title + "  " + tally_text(all),
                     .duration = all.first_start && all.last_end > all.first_start && !all.running
                                     ? check_duration(all.last_end - all.first_start)
                                     : std::string(),
                     .w = w});
            const std::string indent = alone ? "" : "   ";
            // A matrix's jobs together, in the order the first of each
            // comes (failures first already).
            std::vector<std::string_view> bases;
            for (const ci_job& j : flow.jobs)
                if (const auto base = matrix_parts(j.name).first;
                    std::ranges::find(bases, base) == bases.end())
                    bases.push_back(base);
            for (std::string_view base : bases)
            {
                std::vector<size_t> members;
                for (size_t j = 0; j < flow.jobs.size(); ++j)
                    if (matrix_parts(flow.jobs[j].name).first == base) members.push_back(j);
                const bool matrix = members.size() > 1 ||
                                    !matrix_parts(flow.jobs[members.front()].name).second.empty();
                if (matrix && members.size() > 1)
                {
                    tally t;
                    for (size_t j : members) t.add(flow.jobs[j]);
                    rows.push_back({.kind = row::group,
                                    .state = t.state(),
                                    .text = indent + std::string(base) + ": " + tally_text(t),
                                    .w = w});
                }
                for (size_t j : members)
                {
                    const ci_job& job = flow.jobs[j];
                    const auto variant = matrix_parts(job.name).second;
                    const bool under = matrix && members.size() > 1;
                    rows.push_back(
                        {.kind = row::job,
                         .state = job.state,
                         .text = indent + (under ? "   " + std::string(variant) : job.name),
                         .duration = job_duration(job, now),
                         .w = w,
                         .j = j});
                }
            }
        }
        notify_reset();
    }
};

class forge_panel::artifact_model : public rap::list_model
{
public:
    std::vector<ci_artifact> rows;
    using rap::list_model::notify_reset;
    static std::vector<rap::column> columns()
    {
        using namespace rap::literals;
        return {{.title = _("Artifact"), .stretch = true},
                {.title = _("Size"), .width = 9_ch, .align = rap::align::end},
                {.title = _("Workflow"), .width = 14_ch}};
    }
    size_t row_count() const override { return rows.size(); }
    std::string text(size_t r, size_t c) const override
    {
        if (r >= rows.size()) return {};
        const ci_artifact& a = rows[r];
        switch (c)
        {
        case 0: return a.expired ? rap::i18n::format(_("{0} (expired)"), a.name) : a.name;
        case 1: return size_text(a.size);
        default: return a.workflow;
        }
    }
    rap::text_style style(size_t r, size_t) const override
    {
        if (r < rows.size() && rows[r].expired)
            return {.fg = rap::paint{rap::theme_color::placeholder_text}};
        return {};
    }
};

class forge_panel::annotation_model : public rap::list_model
{
public:
    std::vector<ci_annotation> rows;
    using rap::list_model::notify_reset;
    static std::vector<rap::column> columns() { return {{.title = _("Error"), .stretch = true}}; }
    size_t row_count() const override { return rows.size(); }
    std::string text(size_t r, size_t) const override
    {
        if (r >= rows.size()) return {};
        const ci_annotation& a = rows[r];
        std::string where = a.path;
        if (!where.empty() && a.line) where += ":" + count(a.line);
        std::string message = a.message.substr(0, a.message.find('\n'));
        return where.empty() ? message : where + ": " + message;
    }
    std::string tooltip(size_t r, size_t, rap::point_f) const override
    {
        return r < rows.size() ? rows[r].message : std::string();
    }
};

// -- the panel ----------------------------------------------------------------

// What the workers share with the panel: whether it's still there, and the
// accounts' tokens, so the keychain (or gh) isn't asked each time.
struct forge_panel::shared
{
    std::mutex mutex;
    bool alive = true;
    std::map<std::string, std::string> tokens;  // "host login" -> token
};

namespace
{

// On a worker: the account's token, from what's kept or the keychain.
template <class Shared>
std::optional<std::string> token_for(Shared& sh, const forge_account& a)
{
    // A public forge asked as no one: no token, and nothing to look up.
    if (a.login.empty()) return std::string();
    const std::string key = a.host + ' ' + a.login;
    {
        std::lock_guard lock(sh.mutex);
        if (!sh.alive) return std::nullopt;
        if (auto it = sh.tokens.find(key); it != sh.tokens.end()) return it->second;
    }
    std::string token;
    if (auto t = load_token(a)) token = t->access;
    std::lock_guard lock(sh.mutex);
    if (!sh.alive) return std::nullopt;
    if (!token.empty()) sh.tokens[key] = token;
    return token;
}

}  // namespace

forge_panel::forge_panel(host h)
    : host_(std::move(h))
    , shared_(std::make_shared<shared>())
    , matrix_(std::make_unique<matrix_model>())
    , artifact_list_(std::make_unique<artifact_model>())
    , annotation_list_(std::make_unique<annotation_model>())
{
}

forge_panel::~forge_panel()
{
    std::lock_guard lock(shared_->mutex);
    shared_->alive = false;
    if (download_cancel_) *download_cancel_ = true;
}

void forge_panel::with_token(const forge_remote& r,
                             std::function<std::function<void()>(std::string token)> work)
{
    std::thread(
        [sh = shared_, account = r.account, work = std::move(work), &ui = host_.ui]
        {
            const auto token = token_for(*sh, account);
            if (!token) return;
            auto done = work(*token);
            ui.post(
                [sh, done = std::move(done)]
                {
                    {
                        std::lock_guard lock(sh->mutex);
                        if (!sh->alive) return;
                    }
                    if (done) done();
                });
        })
        .detach();
}

void forge_panel::built()
{
    parts_->on_moved(
        [this]
        {
            if (parts_state_ == parts::pr_and_lists) part_sizes_ = parts_->sizes();
        });
}

// The divider between the PR (and the build's line) and the build's lists.
// With a PR, where it was last dragged to, else two fifths of the height
// for the lists; without one, the build's line as high as it needs and the
// lists the rest. The upper part takes what the window gains or loses.
void forge_panel::fit_parts(bool pr, bool lists)
{
    build_body_->set_visible(lists);
    const parts state = !lists ? parts::one : pr ? parts::pr_and_lists : parts::lists;
    if (state == parts_state_) return;
    float total = 0;
    for (float s : parts_->sizes()) total += s;
    if (state != parts::one && total <= 0) return;  // not laid out yet: next time
    parts_state_ = state;
    if (state == parts::pr_and_lists)
    {
        if (part_sizes_.size() == 2)
            parts_->set_sizes(part_sizes_);
        else
            parts_->set_sizes({total * 0.6f, total * 0.4f});
    }
    else if (state == parts::lists)
    {
        // The line: a mark's height, and the box's spacing around it.
        const float line = float(k_mark_size) + 14;
        parts_->set_sizes({line, std::max(total - line, line)});
    }
}

void forge_panel::set_part_sizes(std::vector<float> sizes)
{
    if (sizes.size() == 2 && sizes[0] > 0 && sizes[1] > 0) part_sizes_ = std::move(sizes);
}

rap::node forge_panel::layout()
{
    using namespace rap::layout;
    auto mark = [](rap::canvas*& bind, const std::function<void(rap::painter&, rap::size_f)>& paint)
    {
        return canvas{}
            .focusable(false)
            .on_paint(paint)
            .width(rap::extent{k_mark_size, rap::extent::unit::px})
            .height(rap::extent{k_mark_size, rap::extent::unit::px})
            .bind(bind);
    };
    auto background = [](rap::painter& p, rap::size_f size)
    { p.rect({0, 0, size.w, size.h}, 0, rap::paint{rap::theme_color::window}, std::nullopt); };
    return vbox{
        splitter{
            vertical,
            // Above the divider: the PR, then the build's line under it.
            vbox{
                // Pull request
                vbox{
                    label{}.wrap().selectable().bind(pr_title_),
                    hbox{
                        canvas{}
                            .focusable(false)
                            .on_paint(
                                [this, background](rap::painter& p, rap::size_f size)
                                {
                                    background(p, size);
                                    if (!picture_) return;
                                    // Whole and in shape if laid out
                                    // smaller than asked.
                                    const float iw = float(picture_->width()),
                                                ih = float(picture_->height());
                                    p.image(*picture_, {0, 0, iw, ih},
                                            rap::fit_inside({0, 0, size.w, size.h}, iw, ih));
                                })
                            .width(rap::extent{k_picture_size, rap::extent::unit::px})
                            .height(rap::extent{k_picture_size, rap::extent::unit::px})
                            .bind(pr_avatar_),
                        label{}.ellipsize().bind(pr_author_).stretch(),
                    },
                    hbox{
                        mark(pr_merge_mark_,
                             [this, background](rap::painter& p, rap::size_f size)
                             {
                                 background(p, size);
                                 paint_mark(p, size, merge_mark_state_,
                                            mergeable(merge_mark_kind_));
                             }),
                        label{}.wrap().bind(pr_merge_).stretch(),
                    },
                    hbox{
                        mark(pr_checks_mark_,
                             [this, background](rap::painter& p, rap::size_f size)
                             {
                                 background(p, size);
                                 paint_mark(p, size, checks_mark_state_);
                             }),
                        label{}.wrap().bind(pr_checks_).stretch(),
                    },
                    label{}.wrap().selectable().bind(pr_reviews_),
                    // The description takes what the part has left, and
                    // scrolls: as a label, a long one widened the column
                    // past the window.
                    text_view{}.read_only().compact().bind(pr_body_).height(3_lines).stretch(),
                    label{}.wrap().bind(pr_also_),
                    hbox{
                        button{""}
                            .bind(pr_buttons_[0])
                            .on_click(
                                [this]
                                {
                                    if (pr_actions_.size() > 0 && pr_actions_[0])
                                        pr_actions_[0]();
                                }),
                        button{""}
                            .bind(pr_buttons_[1])
                            .on_click(
                                [this]
                                {
                                    if (pr_actions_.size() > 1 && pr_actions_[1])
                                        pr_actions_[1]();
                                }),
                        button{""}
                            .bind(pr_buttons_[2])
                            .on_click(
                                [this]
                                {
                                    if (pr_actions_.size() > 2 && pr_actions_[2])
                                        pr_actions_[2]();
                                }),
                        spacer{},
                    },
                }
                    .stretch()
                    .handle(pr_part_),
                // Build
                vbox{
                    hbox{
                        mark(build_mark_,
                             [this, background](rap::painter& p, rap::size_f size)
                             {
                                 background(p, size);
                                 paint_mark(p, size, build_mark_state_);
                             }),
                        label{}.ellipsize().bind(build_summary_).stretch(),
                        label{_("Show all")}
                            .link(
                                [this]
                                {
                                    matrix_open_ = !matrix_open_;
                                    matrix_opened_by_user_ = true;
                                    update_build();
                                })
                            .bind(build_toggle_),
                    },
                }
                    .handle(build_part_),
                // Without a PR, holds the build's line at the top.
                vbox{spacer{}}.stretch().handle(top_spacer_),
            },
            // Below it: the build's checks, why a job failed, its artifacts;
            // hidden while there's none of that to show.
            vbox{
                list{matrix_model::columns(), matrix_.get()}
                    .headers(false)
                    .compact()
                    .selection(rap::selection_mode::single)
                    .bind(matrix_list_)
                    .on_selection_changed(
                        [this]
                        {
                            const auto sel = matrix_list_->selection();
                            if (sel.size() != 1 || sel.front() >= matrix_->rows.size())
                                return;
                            const auto& row = matrix_->rows[sel.front()];
                            if (row.kind != matrix_model::row::job) return;
                            ask_failure(row.w, row.j);
                        })
                    .on_activate(
                        [this](size_t r)
                        {
                            if (!sha_ || r >= matrix_->rows.size()) return;
                            auto it = builds_.find(*sha_);
                            if (it == builds_.end()) return;
                            const auto& row = matrix_->rows[r];
                            const ci_workflow& w = it->second.ci.workflows[row.w];
                            const std::string url = row.kind == matrix_model::row::job
                                                        ? w.jobs[row.j].url
                                                        : w.url;
                            if (!url.empty()) rap::sys::open_url(url);
                        })
                    .height(4_lines)
                    .stretch(2),
                // Why the selected job failed.
                vbox{
                    text_view{}.read_only().compact().bind(failure_title_).height(3_lines),
                    list{annotation_model::columns(), annotation_list_.get()}
                        .headers(false)
                        .compact()
                        .selection(rap::selection_mode::single)
                        .bind(annotations_)
                        .on_activate(
                            [this](size_t r)
                            {
                                if (r < annotation_list_->rows.size() &&
                                    !annotation_list_->rows[r].url.empty())
                                    rap::sys::open_url(annotation_list_->rows[r].url);
                            })
                        .height(4_lines),
                    text_view{}
                        .read_only()
                        .monospace()
                        .wrap(false)
                        .bind(log_tail_)
                        .height(4_lines)
                        .stretch(3),
                    label{_("Open the full log")}
                        .link(
                            [this]
                            {
                                if (!full_log_url_.empty())
                                    rap::sys::open_url(full_log_url_);
                            })
                        .bind(full_log_),
                }
                    .stretch(3)
                    .handle(failure_part_),
                // The artifacts the runs left.
                vbox{
                    list{artifact_model::columns(), artifact_list_.get()}
                        .compact()
                        .selection(rap::selection_mode::single)
                        .bind(artifacts_)
                        .on_selection_changed(
                            [this]
                            {
                                const auto sel = artifacts_->selection();
                                download_->set_enabled(
                                    !downloading_ && sel.size() == 1 &&
                                    sel.front() < artifact_list_->rows.size() &&
                                    !artifact_list_->rows[sel.front()].expired);
                            })
                        .on_activate([this](size_t) { download_selected(); })
                        .height(4_lines)
                        .stretch(),
                    hbox{
                        button{_("_Download...")}.bind(download_).enabled(false).on_click(
                            [this] { download_selected(); }),
                        progress_bar{}.bind(download_progress_).stretch(),
                        spacer{},
                    },
                    label{}.wrap().selectable().bind(download_status_),
                }
                    .stretch()
                    .handle(artifact_part_),
            }
                .handle(build_body_),
        }
            .bind(parts_)
            .stretch(),
    }
        .width(30_ch)
        .visible(false)
        .handle(root_);
}

// -- which commits are a PR's -------------------------------------------------

void forge_panel::set_pull_requests(const std::vector<open_pr>& prs,
                                    const std::vector<forge_remote>& remotes)
{
    remotes_ = remotes;
    // A PR's head branch as fetched from the repository it's in, and its
    // base as fetched from where it's open.
    const auto heads = match_pull_requests(remotes, prs);
    std::vector<pr_entry> entries;
    for (const open_pr& p : prs)
    {
        pr_entry e{.pr = p};
        for (const auto& [ref, head] : heads)
            if (head.pr.number == p.pr.number && head.into.name == p.into.name) e.head_ref = ref;
        if (e.head_ref.empty()) continue;  // not fetched here: none of its commits are
        e.base_ref = "refs/remotes/" + p.into.name + "/" + p.pr.base_branch;
        const std::string key = e.head_ref + ' ' + p.pr.head_sha + ' ' + e.base_ref;
        if (auto it = commits_cache_.find(key); it != commits_cache_.end())
        {
            e.commits = it->second;
            e.known = true;
        }
        entries.push_back(std::move(e));
    }
    prs_ = std::move(entries);
    forget_misplaced_builds();
    work_out_commits();
    update();
}

// Builds asked of the wrong place: a commit that was only here (it may
// have been pushed since), or one asked of a fork that turned out to be a
// PR's, whose runs are on the repository it's open in.
void forge_panel::forget_misplaced_builds()
{
    std::erase_if(builds_,
                  [this](const auto& kept)
                  {
                      const build& b = kept.second;
                      if (b.local) return true;
                      if (!b.answered || !b.ci.empty()) return false;
                      bool head = false;
                      if (const pr_entry* e = pull_request_of(kept.first, head, nullptr))
                          return e->pr.into.name != b.remote.name;
                      // Asked of a fork before the PRs said where they're open.
                      return std::ranges::any_of(prs_, [&](const pr_entry& p)
                                                 { return p.pr.into.name != b.remote.name; }) &&
                             std::ranges::none_of(prs_, [&](const pr_entry& p)
                                                  { return p.pr.into.name == b.remote.name; });
                  });
}

void forge_panel::work_out_commits()
{
    std::vector<std::pair<std::string, std::pair<std::string, std::string>>> wanted;
    for (const pr_entry& e : prs_)
        if (!e.known)
            wanted.push_back(
                {e.head_ref + ' ' + e.pr.pr.head_sha + ' ' + e.base_ref, {e.head_ref, e.base_ref}});
    if (wanted.empty()) return;
    std::thread(
        [sh = shared_, workdir = host_.workdir, wanted = std::move(wanted), this, &ui = host_.ui]
        {
            std::map<std::string, std::vector<std::string>> found;
            for (const auto& [key, refs] : wanted)
            {
                {
                    std::lock_guard lock(sh->mutex);
                    if (!sh->alive) return;
                }
                found[key] = pull_request_commits(workdir, refs.first, refs.second);
            }
            ui.post(
                [sh, found = std::move(found), this]() mutable
                {
                    {
                        std::lock_guard lock(sh->mutex);
                        if (!sh->alive) return;
                    }
                    for (auto& [key, commits] : found) commits_cache_[key] = std::move(commits);
                    for (pr_entry& e : prs_)
                        if (auto it = commits_cache_.find(e.head_ref + ' ' + e.pr.pr.head_sha +
                                                          ' ' + e.base_ref);
                            it != commits_cache_.end())
                        {
                            e.commits = it->second;
                            e.known = true;
                        }
                    forget_misplaced_builds();
                    update();
                });
        })
        .detach();
}

const forge_panel::pr_entry* forge_panel::pull_request_of(const std::string& sha, bool& head,
                                                          std::vector<int>* others) const
{
    // The PRs come newest updated first: the first that has it is shown.
    const pr_entry* shown = nullptr;
    for (const pr_entry& e : prs_)
    {
        const bool is_head =
            e.pr.pr.head_sha == sha || (!e.commits.empty() && e.commits.front() == sha);
        if (!is_head && std::ranges::find(e.commits, sha) == e.commits.end()) continue;
        if (!shown)
        {
            shown = &e;
            head = is_head;
        }
        else if (others)
            others->push_back(e.pr.pr.number);
    }
    return shown;
}

// -- what's shown -------------------------------------------------------------

void forge_panel::show_commit(std::optional<std::string> sha, std::string author_name,
                              std::string author_email)
{
    if (sha == sha_) return;
    sha_ = std::move(sha);
    author_name_ = std::move(author_name);
    author_email_ = std::move(author_email);
    failed_job_.reset();
    if (!sha_ || *sha_ != matrix_sha_)
    {
        matrix_opened_by_user_ = false;
        matrix_open_ = false;
    }
    update();
}

void forge_panel::update()
{
    if (!root_) return;
    update_pull_request();
    update_build();
    update_watch();
}

// While the build on show runs, it's asked after: GitHub's quickly by
// pr_watch, which hands its runs to commit_checks; the others' again now
// and then. A finished one is kept as it is.
void forge_panel::update_watch()
{
    const build* b = nullptr;
    if (sha_)
        if (auto it = builds_.find(*sha_);
            it != builds_.end() && it->second.answered && !it->second.local)
            b = &it->second;
    const bool running = b && b->ci.state() == check_state::pending;
    if (host_.watch)
        host_.watch(sha_, b ? std::optional<forge_remote>(b->remote) : std::nullopt, running);
    if (!running || b->remote.account.kind == forge_kind::github)
    {
        poll_timer_.reset();
        return;
    }
    if (poll_after_ == b->at) return;
    poll_after_ = b->at;
    poll_timer_ = host_.ui.timer(int(std::chrono::milliseconds(k_running_poll).count()), false,
                                 [this]
                                 {
                                     if (sha_) ask_build(*sha_);
                                 });
}

void forge_panel::commit_checks(const std::string& sha, const commit_runs& runs)
{
    auto it = builds_.find(sha);
    if (it == builds_.end() || !it->second.answered) return;
    build& b = it->second;
    // Each job by its id; a job not seen yet (a workflow that's started
    // since), or the end of it all, asks for the whole build again.
    bool unknown = runs.state != check_state::pending;
    for (size_t i = 0; i < runs.runs.size(); ++i)
    {
        bool found = false;
        for (ci_workflow& w : b.ci.workflows)
            for (ci_job& j : w.jobs)
                if (runs.ids[i] ? j.id == runs.ids[i] : (!j.id && j.name == runs.runs[i].name))
                {
                    j.state = runs.runs[i].state;
                    j.started_at = runs.runs[i].started_at;
                    j.completed_at = runs.runs[i].completed_at;
                    found = true;
                }
        unknown = unknown || !found;
    }
    if (unknown)
        ask_build(sha);
    else
        order_failures_first(b.ci);
    if (sha_ == sha) update();
}

void forge_panel::update_pull_request()
{
    bool head = false;
    std::vector<int> others;
    const pr_entry* e = sha_ ? pull_request_of(*sha_, head, &others) : nullptr;
    pr_part_->set_visible(e != nullptr);
    if (!e)
    {
        pr_url_.clear();
    }
    else
    {
        // The details asked for, when they're in.
        const std::string key = e->pr.into.name + " #" + count(e->pr.pr.number);
        const auto known = details_.find(key);
        const pull_request& pr =
            known != details_.end() && known->second.head_sha == e->pr.pr.head_sha ? known->second
                                                                                   : e->pr.pr;
        if ((!pr.body_known || !pr.reviews_known) && !details_asked_.contains(key)) ask_details(*e);

        const std::string number = "#" + count(pr.number);
        pr_title_->set_text(head ? number + " " + pr.title
                                 : rap::i18n::format(_("Part of {0}: {1}"), number, pr.title));
        std::string who = pr.author.empty() ? std::string() : pr.author + " · ";
        who += rap::i18n::format(pr.draft ? _("Draft, into {0}") : _("Open, into {0}"),
                                 pr.base_branch);
        pr_author_->set_text(who);
        if (!pr.author_avatar.empty()) ask_avatar(pr.author_avatar);
        auto picture = pictures_.find(pr.author_avatar);
        auto shown_picture = picture == pictures_.end() ? nullptr : picture->second;
        if (shown_picture != picture_)
        {
            picture_ = shown_picture;
            pr_avatar_->invalidate();
        }
        // The commit's own picture is beside its text already: the PR's
        // author's only when that's someone else.
        pr_avatar_->set_visible(
            !author_is(pr.author, author_name_, author_email_, e->pr.into.account));

        // Whether it can be merged, coloured as the label's tick is.
        std::string merge = merge_text(pr);
        if (pr.conflicts)
        {
            merge_mark_state_ = check_state::success;
            merge_mark_kind_ = int(mergeable::conflicts);
        }
        else if (!pr.blocks.empty())
        {
            merge_mark_state_ = check_state::success;
            merge_mark_kind_ = int(mergeable::not_yet);
        }
        else if (pr.checks == check_state::pending)
        {
            merge_mark_state_ = check_state::pending;
            merge_mark_kind_ = int(mergeable::yes);
            merge = _("Can be merged once the checks pass");
        }
        else if (pr.checks == check_state::failure)
        {
            merge_mark_state_ = check_state::failure;
            merge_mark_kind_ = int(mergeable::yes);
            merge = _("Checks failed: fix them first");
        }
        else
        {
            merge_mark_state_ = check_state::success;
            merge_mark_kind_ = int(mergeable::yes);
        }
        pr_merge_->set_text(merge);
        pr_merge_mark_->invalidate();

        // The head's checks in a line; the commit's own are under Build.
        checks_mark_state_ = pr.checks;
        pr_checks_->set_text(rap::i18n::format(_("Head: {0}"), checks_text(pr, now_seconds())));
        pr_checks_mark_->invalidate();

        // Reviews
        if (!pr.reviews_known)
            pr_reviews_->set_text(_("Reviews: asking..."));
        else
        {
            const review_summary r = summarise_reviews(pr.reviews);
            std::string text;
            auto line = [&](const char* label, const std::vector<std::string>& who)
            {
                if (who.empty()) return;
                text += (text.empty() ? "" : "\n") + rap::i18n::format(label, names(who));
            };
            line(_("Approved by {0}"), r.approved);
            line(_("Changes requested by {0}"), r.changes_requested);
            line(_("Commented: {0}"), r.commented);
            line(_("Waiting on {0}"), r.waiting);
            pr_reviews_->set_text(text.empty() ? std::string(_("No reviews yet")) : text);
        }

        // Its description, all of it, scrolling in its view.
        if (!pr.body_known)
            pr_body_->set_text(_("Asking for its description..."));
        else if (pr_url_ != pr.url + pr.head_sha || pr_body_text_ != clean_body(pr.body))
        {
            pr_body_text_ = clean_body(pr.body);
            pr_body_->set_text(pr_body_text_.empty() ? std::string(_("No description."))
                                                     : pr_body_text_);
        }
        pr_url_ = pr.url + pr.head_sha;

        if (others.empty())
            pr_also_->set_visible(false);
        else
        {
            std::string list;
            for (int n : others) list += (list.empty() ? "#" : ", #") + count(n);
            pr_also_->set_text(rap::i18n::format(_("Also in {0}"), list));
            pr_also_->set_visible(true);
        }

        // The log's own actions for it, as its menu has them.
        pr_actions_.clear();
        size_t b = 0;
        if (host_.actions)
            for (const rap::menu_item& item : host_.actions(e->pr))
            {
                if (item.is_separator() || b >= pr_buttons_.size()) continue;
                // The menu's "Merge... (needs write access)": the why in the
                // tooltip, so the buttons keep the column narrow.
                const size_t why = item.label.find(" (");
                pr_buttons_[b]->set_text(item.label.substr(0, why));
                pr_buttons_[b]->set_tooltip(why == std::string::npos ? std::string()
                                                                     : menu_text(item.label));
                pr_buttons_[b]->set_enabled(item.enabled);
                pr_buttons_[b]->set_visible(true);
                pr_actions_.push_back(item.action);
                ++b;
            }
        for (; b < pr_buttons_.size(); ++b) pr_buttons_[b]->set_visible(false);
    }
}

void forge_panel::update_build()
{
    const build* b = nullptr;
    if (sha_)
    {
        auto it = builds_.find(*sha_);
        if (it == builds_.end() || !it->second.asked)
        {
            ask_build(*sha_);
            it = builds_.find(*sha_);
        }
        if (it != builds_.end()) b = &it->second;
    }
    // Shown once there's something to show: not for a commit that's only
    // here, nor while it's asked (it's usually quick, and often nothing).
    const bool shown = b && b->answered && !b->local && (!b->ci.empty() || !b->error.empty());
    build_part_->set_visible(shown);
    const bool pr_shown = sha_ && [&]
    {
        bool head = false;
        return pull_request_of(*sha_, head, nullptr) != nullptr;
    }();
    top_spacer_->set_visible(!pr_shown);
    if (const bool visible = shown || pr_shown; visible != visible_)
    {
        visible_ = visible;
        // The host hears while the column's still there to be measured.
        if (!visible && host_.shown) host_.shown(false);
        root_->set_visible(visible);
        if (visible && host_.shown) host_.shown(true);
    }
    if (!shown)
    {
        failure_part_->set_visible(false);
        fit_parts(pr_shown, false);
        return;
    }

    if (!b->error.empty() && b->ci.empty())
    {
        build_mark_state_ = check_state::none;
        build_summary_->set_text(rap::i18n::format(_("CI: {0}"), b->error));
        build_toggle_->set_visible(false);
        matrix_list_->set_visible(false);
        artifact_part_->set_visible(false);
        failure_part_->set_visible(false);
        fit_parts(pr_shown, false);
        build_mark_->invalidate();
        return;
    }

    tally all;
    for (const ci_workflow& w : b->ci.workflows)
        for (const ci_job& j : w.jobs) all.add(j);
    build_mark_state_ = all.state();
    build_mark_->invalidate();
    build_summary_->set_text(rap::i18n::format(_("Build: {0}"), build_text(all)));
    // Calm when it all passed: a line, opened on asking. A failure opens it.
    if (!matrix_opened_by_user_) matrix_open_ = all.failed > 0;
    matrix_sha_ = *sha_;
    build_toggle_->set_text(matrix_open_ ? _("Hide") : _("Show all"));
    build_toggle_->set_visible(true);
    matrix_list_->set_visible(matrix_open_);
    if (matrix_open_)
    {
        // The rows again keep the job picked, if it's still there.
        const auto picked = failed_job_;
        matrix_->set(b->ci, now_seconds());
        if (picked)
            for (size_t r = 0; r < matrix_->rows.size(); ++r)
            {
                const auto& row = matrix_->rows[r];
                if (row.kind != matrix_model::row::job) continue;
                const ci_job& j = b->ci.workflows[row.w].jobs[row.j];
                if (j.id == picked->id && j.name == picked->name)
                {
                    matrix_list_->select({r});
                    break;
                }
            }
    }
    update_failure();

    const bool artifacts = b->artifacts_known && !b->artifacts.empty();
    artifact_part_->set_visible(artifacts);
    fit_parts(pr_shown, matrix_open_ || artifacts);
    if (b->artifacts_known && artifact_list_->rows.size() != b->artifacts.size())
    {
        artifact_list_->rows = b->artifacts;
        artifact_list_->notify_reset();
        download_->set_enabled(false);
    }
    else if (b->artifacts_known)
    {
        bool same = true;
        for (size_t i = 0; i < b->artifacts.size() && same; ++i)
            same = artifact_list_->rows[i].download_url == b->artifacts[i].download_url &&
                   artifact_list_->rows[i].expired == b->artifacts[i].expired;
        if (!same)
        {
            artifact_list_->rows = b->artifacts;
            artifact_list_->notify_reset();
            download_->set_enabled(false);
        }
    }
    download_progress_->set_visible(downloading_);
}

void forge_panel::update_failure()
{
    if (!failed_job_ || !matrix_open_)
    {
        failure_part_->set_visible(false);
        return;
    }
    const ci_job& job = *failed_job_;
    if (job.state != check_state::failure)
    {
        failure_part_->set_visible(false);
        return;
    }
    failure_part_->set_visible(true);
    full_log_url_ = job.url;
    full_log_->set_visible(!job.url.empty());
    std::string title = job.name;
    const auto found = failures_.find(job.id);
    const job_failure* f = found != failures_.end() ? &found->second : nullptr;
    const auto error = failure_errors_.find(job.id);
    if (job.id && !f && error == failure_errors_.end())
        title = rap::i18n::format(_("{0}: asking why it failed..."), job.name);
    else if (auto e = error; e != failure_errors_.end())
        title = rap::i18n::format(_("{0}: couldn't ask why it failed: {1}"), job.name, e->second);
    else if (f && !f->step.empty())
        title = f->step_number ? rap::i18n::format(_("{0}: failed at {1} (step {2})"), job.name,
                                                   f->step, count(f->step_number))
                               : rap::i18n::format(_("{0}: failed at {1}"), job.name, f->step);
    else if (!job.failure_reason.empty())
        title = rap::i18n::format(_("{0}: failed ({1})"), job.name, job.failure_reason);
    else if (!job.id)
        title = rap::i18n::format(
            _("{0}: the forge says only that it failed; its page may say why."), job.name);
    if (f && !job.failure_reason.empty() && f->step.empty())
        title = rap::i18n::format(_("{0}: failed ({1})"), job.name, job.failure_reason);
    failure_title_->set_text(title);

    const std::vector<ci_annotation> notes = f ? f->annotations : std::vector<ci_annotation>{};
    annotations_->set_visible(!notes.empty());
    if (annotation_list_->rows.size() != notes.size() ||
        !std::ranges::equal(annotation_list_->rows, notes, {}, &ci_annotation::message,
                            &ci_annotation::message))
    {
        annotation_list_->rows = notes;
        annotation_list_->notify_reset();
    }
    const std::string tail = f ? f->log_tail : std::string();
    log_tail_->set_visible(!tail.empty());
    if (log_tail_->text() != tail)
    {
        log_tail_->set_text(tail);
        log_tail_->select(tail.size(), tail.size());  // its end, where it failed
    }
}

// -- asking the forge ---------------------------------------------------------

void forge_panel::ask_details(const pr_entry& e)
{
    const std::string key = e.pr.into.name + " #" + count(e.pr.pr.number);
    details_asked_.insert(key);
    with_token(e.pr.into,
               [this, key, kind = e.pr.into.account.kind, into = e.pr.into.repo,
                pr = e.pr.pr](std::string token) -> std::function<void()>
               {
                   auto got = pull_request_details(kind, into, pr, token);
                   return [this, key, got = std::move(got), pr]() mutable
                   {
                       if (got)
                           details_[key] = std::move(*got);
                       else
                       {
                           // Said where they'd be; not asked again this time.
                           pull_request shown = pr;
                           shown.body_known = shown.reviews_known = true;
                           if (!pr.body_known)
                               shown.body =
                                   rap::i18n::format(_("Couldn't ask for it: {0}"), got.error());
                           details_[key] = std::move(shown);
                       }
                       update();
                   };
               });
}

void forge_panel::ask_avatar(const std::string& url)
{
    if (url.empty() || pictures_asked_.contains(url)) return;
    pictures_asked_.insert(url);
    // At the size it's drawn, for the screen's scale; GitHub's and Gitea's
    // take a size. No token: pictures are public.
    const int px =
        int(k_picture_size * std::max(1.f, pr_avatar_ ? pr_avatar_->device_scale() : 1.f));
    std::string at = url;
    if (url.find("avatars.githubusercontent.com") != std::string::npos)
        at += (url.find('?') == std::string::npos ? "?s=" : "&s=") + count(px * 2);
    std::thread(
        [sh = shared_, at, url, this, &ui = host_.ui]
        {
            const auto reply = rap::sys::http_get(at, {.headers = {{"User-Agent", "Tortilla"}}});
            std::shared_ptr<rap::sys::decoded_image> decoded;
            if (reply.status == 200)
                if (auto d = rap::sys::decode_image(
                        {reinterpret_cast<const uint8_t*>(reply.body.data()), reply.body.size()}))
                    decoded = std::make_shared<rap::sys::decoded_image>(std::move(*d));
            ui.post(
                [sh, decoded, url, this]
                {
                    {
                        std::lock_guard lock(sh->mutex);
                        if (!sh->alive) return;
                    }
                    std::shared_ptr<rap::image> image;
                    if (decoded)
                        image = host_.ui.create_image(decoded->width, decoded->height,
                                                      decoded->format, decoded->pixels.data(),
                                                      size_t(decoded->width) * 4);
                    pictures_[url] = std::move(image);
                    update();
                });
        })
        .detach();
}

void forge_panel::ask_build(const std::string& sha)
{
    build& b = builds_[sha];
    if (b.asking) return;
    b.asked = b.asking = true;
    // Where to ask: the PR's own repository first (a fork's commits are its
    // runs' there), then the remotes that have the commit, in order.
    std::vector<forge_remote> candidates;
    bool head = false;
    if (const pr_entry* e = pull_request_of(sha, head, nullptr)) candidates.push_back(e->pr.into);
    // Then where PRs are open (upstream, not a fork whose branch merely
    // has the commit too), then the rest.
    auto add = [&](const forge_remote& r)
    {
        if (std::ranges::none_of(candidates,
                                 [&](const forge_remote& c) { return c.name == r.name; }))
            candidates.push_back(r);
    };
    for (const pr_entry& e : prs_)
        if (std::ranges::any_of(remotes_,
                                [&](const forge_remote& r) { return r.name == e.pr.into.name; }))
            add(e.pr.into);
    for (const forge_remote& r : remotes_) add(r);
    if (candidates.empty())
    {
        b.answered = b.local = true;
        b.asking = false;
        return;
    }
    const bool in_pr = pull_request_of(sha, head, nullptr) != nullptr;
    const bool had_artifacts = b.artifacts_known;
    std::thread(
        [sh = shared_, workdir = host_.workdir, sha, candidates = std::move(candidates), in_pr,
         had_artifacts, this, &ui = host_.ui]
        {
            // A PR's commits are on its repository whether fetched there or
            // not; any other only where it's been pushed.
            const forge_remote* where =
                in_pr ? &candidates.front() : remote_with_commit(workdir, sha, candidates);
            auto post = [&](std::function<void()> f)
            {
                ui.post(
                    [sh, f = std::move(f)]
                    {
                        {
                            std::lock_guard lock(sh->mutex);
                            if (!sh->alive) return;
                        }
                        f();
                    });
            };
            if (!where)
            {
                post(
                    [this, sha]
                    {
                        build& b = builds_[sha];
                        b.answered = b.local = true;
                        b.asking = false;
                        update();
                    });
                return;
            }
            auto token = token_for(*sh, where->account);
            if (!token) return;
            auto ci = tortilla::commit_checks(where->account.kind, where->repo, sha, *token);
            // A fork's commit whose CI ran where it was proposed (a pull
            // request's runs are the upstream's): the other remotes, until
            // one has some; two at most.
            int tries = 0;
            for (const forge_remote& other : candidates)
            {
                if (!ci || !ci->empty() || tries == 2) break;
                if (&other == where) continue;
                ++tries;
                const auto other_token = token_for(*sh, other.account);
                if (!other_token) return;
                auto found =
                    tortilla::commit_checks(other.account.kind, other.repo, sha, *other_token);
                if (found && !found->empty())
                {
                    ci = std::move(found);
                    where = &other;
                    token = other_token;
                }
            }
            post(
                [this, sha, ci, remote = *where]
                {
                    build& b = builds_[sha];
                    b.answered = true;
                    b.asking = false;
                    b.remote = remote;
                    b.at = std::chrono::steady_clock::now();
                    if (ci)
                    {
                        b.ci = *ci;
                        b.error.clear();
                    }
                    else
                        b.error = ci.error();
                    if (sha_ == sha) update();
                });
            // The artifacts once, and again only while the runs go on.
            if (!ci || ci->empty() || (had_artifacts && ci->state() != check_state::pending))
                return;
            auto artifacts = commit_artifacts(where->account.kind, where->repo, sha, *ci, *token);
            post(
                [this, sha, artifacts = std::move(artifacts)]
                {
                    build& b = builds_[sha];
                    b.artifacts_known = true;
                    if (artifacts) b.artifacts = *artifacts;
                    if (sha_ == sha) update();
                });
        })
        .detach();
}

void forge_panel::ask_failure(size_t workflow, size_t job)
{
    if (!sha_) return;
    auto it = builds_.find(*sha_);
    if (it == builds_.end() || workflow >= it->second.ci.workflows.size() ||
        job >= it->second.ci.workflows[workflow].jobs.size())
        return;
    const build& b = it->second;
    const ci_job j = b.ci.workflows[workflow].jobs[job];
    failed_job_ = j;
    if (j.state == check_state::failure && j.id && !failures_asked_.contains(j.id))
    {
        failures_asked_.insert(j.id);
        with_token(b.remote,
                   [this, kind = b.remote.account.kind, repo = b.remote.repo,
                    j](std::string token) -> std::function<void()>
                   {
                       auto got = job_failure_details(kind, repo, j, token);
                       return [this, id = j.id, got = std::move(got)]
                       {
                           if (got)
                               failures_[id] = *got;
                           else
                               failure_errors_[id] = got.error();
                           update_failure();
                       };
                   });
    }
    update_failure();
}

void forge_panel::download_selected()
{
    if (downloading_ || !sha_) return;
    const auto sel = artifacts_->selection();
    if (sel.size() != 1 || sel.front() >= artifact_list_->rows.size()) return;
    const ci_artifact a = artifact_list_->rows[sel.front()];
    if (a.expired) return;
    auto it = builds_.find(*sha_);
    if (it == builds_.end()) return;
    const forge_remote remote = it->second.remote;
    rap::file_dialog_spec spec{.mode = rap::file_dialog_spec::mode::save,
                               .title = _("Save Artifact"),
                               // GitHub's are zips, unless uploaded as they are
                               // (a name with its own extension, then).
                               .initial_path =
                                   a.name.find('.') == std::string::npos ? a.name + ".zip" : a.name,
                               .filters = {{_("Zip archives"), "*.zip"}}};
    host_.ui.choose_files(
        host_.window ? host_.window() : nullptr, spec,
        [this, alive = std::weak_ptr<bool>(alive_), a, remote](std::vector<std::string> chosen)
        {
            if (!alive.lock() || chosen.empty()) return;
            const std::string to = chosen.front();
            downloading_ = true;
            download_->set_enabled(false);
            download_progress_->set_visible(true);
            download_status_->set_text(
                a.size ? rap::i18n::format(_("Downloading {0} ({1})..."), a.name, size_text(a.size))
                       : rap::i18n::format(_("Downloading {0}..."), a.name));
            // The bar fills as the bytes come, when the forge says how many;
            // else it says it's busy.
            download_progress_->set_fraction(0);
            if (!a.size)
                download_timer_ =
                    host_.ui.timer(100, true, [this] { download_progress_->pulse(); });
            download_cancel_ = std::make_shared<std::atomic<bool>>(false);
            auto progress = [this, alive = std::weak_ptr<bool>(alive_), &ui = host_.ui,
                             size = a.size, last = std::make_shared<int>(-1)](
                                std::uint64_t received, std::optional<std::uint64_t> total)
            {
                const std::uint64_t whole = total ? *total : std::uint64_t(size);
                if (!whole) return;
                const int percent = int(std::min<std::uint64_t>(100, received * 100 / whole));
                if (percent == *last) return;  // a post a percent, not a chunk
                *last = percent;
                ui.post(
                    [this, alive, percent]
                    {
                        if (alive.lock() && downloading_)
                            download_progress_->set_fraction(float(percent) / 100);
                    });
            };
            with_token(remote,
                       [this, a, to, kind = remote.account.kind, cancel = download_cancel_,
                        progress](std::string token) -> std::function<void()>
                       {
                           const std::string error =
                               download_artifact(kind, a, to, token, cancel.get(), progress);
                           return [this, error, to]
                           {
                               downloading_ = false;
                               download_timer_.reset();
                               download_progress_->set_visible(false);
                               download_status_->set_text(
                                   error.empty()
                                       ? rap::i18n::format(_("Saved {0}"), to)
                                       : rap::i18n::format(_("Couldn't download it: {0}"), error));
                               const auto sel = artifacts_->selection();
                               download_->set_enabled(sel.size() == 1);
                           };
                       });
        });
}

}  // namespace tortilla
