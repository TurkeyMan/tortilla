#include "log_model.h"

#include "git/status.h"
#include "i18n.h"
#include "log/ref_labels.h"
#include "util/calendar.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <thread>
#include <unordered_set>

namespace tortilla
{
namespace
{

using rap::color;
using rap::paint;
using rap::point_f;
using rap::rect_f;

constexpr size_t k_batch_size = 2000;
constexpr uint8_t k_unknown = 0xff;
constexpr uint8_t k_pending = 0xfe;  // asked for, not computed yet

// Lane colours: BranchLine1..8 [COL:41-48]. Lane 0 is black, which we take
// from the theme's text colour so it stays visible on dark themes.
// Lanes are coloured by column, as TortoiseGit does [COL:41-48], but from
// a softer palette than its primaries (GNOME's, which also reads on dark
// themes).
rap::paint lane_colour(uint16_t lane)
{
    static const color colours[] = {{0x35, 0x84, 0xe4}, {0xe6, 0x61, 0x00}, {0x26, 0xa2, 0x69},
                                    {0x91, 0x41, 0xac}, {0xe0, 0x1b, 0x24}, {0x21, 0x90, 0xa4},
                                    {0xc8, 0x88, 0x00}, {0x86, 0x5e, 0x3c}};
    return colours[lane % 8];
}

std::string lower(std::string s)
{
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

// TortoiseGit's simple search syntax [MAN-LOG:715-764]: space-separated
// terms that must all match, -term excludes, +term re-includes, "quoted
// phrases" literal ("" is a quote), a leading ! inverts everything.
struct parsed
{
    bool invert = false;
    std::vector<std::pair<std::string, bool>> terms;  // text, exclude
};

parsed parse_search(std::string_view s)
{
    parsed out;
    size_t i = 0;
    while (i < s.size() && s[i] == ' ') ++i;
    if (i < s.size() && s[i] == '!')
    {
        out.invert = true;
        ++i;
    }
    while (i < s.size())
    {
        while (i < s.size() && s[i] == ' ') ++i;
        if (i >= s.size()) break;
        bool exclude = false;
        if (s[i] == '-' || s[i] == '+')
        {
            exclude = s[i] == '-';
            ++i;
        }
        std::string term;
        if (i < s.size() && s[i] == '"')
        {
            ++i;
            while (i < s.size())
            {
                if (s[i] == '"')
                {
                    if (i + 1 < s.size() && s[i + 1] == '"')
                    {
                        term += '"';
                        i += 2;
                        continue;
                    }
                    ++i;
                    break;
                }
                term += s[i++];
            }
        }
        else
        {
            while (i < s.size() && s[i] != ' ') term += s[i++];
        }
        if (!term.empty()) out.terms.push_back({term, exclude});
    }
    return out;
}

std::string format_date(int64_t time, int offset_minutes)
{
    std::time_t t = time + int64_t(offset_minutes) * 60;
    std::tm tm = utc_calendar(t);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &tm);
    return buf;
}

}  // namespace

log_model::log_model(rap::ui& ui)
    : ui_(ui)
{
}

log_model::~log_model()
{
    cancel();
}

std::vector<rap::column> log_model::columns()
{
    using namespace rap::literals;
    // Graph | Actions | Message | Author | Date, the rest hidden [GLLB:321-336]
    std::vector<rap::column> c(column_count);
    c[graph] = {.title = _("Graph"), .width = 60_px, .painted = true};
    c[actions] = {.title = _("Actions"), .width = 90_px, .painted = true};
    c[message] = {.title = _("Message"), .width = 200_px, .stretch = true, .painted = true};
    c[author] = {.title = _("Author"), .width = 18_ch};
    c[date] = {.title = _("Date"), .width = 19_ch};
    c[hash] = {.title = _("SHA-1"), .width = 42_ch, .visible = false};
    c[email] = {.title = _("Email"), .width = 24_ch, .visible = false};
    c[commit_name] = {.title = _("Commit Name"), .width = 18_ch, .visible = false};
    c[commit_email] = {.title = _("Commit Email"), .width = 24_ch, .visible = false};
    c[commit_date] = {.title = _("Commit Date"), .width = 19_ch, .visible = false};
    return c;
}

struct log_model::actions_work
{
    std::mutex mutex;
    std::condition_variable wake;
    std::vector<std::pair<size_t, oid>> queue;  // tortilla::commit index, id
    bool stop = false;
    std::thread thread;
};

void log_model::cancel()
{
    ++*generation_;
    stop_actions();
    status_ = {};
    if (worker_.joinable())
    {
        worker_.request_stop();
        worker_.join();
    }
}

// The working tree last: on a big or slow tree it takes seconds and the
// history shouldn't wait for it. git's own status is much faster than
// libgit2's there (see git/status.h); both commands run at once, and
// dropping the handles (reload, close) kills them.
void log_model::start_status()
{
    if (!working_tree_ || !repo_) return;
    auto out = std::make_shared<std::pair<std::string, std::string>>();
    auto pending = std::make_shared<int>(2);
    auto finished = [this, out, pending](int)
    {
        if (--*pending) return;
        wt_files_ = parse_status(out->first, out->second);
        wt_known_ = true;
        notify_changed(0, 1);
        if (on_working_tree_) on_working_tree_();
        ui_.post([this] { status_ = {}; });
    };
    std::string dir = repo_->workdir();
    // A log of a path: its changes only, run from the top so the paths
    // come out repository-relative as usual.
    auto limited = [this](std::vector<std::string> args)
    {
        if (walk_paths_.empty()) return args;
        args.push_back("--");
        for (const std::string& p : walk_paths_) args.push_back(":(top)" + p);
        return args;
    };
    status_[0] = git_process::start(
        ui_, dir, limited(k_status_args), [out](const std::string& c) { out->first += c; },
        finished, git_output::stdout_only);
    status_[1] = git_process::start(
        ui_, dir, limited(k_numstat_args), [out](const std::string& c) { out->second += c; },
        finished, git_output::stdout_only);
}

void log_model::stop_actions()
{
    if (!actions_work_) return;
    {
        std::lock_guard lock(actions_work_->mutex);
        actions_work_->stop = true;
    }
    actions_work_->wake.notify_all();
    actions_work_->thread.join();
    actions_work_.reset();
}

void log_model::start_actions(unsigned gen)
{
    stop_actions();
    auto work = actions_work_ = std::make_shared<actions_work>();
    auto generation = generation_;
    work->thread = std::thread(
        [this, work, gen, generation, path = path_]
        {
            auto repo = repo::open(path);
            if (!repo) return;
            for (;;)
            {
                std::vector<std::pair<size_t, oid>> batch;
                {
                    std::unique_lock lock(work->mutex);
                    work->wake.wait(lock, [&] { return work->stop || !work->queue.empty(); });
                    if (work->stop) return;
                    // Latest requests first: they're the rows on screen now.
                    while (!work->queue.empty() && batch.size() < 16)
                    {
                        batch.push_back(work->queue.back());
                        work->queue.pop_back();
                    }
                }
                std::vector<std::pair<size_t, uint8_t>> results;
                for (auto& [index, id] : batch)
                {
                    if (work->stop) return;
                    results.push_back({index, repo->actions(id)});
                }
                ui_.post(
                    [this, results = std::move(results), gen, generation]
                    {
                        if (*generation != gen) return;
                        std::vector<size_t> rows;
                        for (auto [index, mask] : results)
                            if (index < actions_.size())
                            {
                                actions_[index] = mask;
                                actions_cache_[commits_[index].id] = mask;
                                if (auto row = row_of_index(index)) rows.push_back(*row);
                            }
                        // Just these rows: lists keep the others as drawn.
                        std::ranges::sort(rows);
                        for (size_t i = 0; i < rows.size();)
                        {
                            size_t j = i + 1;
                            while (j < rows.size() && rows[j] == rows[j - 1] + 1) ++j;
                            notify_changed(rows[i], j - i);
                            i = j;
                        }
                    });
            }
        });
}

void log_model::load(const std::string& path, const walk_options& opts,
                    std::function<void(const std::string&)> done)
{
    cancel();
    commits_.clear();
    rows_.clear();
    actions_.clear();
    visible_.clear();
    refs_.clear();
    label_boxes_.clear();
    wt_files_.clear();
    loading_ = true;
    wt_known_ = false;
    path_ = path;
    walk_refs_ = opts.refs;
    walk_rev_ = opts.rev;
    walk_paths_ = opts.paths;

    auto repo = repo::open(path);
    if (!repo)
    {
        working_tree_ = false;
        loading_ = false;
        notify_reset();
        done(repo.error());
        return;
    }
    repo_ = std::move(*repo);
    head_ = repo_->head_id();
    working_tree_ = !repo_->bare();
    notify_reset();

    unsigned gen = *generation_;
    auto generation = generation_;
    start_actions(gen);
    worker_ = std::jthread(
        [this, path, opts, done, gen, generation](std::stop_token stop)
        {
            // Everything the UI thread touches arrives through post(); the
            // generation check drops stale batches after a reload or cancel.
            auto deliver = [&](std::function<void()> f)
            {
                ui_.post(
                    [f = std::move(f), gen, generation]
                    {
                        if (*generation == gen) f();
                    });
            };

            auto repo = repo::open(path);
            if (!repo)
            {
                deliver([done, e = repo.error()] { done(e); });
                return;
            }

            // Labels: walking several refs, they're also where the walk starts,
            // so they come first; otherwise the history goes first and the
            // labels follow a moment later.
            auto deliver_refs = [&](ref_map refs)
            {
                deliver(
                    [this, refs = std::make_shared<ref_map>(std::move(refs))]() mutable
                    {
                        refs_ = std::move(*refs);
                        notify_changed(0, row_count());
                    });
            };
            walk_options walk_opts = opts;
            if (opts.refs != walk_refs::head)
            {
                ref_map refs = repo->refs();
                for (auto& [tip, labels] : refs)
                    if (std::ranges::any_of(labels, [&](const ref& r) { return walks_ref(opts.refs, r.full_name); }))
                        walk_opts.tips.push_back(tip);
                deliver_refs(std::move(refs));
            }

            lane_builder lanes;
            batch batch;
            auto flush = [&]
            {
                if (batch.commits.empty()) return;
                deliver([this, b = std::make_shared<log_model::batch>(std::move(batch))]
                        { append(std::move(*b)); });
                batch = {};
            };
            auto result = repo->walk(walk_opts,
                                     [&](tortilla::commit&& c)
                                     {
                                         batch.rows.push_back(lanes.next(c.id, c.parents));
                                         batch.commits.push_back(std::move(c));
                                         if (batch.commits.size() >= k_batch_size) flush();
                                         return !stop.stop_requested();
                                     });
            flush();
            if (opts.refs == walk_refs::head && !stop.stop_requested()) deliver_refs(repo->refs());
            deliver(
                [this, done, e = result ? std::string() : result.error()]
                {
                    loading_ = false;
                    start_status();
                    done(e);
                });
        });
}

void log_model::append(batch&& b)
{
    size_t first = row_count();
    size_t first_commit = commits_.size();
    std::ranges::move(b.commits, std::back_inserter(commits_));
    std::ranges::move(b.rows, std::back_inserter(rows_));
    actions_.resize(commits_.size(), k_unknown);
    if (filtered_)
        for (size_t i = first_commit; i < commits_.size(); ++i)
            if (matches(commits_[i])) visible_.push_back(i);
    if (row_count() > first) notify_inserted(first, row_count() - first);
}

size_t log_model::row_count() const
{
    return (filtered_ ? visible_.size() : commits_.size()) + (working_tree_ ? 1 : 0);
}

size_t log_model::index(size_t row) const
{
    size_t i = row - (working_tree_ ? 1 : 0);
    return filtered_ ? visible_[i] : i;
}

std::optional<size_t> log_model::row_of_index(size_t index) const
{
    const size_t wt = working_tree_ ? 1 : 0;
    if (!filtered_) return index < commits_.size() ? std::optional(index + wt) : std::nullopt;
    auto it = std::ranges::lower_bound(visible_, index);  // ascending, as the walk found them
    if (it == visible_.end() || *it != index) return std::nullopt;
    return size_t(it - visible_.begin()) + wt;
}

void log_model::refresh_refs(std::function<void(bool)> done)
{
    if (loading_ || !repo_) return done(true);
    auto generation = generation_;
    const unsigned gen = *generation_;
    // for-each-ref: tens of ms on a slow disk, so off the UI thread.
    std::thread(
        [this, path = path_, done = std::move(done), gen, generation]
        {
            auto repo = repo::open(path);
            auto refs = std::make_shared<std::optional<ref_map>>();
            if (repo) *refs = repo->refs();
            ui_.post(
                [this, refs, done, gen, generation]
                {
                    if (*generation != gen) return;  // reloaded or gone meanwhile
                    if (!*refs) return done(true);
                    done(apply_refs(std::move(**refs)));
                });
        })
        .detach();
}

bool log_model::apply_refs(ref_map next)
{
    auto names = [](const std::vector<ref>& refs)
    {
        std::vector<std::string> out;
        for (const ref& r : refs) out.push_back(r.full_name + (r.current ? "*" : ""));
        return out;
    };
    std::vector<oid> changed;
    for (auto& [id, refs] : refs_)
        if (auto it = next.find(id); it == next.end() || names(it->second) != names(refs))
            changed.push_back(id);
    for (auto& [id, refs] : next)
        if (!refs_.contains(id)) changed.push_back(id);
    if (changed.empty()) return false;

    std::unordered_map<oid, size_t, oid_hash> row_by_id;
    for (size_t row = 0; row < row_count(); ++row)
        if (const tortilla::commit* c = commit(row)) row_by_id.emplace(c->id, row);

    bool reload = false;
    if (walk_refs_ == walk_refs::head && !walk_rev_.empty())
    {
        // Started from a ref: reload when it moves.
        auto names_rev = [&](const ref_map& refs, const oid& id)
        {
            auto it = refs.find(id);
            return it != refs.end() && std::ranges::any_of(it->second, [&](const ref& r)
                                                           { return r.full_name == walk_rev_; });
        };
        for (const oid& id : changed)
            if (names_rev(refs_, id) != names_rev(next, id)) reload = true;
    }
    if (walk_refs_ != walk_refs::head)
    {
        std::unordered_set<oid, oid_hash> parents;  // commits with a child loaded
        for (const tortilla::commit& c : commits_) parents.insert(c.parents.begin(), c.parents.end());
        for (const oid& id : changed)
        {
            auto it = next.find(id);
            const bool labelled = it != next.end() && std::ranges::any_of(it->second, [&](const ref& r)
                                                                          { return walks_ref(walk_refs_, r.full_name); });
            bool loaded = false;
            for (const tortilla::commit& c : commits_)
                if (c.id == id)
                {
                    loaded = true;
                    break;
                }
            // A new tip the walk never reached, or a tip that lost its last
            // label with no child above it (so maybe nothing reaches it now).
            if ((labelled && !loaded) ||
                (!labelled && loaded && !parents.contains(id) && id != head_))
                reload = true;
        }
    }
    refs_ = std::move(next);
    for (const oid& id : changed)
        if (auto it = row_by_id.find(id); it != row_by_id.end()) notify_changed(it->second, 1);
    return reload;
}

std::optional<ref> log_model::label_at(size_t row, float x) const
{
    const tortilla::commit* c = commit(row);
    if (!c) return std::nullopt;
    auto it = label_boxes_.find(c->id);
    if (it == label_boxes_.end()) return std::nullopt;
    for (const label_box& b : it->second)
        if (x >= b.left && x < b.right) return b.ref;
    return std::nullopt;
}

const tortilla::commit* log_model::commit(size_t row) const
{
    if (is_working_tree(row)) return nullptr;
    return &commits_[index(row)];
}

std::optional<size_t> log_model::row_of(const oid& id) const
{
    for (size_t row = working_tree_ ? 1 : 0; row < row_count(); ++row)
        if (commits_[index(row)].id == id) return row;
    return std::nullopt;
}

const std::vector<ref>* log_model::refs_of(size_t row) const
{
    if (is_working_tree(row)) return nullptr;
    auto it = refs_.find(commits_[index(row)].id);
    return it == refs_.end() ? nullptr : &it->second;
}

bool log_model::is_head(size_t row) const
{
    return !is_working_tree(row) && head_ && commits_[index(row)].id == *head_;
}

void log_model::set_labels(unsigned mask)
{
    labels_ = mask;
    notify_changed(0, row_count());
}

bool log_model::show_label(const ref& r) const
{
    switch (r.kind)
    {
    case ref_kind::tag: return labels_ & label_tags;
    case ref_kind::local_branch: return labels_ & label_local;
    case ref_kind::remote_branch: return labels_ & label_remote;
    default: return labels_ & label_other;
    }
}

std::string log_model::set_filter(const log_filter& f)
{
    filter_ = f;
    regex_.reset();
    terms_.clear();
    invert_ = false;
    if (f.regex && !f.text.empty())
    {
        std::string pattern = f.text;
        if (pattern.starts_with('!'))
        {
            invert_ = true;
            pattern.erase(0, 1);
        }
        try
        {
            auto flags = std::regex::ECMAScript;
            if (!f.case_sensitive) flags |= std::regex::icase;
            regex_.emplace(pattern, flags);
        }
        catch (const std::regex_error& e)
        {
            return e.what();
        }
    }
    else
    {
        parsed p = parse_search(f.text);
        invert_ = p.invert;
        for (auto& [text, exclude] : p.terms)
            terms_.push_back({f.case_sensitive ? text : lower(text), exclude});
    }
    refilter();
    return {};
}

void log_model::refilter()
{
    filtered_ = !filter_.text.empty();
    visible_.clear();
    if (filtered_)
        for (size_t i = 0; i < commits_.size(); ++i)
            if (matches(commits_[i])) visible_.push_back(i);
    notify_reset();
}

bool log_model::matches(const tortilla::commit& c) const
{
    // The text of every selected field, newline-separated.
    std::string hay;
    unsigned f = filter_.fields;
    if (f & filter_subject) hay += c.summary + '\n';
    if (f & filter_messages) hay += c.message + '\n';
    if (f & filter_authors) hay += c.author + '\n' + c.committer + '\n';
    if (f & filter_emails) hay += c.email + '\n' + c.committer_email + '\n';
    if (f & filter_revs) hay += c.id.hex() + '\n';
    if (f & filter_refname)
        if (auto it = refs_.find(c.id); it != refs_.end())
            for (auto& r : it->second) hay += r.full_name + '\n';

    bool hit;
    if (regex_)
    {
        hit = std::regex_search(hay, *regex_);
    }
    else
    {
        if (!filter_.case_sensitive) hay = lower(hay);
        hit = true;
        for (auto& t : terms_)
            if ((hay.find(t.text) != std::string::npos) == t.exclude)
            {
                hit = false;
                break;
            }
    }
    return hit != invert_;
}

// TODO: compute on a worker and show the "fetching" icon meanwhile, as
// TortoiseGit does [GLLB:1356-1425]. A tree diff per visible row is fast
// enough for now.
uint8_t log_model::actions_for(size_t row) const
{
    if (is_working_tree(row))
    {
        if (!wt_known_) return k_pending;
        uint8_t mask = 0;
        for (auto& f : wt_files_)
            mask |= f.status == 'A' || f.status == '?' ? action_added
                    : f.status == 'D'                  ? action_deleted
                    : f.status == 'R'                  ? action_renamed
                                                       : action_modified;
        return mask;
    }
    size_t i = index(row);
    uint8_t& a = actions_[i];
    if (a == k_unknown)
        if (auto it = actions_cache_.find(commits_[i].id); it != actions_cache_.end()) a = it->second;
    if (a == k_unknown && actions_work_)
    {
        a = k_pending;
        std::lock_guard lock(actions_work_->mutex);
        auto& q = actions_work_->queue;
        q.push_back({i, commits_[i].id});
        // Rows scrolled past long ago aren't worth computing first.
        if (q.size() > 256)
        {
            for (auto it = q.begin(); it != q.begin() + 64; ++it) actions_[it->first] = k_unknown;
            q.erase(q.begin(), q.begin() + 64);
        }
        actions_work_->wake.notify_one();
    }
    return a;
}

std::string log_model::text(size_t row, size_t column) const
{
    if (is_working_tree(row)) return column == message ? _("Working tree changes") : "";
    const tortilla::commit& c = commits_[index(row)];
    switch (column)
    {
    case message: return c.summary;
    case author: return c.author;
    case date: return format_date(c.time, c.offset_minutes);
    case hash: return c.id.hex();
    case email: return c.email;
    case commit_name: return c.committer;
    case commit_email: return c.committer_email;
    case commit_date: return format_date(c.commit_time, c.commit_offset_minutes);
    }
    return {};
}

rap::text_style log_model::style(size_t row, size_t) const
{
    // TODO: draw filter matches in FilterMatch red (200,0,0) [GLLB:3938-3997];
    // needs per-span styles in list cells.
    rap::text_style s;
    s.bold = is_head(row);  // the HEAD commit is a bold row, not a label [GLLB:1097-1110]
    return s;
}

void log_model::paint(size_t row, size_t column, rap::painter& p,
                     const rap::cell_info& cell) const
{
    switch (column)
    {
    case graph:
        // No graph while a text filter hides commits [GLLB:1135-1154].
        if (!is_working_tree(row) && !filtered_) paint_graph(rows_[index(row)], p, cell);
        break;
    case actions: paint_actions(actions_for(row), p, cell); break;
    case message: paint_message(row, p, cell); break;
    }
}

// TortoiseGit's structure [GLLB:663-1048] (lanes by column, squares for
// merges and fork points) drawn in a lighter style: small nodes set off by
// a gap where their lanes stop short, and lanes that join or branch as
// smooth curves leaving and entering their lane vertically, so they meet
// the straight segments in the neighbouring rows without a kink.
void log_model::paint_graph(const graph_row& g, rap::painter& p, const rap::cell_info& cell) const
{
    // Tight lane pitch keeps busy histories (many parallel lanes) narrow.
    float h = cell.size.h, mid = h / 2;
    float lw = std::clamp(h * 0.6f, 9.f, 13.f), r = 3.5f, ring = 1.5f, line = 1.75f;
    // Lane centres on the pixel grid so vertical lines are crisp.
    rap::pixel_grid grid(p);
    line = grid.stroke_width(line);
    auto x = [&](uint16_t lane) { return grid.stroke(lw * lane + lw / 2 + 3, line); };

    // Joins and forks, TortoiseGit's way [GLLB:842-888]: a horizontal riser
    // through the node at mid-row, and each lane meeting the node turns
    // into it with a small rounded corner. A lane that both comes in from
    // above and goes out below (a merge whose branch continues in the same
    // column) runs straight through, joined to the node by the riser.
    std::vector<uint16_t> in, out, through;
    for (auto& s : g.top)
        if (s.from != s.to) in.push_back(s.from);
    for (auto& s : g.bottom)
        if (s.from != s.to) out.push_back(s.to);
    for (uint16_t lane : in)
        if (std::ranges::contains(out, lane)) through.push_back(lane);

    // Quarter-circle turns about half a row high, so branches visibly sweep
    // into and out of the riser.
    float corner = std::min(mid * 0.9f, lw * 0.85f);
    constexpr float pi = 3.14159265f;
    uint16_t lo = g.node, hi = g.node;
    for (auto* v : {&in, &out})
        for (uint16_t lane : *v)
        {
            lo = std::min(lo, lane);
            hi = std::max(hi, lane);
        }
    float cy = grid.stroke(mid, line);
    const float cx = x(g.node);

    // The node on the device grid at any scale: a whole number of device
    // pixels across, the parity of the lane's stroke, so dot, lane and riser
    // share a centre. The gap round it is the row's own background showing,
    // whatever the toolkit painted there (selected, hovered, alternate):
    // the lines meeting the node stop short rather than a ring being filled.
    auto snapped = [&](float diameter)
    {
        const int wl = int(std::round(line * grid.k));
        const float want = diameter * grid.k;
        int d = std::max(1, int(std::round(want)));
        if ((d - wl) % 2)
            d = std::abs(want - (d - 1)) < std::abs(want - (d + 1)) && d > 1 ? d - 1 : d + 1;
        return d / grid.k;
    };
    const bool square = g.merge || g.fork;
    // A square reads larger than a circle of the same radius, so it's a
    // little smaller, for the same visual weight.
    const float size = snapped(2 * (square ? r * 0.88f : r));
    const float gap = size / 2 + grid.size(ring);

    // Vertical lines run a pixel past the row's edges (the cell clips), so
    // neighbouring rows' antialiased ends overlap with no seam between them.
    const float top = -grid.px(), bottom = h + grid.px();

    // A corner arc as far as the node's gap: the arcs onto and off the riser
    // can end inside it, where the painted ring used to cover them. The arc
    // then stops where its circle crosses the gap's.
    auto arc = [&](rap::point_f c, float a0, float a1, const rap::paint& colour)
    {
        auto outside = [&](float a)
        {
            const float dx = c.x + corner * std::cos(a) - cx, dy = c.y + corner * std::sin(a) - cy;
            return dx * dx + dy * dy >= gap * gap;
        };
        const bool in0 = !outside(a0), in1 = !outside(a1);
        if (in0 && in1) return;
        if (in0 || in1)
        {
            // From the arc's centre: the node's direction, and either side
            // of it by the angle the law of cosines gives the crossings.
            const float dx = cx - c.x, dy = cy - c.y, d = std::hypot(dx, dy);
            const float cos_a = (corner * corner + d * d - gap * gap) / (2 * corner * d);
            const float toward = std::atan2(dy, dx), a = std::acos(std::clamp(cos_a, -1.f, 1.f));
            const float lo_a = std::min(a0, a1), hi_a = std::max(a0, a1);
            for (float cross : {toward - a, toward + a, toward - a + 2 * pi, toward + a - 2 * pi})
                if (cross >= lo_a && cross <= hi_a) (in0 ? a0 : a1) = cross;
        }
        p.arc(c, corner, std::min(a0, a1), std::max(a0, a1), colour, line);
    };

    if (lo != hi)
    {
        // The riser stops where a far lane's corner starts, or at a lane
        // that runs straight through, and short of the node either side.
        auto end = [&](uint16_t lane, float dir)
        { return std::ranges::contains(through, lane) ? x(lane) : x(lane) - dir * corner; };
        const rap::paint riser{rap::theme_color::text};
        // Only what lies outside the node's gap: a near corner can start
        // inside it.
        if (lo < g.node && end(lo, -1) < cx - gap)
            p.line({end(lo, -1), cy}, {cx - gap, cy}, riser, line);
        if (hi > g.node && end(hi, 1) > cx + gap)
            p.line({cx + gap, cy}, {end(hi, 1), cy}, riser, line);
    }

    for (auto& s : g.top)
    {
        rap::paint c = lane_colour(s.from);
        float xf = x(s.from);
        if (s.from == s.to || std::ranges::contains(through, s.from))
        {
            // Down to the node's gap (the node's lane), or straight through.
            p.line({xf, top}, {xf, s.from == g.node ? cy - gap : bottom}, c, line);
            continue;
        }
        // Down the lane, then a quarter circle onto the riser.
        float dir = s.to > s.from ? 1.f : -1.f;
        p.line({xf, top}, {xf, cy - corner}, c, line);
        if (dir > 0)
            arc({xf + corner, cy - corner}, pi / 2, pi, c);
        else
            arc({xf - corner, cy - corner}, 0, pi / 2, c);
    }
    for (auto& s : g.bottom)
    {
        if (std::ranges::contains(through, s.to)) continue;  // drawn with the top half
        rap::paint c = lane_colour(s.to);
        float xt = x(s.to);
        if (s.from == s.to)
        {
            p.line({xt, s.to == g.node ? cy + gap : mid}, {xt, bottom}, c, line);
            continue;
        }
        // Off the riser with a quarter circle, then down the lane.
        float dir = s.to > s.from ? 1.f : -1.f;
        if (dir > 0)
            arc({xt - corner, cy + corner}, -pi / 2, 0, c);
        else
            arc({xt + corner, cy + corner}, pi, 3 * pi / 2, c);
        p.line({xt, cy + corner}, {xt, bottom}, c, line);
    }

    const rap::paint node = lane_colour(g.node);
    if (square)
        p.rect({cx - size / 2, cy - size / 2, size, size}, 1, node, std::nullopt);
    else
        p.circle({cx, cy}, size / 2, node, std::nullopt);
}

// Five fixed 16 px slots: modified, added, deleted, renamed, conflicted
// [GLLB:1356-1425], drawn in the style chosen in Settings > General.
void log_model::paint_actions(uint8_t mask, rap::painter& p, const rap::cell_info& cell) const
{
    float s = std::min(13.f, cell.size.h - 3), oy = (cell.size.h - s) / 2;
    if (mask == k_pending)
    {
        // "Fetching": three small grey dots in the first slot.
        for (int i = 0; i < 3; ++i)
            p.circle({5 + 4 + i * 4.f, cell.size.h / 2}, 1.2f,
                     rap::paint{rap::theme_color::placeholder_text}, std::nullopt);
        return;
    }
    // On whole device pixels, so the icons' straight edges are crisp at
    // fractional scales.
    const rap::pixel_grid grid(p);
    s = grid.size(s);
    oy = grid.snap(oy);
    for (int n = 0; n < 5; ++n)
        if (mask & (1 << n))
            paint_action_icon(p, action_bits(1 << n), grid.snap(5 + n * 16 + (16 - s) / 2), oy, s,
                              icon_style_);
}

// Ref labels (see ref_labels.h), then a space, then the subject.
void log_model::paint_message(size_t row, rap::painter& p, const rap::cell_info& cell) const
{
    rap::text_style subject = style(row, message);
    if (is_working_tree(row))
    {
        const char* text = _("Working tree changes");
        rap::size_f sz = p.measure(text, subject);
        p.text({2, (cell.size.h - sz.h) / 2}, text, subject);
        return;
    }

    const tortilla::commit& c = commits_[index(row)];
    float x = 0, h = cell.size.h;

    if (auto it = refs_.find(c.id); it != refs_.end())
    {
        float end = paint_ref_labels(
            p, it->second, x, h, [this](const ref& r) { return show_label(r); }, &label_boxes_[c.id]);
        if (end > x) x = end + p.measure(" ", subject).w;
    }

    rap::size_f sz = p.measure(c.summary, subject);
    p.text({x + 2, (h - sz.h) / 2}, c.summary, subject);
}

std::vector<std::string> format_patch_args(const patch_range& range, const std::string& folder,
                                           bool signoff)
{
    std::vector<std::string> args{"format-patch", "-o", folder};
    if (signoff) args.push_back("--signoff");
    // One revision alone means "since" to format-patch, so a single commit
    // takes -1, and a range from a root commit takes --root, as there's no
    // parent for oldest^ to name.
    if (range.oldest == range.newest)
        args.insert(args.end(), {"-1", range.newest});
    else if (range.oldest_is_root)
        args.insert(args.end(), {"--root", range.newest});
    else
        args.push_back(range.oldest + "^.." + range.newest);
    return args;
}

std::vector<std::string> note_args(const std::string& hash, const std::string& text,
                                   const std::string& file)
{
    // git notes add refuses an empty note, so emptying one removes it; a
    // commit that never had one isn't an error then.
    if (std::ranges::all_of(text, [](unsigned char ch) { return std::isspace(ch) != 0; }))
        return {"notes", "remove", "--ignore-missing", hash};
    return {"notes", "add", "-f", "-F", file, hash};
}

}  // namespace tortilla
