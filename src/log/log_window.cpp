#include "log_window.h"

#include "branch/browse_refs.h"

#include "git/command.h"
#include "i18n.h"
#include "settings.h"
#include "ui/menu_text.h"
#include "ui/settings_dialog.h"
#include "util/calendar.h"

#include <rapport/sys.h>

#include <ctime>
#include <filesystem>
#include <numeric>
#include <unordered_set>

namespace tortilla
{
namespace
{

// Jump combo entries [spec 1.9.6], in order.
enum jump_type
{
    jump_author_email,
    jump_committer_email,
    jump_merge_point,
    jump_parent1,
    jump_parent2,
    jump_tag,
    jump_tag_ff,
    jump_branch,
    jump_branch_ff,
    jump_history,
};

std::string format_time(int64_t time, int offset_minutes)
{
    std::time_t t = time + int64_t(offset_minutes) * 60;
    std::tm tm = utc_calendar(t);
    char buf[48];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tm);
    char tz[16];
    std::snprintf(tz, sizeof tz, " %+03d%02d", offset_minutes / 60, std::abs(offset_minutes % 60));
    return std::string(buf) + tz;
}

rap::date local_date(int64_t time)
{
    std::time_t t = time;
    std::tm tm = local_calendar(t);
    return {tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday};
}

// From means 00:00:00 of the day, To 23:59:59 [spec 1.9.3].
int64_t local_time(rap::date d, bool end_of_day)
{
    std::tm tm{};
    tm.tm_year = d.year - 1900;
    tm.tm_mon = d.month - 1;
    tm.tm_mday = d.day;
    tm.tm_hour = end_of_day ? 23 : 0;
    tm.tm_min = end_of_day ? 59 : 0;
    tm.tm_sec = end_of_day ? 59 : 0;
    tm.tm_isdst = -1;
    return std::mktime(&tm);
}

// TortoiseGit's jump buttons are green arrows (IDI_JUMPUP/IDI_JUMPDOWN).
rap::icon jump_arrow(bool up)
{
    return rap::icon::drawn(
        [up](rap::painter& p, rap::size_f s)
        {
            float w = s.w, h = s.h, cx = w / 2;
            float tip = up ? h * 0.1f : h * 0.9f, base = up ? h * 0.55f : h * 0.45f;
            float tail = up ? h * 0.9f : h * 0.1f;
            rap::color green{0x2e, 0xa0, 0x43}, edge{0x1d, 0x6b, 0x2b};
            p.polygon({{cx, tip},
                       {w * 0.9f, base},
                       {w * 0.65f, base},
                       {w * 0.65f, tail},
                       {w * 0.35f, tail},
                       {w * 0.35f, base},
                       {w * 0.1f, base}},
                      rap::paint{green}, rap::paint{edge});
        });
}

rap::menu_item toggle(std::string label, bool on, std::function<void()> action, bool enabled = true)
{
    return {
        .label = std::move(label), .action = std::move(action), .enabled = enabled, .checked = on};
}

}  // namespace

std::expected<std::unique_ptr<log_window>, std::string>
log_window::open(rap::ui& ui, const std::string& path, bool all_branches)
{
    auto repo = repo::open(path);
    if (!repo) return std::unexpected(repo.error());
    std::unique_ptr<log_window> w(new log_window(ui, std::move(*repo), path));
    w->build();
    if (all_branches) w->set_walk(walk_refs::all);
    w->reload();
    w->watch_repository();
    return w;
}

log_window::log_window(rap::ui& ui, repo repo, std::string path)
    : ui_(ui)
    , repo_(std::move(repo))
    , path_(std::move(path))
    , log_(ui)
    , files_worker_(std::make_unique<repo_worker>(ui, path_))
    , files_({.ui = ui,
              .window = [this] { return window_.get(); },
              .workdir = repo_.workdir(),
              // Working-tree changes: git runs quietly, and the log reloads after.
              .run =
                  [this](const std::string& title, std::vector<std::vector<std::string>> commands)
              {
                  run_quietly(title, std::move(commands),
                              [this](bool, const std::string&) { reload(); });
              },
              .refresh = [this] { reload(); },
              .open_settings = [this](settings_page page) { open_settings(page); },
              .selection_changed = [this] { update_status(); }})
{
}

void log_window::build()
{
    using namespace rap::layout;
    namespace a = rap;

    std::string title = repo_.workdir();
    if (title.size() > 1 && title.back() == '/') title.pop_back();
    title = rap::sys::path_to_utf8(rap::sys::path_from_utf8(title).make_preferred());

    window_ = ui_.create(window{
        rap::i18n::format(_("{0} - Log Messages - Tortilla"), title),
        {vbox{
            // Filter bar [spec 1.2, row 1]
            hbox{
                menu_button{"HEAD"}
                    .bind(ref_button_)
                    .on_menu([this] { return ref_menu(); })
                    .width(22_ch),
                label{_("From:")},
                date_picker{}.bind(from_).on_changed([this](a::date) { dates_changed(); }),
                label{_("To:")},
                date_picker{}.bind(to_).on_changed([this](a::date) { dates_changed(); }),
                entry{}
                    .bind(search_)
                    .on_changed([this](const std::string&) { schedule_filter(); })
                    .on_activate([this] { apply_filter(); })
                    .stretch(),
                // TODO: TortoiseGit puts this magnifier inside the search box.
                menu_button{""}
                    .icon(rap::icon::named("edit-find-symbolic"))
                    .on_menu([this] { return filter_menu(); })
                    .tooltip(_("Filter options")),
                drop_down{{_("Author Email"), _("Committer Email"), _("Merge Point"), _("Parent 1"),
                           _("Parent 2"), _("Tag"), _("Tag (FF)"), _("Branch"), _("Branch (FF)"),
                           _("Selection History")},
                          0}
                    .bind(jump_type_),
                button{_("Jump up (Alt+Up)")}
                    .icon(jump_arrow(true))
                    .icon_only()
                    .on_click([this] { jump(true); }),
                button{_("Jump down (Alt+Down)")}
                    .icon(jump_arrow(false))
                    .icon_only()
                    .on_click([this] { jump(false); }),
            },
            // Commit list, details, changed files [spec 1.2, main area]
            splitter{
                vertical,
                list{log_model::columns(), &log_}
                    .soft_selection()
                    .auto_scroll()
                    .bind(commit_list_)
                    .on_selection_changed([this] { selection_changed(); })
                    .on_context_menu([this](std::optional<size_t> row)
                                     { return context_menu(row); })
                    .on_header_menu([this](size_t) { return header_menu(); })
                    .on_cell_drag(label_drag())
                    .on_activate([this](size_t row) { row_activated(row); })
                    .on_cell_context_menu([this](size_t row, size_t column, rap::point_f at)
                                          { return cell_menu(row, column, at); })
                    .stretch(3),
                // The details pane keeps its height: dragging the upper
                // divider moves the lower one with it, taking space from the
                // file list until that reaches its minimum. (TortoiseGit
                // shrinks the details first [spec 1.3]; this is deliberate.)
                hbox{
                    text_view{}.read_only().bind(details_).stretch(),
                    // Kept top right, as TortoiseGit has it [spec 1.2].
                    vbox{
                        canvas{}
                            .on_paint(
                                [this](rap::painter& p, rap::size_f size)
                                {
                                    p.rect({0, 0, size.w, size.h}, 0,
                                           rap::paint{rap::theme_color::window}, std::nullopt);
                                    if (!avatar_) return;
                                    p.image(*avatar_,
                                            {0, 0, float(avatar_->width()),
                                             float(avatar_->height())},
                                            {0, 0, size.w, size.h});
                                })
                            .width(64_px)
                            .height(64_px)
                            .bind(avatar_canvas_),
                        spacer{},
                    },
                }
                    .height(9_lines),
                files_.list("log.files").height(4_lines).stretch(2),
            }
                .bind(splitter_)
                .stretch(),
            // Bottom area [spec 1.2]
            label{}.selectable().ellipsize().bind(status_),
            hbox{
                vbox{
                    // TODO: enabled once the log can be opened for a path.
                    check{_("Show _Whole Project")}.enabled(false),
                    check{_("_All Branches")}
                        .bind(all_branches_)
                        .on_toggled(
                            [this](bool)
                            {
                                set_walk(all_branches_->checked() ? walk_refs::all : walk_refs::head);
                                reload();
                            }),
                },
                // TODO: path filter.
                entry{}.placeholder(std::string("   ") + _("Filter paths")).enabled(false).stretch(),
            },
            hbox{
                button{_("Refresh")}.tooltip(_("Reload the log (F5)")).on_click([this] { reload(); }),
                button{_("S_tatistics")}.bind(statistics_).enabled(false),
                progress_bar{}.bind(progress_).stretch(),
                menu_button{_("Walk Be_havior")}.bind(walk_).on_menu([this] { return walk_menu(); }),
                menu_button{_("_View")}.bind(view_).on_menu([this] { return view_menu(); }),
                spacer{},
                button{_("OK")}.on_click(
                    [this]
                    {
                        save_layout();
                        window_->close();
                        if (closed_) closed_();
                    }),
            },
        }}}.size(140_ch, 50_lines));

    window_->on_close_request(
        [this]
        {
            save_layout();
            if (closed_) closed_();
            return true;
        });
    // F5 reloads [TortoiseGit: Refresh], e.g. after git ran elsewhere.
    window_->on_key(
        [this](const rap::key_event& e)
        {
            if (!e.press || e.key != rap::key::f5 || e.modifiers) return false;
            reload();
            return true;
        });
    // When the working-tree status arrives, refresh it if it's on show.
    log_.on_working_tree(
        [this]
        {
            auto sel = commit_list_->selection();
            if (sel.size() == 1 && log_.is_working_tree(sel.front())) show_commit(sel.front());
        });
    search_->set_placeholder(filter_cue());
    progress_->set_visible(false);
    // Today until the loaded range replaces it (not 1970 while loading).
    from_->set_date(local_date(std::time(nullptr)));
    to_->set_date(local_date(std::time(nullptr)));
    update_ref_button();
    if (auto saved = settings::get().value("log.actionIcons"); !saved.empty())
        log_.set_action_icon_style(action_icon_style(std::clamp(std::atoi(saved.c_str()), 0, 3)));
    restore_layout();
}

// Window size and position, splitter panes, column widths and visibility
// persist across runs, as TortoiseGit's do (LogDlgSizer1/2 and the column
// state, spec 1.3 and 1.4). Position is only restored where the platform
// lets apps place windows (not on Wayland).
void log_window::save_layout()
{
    settings& st = settings::get();
    st.save_window("log.window", *window_);
    st.set_numbers("log.panes", splitter_->sizes());
    st.set_numbers("log.columns", commit_list_->column_widths());
    auto order = [](const std::vector<size_t>& o)
    {
        std::vector<float> v;
        for (size_t i : o) v.push_back(float(i));
        return v;
    };
    st.set_numbers("log.columns.order", order(commit_list_->column_order()));
    files_.save_layout();
    std::string visible;
    for (size_t i = 0; i < log_model::column_count; ++i)
        visible += commit_list_->column_visible(i) ? '1' : '0';
    st.set("log.columns.visible", visible);

    // Options. All Branches is per repository, as in TortoiseGit [spec
    // 1.9.7]; the rest apply everywhere.
    auto flag = [](bool b) { return std::string(b ? "1" : "0"); };
    // "1" for all; basic and local refs (the box indeterminate) by name.
    st.set("log.allBranches." + repo_.workdir(), walk_options_.refs == walk_refs::all     ? "1"
                                                 : walk_options_.refs == walk_refs::basic ? "basic"
                                                 : walk_options_.refs == walk_refs::local ? "local"
                                                                                          : "0");
    std::string history;
    for (const std::string& r : ref_history_) history += (history.empty() ? "" : " ") + r;
    st.set("log.refHistory." + repo_.workdir(), history);
    st.set("log.walk.firstParent", flag(walk_options_.first_parent));
    st.set("log.walk.noMerges", flag(walk_options_.no_merges));
    st.set("log.view.labels", std::to_string(log_.labels()));
    st.set("log.view.unversioned", flag(show_unversioned_));
    st.set("log.filter.fields", std::to_string(filter_.fields));
    st.set("log.filter.regex", flag(filter_.regex));
    st.set("log.filter.caseSensitive", flag(filter_.case_sensitive));
    st.set("log.jumpType", std::to_string(jump_type_->current()));
}

void log_window::restore_layout()
{
    settings& st = settings::get();
    st.restore_window("log.window", *window_);
    if (auto v = st.numbers("log.panes"); v.size() == 3) splitter_->set_sizes(v);
    if (auto v = st.numbers("log.columns"); !v.empty()) commit_list_->set_column_widths(v);
    // The lists ignore an order that doesn't fit their columns.
    auto order = [](const std::vector<float>& v)
    {
        std::vector<size_t> o;
        for (float f : v) o.push_back(size_t(f));
        return o;
    };
    commit_list_->set_column_order(order(st.numbers("log.columns.order")));
    files_.restore_layout();
    std::string visible = st.value("log.columns.visible");
    for (size_t i = 0; i < visible.size() && i < log_model::column_count; ++i)
        commit_list_->set_column_visible(i, visible[i] == '1');

    auto flag = [&](const std::string& key, bool fallback)
    {
        std::string v = st.value(key);
        return v.empty() ? fallback : v == "1";
    };
    auto number = [&](const std::string& key, long fallback)
    {
        std::string v = st.value(key);
        return v.empty() ? fallback : std::strtol(v.c_str(), nullptr, 10);
    };
    // Ref names have no spaces.
    ref_history_.clear();
    const std::string history = st.value("log.refHistory." + repo_.workdir());
    for (size_t at = 0; at < history.size();)
    {
        size_t space = history.find(' ', at);
        if (space == std::string::npos) space = history.size();
        if (space > at) ref_history_.push_back(history.substr(at, space - at));
        at = space + 1;
    }
    const std::string refs = st.value("log.allBranches." + repo_.workdir());
    set_walk(refs == "1" ? walk_refs::all
             : refs == "basic" ? walk_refs::basic
             : refs == "local" ? walk_refs::local
                               : walk_refs::head);
    walk_options_.first_parent = flag("log.walk.firstParent", false);
    walk_options_.no_merges = flag("log.walk.noMerges", false);
    walk_->set_pushed(walk_options_.first_parent || walk_options_.no_merges);
    update_ref_button();
    log_.set_labels(unsigned(number("log.view.labels", label_all)) & label_all);
    show_unversioned_ = flag("log.view.unversioned", true);
    filter_.fields = unsigned(number("log.filter.fields", filter_all)) & filter_all;
    if (filter_.fields == 0) filter_.fields = filter_all;
    filter_.regex = flag("log.filter.regex", false);
    filter_.case_sensitive = flag("log.filter.caseSensitive", false);
    search_->set_placeholder(filter_cue());
    jump_type_->set_current(int(std::clamp(number("log.jumpType", 0), 0L, 9L)));
}

// While loading, a progress bar replaces Statistics, Walk Behavior and View
// [spec 1.2].
// The repository as the log shows it: HEAD and every ref, by their files'
// times and sizes (a merge, rebase or cherry-pick in progress too). Not the
// index: the log's own git status touches it.
std::string log_window::repository_state() const
{
    std::string state;
    std::error_code ec;
    auto add = [&](const std::filesystem::path& p)
    {
        const auto status = std::filesystem::status(p, ec);
        if (!std::filesystem::exists(status)) return;
        const auto time = std::filesystem::last_write_time(p, ec).time_since_epoch().count();
        const auto size =
            std::filesystem::is_regular_file(status) ? std::filesystem::file_size(p, ec) : 0;
        state += rap::sys::path_to_utf8(p) + ':' + std::to_string(time) + ':' +
                 std::to_string(size) + '\n';
    };
    for (const char* f :
         {"HEAD", "MERGE_HEAD", "CHERRY_PICK_HEAD", "REVERT_HEAD", "tortilla-rebase-active"})
        add(git_dir_ / f);
    add(common_dir_ / "packed-refs");
    for (auto it = std::filesystem::recursive_directory_iterator(common_dir_ / "refs", ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
        add(it->path());
    return state;
}

// Another window or program may change the repository (a rebase, a pull,
// git on the command line): the system says when the git folder or its
// refs change, and the log reloads if HEAD or a ref did (not for the index,
// which the log's own status touches).
void log_window::watch_repository()
{
    git_dir_ = rap::sys::path_from_utf8(repo_.git_dir());
    const git_result common = run_git(repo_.workdir().empty() ? repo_.git_dir() : repo_.workdir(),
                                      {"rev-parse", "--git-common-dir"}, git_output::stdout_only);
    std::string dir = common.ok() ? common.output : "";
    while (!dir.empty() && (dir.back() == '\n' || dir.back() == '\r')) dir.pop_back();
    // (Relative to where git ran; a worktree's refs are in the main one's.)
    const std::filesystem::path common_dir = rap::sys::path_from_utf8(dir);
    const std::string ran_in = repo_.workdir().empty() ? repo_.git_dir() : repo_.workdir();
    common_dir_ = dir.empty()                ? git_dir_
                  : common_dir.is_absolute() ? common_dir
                                             : rap::sys::path_from_utf8(ran_in) / common_dir;
    state_ = repository_state();
    auto changed = [this, alive = std::weak_ptr<bool>(alive_)](rap::sys::watch_batch)
    {
        // (On the watch's thread.)
        ui_.post(
            [this, alive]
            {
                if (alive.lock() && repository_state() != state_) reload();
            });
    };
    const rap::sys::watch_options settle{.settle = std::chrono::milliseconds(200)};
    for (auto [folder, recursive] : {std::pair{git_dir_, false}, std::pair{common_dir_, false},
                                     std::pair{common_dir_ / "refs", true}})
    {
        if (folder == common_dir_ && common_dir_ == git_dir_) continue;
        auto options = settle;
        options.recursive = recursive;
        if (auto w = rap::sys::directory_watch::start(folder, options, changed))
            watches_.push_back(std::move(*w));
    }
}

void log_window::reload()
{
    // (The state as this reload sees it: the log's own changes don't reload
    // it twice.)
    if (!git_dir_.empty()) state_ = repository_state();
    if (log_.row_count())
    {
        kept_selection keep;
        for (size_t row : commit_list_->selection())
            if (log_.is_working_tree(row))
                keep.working_tree = true;
            else if (const commit* c = log_.commit(row))
                keep.commits.push_back(c->id);
        if (auto cur = commit_list_->current(); cur && !log_.is_working_tree(*cur))
            if (const commit* c = log_.commit(*cur)) keep.current = c->id;
        if (!keep.commits.empty() || keep.working_tree) kept_ = std::move(keep);
    }
    statistics_->set_visible(false);
    walk_->set_visible(false);
    view_->set_visible(false);
    progress_->set_visible(true);
    pulse_timer_ = ui_.timer(100, true, [this] { progress_->pulse(); });
    status_->set_text(_("Loading…"));

    // A ref picked to start from that has since gone: back to HEAD.
    if (!walk_options_.rev.empty() &&
        !run_git(repo_.workdir().empty() ? repo_.git_dir() : repo_.workdir(),
                 {"rev-parse", "--verify", "-q", walk_options_.rev + "^{commit}"}, git_output::stdout_only)
             .ok())
        set_walk(walk_refs::head);

    walk_options opts = walk_options_;
    opts.since = since_;
    opts.until = until_;
    log_.load(path_, opts, [this](const std::string& e) { loaded(e); });
    if (!filter_.text.empty()) log_.set_filter(filter_);
}

void log_window::loaded(const std::string& error)
{
    pulse_timer_.reset();
    progress_->set_visible(false);
    statistics_->set_visible(true);
    walk_->set_visible(true);
    view_->set_visible(true);
    if (!error.empty())
    {
        status_->set_text(rap::i18n::format(_("Error: {0}"), error));
        return;
    }

    // Without explicit limits the pickers show the range actually loaded.
    size_t first = log_.row_count() - log_.commit_rows();
    if (log_.commit_rows() && !since_ && !until_)
    {
        from_->set_date(local_date(log_.commit(log_.row_count() - 1)->time));
        to_->set_date(local_date(log_.commit(first)->time));
    }
    restore_selection();
    if (log_.row_count() && !commit_list_->current()) commit_list_->set_current(0);
    update_status();
}

void log_window::restore_selection()
{
    if (!kept_) return;
    kept_selection keep = std::move(*kept_);
    kept_.reset();
    std::unordered_set<oid, oid_hash> wanted(keep.commits.begin(), keep.commits.end());
    std::vector<size_t> rows;
    std::optional<size_t> current;
    for (size_t row = 0; row < log_.row_count(); ++row)
    {
        const commit* c = log_.commit(row);
        if (log_.is_working_tree(row) ? keep.working_tree : c && wanted.contains(c->id))
            rows.push_back(row);
        if (c && keep.current && c->id == *keep.current) current = row;
    }
    if (rows.empty()) return;  // gone (a deleted branch's commits): start at the top
    if (!current) current = rows.front();
    commit_list_->set_current(*current);
    commit_list_->select(rows);
    commit_list_->scroll_to(*current);
}

void log_window::selection_changed()
{
    auto sel = commit_list_->selection();
    if (sel.size() == 1)
    {
        show_commit(sel.front());
        if (const commit* c = log_.commit(sel.front()); c && !navigating_)
        {
            history_.resize(std::min(history_.size(), history_pos_ + 1));
            if (history_.empty() || history_.back() != c->id) history_.push_back(c->id);
            history_pos_ = history_.size() - 1;
        }
    }
    else
    {
        details_->set_text("");
        show_avatar({});
        files_.clear();
    }
    update_status();
}

// The picture at the canvas's size in device pixels, so it's drawn 1:1.
void log_window::show_avatar(const std::string& email)
{
    const float scale = avatar_canvas_ ? avatar_canvas_->device_scale() : 1;
    const int px = int(std::lround(64 * std::max(scale, 1.f)));
    bool answered = false;
    auto show = [this](std::shared_ptr<rap::image> image)
    {
        if (image == avatar_) return;
        avatar_ = std::move(image);
        if (avatar_canvas_) avatar_canvas_->invalidate();
    };
    gravatar_.request(email, px,
                      [show, &answered](std::shared_ptr<rap::image> image)
                      {
                          answered = true;
                          show(std::move(image));
                      });
    // Being fetched: not the last author's picture meanwhile.
    if (!answered) show(nullptr);
}

void log_window::show_commit(size_t row)
{
    if (log_.is_working_tree(row))
    {
        show_avatar({});
        // [spec 1.8]: "Fetching Status..." until the status arrives.
        if (!log_.working_tree_known())
        {
            details_->set_text(std::string(_("Working tree changes")) + "\n\n" + _("Fetching status..."));
            files_.set_source({.what = changes_source::working_tree});
            files_.clear();
            return;
        }
        const size_t changed = log_.working_tree_files().size();
        details_->set_text(std::string(_("Working tree changes")) + "\n\n" +
                           rap::i18n::format(N_("{0} file changed", "{0} files changed", changed), changed));
        files_.set_source({.what = changes_source::working_tree});
        files_.set_files(log_.working_tree_files(), show_unversioned_);
        return;
    }

    const commit& c = *log_.commit(row);
    std::string text;
    std::vector<rap::text_view::span> spans;
    auto line = [&](const std::string& label, const std::string& value)
    {
        spans.push_back({text.size(), text.size() + label.size(), {.bold = true}});
        text += label + value + "\n";
    };
    line(_("SHA-1: "), c.id.hex());
    line(_("Author: "), c.author + " <" + c.email + ">");
    line(_("Date: "), format_time(c.time, c.offset_minutes));
    if (c.committer != c.author || c.commit_time != c.time)
        line(_("Committer: "), c.committer + " <" + c.committer_email + "> " +
                                format_time(c.commit_time, c.commit_offset_minutes));
    for (auto& p : c.parents) line(_("Parent: "), p.hex());
    text += "\n" + c.message;
    details_->set_text(text, spans);
    show_avatar(c.email);

    // The file list comes from the worker, so the click (and its highlight)
    // never waits for git: the files as soon as the tree diff has them, then
    // their line counts, which read every file (a commit of thousands of
    // files on a slow disk takes many seconds). Clicking elsewhere drops
    // the counting.
    changes_source source{.what = changes_source::revision,
                          .hash = c.id.hex(),
                          .parent = c.parents.empty() ? k_empty_tree : c.parents.front().hex()};
    if (!c.parents.empty())
    {
        source.parent_title = source.parent.substr(0, 8);
        if (auto r = log_.row_of(c.parents.front()))
            if (const commit* p = log_.commit(*r)) source.parent_title += " " + p->summary;
    }
    files_.set_source(std::move(source));
    files_.clear();
    oid id = c.id;
    files_worker_->submit(
        [this, id](repo& repo, const repo_worker::progress& progress) -> std::function<void()>
        {
            (void)repo.changes(id, {.listed =
                                        [&](const std::vector<file_change>& files)
                                    {
                                        progress.post(
                                            [this, files]
                                            {
                                                files_.set_files(files);
                                                update_status();
                                            });
                                    },
                                    .counted =
                                        [&](size_t first, const std::vector<file_change>& files)
                                    {
                                        progress.post(
                                            [this, first, files]
                                            {
                                                files_.set_counts(first, files);
                                                update_status();
                                            });
                                    },
                                    .stop = [&] { return progress.stale(); }});
            return {};
        });
}

// "Showing %1 revision(s), from revision %2 to revision %3 - %4
// revision(s) selected, %5 file(s) selected" plus, for one revision, the
// line and file statistics [spec 1.10].
void log_window::update_status()
{
    if (log_.loading()) return;
    size_t n = log_.commit_rows();
    std::string s = rap::i18n::format(_("Showing {0} revision(s)"), n);
    if (n)
    {
        size_t first = log_.row_count() - n;
        s = rap::i18n::format(_("Showing {0} revision(s), from revision {1} to revision {2}"), n,
                              log_.commit(log_.row_count() - 1)->id.hex(8), log_.commit(first)->id.hex(8));
    }
    auto sel = commit_list_->selection();
    s = rap::i18n::format(_("{0} - {1} revision(s) selected, {2} file(s) selected"), s, sel.size(),
                          files_.selection().size());
    if (sel.size() == 1)
    {
        size_t added = 0, removed = 0, mod = 0, add = 0, del = 0, ren = 0;
        for (auto& f : files_.files())
        {
            added += f.added;
            removed += f.removed;
            switch (f.status)
            {
            case 'A':
            case '?': ++add; break;
            case 'D': ++del; break;
            case 'R': ++ren; break;
            default: ++mod;
            }
        }
        s = rap::i18n::format(
            _("{0}; line: {1}(+) {2}(-) files: modified = {3} added = {4} deleted = {5} replaced = {6}"), s,
            added, removed, mod, add, del, ren);
    }
    status_->set_text(s);
}

// The ref selector shows the current ref, or a mode [spec 1.9.1].
void log_window::start_at(const std::string& rev)
{
    set_walk(walk_refs::head, rev);
    reload();
}

void log_window::set_walk(walk_refs refs, std::string rev)
{
    walk_options_.refs = refs;
    if (!rev.empty())
    {
        std::erase(ref_history_, rev);
        ref_history_.insert(ref_history_.begin(), rev);
        if (ref_history_.size() > 5) ref_history_.resize(5);
    }
    walk_options_.rev = std::move(rev);
    // All checks the box; basic or local refs make it indeterminate.
    all_branches_->set_state(refs == walk_refs::all    ? rap::check_state::checked
                             : refs == walk_refs::head ? rap::check_state::unchecked
                                                       : rap::check_state::indeterminate);
    update_ref_button();
}

void log_window::update_ref_button()
{
    std::string text, full;
    switch (walk_options_.refs)
    {
    case walk_refs::all: text = _("<All Branches>"); break;
    case walk_refs::basic: text = _("<Basic Refs>"); break;
    case walk_refs::local: text = _("<Local Branches>"); break;
    case walk_refs::head:
        if (!walk_options_.rev.empty())
        {
            full = walk_options_.rev;
            text = full;
            for (std::string_view prefix : {"refs/heads/", "refs/remotes/", "refs/tags/", "refs/"})
                if (text.starts_with(prefix))
                {
                    text.erase(0, prefix.size());
                    break;
                }
        }
        else
        {
            std::string head = repo_.head_name();
            text = head == "HEAD" ? _("<No branch>") : head;
            full = head == "HEAD" ? "HEAD" : "refs/heads/" + head;
        }
        break;
    }
    ref_button_->set_text(text);
    ref_button_->set_tooltip(full.empty() ? text : full);
}

// TortoiseGit's right-click menu [spec 1.9.1], with the branches and tags
// to start from in submenus as well.
// TODO: TortoiseGit's left click opens Browse References straight away.
rap::menu log_window::ref_menu()
{
    const std::string head = repo_.head_name();
    const bool at_head = walk_options_.refs == walk_refs::head && walk_options_.rev.empty();
    auto pick = [this](walk_refs refs, std::string rev = {})
    {
        return [this, refs, rev]
        {
            set_walk(refs, rev);
            reload();
        };
    };
    auto mode = [&](std::string label, walk_refs refs)
    {
        return rap::menu_item{.label = std::move(label),
                              .action = pick(refs),
                              .checked = walk_options_.refs == refs,
                              .radio = true};
    };
    const bool fetch_head =
        run_git(repo_.workdir().empty() ? repo_.git_dir() : repo_.workdir(),
                {"rev-parse", "--verify", "-q", "FETCH_HEAD^{commit}"},
                git_output::stdout_only)
            .ok();
    rap::menu out{
        {.label = _("Browse References"),
         .action =
             [this]
         {
             // One ref to start from; the log walks from one [spec 1.9.1].
             const std::string initial = walk_options_.rev.empty() ? std::string() : walk_options_.rev;
             pick_ref(ui_, window_.get(), repo_.workdir().empty() ? repo_.git_dir() : repo_.workdir(),
                      {.initial = initial},
                      [this, alive = std::weak_ptr<bool>(alive_)](const std::string& picked)
                      {
                          if (!alive.lock() || picked.empty()) return;
                          set_walk(walk_refs::head, picked);
                          reload();
                      });
         }},
        {.label = head == "HEAD" ? "HEAD" : "HEAD -> \"" + menu_text(head) + "\"",
         .action = pick(walk_refs::head),
         .checked = at_head,
         .radio = true},
        {.label = "FETCH__HEAD",
         .action = pick(walk_refs::head, "FETCH_HEAD"),
         .enabled = fetch_head,
         .checked = walk_options_.refs == walk_refs::head && walk_options_.rev == "FETCH_HEAD",
         .radio = true},
        mode(_("All"), walk_refs::all),
        mode(_("All basic refs"), walk_refs::basic),
        mode(_("All local branches"), walk_refs::local),
    };

    // Each branch and tag, by kind, sorted by name.
    rap::menu local, remote, tags;
    std::vector<ref> refs;
    for (auto& [id, labels] : repo_.refs())
        for (const ref& r : labels)
            if (r.kind == ref_kind::local_branch || r.kind == ref_kind::remote_branch ||
                r.kind == ref_kind::tag)
                refs.push_back(r);
    std::ranges::sort(refs, {}, &ref::full_name);
    for (const ref& r : refs)
    {
        if (r.full_name.ends_with("/HEAD")) continue;  // origin/HEAD: the default branch's alias
        rap::menu_item item{.label = menu_text(r.name),
                            .action = pick(walk_refs::head, r.full_name),
                            .checked = walk_options_.refs == walk_refs::head &&
                                       walk_options_.rev == r.full_name,
                            .radio = true};
        (r.kind == ref_kind::local_branch ? local : r.kind == ref_kind::remote_branch ? remote : tags)
            .push_back(std::move(item));
    }
    if (!local.empty() || !remote.empty() || !tags.empty()) out.push_back(rap::menu_item::separator());
    if (!local.empty()) out.push_back({.label = _("_Branches"), .children = std::move(local)});
    if (!remote.empty()) out.push_back({.label = _("_Remote branches"), .children = std::move(remote)});
    if (!tags.empty()) out.push_back({.label = _("_Tags"), .children = std::move(tags)});

    // Recently picked, as TortoiseGit lists them under a separator.
    if (!ref_history_.empty()) out.push_back(rap::menu_item::separator());
    for (const std::string& r : ref_history_)
        out.push_back({.label = menu_text(r.size() > 150 ? r.substr(0, 150) : r),
                       .action = pick(walk_refs::head, r)});
    return out;
}

// Cue text lists the selected fields [spec 1.9.2].
std::string log_window::filter_cue() const
{
    static const std::pair<unsigned, const char*> fields[] = {
        {filter_subject, T_("Subject")}, {filter_messages, T_("Messages")}, {filter_paths, T_("Paths")},
        {filter_authors, T_("Authors")}, {filter_emails, T_("Emails")},     {filter_revs, T_("SHA-1")},
        {filter_refname, T_("Refname")}};
    std::string cue;
    for (auto [bit, name] : fields)
        if (filter_.fields & bit) cue += std::string(cue.empty() ? "" : ", ") + _(name);
    return "   " + rap::i18n::format(_("Filter by {0}"), cue);
}

rap::menu log_window::filter_menu()
{
    auto field = [this](const char* label, unsigned bit, bool enabled = true)
    {
        return toggle(
            label, filter_.fields & bit,
            [this, bit]
            {
                filter_.fields ^= bit;
                search_->set_placeholder(filter_cue());
                apply_filter();
            },
            enabled);
    };
    return {
        field(_("Subject"), filter_subject),
        field(_("Messages"), filter_messages),
        field(_("Paths"), filter_paths, false),
        field(_("Authors"), filter_authors),
        field(_("Emails"), filter_emails),
        field(_("SHA-1"), filter_revs),
        field(_("Refname"), filter_refname),
        rap::menu_item::separator(),
        {.label = _("Toggle filters"),
         .action =
             [this]
         {
             filter_.fields = ~filter_.fields & filter_all;
             search_->set_placeholder(filter_cue());
             apply_filter();
         }},
        {.label = _("All"),
         .action =
             [this]
         {
             filter_.fields = filter_all;
             search_->set_placeholder(filter_cue());
             apply_filter();
         }},
        rap::menu_item::separator(),
        toggle(_("Use regular expression"), filter_.regex,
               [this]
               {
                   filter_.regex = !filter_.regex;
                   apply_filter();
               }),
        toggle(_("Case-sensitive"), filter_.case_sensitive,
               [this]
               {
                   filter_.case_sensitive = !filter_.case_sensitive;
                   apply_filter();
               }),
    };
}

// Typing filters after a second; Enter filters at once [spec 1.9.2].
void log_window::schedule_filter()
{
    filter_timer_ = ui_.timer(1000, false, [this] { apply_filter(); });
}

void log_window::apply_filter()
{
    filter_timer_.reset();
    filter_.text = search_->text();
    std::optional<oid> keep;
    if (auto c = commit_list_->current(); c && log_.commit(*c)) keep = log_.commit(*c)->id;

    std::string error = log_.set_filter(filter_);
    if (!error.empty())
    {
        status_->set_text(rap::i18n::format(_("Invalid regular expression: {0}"), error));
        return;
    }
    if (keep)
        if (auto row = log_.row_of(*keep))
        {
            commit_list_->set_current(*row);
            commit_list_->scroll_to(*row);
        }
    update_status();
}

// Walk Behavior [spec 1.9.4]; drawn pushed while any option is on.
rap::menu log_window::walk_menu()
{
    auto flip = [this](bool walk_options::* field)
    {
        return [this, field]
        {
            walk_options_.*field = !(walk_options_.*field);
            walk_->set_pushed(walk_options_.first_parent || walk_options_.no_merges);
            reload();
        };
    };
    return {
        toggle(_("First Parent"), walk_options_.first_parent, flip(&walk_options::first_parent)),
        toggle(_("No _merges"), walk_options_.no_merges, flip(&walk_options::no_merges)),
        // TODO: the rest of the walk options.
        toggle(_("_Follow renames"), false, {}, false),
        toggle(_("Full history"), false, {}, false),
        rap::menu_item::separator(),
        toggle(_("_Compressed Graph"), false, {}, false),
        toggle(_("Show la_beled commits only"), false, {}, false),
    };
}

// View [spec 1.9.5]. Not TortoiseGit's Action Icons (Settings > General
// has them) or Gravatar (always shown).
rap::menu log_window::view_menu()
{
    auto label = [this](const char* text, unsigned bit)
    {
        return toggle(text, log_.labels() & bit,
                      [this, bit] { log_.set_labels(log_.labels() ^ bit); });
    };
    return {
        // Unrelated paths only exist once the log can be opened for a path.
        toggle(_("H_ide Unrelated Changed Paths"), false, {}, false),
        toggle(_("_Gray Unrelated Changed Paths"), true, {}, false),
        rap::menu_item::separator(),
        toggle(_("Show _Unversioned Files"), show_unversioned_,
               [this]
               {
                   show_unversioned_ = !show_unversioned_;
                   selection_changed();
               }),
        rap::menu_item::separator(),
        {.label = _("_Labels"),
         .children = {label(_("_Tags"), label_tags), label(_("_Local branches"), label_local),
                      label(_("_Remote branches"), label_remote), label(_("Other refs"), label_other)}},
        rap::menu_item::separator(),
        toggle(_("View Patch"), false, {}, false),
        rap::menu_item::separator(),
        {.label = _("_Settings..."), .action = [this] { open_settings(settings_page::general); }},
    };
}

void log_window::open_settings(settings_page page)
{
    show_settings(ui_, window_.get(), repo_.workdir(), page,
                  [this]
                  {
                      // What the pages change that's on show here.
                      const int style =
                          std::atoi(settings::get().value("log.actionIcons", "1").c_str());
                      log_.set_action_icon_style(action_icon_style(std::clamp(style, 0, 3)));
                  });
}

// Header right-click: reset, then a check item per column [spec 1.4].
rap::menu log_window::header_menu()
{
    auto columns = log_model::columns();
    rap::menu menu{{.label = _("Reset columns"),
                    .action =
                        [this]
                    {
                        auto defaults = log_model::columns();
                        std::vector<size_t> order(defaults.size());
                        std::iota(order.begin(), order.end(), size_t(0));
                        commit_list_->set_column_order(order);
                        for (size_t i = 0; i < defaults.size(); ++i)
                            commit_list_->set_column_visible(i, defaults[i].visible);
                    }},
                   rap::menu_item::separator()};
    for (size_t i = 0; i < columns.size(); ++i)
        menu.push_back(
            toggle(columns[i].title, commit_list_->column_visible(i), [this, i]
                   { commit_list_->set_column_visible(i, !commit_list_->column_visible(i)); }));
    return menu;
}

// From is clamped to To and back; changes restart the log [spec 1.9.3].
void log_window::dates_changed()
{
    rap::date from = from_->date(), to = to_->date();
    if (from > to)
    {
        to = from;
        to_->set_date(to);
    }
    since_ = local_time(from, false);
    until_ = local_time(to, true);
    reload();
}

// Up looks toward newer commits for the next row matching the selection
// by the chosen criterion; down the reverse [spec 1.9.6].
void log_window::jump(bool up)
{
    auto cur = commit_list_->current();
    if (!cur) return;
    auto type = jump_type(jump_type_->current());

    if (type == jump_history)
    {
        if (history_.empty()) return;
        if (up ? history_pos_ == 0 : history_pos_ + 1 >= history_.size()) return;
        history_pos_ += up ? -1 : 1;
        if (auto row = log_.row_of(history_[history_pos_]))
        {
            navigating_ = true;
            commit_list_->set_current(*row);
            commit_list_->scroll_to(*row);
            navigating_ = false;
        }
        return;
    }

    const commit* sel = log_.commit(*cur);
    if (!sel) return;
    auto has_ref = [&](size_t row, bool tags)
    {
        if (auto* refs = log_.refs_of(row))
            for (auto& r : *refs)
                if (tags ? r.kind == ref_kind::tag
                         : r.kind == ref_kind::local_branch || r.kind == ref_kind::remote_branch)
                    return true;
        return false;
    };
    auto matches = [&](size_t row)
    {
        const commit* c = log_.commit(row);
        if (!c) return false;
        auto parent_is = [](const commit& child, const commit& parent, size_t n)
        { return child.parents.size() > n && child.parents[n] == parent.id; };
        switch (type)
        {
        case jump_author_email: return c->email == sel->email;
        case jump_committer_email: return c->committer_email == sel->committer_email;
        case jump_merge_point: return c->parents.size() > 1;
        case jump_parent1: return up ? parent_is(*c, *sel, 0) : parent_is(*sel, *c, 0);
        case jump_parent2: return up ? parent_is(*c, *sel, 1) : parent_is(*sel, *c, 1);
        case jump_tag: return has_ref(row, true);
        case jump_branch: return has_ref(row, false);
        case jump_tag_ff:
        case jump_branch_ff:
            return has_ref(row, type == jump_tag_ff) &&
                   (up ? repo_.is_ancestor(sel->id, c->id) : repo_.is_ancestor(c->id, sel->id));
        case jump_history: break;
        }
        return false;
    };

    if (up)
    {
        for (size_t row = *cur; row-- > 0;)
            if (matches(row))
            {
                commit_list_->select({row});
                commit_list_->set_current(row);
                commit_list_->scroll_to(row);
                return;
            }
    }
    else
    {
        for (size_t row = *cur + 1; row < log_.row_count(); ++row)
            if (matches(row))
            {
                commit_list_->select({row});
                commit_list_->set_current(row);
                commit_list_->scroll_to(row);
                return;
            }
    }
    ui_.message(window_.get(), {.icon = rap::message_spec::icon::info,
                                .title = "Tortilla",
                                .text = _("No more revisions found.")});
}

}  // namespace tortilla
