#include "blame_window.h"

#include "changes/changes_view.h"
#include "git/command.h"
#include "git/repo.h"
#include "i18n.h"
#include "log/log_window.h"
#include "settings.h"
#include "ui/compare.h"
#include "ui/settings_dialog.h"
#include "ui/windows.h"
#include "util/calendar.h"

#include <rapport/sys.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <ctime>
#include <filesystem>

namespace tortilla
{
namespace
{

namespace fs = std::filesystem;

enum column : size_t
{
    info,  // painted: the commit's hash, author and date, shaded by age
    number,
    code,
    column_count,
};

std::string date_text(int64_t t)
{
    if (!t) return {};
    std::tm tm = local_calendar(std::time_t(t));
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d", &tm);
    return buf;
}

// Tabs to the next multiple of four, as most editors show code.
std::string expand_tabs(const std::string& s)
{
    std::string out;
    size_t column = 0;
    for (char c : s)
    {
        if (c == '\t')
        {
            const size_t n = 4 - column % 4;
            out.append(n, ' ');
            column += n;
            continue;
        }
        out += c;
        // A UTF-8 continuation byte doesn't start a column.
        if ((static_cast<unsigned char>(c) & 0xc0) != 0x80) ++column;
    }
    return out;
}

rap::color mix(rap::color a, rap::color b, float f)
{
    auto m = [f](uint8_t x, uint8_t y) { return uint8_t(std::lround(x + (y - x) * f)); };
    return {m(a.r, b.r), m(a.g, b.g), m(a.b, b.b), 255};
}

std::string lower(std::string s)
{
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

}  // namespace

class blame_window::rows : public rap::list_model
{
public:
    using rap::list_model::notify_changed;
    using rap::list_model::notify_reset;

    std::vector<blame_line> lines;
    std::vector<std::string> shown;  // each line's text, tabs expanded
    std::vector<bool> block_start;   // the first of a run of lines from one commit
    int64_t oldest = 0, newest = 0;
    const blame_commit* selected = nullptr;  // its lines are highlighted

    void set(std::vector<blame_line> l)
    {
        lines = std::move(l);
        shown.clear();
        block_start.clear();
        oldest = newest = 0;
        for (size_t i = 0; i < lines.size(); ++i)
        {
            shown.push_back(expand_tabs(lines[i].text));
            block_start.push_back(i == 0 || lines[i].commit != lines[i - 1].commit);
            const blame_commit& c = *lines[i].commit;
            if (c.uncommitted()) continue;
            if (!oldest || c.author_time < oldest) oldest = c.author_time;
            if (c.author_time > newest) newest = c.author_time;
        }
        notify_reset();
    }

    size_t row_count() const override { return lines.size(); }
    std::string text(size_t row, size_t col) const override
    {
        if (row >= lines.size()) return {};
        const blame_commit& c = *lines[row].commit;
        switch (col)
        {
        case info:
            return c.uncommitted()
                       ? std::string(_("Not committed yet"))
                       : c.hash.substr(0, 8) + "  " + c.author + "  " + date_text(c.author_time);
        case number: return std::to_string(row + 1);
        case code: return shown[row];
        default: return {};
        }
    }
    rap::text_style style(size_t row, size_t col) const override
    {
        rap::text_style s;
        s.monospace = col == code || col == number;
        if (col == number) s.fg = rap::theme_color::placeholder_text;
        return s;
    }
    // The commit, on the first of its lines; every line shaded from the
    // theme's base towards its accent, more the newer the commit.
    void paint(size_t row, size_t col, rap::painter& p, const rap::cell_info& cell) const override
    {
        if (col != info || row >= lines.size()) return;
        const blame_commit& c = *lines[row].commit;
        const bool mine = selected && &c == selected;
        if (!cell.selected)
        {
            float age = 1;
            if (!c.uncommitted() && newest > oldest)
                age = float(c.author_time - oldest) / float(newest - oldest);
            const float f = (mine ? 0.45f : 0.08f) + 0.27f * age;
            const rap::color bg =
                mix(p.resolve(rap::theme_color::base), p.resolve(rap::theme_color::accent), f);
            p.rect({0, 0, cell.size.w, cell.size.h}, 0, rap::paint{bg}, std::nullopt);
        }
        if (!block_start[row]) return;
        rap::text_style s;
        s.fg = cell.selected ? rap::theme_color::highlight_text : rap::theme_color::text;
        const std::string t = text(row, info);
        const rap::size_f sz = p.measure(t, s);
        p.push_clip({0, 0, cell.size.w, cell.size.h});
        p.text({4, (cell.size.h - sz.h) / 2}, t, s);
        p.pop_clip();
    }
};

std::expected<std::unique_ptr<blame_window>, std::string>
blame_window::open(rap::ui& ui, const std::string& path, const std::string& revision)
{
    auto repo = repo::open(path);
    if (!repo) return std::unexpected(repo.error());
    if (repo->bare()) return std::unexpected(std::string(_("This needs a working tree.")));
    std::error_code ec;
    const fs::path abs = fs::absolute(rap::sys::path_from_utf8(path), ec);
    const fs::path rel = fs::relative(abs, rap::sys::path_from_utf8(repo->workdir()), ec);
    if (ec || rel.empty() || rel == "." || *rel.begin() == ".." || fs::is_directory(abs, ec))
        return std::unexpected(std::string(_("Blame needs a file in a repository.")));
    std::string r = rap::sys::path_to_utf8(rel);
    std::ranges::replace(r, '\\', '/');
    std::unique_ptr<blame_window> w(new blame_window(ui, repo->workdir(), std::move(r), revision));
    w->build();
    return w;
}

blame_window::blame_window(rap::ui& ui, std::string workdir, std::string path, std::string revision)
    : ui_(ui)
    , workdir_(std::move(workdir))
    , path_(std::move(path))
    , revision_(std::move(revision))
    , rows_(std::make_unique<rows>())
{
}

blame_window::~blame_window() = default;

void blame_window::build()
{
    using namespace rap::layout;
    using namespace rap::literals;
    std::vector<rap::column> columns(column_count);
    columns[info] = {.title = _("Commit"), .width = 40_ch, .painted = true};
    columns[number] = {.title = _("Line"), .width = 7_ch, .align = rap::align::end};
    columns[code] = {.title = path_, .width = 80_ch, .stretch = true};
    std::string where = workdir_;
    while (where.size() > 1 && (where.back() == '/' || where.back() == '\\')) where.pop_back();
    const std::string title = rap::sys::path_to_utf8(
        (rap::sys::path_from_utf8(where) / rap::sys::path_from_utf8(path_)).make_preferred());
    settings& st = settings::get();

    window_ = ui_.create(window{
        rap::i18n::format(_("{0} - Blame - Tortilla"), title),
        {vbox{
            // The file's history: older to the left [the slider is Tortilla's].
            hbox{
                button{_("_Older")}
                    .bind(older_)
                    .tooltip(_("The revision before"))
                    .on_click(
                        [this]
                        {
                            if (current_ + 1 < stops_.size()) go_to(current_ + 1);
                        }),
                slider{0, 1, 1}
                    .step(1, 1)
                    .bind(slider_)
                    .on_changed(
                        [this](double v)
                        {
                            // Where it's going said at once; blamed when it stops a moment.
                            const size_t n = stops_.size();
                            if (n < 2) return;
                            const size_t to =
                                n - 1 - size_t(std::clamp(std::lround(v), 0L, long(n - 1)));
                            revision_label_->set_text(stops_[to].label);
                            slide_timer_ = ui_.timer(250, false, [this, to] { go_to(to); });
                        })
                    .stretch(),
                button{_("_Newer")}
                    .bind(newer_)
                    .tooltip(_("The revision after"))
                    .on_click(
                        [this]
                        {
                            if (current_ > 0) go_to(current_ - 1);
                        }),
            },
            label{}.bind(revision_label_).ellipsize(),
            hbox{
                label{_("Find:")},
                entry{}.bind(find_).on_activate([this] { find_next(); }).stretch(),
                check{_("Ignore _whitespace"), st.value("blame.ignoreWhitespace") == "1"}
                    .bind(ignore_whitespace_)
                    .on_toggled(
                        [this](bool on)
                        {
                            settings::get().set("blame.ignoreWhitespace", on ? "1" : "0");
                            go_to(current_);
                        }),
                check{_("Detect _moved or copied lines"), st.value("blame.detectMoves") == "1"}
                    .bind(detect_moves_)
                    .on_toggled(
                        [this](bool on)
                        {
                            settings::get().set("blame.detectMoves", on ? "1" : "0");
                            go_to(current_);
                        }),
            },
            splitter{
                vertical,
                list{columns, rows_.get()}
                    .bind(list_)
                    .selection(rap::selection_mode::single)
                    .on_selection_changed([this] { update_details(); })
                    .on_activate(
                        [this](size_t row)
                        {
                            if (row < rows_->lines.size()) show_changes(rows_->lines[row]);
                        })
                    .on_context_menu([this](std::optional<size_t> row) { return line_menu(row); })
                    .stretch(4),
                text_view{}.read_only().bind(details_).height(5_lines),
            }
                .bind(splitter_)
                .stretch(),
            label{}.bind(status_).ellipsize(),
        }}}.size(140_ch, 45_lines));
    st.restore_window("blame.window", *window_);
    if (auto v = st.numbers("blame.columns"); v.size() == column_count) list_->set_column_widths(v);
    // The lines get most of the height, the commit's details a strip.
    if (auto v = st.numbers("blame.panes"); v.size() == 2)
        splitter_->set_sizes(v);
    else
        splitter_->set_sizes({600, 110});
    window_->on_close_request(
        [this]
        {
            settings& st = settings::get();
            st.save_window("blame.window", *window_);
            st.set_numbers("blame.columns", list_->column_widths());
            st.set_numbers("blame.panes", splitter_->sizes());
            if (closed_) closed_();
            return true;
        });
    window_->on_key(
        [this](const rap::key_event& e)
        {
            if (!e.press) return false;
            if (e.key == rap::key::f3)
            {
                find_next();
                return true;
            }
            if (e.key == rap::key::character && (e.codepoint == U'f' || e.codepoint == U'F') &&
                (e.modifiers & rap::mod_ctrl))
            {
                find_->focus();
                return true;
            }
            return false;
        });

    // What it was opened on first; the rest of the history as it arrives.
    stops_ = {{revision_, path_, revision_.empty() ? std::string(_("Working tree")) : revision_}};
    go_to(0);
    load_history();
}

// The file's revisions, newest first, across renames, for the slider.
void blame_window::load_history()
{
    auto output = std::make_shared<std::string>();
    history_ = git_process::start(
        ui_, workdir_, file_revision_args(path_, revision_),
        [output](const std::string& c) { *output += c; },
        [this, output, alive = std::weak_ptr<bool>(alive_)](int code)
        {
            if (!alive.lock() || code != 0) return;
            const std::string opened = stops_.front().hash;
            for (const file_revision& r : parse_file_revisions(*output))
            {
                // A revision given that changed the file is the first stop already.
                if (!opened.empty() && (r.hash == opened || r.hash.starts_with(opened))) continue;
                stops_.push_back({r.hash, r.path,
                                  r.hash.substr(0, 8) + "  " + date_text(r.time) + "  " + r.author +
                                      "  " + r.summary});
            }
            update_slider();
        },
        git_output::stdout_only);
}

void blame_window::go_to(size_t stop)
{
    if (stop >= stops_.size()) return;
    current_ = stop;
    update_slider();
    blame(stops_[stop]);
}

void blame_window::update_slider()
{
    const size_t n = stops_.size();
    slider_->set_range(0, double(std::max<size_t>(n, 2) - 1));
    slider_->set_value(double(n - 1 - std::min(current_, n - 1)));
    slider_->set_enabled(n > 1);
    older_->set_enabled(current_ + 1 < n);
    newer_->set_enabled(current_ > 0);
    std::string text = stops_[current_].label;
    if (n > 1)
        text += "   " + rap::i18n::format(_("({0} of {1})"), std::to_string(n - current_),
                                          std::to_string(n));
    revision_label_->set_text(text);
}

// Blamed through git in the background, kept per revision and options.
void blame_window::blame(const stop& s)
{
    const blame_options o{.revision = s.hash,
                          .ignore_whitespace = ignore_whitespace_->checked(),
                          .detect_moves = detect_moves_->checked()};
    const std::string key =
        s.hash + ":" + s.path + (o.ignore_whitespace ? ":w" : "") + (o.detect_moves ? ":m" : "");
    if (auto it = cache_.find(key); it != cache_.end())
    {
        running_.reset();
        shown(it->second);
        return;
    }
    status_->set_text(_("Blaming…"));
    auto output = std::make_shared<std::string>();
    running_ = git_process::start(
        ui_, workdir_, blame_args(s.path, o), [output](const std::string& c) { *output += c; },
        [this, output, key, alive = std::weak_ptr<bool>(alive_)](int code)
        {
            if (!alive.lock()) return;
            if (code != 0)
            {
                status_->set_text(rap::i18n::format(_("git blame failed: {0}"), *output));
                return;
            }
            shown(cache_[key] = parse_blame(*output));
        },
        git_output::stdout_only);
}

// New lines in place of the old, the same line selected and in view.
void blame_window::shown(const std::vector<blame_line>& lines)
{
    const size_t keep = selected_line().value_or(0);
    rows_->selected = nullptr;
    rows_->set(lines);
    if (!lines.empty())
    {
        const size_t row = std::min(keep, lines.size() - 1);
        list_->select({row});
        list_->set_current(row);
        list_->scroll_to(row);
    }
    update_details();
    status_->set_text(
        rap::i18n::format(N_("{0} line", "{0} lines", lines.size()), std::to_string(lines.size())));
}

std::optional<size_t> blame_window::selected_line() const
{
    const auto sel = list_->selection();
    if (sel.empty() || sel.front() >= rows_->lines.size()) return std::nullopt;
    return sel.front();
}

// The selected line's commit: its details below, its lines highlighted.
void blame_window::update_details()
{
    const auto row = selected_line();
    const blame_commit* c = row ? rows_->lines[*row].commit.get() : nullptr;
    if (c != rows_->selected)
    {
        rows_->selected = c;
        rows_->notify_changed(0, rows_->row_count());
    }
    if (!c)
    {
        details_->set_text({});
        return;
    }
    if (c->uncommitted())
    {
        details_->set_text(_("Not committed yet: the working tree's own change."));
        return;
    }
    const git_result message =
        run_git(workdir_, {"log", "-1", "--format=%B", c->hash}, git_output::stdout_only);
    std::string text = c->hash + "\n" + c->author + " <" + c->author_email + ">  " +
                       date_text(c->author_time) + "\n";
    if (rows_->lines[*row].filename != path_)
        text += rap::i18n::format(_("as {0}"), rows_->lines[*row].filename) + "\n";
    text += "\n" + (message.ok() ? message.output : c->summary);
    details_->set_text(text);
}

void blame_window::find_next()
{
    const std::string what = lower(find_->text());
    const size_t n = rows_->lines.size();
    if (what.empty() || !n) return;
    const size_t from = selected_line().value_or(n - 1);
    for (size_t i = 1; i <= n; ++i)
    {
        const size_t row = (from + i) % n;
        if (lower(rows_->shown[row]).find(what) != std::string::npos)
        {
            list_->select({row});
            list_->set_current(row);
            list_->scroll_to(row);
            return;
        }
    }
    status_->set_text(rap::i18n::format(_("“{0}” isn't in the file."), find_->text()));
}

rap::menu blame_window::line_menu(std::optional<size_t> row)
{
    if (!row || *row >= rows_->lines.size()) return {};
    const blame_line line = rows_->lines[*row];
    const blame_commit& c = *line.commit;
    rap::menu m;
    if (!c.uncommitted())
        m.push_back({.label = _("Blame _previous revision"),
                     .action = [this, line] { blame_previous(line); },
                     .enabled = !c.previous_hash.empty()});
    m.push_back({.label = _("Show _changes"), .action = [this, line] { show_changes(line); }});
    if (!c.uncommitted())
        m.push_back({.label = _("Show _log"), .action = [this, line] { show_log(line); }});
    m.push_back({});
    if (!c.uncommitted())
    {
        m.push_back({.label = _("Copy SHA-1"),
                     .action = [this, hash = c.hash] { ui_.set_clipboard(hash); }});
        m.push_back({.label = _("Copy log message"),
                     .action = [this, hash = c.hash]
                     {
                         const git_result r = run_git(workdir_, {"log", "-1", "--format=%B", hash},
                                                      git_output::stdout_only);
                         if (r.ok()) ui_.set_clipboard(r.output);
                     }});
    }
    m.push_back(
        {.label = _("Copy line"), .action = [this, text = line.text] { ui_.set_clipboard(text); }});
    return m;
}

// The file as it was before this line's commit: the slider's next older
// stop when that's the commit before (it usually is), else that revision.
void blame_window::blame_previous(const blame_line& line)
{
    const blame_commit& c = *line.commit;
    for (size_t i = 0; i < stops_.size(); ++i)
        if (stops_[i].hash == c.hash && i + 1 < stops_.size()) return go_to(i + 1);
    const stop s{
        c.previous_hash, c.previous_filename.empty() ? line.filename : c.previous_filename,
        rap::i18n::format(_("{0}, before {1}"), c.previous_hash.substr(0, 8), c.hash.substr(0, 8))};
    revision_label_->set_text(s.label);
    blame(s);
}

// What this line's commit did to the file; the working tree's own lines
// against HEAD.
void blame_window::show_changes(const blame_line& line)
{
    const blame_commit& c = *line.commit;
    std::vector<std::string> revisions = c.uncommitted() ? std::vector<std::string>{"HEAD"}
                                         : c.previous_hash.empty()
                                             ? std::vector<std::string>{k_empty_tree, c.hash}
                                             : std::vector<std::string>{c.hash + "^", c.hash};
    // Both names when the commit renamed the file, so git pairs them rather
    // than calling it added.
    std::vector<std::string> files{c.uncommitted() ? path_ : line.filename};
    if (!c.uncommitted() && !c.previous_filename.empty() && c.previous_filename != line.filename)
        files.insert(files.begin(), c.previous_filename);
    difftool_ = compare_revisions(
        ui_, window_.get(), workdir_, std::move(revisions), std::move(files),
        [this] { show_settings(ui_, window_.get(), workdir_, settings_page::diff_viewer); });
}

// The file's log, at this line's commit.
void blame_window::show_log(const blame_line& line)
{
    const std::string file = rap::sys::path_to_utf8(rap::sys::path_from_utf8(workdir_) /
                                                    rap::sys::path_from_utf8(path_));
    auto log = log_window::open(ui_, file);
    if (!log)
    {
        ui_.message(
            window_.get(),
            {.icon = rap::message_spec::icon::error, .title = "Tortilla", .text = log.error()});
        return;
    }
    (*log)->select_commit(line.commit->hash);
    windows::keep(std::move(*log));
}

}  // namespace tortilla
