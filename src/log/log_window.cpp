#include "log_window.h"

#include "branch/browse_refs.h"

#include "git/command.h"
#include "git/lfs.h"
#include "i18n.h"
#include "settings.h"
#include "ui/menu_text.h"
#include "ui/repo_icon.h"
#include "ui/settings_dialog.h"
#include "util/calendar.h"

#include <rapport/sys.h>

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <numeric>
#include <unordered_set>

namespace tortilla
{
namespace
{

// The author's picture, top right of the details, in logical px.
constexpr int k_avatar_size = 96;

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
log_window::open(rap::ui& ui, const std::string& path, bool all_branches, bool outer)
{
    std::string from = path;
    std::error_code ec;
    auto abs = std::filesystem::absolute(rap::sys::path_from_utf8(path), ec);
    if (outer) abs = (abs.has_filename() ? abs : abs.parent_path()).parent_path();
    // A path gone from the working tree (a file's name before a rename) is
    // looked for from the nearest folder that's still there.
    while (!std::filesystem::exists(abs, ec) && abs.has_parent_path() && abs.parent_path() != abs)
        abs = abs.parent_path();
    if (outer || abs != std::filesystem::absolute(rap::sys::path_from_utf8(path), ec))
        from = rap::sys::path_to_utf8(abs);
    auto repo = repo::open(from);
    if (!repo) return std::unexpected(repo.error());
    std::unique_ptr<log_window> w(new log_window(ui, std::move(*repo), path));
    w->build();
    if (all_branches) w->set_walk(walk_refs::all);
    // Before the first load: it knows the forge remotes, and so whether
    // there are pull requests to wait for.
    w->pr_watch_ = std::make_unique<pr_watch>(
        ui, w->repo_location(),
        [p = w.get()]
        {
            p->prs_known_ = true;
            p->update_pull_requests();
            p->tips_heard(p->pr_watch_->branch_tips());
        },
        allowed_now(check_setting()));
    w->reload();
    w->watch_repository();
    w->start_sync_watch();
    warn_if_lfs_missing(ui, w->window_.get(), w->repo_.workdir());
    return w;
}

log_window::log_window(rap::ui& ui, repo repo, std::string path)
    : ui_(ui)
    , repo_(std::move(repo))
    , path_(std::move(path))
    , log_(ui)
    // The window's repository, not path_'s (a submodule's folder in the
    // repository around it is the submodule's own repository).
    , files_worker_(std::make_unique<repo_worker>(ui, repo_location()))
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
    std::error_code ec;
    const auto root = rap::sys::path_from_utf8(repo_.workdir());
    const auto rel = std::filesystem::relative(
        std::filesystem::absolute(rap::sys::path_from_utf8(path_), ec), root, ec);
    if (!repo_.workdir().empty() && !ec && !rel.empty() && rel != "." && *rel.begin() != "..")
    {
        rel_path_ = rap::sys::path_to_utf8(rel);
        std::ranges::replace(rel_path_, '\\', '/');
    }
}

// The repository's folder, or the path the history is limited to [spec 1.9.7].
std::string log_window::title_text() const
{
    std::string title = repo_.workdir();
    if (title.size() > 1 && title.back() == '/') title.pop_back();
    if (path_limited()) title += "/" + rel_path_;
    title = rap::sys::path_to_utf8(rap::sys::path_from_utf8(title).make_preferred());
    if (picked_) return rap::i18n::format(_("{0} - {1} - Tortilla"), title, pick_purpose_);
    return rap::i18n::format(_("{0} - Log Messages - Tortilla"), title);
}

std::string log_window::repo_location() const
{
    return repo_.workdir().empty() ? repo_.git_dir() : repo_.workdir();
}

bool log_window::path_limited() const
{
    return !rel_path_.empty() && (!only_path_ || only_path_->checked());
}

// The log's path, a folder's with a "/" after it; elided in the middle past
// `limit` characters (0: never).
std::string log_window::path_text(size_t limit) const
{
    std::string text = rel_path_;
    std::error_code ec;
    if (std::filesystem::is_directory(
            rap::sys::path_from_utf8(repo_.workdir()) / rap::sys::path_from_utf8(rel_path_), ec))
        text += '/';
    if (limit && text.size() > limit)
    {
        // Whole UTF-8 characters either side of the ellipsis.
        size_t head = limit / 2 - 1, tail = text.size() - (limit - head - 1);
        while (head && (text[head] & 0xC0) == 0x80) --head;
        while (tail < text.size() && (text[tail] & 0xC0) == 0x80) ++tail;
        text = text.substr(0, head) + "…" + text.substr(tail);
    }
    return text;
}

void log_window::build()
{
    using namespace rap::layout;
    namespace a = rap;

    window_ = create_window(
        ui_,
        window{
            title_text(),
            {vbox{
                // Filter bar [spec 1.2, row 1]
                hbox{
                    // Sync, far left: its icon's badge says what the remotes
                    // have that the repository hasn't, or that a remote failed.
                    button{_("Fetch")}
                        .icon(fetch_icon())
                        .icon_only()
                        .bind(fetch_)
                        .on_click([this] { sync(); }),
                    menu_button{"HEAD"}
                        .bind(ref_button_)
                        .on_menu([this] { return show_menu(); })
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
                    drop_down{{_("Author Email"), _("Committer Email"), _("Merge Point"),
                               _("Parent 1"), _("Parent 2"), _("Tag"), _("Tag (FF)"), _("Branch"),
                               _("Branch (FF)"), _("Selection History")},
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
                // The current branch behind its upstream, after a sync.
                hbox{
                    label{}.bind(hint_text_).ellipsize().stretch(),
                    button{_("_Fast-forward")}.bind(hint_ff_).on_click([this]
                                                                       { fast_forward_current(); }),
                    button{_("_Pull...")}.bind(hint_pull_).on_click([this] { keep_pull_dialog(); }),
                    button{_("Hide")}.flat().on_click(
                        [this]
                        {
                            hint_hidden_ = current_.branch + ' ' + current_.upstream + ' ' +
                                           std::to_string(current_.behind);
                            hint_bar_->set_visible(false);
                        }),
                }
                    .handle(hint_bar_)
                    .visible(false),
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
                        // Commits dragged out, into a rebase's list or anywhere
                        // taking text: their hashes, newest first as the rows run.
                        .on_drag_out({.begin = [this](std::vector<size_t> rows)
                                          -> std::optional<rap::data_offer>
                                      {
                                          std::ranges::sort(rows);
                                          std::vector<std::string> hashes;
                                          for (size_t row : rows)
                                              if (const commit* c = log_.commit(row))
                                                  hashes.push_back(c->id.hex());
                                          if (hashes.empty()) return std::nullopt;
                                          const std::string data = commits_payload(hashes);
                                          return rap::data_offer{
                                              {{commits_type, data}, {"text/plain", data}}};
                                      }})
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
                                .width(rap::extent{k_avatar_size, rap::extent::unit::px})
                                .height(rap::extent{k_avatar_size, rap::extent::unit::px})
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
                hbox{
                    // A sync's red "!", while its warning is on show.
                    canvas{}
                        .focusable(false)
                        .bind(status_mark_)
                        .width(rap::extent{18, rap::extent::unit::px})
                        .visible(false)
                        .on_paint([](rap::painter& p, rap::size_f s) { paint_failure_mark(p, s); })
                        // Clicked: what git said.
                        .on_pointer(
                            [this](const rap::pointer_event& e)
                            {
                                if (e.type != rap::pointer_event_type::press || e.button != 1)
                                    return false;
                                show_sync_output();
                                return true;
                            }),
                    label{}.selectable().ellipsize().bind(status_).stretch(),
                },
                hbox{
                    vbox{
                        // TortoiseGit's Show Whole Project the other way up, to
                        // say what's in effect: ticked, the history of the
                        // path only. Per repository, saved as TortoiseGit's
                        // [spec 1.9.7]; only with a path to leave out.
                        check{rap::i18n::format(_("_Only changes to {0}"), path_text(40)),
                              settings::get().value("log.wholeProject." + repo_.workdir()) != "1"}
                            .bind(only_path_)
                            .visible(!rel_path_.empty())
                            .tooltip(path_text(0))
                            .on_toggled(
                                [this](bool only)
                                {
                                    settings::get().set("log.wholeProject." + repo_.workdir(),
                                                        only ? "0" : "1");
                                    window_->set_title(title_text());
                                    update_file_view();  // nothing is unrelated without a path
                                    reload();
                                }),
                    },
                    // The selected commit's files, by path: a second after
                    // typing stops, at once on Enter, cleared by Escape (the
                    // window's keys) [spec 1.11.2].
                    entry{}
                        .bind(path_filter_)
                        .placeholder(std::string("   ") + _("Filter paths"))
                        .on_changed(
                            [this](const std::string&)
                            {
                                path_filter_timer_ =
                                    ui_.timer(1000, false, [this] { update_file_view(); });
                            })
                        .on_activate([this] { update_file_view(); })
                        .stretch(),
                },
                hbox{
                    button{_("S_tatistics")}.bind(statistics_).enabled(false),
                    progress_bar{}.bind(progress_).stretch(),
                    menu_button{_("Walk Be_havior")}.bind(walk_).on_menu([this]
                                                                         { return walk_menu(); }),
                    menu_button{_("_View")}.bind(view_).on_menu([this] { return view_menu(); }),
                    spacer{},
                    button{_("OK")}.bind(ok_).on_click(
                        [this]
                        {
                            if (picked_)
                            {
                                std::vector<commit> chosen;
                                for (size_t row : commit_list_->selection())
                                    if (const commit* c = log_.commit(row);
                                        c && c->parents.size() <= 1)
                                        chosen.push_back(*c);
                                picked_(std::move(chosen));
                            }
                            closing();
                            window_->close();
                            if (closed_) closed_();
                        }),
                    button{_("Cancel")}.bind(cancel_).on_click(
                        [this]
                        {
                            closing();
                            window_->close();
                            if (closed_) closed_();
                        }),
                },
            }}}
            .size(140_ch, 50_lines));

    window_->on_close_request(
        [this]
        {
            closing();
            if (closed_) closed_();
            return true;
        });
    // F5 reloads [TortoiseGit: Refresh], e.g. after git ran elsewhere.
    window_->on_key(
        [this](const rap::key_event& e)
        {
            if (!e.press || e.modifiers) return false;
            // Escape in the path filter box clears it [spec 1.11.2].
            if (e.key == rap::key::escape && path_filter_->has_focus() &&
                !path_filter_->text().empty())
            {
                path_filter_->set_text("");
                update_file_view();
                return true;
            }
            if (e.key != rap::key::f5) return false;
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
    cancel_->set_visible(false);  // picking commits only
    // Today until the loaded range replaces it (not 1970 while loading).
    from_->set_date(local_date(std::time(nullptr)));
    to_->set_date(local_date(std::time(nullptr)));
    update_ref_button();
    if (auto saved = settings::get().value("log.actionIcons"); !saved.empty())
        log_.set_action_icon_style(action_icon_style(std::clamp(std::atoi(saved.c_str()), 0, 3)));
    restore_layout();
}

void log_window::closing()
{
    save_layout();
    if (!patch_) return;
    patch_viewer p = std::move(*patch_);
    patch_.reset();  // first, so its closing leaves the setting on
    p.close();
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

    // Options. What's shown is per repository, as All Branches is in
    // TortoiseGit [spec 1.9.7]; the rest apply everywhere.
    auto flag = [](bool b) { return std::string(b ? "1" : "0"); };
    save_branches();
    std::string history;
    for (const std::string& r : ref_history_) history += (history.empty() ? "" : " ") + r;
    st.set("log.refHistory." + repo_.workdir(), history);
    st.set("log.walk.firstParent", flag(walk_options_.first_parent));
    st.set("log.walk.noMerges", flag(walk_options_.no_merges));
    st.set("log.walk.fullHistory", flag(walk_options_.full_history));
    st.set("log.walk.follow", flag(walk_options_.follow));
    st.set("log.view.labels", std::to_string(log_.labels()));
    st.set("log.view.unversioned", flag(show_unversioned_));
    st.set("log.view.unrelated", unrelated_ == file_view::hide_unrelated   ? "hide"
                                 : unrelated_ == file_view::gray_unrelated ? "gray"
                                                                           : "show");
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
    restore_branches();
    walk_options_.first_parent = flag("log.walk.firstParent", false);
    walk_options_.no_merges = flag("log.walk.noMerges", false);
    walk_options_.full_history = flag("log.walk.fullHistory", false);
    walk_options_.follow = flag("log.walk.follow", true);
    update_walk_button();
    update_ref_button();
    log_.set_labels(unsigned(number("log.view.labels", label_all)) & label_all);
    show_unversioned_ = flag("log.view.unversioned", true);
    const std::string unrelated = st.value("log.view.unrelated");
    unrelated_ = unrelated == "hide"   ? file_view::hide_unrelated
                 : unrelated == "show" ? file_view::show_unrelated
                                       : file_view::gray_unrelated;
    update_file_view();
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
    // Not refs/prefetch: the sync's downloads ahead of a click (git/sync.h)
    // change nothing the log shows.
    const auto prefetch = common_dir_ / "refs" / "prefetch";
    for (auto it = std::filesystem::recursive_directory_iterator(common_dir_ / "refs", ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
    {
        if (it->path() == prefetch)
        {
            it.disable_recursion_pending();
            continue;
        }
        add(it->path());
    }
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
    status_->set_text(show_summary() + " — " + _("Loading…"));

    // A ref picked to start from that has since gone: back to HEAD. (A
    // range, a..b, is no one rev to --verify.)
    const std::string& rev = walk_options_.rev;
    if (!rev.empty() &&
        !run_git(repo_.workdir().empty() ? repo_.git_dir() : repo_.workdir(),
                 rev.find("..") != std::string::npos
                     ? std::vector<std::string>{"rev-parse", "-q", rev}
                     : std::vector<std::string>{"rev-parse", "--verify", "-q", rev + "^{commit}"},
                 git_output::stdout_only)
             .ok())
        set_walk(walk_refs::head);

    apply_follow();
    walk_options opts = walk_options_;
    opts.since = since_;
    opts.until = until_;
    if (path_limited()) opts.paths = {rel_path_};
    opts.follow = walk_options_.follow && one_file();
    apply_branches();
    log_.load(repo_location(), opts, [this](const std::string& e) { loaded(e); });
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
    // View Patch as it was left, once the log is there to follow; not
    // while picking commits for another window.
    if (!patch_restored_)
    {
        patch_restored_ = true;
        if (!picked_ && settings::get().value("log.viewPatch") == "1") show_patch(true);
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
    update_pull_requests();
    // The refs may have moved: what a sync would bring, and the current
    // branch against its upstream.
    recheck_changes();
    update_branch_hint();
}

void log_window::update_pull_requests()
{
    if (!pr_watch_) return;
    // Ones set to merge when ready that have gone: merged, so fetched.
    std::erase_if(awaiting_merge_,
                  [this](const std::pair<std::string, int>& a)
                  {
                      if (!pr_watch_->error().empty()) return false;
                      const bool open = std::ranges::any_of(
                          pr_watch_->pull_requests(), [&](const open_pr& p)
                          { return p.into.name == a.first && p.pr.number == a.second; });
                      if (!open) fetch_after_forge(a.first);
                      return !open;
                  });
    pr_heads_ = match_pull_requests(pr_watch_->remotes(), pr_watch_->pull_requests());
    std::unordered_map<std::string, pull_request> prs;
    for (const auto& [name, p] : pr_heads_) prs.emplace(name, p.pr);
    log_.set_pull_requests(std::move(prs));

    // The pull requests' branches changed: walked again if they're on show.
    const pr_branches& before = log_.branches().prs;
    const pr_branches now = pull_request_branches(pr_heads_, pr_watch_->pull_requests(),
                                                  prs_known_ || pr_watch_->remotes().empty());
    const bool uses =
        show_.preset == show_preset::pull_requests || show_.preset == show_preset::mine;
    if (uses && !log_.loading() &&
        (now.known != before.known || now.all != before.all || now.mine != before.mine))
        reload();
}

const open_pr* log_window::pull_request_of(const ref& r) const
{
    // A local branch's is its upstream's, for the menu.
    auto it = pr_heads_.find(r.kind == ref_kind::local_branch ? r.upstream : r.full_name);
    return it == pr_heads_.end() ? nullptr : &it->second;
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
        show_avatar(nullptr);
        files_.clear();
    }
    update_status();
    update_pick_button();
    update_patch();
}

void log_window::update_pick_button()
{
    if (!picked_) return;
    bool any = false;
    for (size_t row : commit_list_->selection())
        if (const commit* c = log_.commit(row); c && c->parents.size() <= 1) any = true;
    ok_->set_enabled(any);
}

// The picture at the canvas's size in device pixels, so it's drawn 1:1.
void log_window::show_avatar(const commit* c)
{
    const float scale = avatar_canvas_ ? avatar_canvas_->device_scale() : 1;
    const int px = int(std::lround(k_avatar_size * std::max(scale, 1.f)));
    bool answered = false;
    auto show = [this](std::shared_ptr<rap::image> image)
    {
        if (image == avatar_) return;
        avatar_ = std::move(image);
        if (avatar_canvas_) avatar_canvas_->invalidate();
    };
    avatars::author who;
    if (c)
    {
        who = {.email = c->email, .commit = c->id.hex(), .workdir = repo_location()};
        // Not those that can't be seen: asking would only spend an allowance.
        if (pr_watch_)
            for (const forge_remote& r : pr_watch_->remotes())
                if (pr_watch_->access(r) != repo_access::hidden) who.remotes.push_back(r);
    }
    avatars_.request(std::move(who), px,
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
        show_avatar(nullptr);
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
    show_avatar(&c);

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
        [this, id, text, spans](repo& repo,
                                const repo_worker::progress& progress) -> std::function<void()>
        {
            // The note under the message [spec 1.13], read here so the click
            // never waits on it.
            if (std::string note = repo.note(id); !note.empty())
                progress.post(
                    [this, text, spans, note]() mutable
                    {
                        const std::string label = _("Notes:");
                        text += "\n";
                        spans.push_back({text.size(), text.size() + label.size(), {.bold = true}});
                        text += label + "\n" + note;
                        details_->set_text(text, spans);
                    });
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
    // A sync's warning first, for its while; then what's applied, so a
    // filtered log isn't taken for the whole.
    s = show_summary() + " — " + s;
    if (!notice_.empty()) s = rap::i18n::format(_("{0} - {1}"), notice_, s);
    status_->set_text(s);
}

// The ref selector shows the current ref, or a mode [spec 1.9.1].
void log_window::pick_commits(const std::string& purpose, const std::string& label,
                              std::function<void(std::vector<commit>)> chosen)
{
    picked_ = std::move(chosen);
    pick_purpose_ = purpose;
    ok_->set_text(label);
    cancel_->set_visible(true);
    window_->set_title(title_text());
    update_pick_button();
}

void log_window::start_at(const std::string& rev)
{
    set_walk(walk_refs::head, rev);
    reload();
}

void log_window::select_commit(const std::string& rev)
{
    const auto id = repo_.resolve(rev);
    if (!id) return;
    kept_ = kept_selection{.commits = {*id}, .current = *id};
    if (!log_.loading()) restore_selection();
}

void log_window::set_walk(walk_refs refs, std::string rev)
{
    if (!rev.empty())
    {
        std::erase(ref_history_, rev);
        ref_history_.insert(ref_history_.begin(), rev);
        if (ref_history_.size() > 5) ref_history_.resize(5);
    }
    set_show(refs == walk_refs::head    ? show_preset::current
             : refs == walk_refs::local ? show_preset::local
                                        : show_preset::all);
    walk_options_.rev = std::move(rev);
    update_ref_button();
}

void log_window::set_show(show_preset preset)
{
    if (preset == show_preset::picked)
    {
        if (show_.preset != show_preset::picked) before_pick_ = show_.preset;
    }
    else
        before_pick_.reset();
    show_.preset = preset;
    // The model walks the preset's refs; the window's own walk is HEAD's,
    // or the ref picked to start from.
    walk_options_.refs = walk_refs::head;
    walk_options_.rev.clear();
    update_ref_button();
}

std::string log_window::branches_key(const char* what, bool shared) const
{
    return std::string("log.") + what + "." + (shared ? repo_.common_dir() : repo_.workdir());
}

namespace
{

// Ref names and hide patterns have no spaces.
std::vector<std::string> split_words(std::string_view text)
{
    std::vector<std::string> out;
    for (size_t at = 0; at < text.size();)
    {
        size_t space = text.find(' ', at);
        if (space == std::string_view::npos) space = text.size();
        if (space > at) out.emplace_back(text.substr(at, space - at));
        at = space + 1;
    }
    return out;
}

std::string join_words(const std::vector<std::string>& words)
{
    std::string out;
    for (const std::string& w : words) out += (out.empty() ? "" : " ") + w;
    return out;
}

// Saved for "no pins": a ref's full name starts with refs/ (or is HEAD).
constexpr std::string_view k_no_pins = "none";

}  // namespace

void log_window::save_branches()
{
    settings& st = settings::get();
    const show_preset preset = refs_before_follow_.value_or(before_pick_.value_or(show_.preset));
    st.set(branches_key("show", false), std::string(preset_key(preset)));
    st.set(branches_key("tipsOnly", false), show_.tips_only ? "1" : "0");
    st.set(branches_key("unmerged", false), show_.unmerged ? "1" : "0");
    if (pins_custom_)
        st.set(branches_key("pins", true),
               show_.pins.empty() ? std::string(k_no_pins) : join_words(show_.pins));
    st.set(branches_key("hidden", true), join_words(show_.hidden));
}

void log_window::restore_branches()
{
    settings& st = settings::get();
    std::string preset = st.value(branches_key("show", false));
    if (preset.empty())
    {
        // Before the Show menu: the All Branches box, "1" checked, "basic"
        // or "local" for the ref selector's modes.
        const std::string all = st.value("log.allBranches." + repo_.workdir());
        preset = all == "1" || all == "basic" ? "all" : all == "local" ? "local" : "current";
    }
    show_preset p = preset_from_key(preset);
    if (p == show_preset::picked) p = show_preset::current;  // a set picked is for once
    set_show(p);
    show_.tips_only = st.value(branches_key("tipsOnly", false)) == "1";
    show_.unmerged = st.value(branches_key("unmerged", false)) == "1";
    const std::string pins = st.value(branches_key("pins", true));
    pins_custom_ = !pins.empty();
    show_.pins = pins == k_no_pins ? std::vector<std::string>{} : split_words(pins);
    show_.hidden = split_words(st.value(branches_key("hidden", true)));
}

void log_window::apply_branches()
{
    branch_request r;
    r.filter = show_;
    // Until the user changes them: HEAD and each remote's default branch,
    // as they are now.
    if (!pins_custom_) r.filter.pins = show_.pins = default_pins(repo_);
    if (pr_watch_)
    {
        r.prs = pull_request_branches(pr_heads_, pr_watch_->pull_requests(),
                                      prs_known_ || pr_watch_->remotes().empty());
        r.remotes = pr_watch_->remotes();
    }
    log_.set_branches(std::move(r));
}

std::string log_window::show_summary() const
{
    const branch_outcome& o = log_.outcome();
    std::string s;
    switch (show_.preset)
    {
    case show_preset::current:
        s = walk_options_.rev.empty() ? std::string(_("Current branch"))
                                      : rap::i18n::format(_("From {0}"), walk_options_.rev);
        break;
    case show_preset::all: s = _("All branches"); break;
    case show_preset::local: s = _("Local branches"); break;
    case show_preset::pull_requests:
        s = o.waiting ? _("Pull requests (waiting for the forge: the pins only)")
            : o.none  ? _("Pull requests (none open: the pins only)")
                      : _("Pull requests");
        break;
    case show_preset::mine:
        s = o.none ? _("My branches (none found: the pins only)") : _("My branches");
        break;
    case show_preset::picked:
        s = rap::i18n::format(N_("{0} branch picked", "{0} branches picked", show_.picked.size()),
                              show_.picked.size());
        break;
    }
    if (o.pins_added)
        s += rap::i18n::format(N_(" + {0} pinned", " + {0} pinned", o.pins_added), o.pins_added);
    if (show_.tips_only) s += std::string(" · ") + _("tips only");
    if (show_.unmerged)
        s += " · " + (o.base.empty() ? std::string(_("nothing to call merged"))
                                     : rap::i18n::format(_("not merged into {0}"), o.base));
    if (o.hidden)
        s += " · " + rap::i18n::format(N_("{0} hidden", "{0} hidden", o.hidden), o.hidden);
    if (path_limited()) s = rap::i18n::format(_("Only {0}"), path_text(40)) + " · " + s;
    return s;
}

void log_window::update_ref_button()
{
    std::string text, full;
    switch (show_.preset)
    {
    case show_preset::all: text = _("All branches"); break;
    case show_preset::local: text = _("Local branches"); break;
    case show_preset::pull_requests: text = _("Pull requests"); break;
    case show_preset::mine: text = _("My branches"); break;
    case show_preset::picked:
        text = rap::i18n::format(N_("{0} branch", "{0} branches", show_.picked.size()),
                                 show_.picked.size());
        for (const std::string& p : show_.picked) full += (full.empty() ? "" : "\n") + p;
        break;
    case show_preset::current:
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

// What the log shows: a preset, two toggles that go with any of them, the
// pins and hide patterns, which labels are drawn, and (TortoiseGit's ref
// menu [spec 1.9.1]) a revision to start from instead.
rap::menu log_window::show_menu()
{
    const bool following = walk_options_.follow && one_file();
    auto preset = [&](std::string label, show_preset p, bool checked)
    {
        return rap::menu_item{.label = std::move(label),
                              .action =
                                  [this, p]
                              {
                                  set_show(p);
                                  reload();
                              },
                              .enabled = !following || p == show_preset::current,
                              .checked = checked,
                              .radio = true};
    };
    auto flip = [this](bool branch_filter::* field)
    {
        return [this, field]
        {
            show_.*field = !(show_.*field);
            reload();
        };
    };
    auto label = [this](const char* text, unsigned bit)
    {
        return toggle(text, log_.labels() & bit,
                      [this, bit] { log_.set_labels(log_.labels() ^ bit); });
    };
    const bool at_head = show_.preset == show_preset::current && walk_options_.rev.empty();
    return {
        preset(_("_Current branch"), show_preset::current, at_head),
        preset(_("_All branches"), show_preset::all, show_.preset == show_preset::all),
        preset(_("_Local branches"), show_preset::local, show_.preset == show_preset::local),
        preset(_("_Pull requests"), show_preset::pull_requests,
               show_.preset == show_preset::pull_requests),
        preset(_("_My branches"), show_preset::mine, show_.preset == show_preset::mine),
        {.label = _("P_ick..."),
         .action = [this] { pick_branches(); },
         .enabled = !following,
         .checked = show_.preset == show_preset::picked,
         .radio = true},
        rap::menu_item::separator(),
        toggle(_("Branch _tips only"), show_.tips_only, flip(&branch_filter::tips_only),
               !following),
        toggle(_("Only what's not _merged"), show_.unmerged, flip(&branch_filter::unmerged),
               !following),
        rap::menu_item::separator(),
        {.label = _("Pins and _hidden..."), .action = [this] { edit_pins_and_hidden(); }},
        {.label = _("La_bels"),
         .children = {label(_("_Tags"), label_tags), label(_("_Local branches"), label_local),
                      label(_("_Remote branches"), label_remote),
                      label(_("Other refs"), label_other)}},
        rap::menu_item::separator(),
        {.label = _("_Start from"), .children = start_menu()},
    };
}

// One revision to start from, as TortoiseGit's ref menu has them [spec
// 1.9.1], the branches and tags in submenus.
// TODO: TortoiseGit's left click opens Browse References straight away.
rap::menu log_window::start_menu()
{
    const std::string head = repo_.head_name();
    const bool at_head = show_.preset == show_preset::current && walk_options_.rev.empty();
    auto pick = [this](std::string rev = {})
    {
        return [this, rev]
        {
            set_walk(walk_refs::head, rev);
            reload();
        };
    };
    auto from = [&](const std::string& rev)
    { return show_.preset == show_preset::current && walk_options_.rev == rev; };
    // In process: this menu opens on a click (git would start twice).
    const bool fetch_head = repo_.resolve("FETCH_HEAD").has_value();
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
         .action = pick(),
         .checked = at_head,
         .radio = true},
        {.label = "FETCH__HEAD",
         .action = pick("FETCH_HEAD"),
         .enabled = fetch_head,
         .checked = from("FETCH_HEAD"),
         .radio = true},
    };

    // Each branch and tag, by kind, sorted by name.
    rap::menu local, remote, tags;
    std::vector<ref> refs;
    const ref_map loaded = log_.refs().empty() ? repo_.refs() : ref_map{};
    for (auto& [id, labels] : log_.refs().empty() ? loaded : log_.refs())
        for (const ref& r : labels)
            if (r.kind == ref_kind::local_branch || r.kind == ref_kind::remote_branch ||
                r.kind == ref_kind::tag)
                refs.push_back(r);
    std::ranges::sort(refs, {}, &ref::full_name);
    for (const ref& r : refs)
    {
        if (r.full_name.ends_with("/HEAD")) continue;  // origin/HEAD: the default branch's alias
        rap::menu_item item{.label = menu_text(r.name),
                            .action = pick(r.full_name),
                            .checked = from(r.full_name),
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
        out.push_back(
            {.label = menu_text(r.size() > 150 ? r.substr(0, 150) : r), .action = pick(r)});
    return out;
}

// Cue text lists the selected fields [spec 1.9.2].
std::string log_window::filter_cue() const
{
    static const std::pair<unsigned, const char*> fields[] = {
        {filter_subject, T_("Subject")}, {filter_messages, T_("Messages")},
        {filter_authors, T_("Authors")}, {filter_emails, T_("Emails")},
        {filter_revs, T_("SHA-1")},      {filter_refname, T_("Refname")}};
    std::string cue;
    for (auto [bit, name] : fields)
        if (filter_.fields & bit) cue += std::string(cue.empty() ? "" : ", ") + _(name);
    return "   " + rap::i18n::format(_("Filter by {0}"), cue);
}

rap::menu log_window::filter_menu()
{
    auto field = [this](const char* label, unsigned bit)
    {
        return toggle(label, filter_.fields & bit,
                      [this, bit]
                      {
                          filter_.fields ^= bit;
                          search_->set_placeholder(filter_cue());
                          apply_filter();
                      });
    };
    return {
        field(_("Subject"), filter_subject),
        field(_("Messages"), filter_messages),
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

// Unrelated files are those outside the log's path, so there are none in a
// whole-project log [spec 1.9.5]. Only the list's rows change: the files
// are kept, so no git runs.
void log_window::update_file_view()
{
    path_filter_timer_.reset();
    files_.set_view({.related = path_limited() ? rel_path_ : std::string(),
                     .unrelated = unrelated_,
                     .filter = path_filter_ ? path_filter_->text() : std::string()});
    update_status();
}

// Walk Behavior [spec 1.9.4]; drawn pushed while any option is on.
// Pushed while any walk option is changed from its default.
void log_window::update_walk_button()
{
    walk_->set_pushed(walk_options_.first_parent || walk_options_.no_merges ||
                      (path_limited() && walk_options_.full_history) ||
                      (one_file() && !walk_options_.follow));
}

// Following renames, on by default, is for one file's log walked from HEAD
// [spec 1.9.4]: another preset is set aside meanwhile (the Show menu greys
// them), and so are the pins.
void log_window::apply_follow()
{
    const bool following = walk_options_.follow && one_file();
    if (following && show_.preset != show_preset::current)
    {
        refs_before_follow_ = before_pick_.value_or(show_.preset);
        set_show(show_preset::current);
    }
}

// A log of one file (not a folder), which renames can be followed for.
bool log_window::one_file() const
{
    std::error_code ec;
    return path_limited() &&
           std::filesystem::is_regular_file(
               rap::sys::path_from_utf8(repo_.workdir()) / rap::sys::path_from_utf8(rel_path_), ec);
}

rap::menu log_window::walk_menu()
{
    auto flip = [this](bool walk_options::* field)
    {
        return [this, field]
        {
            walk_options_.*field = !(walk_options_.*field);
            update_walk_button();
            reload();
        };
    };
    // Follow renames is for one file's log [spec 1.9.4] (apply_follow).
    const bool file = one_file();
    const bool following = file && walk_options_.follow;
    return {
        toggle(_("First Parent"), walk_options_.first_parent, flip(&walk_options::first_parent)),
        toggle(_("No _merges"), walk_options_.no_merges, flip(&walk_options::no_merges)),
        toggle(
            _("_Follow renames"), following,
            [this, following]
            {
                walk_options_.follow = !following;
                if (!walk_options_.follow && refs_before_follow_)
                {
                    set_show(*refs_before_follow_);
                    refs_before_follow_.reset();
                }
                update_walk_button();
                reload();
            },
            file),
        // Without history simplification: merges that left the path as one
        // side had it are shown too.
        toggle(_("Full history"), path_limited() && walk_options_.full_history,
               flip(&walk_options::full_history), path_limited()),
        // (TortoiseGit's Compressed Graph and Show labeled commits only are
        // the Show menu's Branch tips only: the labels, and the forks and
        // merges between them.)
    };
}

// View [spec 1.9.5]. Not TortoiseGit's Action Icons (Settings > General
// has them) or Gravatar (always shown); its Labels are the Show menu's.
rap::menu log_window::view_menu()
{
    // Hide and Gray exclude each other, and both can be off: choosing the
    // one that's on turns it off.
    auto unrelated = [this](const char* text, file_view::unrelated_mode mode)
    {
        return toggle(text, unrelated_ == mode,
                      [this, mode]
                      {
                          unrelated_ = unrelated_ == mode ? file_view::show_unrelated : mode;
                          update_file_view();
                      });
    };
    return {
        unrelated(_("H_ide Unrelated Changed Paths"), file_view::hide_unrelated),
        unrelated(_("_Gray Unrelated Changed Paths"), file_view::gray_unrelated),
        rap::menu_item::separator(),
        toggle(_("Show _Unversioned Files"), show_unversioned_,
               [this]
               {
                   show_unversioned_ = !show_unversioned_;
                   selection_changed();
               }),
        rap::menu_item::separator(),
        toggle(_("View _Patch"), patch_.has_value(), [this] { show_patch(!patch_); }),
        rap::menu_item::separator(),
        {.label = _("_Settings..."), .action = [this] { open_settings(settings_page::general); }},
    };
}

void log_window::show_patch(bool on)
{
    settings::get().set("log.viewPatch", on ? "1" : "0");
    if (on == patch_.has_value()) return;
    if (!on)
    {
        patch_viewer p = std::move(*patch_);
        patch_.reset();
        return p.close();
    }
    // Closed by the user: off, and so next time. (Closed with the log, or
    // from the menu, patch_ has gone already.)
    patch_ = open_patch_viewer(ui_, _("View Patch"),
                               [this, alive = std::weak_ptr<bool>(alive_)]
                               {
                                   if (!alive.lock() || !patch_) return;
                                   patch_.reset();
                                   settings::get().set("log.viewPatch", "0");
                               });
    update_patch();
}

// One commit's patch as git show has it, or the working tree's changes; a
// log of a path, that path's part. Nothing for several. git runs off the
// UI thread, a moment after the selection settles.
void log_window::update_patch()
{
    if (!patch_) return;
    patch_timer_ = ui_.timer(
        100, false,
        [this]
        {
            if (!patch_) return;
            const auto sel = commit_list_->selection();
            std::string title = _("View Patch");
            std::vector<std::string> args;
            if (sel.size() == 1 && log_.is_working_tree(sel.front()))
            {
                title = _("Working tree changes");
                args = {"diff", "HEAD"};
            }
            else if (const commit* c = sel.size() == 1 ? log_.commit(sel.front()) : nullptr)
            {
                title = c->id.hex(8) + " " + c->summary;
                args = {"show", "--format=fuller", c->id.hex()};
            }
            if (!args.empty() && path_limited()) args.insert(args.end(), {"--", rel_path_});
            const unsigned gen = ++patch_generation_;
            if (args.empty()) return patch_->show(title, {});
            std::thread(
                [this, &ui = ui_, gen, title, args, alive = std::weak_ptr<bool>(alive_),
                 dir = repo_.workdir().empty() ? repo_.git_dir() : repo_.workdir()]
                {
                    auto r =
                        std::make_shared<git_result>(run_git(dir, args, git_output::stdout_only));
                    ui.post(
                        [this, gen, title, r, alive]
                        {
                            if (!alive.lock() || !patch_ || gen != patch_generation_) return;
                            patch_->show(title, r->ok() ? std::move(r->output) : std::string());
                        });
                })
                .detach();
        });
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
                      if (pr_watch_) pr_watch_->want_branch_tips(allowed_now(check_setting()));
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
