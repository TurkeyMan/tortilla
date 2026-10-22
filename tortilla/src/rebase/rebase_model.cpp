#include "rebase_model.h"

#include "log/ref_labels.h"
#include "util/calendar.h"

#include <algorithm>
#include <ctime>

namespace tortilla
{
namespace
{

std::string formatDate(int64_t time, int offsetMinutes)
{
    std::time_t t = time + int64_t(offsetMinutes) * 60;
    std::tm tm = utcCalendar(t);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &tm);
    return buf;
}

}  // namespace

const char* actionName(RebaseAction a)
{
    switch (a)
    {
    case RebaseAction::Pick: return "Pick";
    case RebaseAction::Squash: return "Squash";
    case RebaseAction::Edit: return "Edit";
    case RebaseAction::Skip: return "Skip";
    }
    return "";
}

// REBASE | ID | SHA-1 | Message | Author | Date [spec 2.3]
std::vector<rap::Column> RebaseModel::columns()
{
    using namespace rap::literals;
    std::vector<rap::Column> c(ColumnCount);
    c[Action] = {.title = "REBASE", .width = 9_ch};
    c[Id] = {.title = "ID", .width = 5_ch, .align = rap::Align::End};
    c[Hash] = {.title = "SHA-1", .width = 42_ch};
    c[Message] = {.title = "Message", .stretch = true, .painted = true};
    c[Author] = {.title = "Author", .width = 18_ch};
    c[Date] = {.title = "Date", .width = 17_ch};
    return c;
}

void RebaseModel::set(std::vector<RebaseRow> rows, RefMap refs)
{
    rows_ = std::move(rows);
    refs_ = std::move(refs);
    notifyReset();
}

bool RebaseModel::canSet(size_t row, RebaseAction a) const
{
    if (row >= rows_.size()) return false;
    const RebaseRow& r = rows_[row];
    if (r.done || r.current) return false;
    if (a == RebaseAction::Squash) return row + 1 < rows_.size() && r.commit.parents.size() <= 1;
    return true;
}

bool RebaseModel::setAction(size_t row, RebaseAction a)
{
    if (!canSet(row, a)) return false;
    rows_[row].action = a;
    notifyChanged(row, 1);
    return true;
}

void RebaseModel::cycle(const std::vector<size_t>& rows)
{
    for (size_t row : rows)
    {
        if (row >= rows_.size()) continue;
        RebaseAction next = rows_[row].action;
        // Pick -> Skip -> Edit -> Squash -> Pick [GLLA:1343-1371]
        for (int i = 0; i < 4; ++i)
        {
            switch (next)
            {
            case RebaseAction::Pick: next = RebaseAction::Skip; break;
            case RebaseAction::Skip: next = RebaseAction::Edit; break;
            case RebaseAction::Edit: next = RebaseAction::Squash; break;
            case RebaseAction::Squash: next = RebaseAction::Pick; break;
            }
            if (canSet(row, next)) break;
        }
        setAction(row, next);
    }
}

void RebaseModel::setAll(RebaseAction a)
{
    for (size_t i = 0; i < rows_.size(); ++i)
        if (canSet(i, a)) rows_[i].action = a;
    notifyChanged(0, rows_.size());
}

void RebaseModel::setUnselected(const std::vector<size_t>& selected, RebaseAction a)
{
    for (size_t i = 0; i < rows_.size(); ++i)
        if (!std::ranges::contains(selected, i) && canSet(i, a)) rows_[i].action = a;
    notifyChanged(0, rows_.size());
}

std::vector<size_t> RebaseModel::move(const std::vector<size_t>& selected, bool up, bool toEnd)
{
    if (selected.empty()) return selected;
    if (up ? selected.front() == 0 : selected.back() + 1 >= rows_.size()) return selected;

    std::vector<size_t> out;
    if (toEnd)
    {
        // Gather the selection at the top or bottom, keeping its order.
        std::vector<RebaseRow> picked, rest;
        for (size_t i = 0; i < rows_.size(); ++i)
            (std::ranges::contains(selected, i) ? picked : rest).push_back(std::move(rows_[i]));
        size_t at = up ? 0 : rest.size();
        rest.insert(rest.begin() + at, std::make_move_iterator(picked.begin()),
                    std::make_move_iterator(picked.end()));
        rows_ = std::move(rest);
        for (size_t i = 0; i < picked.size(); ++i) out.push_back(at + i);
    }
    else if (up)
    {
        for (size_t i : selected)
        {
            std::swap(rows_[i - 1], rows_[i]);
            out.push_back(i - 1);
        }
    }
    else
    {
        for (auto it = selected.rbegin(); it != selected.rend(); ++it)
        {
            std::swap(rows_[*it], rows_[*it + 1]);
            out.insert(out.begin(), *it + 1);
        }
    }

    // A squash that ended up on the bottom row has nothing below it.
    if (!rows_.empty() && rows_.back().action == RebaseAction::Squash)
        rows_.back().action = RebaseAction::Pick;
    notifyChanged(0, rows_.size());
    return out;
}

std::vector<size_t> RebaseModel::moveTo(const std::vector<size_t>& selected, size_t destination)
{
    if (selected.empty()) return selected;
    std::vector<RebaseRow> picked, rest;
    size_t before = 0;  // selected rows ahead of the destination
    for (size_t i = 0; i < rows_.size(); ++i)
    {
        bool sel = std::ranges::contains(selected, i);
        if (sel && i < destination) ++before;
        (sel ? picked : rest).push_back(std::move(rows_[i]));
    }
    size_t at = std::min(destination - before, rest.size());
    rest.insert(rest.begin() + at, std::make_move_iterator(picked.begin()),
                std::make_move_iterator(picked.end()));
    rows_ = std::move(rest);
    if (!rows_.empty() && rows_.back().action == RebaseAction::Squash)
        rows_.back().action = RebaseAction::Pick;
    notifyChanged(0, rows_.size());

    std::vector<size_t> out;
    for (size_t i = 0; i < picked.size(); ++i) out.push_back(at + i);
    return out;
}

void RebaseModel::setState(size_t row, bool done, bool current)
{
    rows_[row].done = done;
    rows_[row].current = current;
    notifyChanged(row, 1);
}

std::string RebaseModel::text(size_t row, size_t column) const
{
    const RebaseRow& r = rows_[row];
    switch (column)
    {
    case Action: return actionName(r.action);
    case Id: return std::to_string(rows_.size() - row);  // the bottom row is 1
    case Hash: return r.commit.id.hex();
    case Message: return r.commit.summary;
    case Author: return r.commit.author;
    case Date: return formatDate(r.commit.time, r.commit.offsetMinutes);
    }
    return {};
}

// Squash rows grey, edit rows khaki, skipped and done rows grey text, the
// row being applied bold [spec 2.4].
rap::TextStyle RebaseModel::style(size_t row, size_t) const
{
    const RebaseRow& r = rows_[row];
    rap::TextStyle s;
    if (r.action == RebaseAction::Squash) s.bg = rap::Color{156, 156, 156};
    if (r.action == RebaseAction::Edit) s.bg = rap::Color{200, 200, 128};
    if (r.action == RebaseAction::Skip || r.done) s.fg = rap::Color{128, 128, 128};
    s.bold = r.current;
    return s;
}

// The subject, after the ref labels (not drawn on done rows).
void RebaseModel::paint(size_t row, size_t, rap::Painter& p, const rap::CellInfo& cell) const
{
    const RebaseRow& r = rows_[row];
    rap::TextStyle ts = style(row, Message);
    ts.bg.reset();
    float x = 2, h = cell.size.h;
    // The same labels as the Log, but not on rows already applied [GLLB:1170].
    if (!r.done)
        if (auto it = refs_.find(r.commit.id); it != refs_.end())
            if (float end = paintRefLabels(p, it->second, x, h); end > x)
                x = end + p.measure(" ", ts).w;
    rap::SizeF sz = p.measure(r.commit.summary, ts);
    p.text({x, (h - sz.h) / 2}, r.commit.summary, ts);
}

}  // namespace tortilla
