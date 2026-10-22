#include "log_model.h"

#include "git/status.h"
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

using rap::Color;
using rap::Paint;
using rap::PointF;
using rap::RectF;

constexpr size_t kBatch = 2000;
constexpr uint8_t kUnknown = 0xff;
constexpr uint8_t kPending = 0xfe;  // asked for, not computed yet

// Lane colours: BranchLine1..8 [COL:41-48]. Lane 0 is black, which we take
// from the theme's text colour so it stays visible on dark themes.
// Lanes are coloured by column, as TortoiseGit does [COL:41-48], but from
// a softer palette than its primaries (GNOME's, which also reads on dark
// themes).
Paint laneColour(uint16_t lane)
{
    static const Color colours[] = {{0x35, 0x84, 0xe4}, {0xe6, 0x61, 0x00}, {0x26, 0xa2, 0x69},
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
struct Parsed
{
    bool invert = false;
    std::vector<std::pair<std::string, bool>> terms;  // text, exclude
};

Parsed parseSearch(std::string_view s)
{
    Parsed out;
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

std::string formatDate(int64_t time, int offsetMinutes)
{
    std::time_t t = time + int64_t(offsetMinutes) * 60;
    std::tm tm = utcCalendar(t);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &tm);
    return buf;
}

}  // namespace

LogModel::LogModel(rap::Ui& ui)
    : ui_(ui)
{
}

LogModel::~LogModel()
{
    cancel();
}

std::vector<rap::Column> LogModel::columns()
{
    using namespace rap::literals;
    // Graph | Actions | Message | Author | Date, the rest hidden [GLLB:321-336]
    std::vector<rap::Column> c(ColumnCount);
    c[Graph] = {.title = "Graph", .width = 60_px, .painted = true};
    c[Actions] = {.title = "Actions", .width = 90_px, .painted = true};
    c[Message] = {.title = "Message", .width = 200_px, .stretch = true, .painted = true};
    c[Author] = {.title = "Author", .width = 18_ch};
    c[Date] = {.title = "Date", .width = 19_ch};
    c[Hash] = {.title = "SHA-1", .width = 42_ch, .visible = false};
    c[Email] = {.title = "Email", .width = 24_ch, .visible = false};
    c[CommitName] = {.title = "Commit Name", .width = 18_ch, .visible = false};
    c[CommitEmail] = {.title = "Commit Email", .width = 24_ch, .visible = false};
    c[CommitDate] = {.title = "Commit Date", .width = 19_ch, .visible = false};
    return c;
}

struct LogModel::ActionsWork
{
    std::mutex mutex;
    std::condition_variable wake;
    std::vector<std::pair<size_t, Oid>> queue;  // commit index, id
    bool stop = false;
    std::thread thread;
};

void LogModel::cancel()
{
    ++*generation_;
    stopActions();
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
void LogModel::startStatus()
{
    if (!workingTree_ || !repo_) return;
    auto out = std::make_shared<std::pair<std::string, std::string>>();
    auto pending = std::make_shared<int>(2);
    auto finished = [this, out, pending](int)
    {
        if (--*pending) return;
        wtFiles_ = parseStatus(out->first, out->second);
        wtKnown_ = true;
        notifyChanged(0, 1);
        if (onWorkingTree_) onWorkingTree_();
        ui_.post([this] { status_ = {}; });
    };
    std::string dir = repo_->workdir();
    status_[0] = GitProcess::start(
        ui_, dir, kStatusArgs, [out](const std::string& c) { out->first += c; }, finished,
        GitOutput::Stdout);
    status_[1] = GitProcess::start(
        ui_, dir, kNumstatArgs, [out](const std::string& c) { out->second += c; }, finished,
        GitOutput::Stdout);
}

void LogModel::stopActions()
{
    if (!actionsWork_) return;
    {
        std::lock_guard lock(actionsWork_->mutex);
        actionsWork_->stop = true;
    }
    actionsWork_->wake.notify_all();
    actionsWork_->thread.join();
    actionsWork_.reset();
}

void LogModel::startActions(unsigned gen)
{
    stopActions();
    auto work = actionsWork_ = std::make_shared<ActionsWork>();
    auto generation = generation_;
    work->thread = std::thread(
        [this, work, gen, generation, path = path_]
        {
            auto repo = Repo::open(path);
            if (!repo) return;
            for (;;)
            {
                std::vector<std::pair<size_t, Oid>> batch;
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
                                actionsCache_[commits_[index].id] = mask;
                                if (auto row = rowOfIndex(index)) rows.push_back(*row);
                            }
                        // Just these rows: lists keep the others as drawn.
                        std::ranges::sort(rows);
                        for (size_t i = 0; i < rows.size();)
                        {
                            size_t j = i + 1;
                            while (j < rows.size() && rows[j] == rows[j - 1] + 1) ++j;
                            notifyChanged(rows[i], j - i);
                            i = j;
                        }
                    });
            }
        });
}

void LogModel::load(const std::string& path, const WalkOptions& opts,
                    std::function<void(const std::string&)> done)
{
    cancel();
    commits_.clear();
    rows_.clear();
    actions_.clear();
    visible_.clear();
    refs_.clear();
    labelBoxes_.clear();
    wtFiles_.clear();
    loading_ = true;
    wtKnown_ = false;
    path_ = path;
    allBranches_ = opts.allBranches;

    auto repo = Repo::open(path);
    if (!repo)
    {
        workingTree_ = false;
        loading_ = false;
        notifyReset();
        done(repo.error());
        return;
    }
    repo_ = std::move(*repo);
    head_ = repo_->headId();
    workingTree_ = !repo_->bare();
    notifyReset();

    unsigned gen = *generation_;
    auto generation = generation_;
    startActions(gen);
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

            auto repo = Repo::open(path);
            if (!repo)
            {
                deliver([done, e = repo.error()] { done(e); });
                return;
            }

            // Labels: with all branches they're also where the walk starts,
            // so they come first; otherwise the history goes first and the
            // labels follow a moment later.
            auto deliverRefs = [&](RefMap refs)
            {
                deliver(
                    [this, refs = std::make_shared<RefMap>(std::move(refs))]() mutable
                    {
                        refs_ = std::move(*refs);
                        notifyChanged(0, rowCount());
                    });
            };
            WalkOptions walkOpts = opts;
            if (opts.allBranches)
            {
                RefMap refs = repo->refs();
                for (auto& [tip, labels] : refs) walkOpts.tips.push_back(tip);
                deliverRefs(std::move(refs));
            }

            LaneBuilder lanes;
            Batch batch;
            auto flush = [&]
            {
                if (batch.commits.empty()) return;
                deliver([this, b = std::make_shared<Batch>(std::move(batch))]
                        { append(std::move(*b)); });
                batch = {};
            };
            auto result = repo->walk(walkOpts,
                                     [&](Commit&& c)
                                     {
                                         batch.rows.push_back(lanes.next(c.id, c.parents));
                                         batch.commits.push_back(std::move(c));
                                         if (batch.commits.size() >= kBatch) flush();
                                         return !stop.stop_requested();
                                     });
            flush();
            if (!opts.allBranches && !stop.stop_requested()) deliverRefs(repo->refs());
            deliver(
                [this, done, e = result ? std::string() : result.error()]
                {
                    loading_ = false;
                    startStatus();
                    done(e);
                });
        });
}

void LogModel::append(Batch&& b)
{
    size_t first = rowCount();
    size_t firstCommit = commits_.size();
    std::ranges::move(b.commits, std::back_inserter(commits_));
    std::ranges::move(b.rows, std::back_inserter(rows_));
    actions_.resize(commits_.size(), kUnknown);
    if (filtered_)
        for (size_t i = firstCommit; i < commits_.size(); ++i)
            if (matches(commits_[i])) visible_.push_back(i);
    if (rowCount() > first) notifyInserted(first, rowCount() - first);
}

size_t LogModel::rowCount() const
{
    return (filtered_ ? visible_.size() : commits_.size()) + (workingTree_ ? 1 : 0);
}

size_t LogModel::index(size_t row) const
{
    size_t i = row - (workingTree_ ? 1 : 0);
    return filtered_ ? visible_[i] : i;
}

std::optional<size_t> LogModel::rowOfIndex(size_t index) const
{
    const size_t wt = workingTree_ ? 1 : 0;
    if (!filtered_) return index < commits_.size() ? std::optional(index + wt) : std::nullopt;
    auto it = std::ranges::lower_bound(visible_, index);  // ascending, as the walk found them
    if (it == visible_.end() || *it != index) return std::nullopt;
    return size_t(it - visible_.begin()) + wt;
}

void LogModel::refreshRefs(std::function<void(bool)> done)
{
    if (loading_ || !repo_) return done(true);
    auto generation = generation_;
    const unsigned gen = *generation_;
    // for-each-ref: tens of ms on a slow disk, so off the UI thread.
    std::thread(
        [this, path = path_, done = std::move(done), gen, generation]
        {
            auto repo = Repo::open(path);
            auto refs = std::make_shared<std::optional<RefMap>>();
            if (repo) *refs = repo->refs();
            ui_.post(
                [this, refs, done, gen, generation]
                {
                    if (*generation != gen) return;  // reloaded or gone meanwhile
                    if (!*refs) return done(true);
                    done(applyRefs(std::move(**refs)));
                });
        })
        .detach();
}

bool LogModel::applyRefs(RefMap next)
{
    auto names = [](const std::vector<Ref>& refs)
    {
        std::vector<std::string> out;
        for (const Ref& r : refs) out.push_back(r.fullName + (r.current ? "*" : ""));
        return out;
    };
    std::vector<Oid> changed;
    for (auto& [id, refs] : refs_)
        if (auto it = next.find(id); it == next.end() || names(it->second) != names(refs))
            changed.push_back(id);
    for (auto& [id, refs] : next)
        if (!refs_.contains(id)) changed.push_back(id);
    if (changed.empty()) return false;

    std::unordered_map<Oid, size_t, OidHash> rowById;
    for (size_t row = 0; row < rowCount(); ++row)
        if (const Commit* c = commit(row)) rowById.emplace(c->id, row);

    bool reload = false;
    if (allBranches_)
    {
        std::unordered_set<Oid, OidHash> parents;  // commits with a child loaded
        for (const Commit& c : commits_) parents.insert(c.parents.begin(), c.parents.end());
        for (const Oid& id : changed)
        {
            const bool labelled = next.contains(id);
            bool loaded = false;
            for (const Commit& c : commits_)
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
    for (const Oid& id : changed)
        if (auto it = rowById.find(id); it != rowById.end()) notifyChanged(it->second, 1);
    return reload;
}

std::optional<Ref> LogModel::labelAt(size_t row, float x) const
{
    const Commit* c = commit(row);
    if (!c) return std::nullopt;
    auto it = labelBoxes_.find(c->id);
    if (it == labelBoxes_.end()) return std::nullopt;
    for (const LabelBox& b : it->second)
        if (x >= b.left && x < b.right) return b.ref;
    return std::nullopt;
}

const Commit* LogModel::commit(size_t row) const
{
    if (isWorkingTree(row)) return nullptr;
    return &commits_[index(row)];
}

std::optional<size_t> LogModel::rowOf(const Oid& id) const
{
    for (size_t row = workingTree_ ? 1 : 0; row < rowCount(); ++row)
        if (commits_[index(row)].id == id) return row;
    return std::nullopt;
}

const std::vector<Ref>* LogModel::refsOf(size_t row) const
{
    if (isWorkingTree(row)) return nullptr;
    auto it = refs_.find(commits_[index(row)].id);
    return it == refs_.end() ? nullptr : &it->second;
}

bool LogModel::isHead(size_t row) const
{
    return !isWorkingTree(row) && head_ && commits_[index(row)].id == *head_;
}

void LogModel::setLabels(unsigned mask)
{
    labels_ = mask;
    notifyChanged(0, rowCount());
}

bool LogModel::showLabel(const Ref& r) const
{
    switch (r.kind)
    {
    case RefKind::Tag: return labels_ & LabelTags;
    case RefKind::LocalBranch: return labels_ & LabelLocal;
    case RefKind::RemoteBranch: return labels_ & LabelRemote;
    default: return labels_ & LabelOther;
    }
}

std::string LogModel::setFilter(const LogFilter& f)
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
            if (!f.caseSensitive) flags |= std::regex::icase;
            regex_.emplace(pattern, flags);
        }
        catch (const std::regex_error& e)
        {
            return e.what();
        }
    }
    else
    {
        Parsed p = parseSearch(f.text);
        invert_ = p.invert;
        for (auto& [text, exclude] : p.terms)
            terms_.push_back({f.caseSensitive ? text : lower(text), exclude});
    }
    refilter();
    return {};
}

void LogModel::refilter()
{
    filtered_ = !filter_.text.empty();
    visible_.clear();
    if (filtered_)
        for (size_t i = 0; i < commits_.size(); ++i)
            if (matches(commits_[i])) visible_.push_back(i);
    notifyReset();
}

bool LogModel::matches(const Commit& c) const
{
    // The text of every selected field, newline-separated.
    std::string hay;
    unsigned f = filter_.fields;
    if (f & FilterSubject) hay += c.summary + '\n';
    if (f & FilterMessages) hay += c.message + '\n';
    if (f & FilterAuthors) hay += c.author + '\n' + c.committer + '\n';
    if (f & FilterEmails) hay += c.email + '\n' + c.committerEmail + '\n';
    if (f & FilterRevs) hay += c.id.hex() + '\n';
    if (f & FilterRefname)
        if (auto it = refs_.find(c.id); it != refs_.end())
            for (auto& r : it->second) hay += r.fullName + '\n';

    bool hit;
    if (regex_)
    {
        hit = std::regex_search(hay, *regex_);
    }
    else
    {
        if (!filter_.caseSensitive) hay = lower(hay);
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
uint8_t LogModel::actionsFor(size_t row) const
{
    if (isWorkingTree(row))
    {
        if (!wtKnown_) return kPending;
        uint8_t mask = 0;
        for (auto& f : wtFiles_)
            mask |= f.status == 'A' || f.status == '?' ? ActionAdded
                    : f.status == 'D'                  ? ActionDeleted
                    : f.status == 'R'                  ? ActionRenamed
                                                       : ActionModified;
        return mask;
    }
    size_t i = index(row);
    uint8_t& a = actions_[i];
    if (a == kUnknown)
        if (auto it = actionsCache_.find(commits_[i].id); it != actionsCache_.end()) a = it->second;
    if (a == kUnknown && actionsWork_)
    {
        a = kPending;
        std::lock_guard lock(actionsWork_->mutex);
        auto& q = actionsWork_->queue;
        q.push_back({i, commits_[i].id});
        // Rows scrolled past long ago aren't worth computing first.
        if (q.size() > 256)
        {
            for (auto it = q.begin(); it != q.begin() + 64; ++it) actions_[it->first] = kUnknown;
            q.erase(q.begin(), q.begin() + 64);
        }
        actionsWork_->wake.notify_one();
    }
    return a;
}

std::string LogModel::text(size_t row, size_t column) const
{
    if (isWorkingTree(row)) return column == Message ? "Working tree changes" : "";
    const Commit& c = commits_[index(row)];
    switch (column)
    {
    case Message: return c.summary;
    case Author: return c.author;
    case Date: return formatDate(c.time, c.offsetMinutes);
    case Hash: return c.id.hex();
    case Email: return c.email;
    case CommitName: return c.committer;
    case CommitEmail: return c.committerEmail;
    case CommitDate: return formatDate(c.commitTime, c.commitOffsetMinutes);
    }
    return {};
}

rap::TextStyle LogModel::style(size_t row, size_t) const
{
    // TODO: draw filter matches in FilterMatch red (200,0,0) [GLLB:3938-3997];
    // needs per-span styles in list cells.
    rap::TextStyle s;
    s.bold = isHead(row);  // the HEAD commit is a bold row, not a label [GLLB:1097-1110]
    return s;
}

void LogModel::paint(size_t row, size_t column, rap::Painter& p,
                     const rap::CellInfo& cell) const
{
    switch (column)
    {
    case Graph:
        // No graph while a text filter hides commits [GLLB:1135-1154].
        if (!isWorkingTree(row) && !filtered_) paintGraph(rows_[index(row)], p, cell);
        break;
    case Actions: paintActions(actionsFor(row), p, cell); break;
    case Message: paintMessage(row, p, cell); break;
    }
}

// TortoiseGit's structure [GLLB:663-1048] (lanes by column, squares for
// merges and fork points) drawn in a lighter style: small nodes ringed in
// the row's background colour, and lanes that join or branch as smooth
// curves leaving and entering their lane vertically, so they meet the
// straight segments in the neighbouring rows without a kink.
void LogModel::paintGraph(const GraphRow& g, rap::Painter& p, const rap::CellInfo& cell) const
{
    // Tight lane pitch keeps busy histories (many parallel lanes) narrow.
    float h = cell.size.h, mid = h / 2;
    float lw = std::clamp(h * 0.6f, 9.f, 13.f), r = 3.5f, ring = 1.5f, line = 1.75f;
    // Lane centres on the pixel grid so vertical lines are crisp.
    rap::PixelGrid grid(p);
    line = grid.strokeWidth(line);
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
    if (lo != hi)
    {
        // The riser stops where a far lane's corner starts, or at a lane
        // that runs straight through.
        auto end = [&](uint16_t lane, float dir)
        { return std::ranges::contains(through, lane) ? x(lane) : x(lane) - dir * corner; };
        float left = lo < g.node ? end(lo, -1) : x(g.node);
        float right = hi > g.node ? end(hi, 1) : x(g.node);
        p.line({left, cy}, {right, cy}, Paint{rap::ThemeColor::Text}, line);
    }

    for (auto& s : g.top)
    {
        Paint c = laneColour(s.from);
        float xf = x(s.from);
        if (s.from == s.to || std::ranges::contains(through, s.from))
        {
            // Straight down to mid (the node's lane, or a pass-through).
            p.line({xf, 0}, {xf, s.from == g.node ? cy : h}, c, line);
            continue;
        }
        // Down the lane, then a quarter circle onto the riser.
        float dir = s.to > s.from ? 1.f : -1.f;
        p.line({xf, 0}, {xf, cy - corner}, c, line);
        if (dir > 0)
            p.arc({xf + corner, cy - corner}, corner, pi / 2, pi, c, line);
        else
            p.arc({xf - corner, cy - corner}, corner, 0, pi / 2, c, line);
    }
    for (auto& s : g.bottom)
    {
        if (std::ranges::contains(through, s.to)) continue;  // drawn with the top half
        Paint c = laneColour(s.to);
        float xt = x(s.to);
        if (s.from == s.to)
        {
            p.line({xt, s.to == g.node ? cy : mid}, {xt, h}, c, line);
            continue;
        }
        // Off the riser with a quarter circle, then down the lane.
        float dir = s.to > s.from ? 1.f : -1.f;
        if (dir > 0)
            p.arc({xt - corner, cy + corner}, corner, -pi / 2, 0, c, line);
        else
            p.arc({xt + corner, cy + corner}, corner, pi, 3 * pi / 2, c, line);
        p.line({xt, cy + corner}, {xt, h}, c, line);
    }

    Paint node = laneColour(g.node);
    Paint background = cell.selected ? rap::ThemeColor::Highlight : rap::ThemeColor::Base;
    float cx = x(g.node);
    if (g.merge || g.fork)
    {
        // Same ring and visual weight as the dots: a square reads larger
        // than a circle of the same radius, so it's drawn a little smaller.
        float half = r * 0.88f, o = half + ring;
        p.rect({cx - o, mid - o, 2 * o, 2 * o}, 1.5f, background, std::nullopt);
        p.rect({cx - half, mid - half, 2 * half, 2 * half}, 1, node, std::nullopt);
    }
    else
    {
        p.circle({cx, mid}, r + ring, background, std::nullopt);
        p.circle({cx, mid}, r, node, std::nullopt);
    }
}

// Five fixed 16 px slots: modified, added, deleted, renamed, conflicted
// [GLLB:1356-1425], drawn in the style chosen under View > Action Icons.
void LogModel::paintActions(uint8_t mask, rap::Painter& p, const rap::CellInfo& cell) const
{
    float s = std::min(13.f, cell.size.h - 3), oy = (cell.size.h - s) / 2;
    if (mask == kPending)
    {
        // "Fetching": three small grey dots in the first slot.
        for (int i = 0; i < 3; ++i)
            p.circle({5 + 4 + i * 4.f, cell.size.h / 2}, 1.2f,
                     rap::Paint{rap::ThemeColor::PlaceholderText}, std::nullopt);
        return;
    }
    for (int n = 0; n < 5; ++n)
        if (mask & (1 << n))
            paintActionIcon(p, Action(1 << n), 5 + n * 16 + (16 - s) / 2, oy, s, iconStyle_);
}

// Ref labels (see ref_labels.h), then a space, then the subject.
void LogModel::paintMessage(size_t row, rap::Painter& p, const rap::CellInfo& cell) const
{
    rap::TextStyle subject = style(row, Message);
    if (isWorkingTree(row))
    {
        rap::SizeF sz = p.measure("Working tree changes", subject);
        p.text({2, (cell.size.h - sz.h) / 2}, "Working tree changes", subject);
        return;
    }

    const Commit& c = commits_[index(row)];
    float x = 0, h = cell.size.h;

    if (auto it = refs_.find(c.id); it != refs_.end())
    {
        float end = paintRefLabels(
            p, it->second, x, h, [this](const Ref& r) { return showLabel(r); }, &labelBoxes_[c.id]);
        if (end > x) x = end + p.measure(" ", subject).w;
    }

    rap::SizeF sz = p.measure(c.summary, subject);
    p.text({x + 2, (h - sz.h) / 2}, c.summary, subject);
}

}  // namespace tortilla
