#include "rebase_model.h"

#include "i18n.h"
#include "log/ref_labels.h"
#include "util/calendar.h"

#include <algorithm>
#include <ctime>

namespace tortilla
{
namespace
{

std::string format_date(int64_t time, int offset_minutes)
{
    std::time_t t = time + int64_t(offset_minutes) * 60;
    std::tm tm = utc_calendar(t);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &tm);
    return buf;
}

}  // namespace

const char* action_name(rebase_action a)
{
    switch (a)
    {
    case rebase_action::pick: return _("Pick");
    case rebase_action::squash: return _("Squash");
    case rebase_action::edit: return _("Edit");
    case rebase_action::skip: return _("Skip");
    }
    return "";
}

// REBASE | ID | SHA-1 | Message | Author | Date [spec 2.3]
std::vector<rap::column> rebase_model::columns()
{
    using namespace rap::literals;
    std::vector<rap::column> c(column_count);
    c[action] = {.title = _("REBASE"), .width = 9_ch};
    c[id] = {.title = _("ID"), .width = 5_ch, .align = rap::align::end};
    c[hash] = {.title = _("SHA-1"), .width = 42_ch};
    c[message] = {.title = _("Message"), .stretch = true, .painted = true};
    c[author] = {.title = _("Author"), .width = 18_ch};
    c[date] = {.title = _("Date"), .width = 17_ch};
    return c;
}

void rebase_model::set(std::vector<rebase_row> rows, ref_map refs)
{
    rows_ = std::move(rows);
    refs_ = std::move(refs);
    notify_reset();
}

void rebase_model::set_refs(ref_map refs)
{
    refs_ = std::move(refs);
    if (!rows_.empty()) notify_changed(0, rows_.size());
}

size_t rebase_model::add(size_t at, const std::vector<commit>& commits)
{
    std::vector<rebase_row> added;
    for (const commit& c : commits)
        if (std::ranges::none_of(rows_, [&](const rebase_row& r) { return r.commit.id == c.id; }) &&
            std::ranges::none_of(added, [&](const rebase_row& r) { return r.commit.id == c.id; }))
            added.push_back({.commit = c});
    at = std::min(at, rows_.size());
    rows_.insert(rows_.begin() + ptrdiff_t(at), added.begin(), added.end());
    if (!added.empty()) notify_inserted(at, added.size());
    return added.size();
}

bool rebase_model::can_set(size_t row, rebase_action a) const
{
    if (row >= rows_.size()) return false;
    const rebase_row& r = rows_[row];
    if (r.done || r.current) return false;
    if (a == rebase_action::squash) return row + 1 < rows_.size() && r.commit.parents.size() <= 1;
    return true;
}

bool rebase_model::set_action(size_t row, rebase_action a)
{
    if (!can_set(row, a)) return false;
    rows_[row].action = a;
    notify_changed(row, 1);
    return true;
}

void rebase_model::cycle(const std::vector<size_t>& rows)
{
    for (size_t row : rows)
    {
        if (row >= rows_.size()) continue;
        rebase_action next = rows_[row].action;
        // Pick -> Skip -> Edit -> Squash -> Pick [GLLA:1343-1371]
        for (int i = 0; i < 4; ++i)
        {
            switch (next)
            {
            case rebase_action::pick: next = rebase_action::skip; break;
            case rebase_action::skip: next = rebase_action::edit; break;
            case rebase_action::edit: next = rebase_action::squash; break;
            case rebase_action::squash: next = rebase_action::pick; break;
            }
            if (can_set(row, next)) break;
        }
        set_action(row, next);
    }
}

void rebase_model::set_all(rebase_action a)
{
    for (size_t i = 0; i < rows_.size(); ++i)
        if (can_set(i, a)) rows_[i].action = a;
    notify_changed(0, rows_.size());
}

void rebase_model::set_unselected(const std::vector<size_t>& selected, rebase_action a)
{
    for (size_t i = 0; i < rows_.size(); ++i)
        if (!std::ranges::contains(selected, i) && can_set(i, a)) rows_[i].action = a;
    notify_changed(0, rows_.size());
}

std::vector<size_t> rebase_model::move(const std::vector<size_t>& selected, bool up, bool to_end)
{
    if (selected.empty()) return selected;
    if (up ? selected.front() == 0 : selected.back() + 1 >= rows_.size()) return selected;

    std::vector<size_t> out;
    if (to_end)
    {
        // Gather the selection at the top or bottom, keeping its order.
        std::vector<rebase_row> picked, rest;
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
    if (!rows_.empty() && rows_.back().action == rebase_action::squash)
        rows_.back().action = rebase_action::pick;
    notify_changed(0, rows_.size());
    return out;
}

std::vector<size_t> rebase_model::move_to(const std::vector<size_t>& selected, size_t destination)
{
    if (selected.empty()) return selected;
    std::vector<rebase_row> picked, rest;
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
    if (!rows_.empty() && rows_.back().action == rebase_action::squash)
        rows_.back().action = rebase_action::pick;
    notify_changed(0, rows_.size());

    std::vector<size_t> out;
    for (size_t i = 0; i < picked.size(); ++i) out.push_back(at + i);
    return out;
}

void rebase_model::set_state(size_t row, bool done, bool current)
{
    rows_[row].done = done;
    rows_[row].current = current;
    notify_changed(row, 1);
}

std::string rebase_model::text(size_t row, size_t column) const
{
    const rebase_row& r = rows_[row];
    switch (column)
    {
    case action: return action_name(r.action);
    case id: return std::to_string(rows_.size() - row);  // the bottom row is 1
    case hash: return r.commit.id.hex();
    case message: return r.commit.summary;
    case author: return r.commit.author;
    case date: return format_date(r.commit.time, r.commit.offset_minutes);
    }
    return {};
}

// Squash rows grey, edit rows khaki, skipped and done rows grey text, the
// row being applied bold [spec 2.4].
rap::text_style rebase_model::style(size_t row, size_t) const
{
    const rebase_row& r = rows_[row];
    rap::text_style s;
    if (r.action == rebase_action::squash) s.bg = rap::color{156, 156, 156};
    if (r.action == rebase_action::edit) s.bg = rap::color{200, 200, 128};
    if (r.action == rebase_action::skip || r.done) s.fg = rap::color{128, 128, 128};
    s.bold = r.current;
    return s;
}

// The subject, after the ref labels (not drawn on done rows).
void rebase_model::paint(size_t row, size_t, rap::painter& p, const rap::cell_info& cell) const
{
    const rebase_row& r = rows_[row];
    rap::text_style ts = style(row, message);
    ts.bg.reset();
    float x = 2, h = cell.size.h;
    // The same labels as the Log, but not on rows already applied [GLLB:1170].
    if (!r.done)
        if (auto it = refs_.find(r.commit.id); it != refs_.end())
            if (float end = paint_ref_labels(p, it->second, x, h); end > x)
                x = end + p.measure(" ", ts).w;
    rap::size_f sz = p.measure(r.commit.summary, ts);
    p.text({x, (h - sz.h) / 2}, r.commit.summary, ts);
}

}  // namespace tortilla
