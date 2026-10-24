#include "changes_window.h"

#include "changes/commit_window.h"
#include "i18n.h"
#include "settings.h"
#include "ui/diff_window.h"
#include "ui/progress_dialog.h"
#include "ui/settings_dialog.h"
#include "ui/stash_dialog.h"

#include <rapport/sys.h>

#include <algorithm>
#include <filesystem>

namespace tortilla
{
namespace
{

namespace fs = std::filesystem;

// Settings: Show unversioned files is the Log's View option too, as in
// TortoiseGit (AddBeforeCommit) [spec 5.3.3].
constexpr const char* k_unversioned = "log.view.unversioned";
constexpr const char* k_all_staged = "changes.allStaged";
std::string whole_project_key(const std::string& workdir)
{
    return "changes.wholeProject." + workdir;
}

bool flag(const std::string& key, bool fallback)
{
    const std::string v = settings::get().value(key);
    return v.empty() ? fallback : v == "1";
}

void set_flag(const std::string& key, bool on)
{
    settings::get().set(key, on ? "1" : "0");
}

}  // namespace

std::expected<std::unique_ptr<changes_window>, std::string>
changes_window::open(rap::ui& ui, const std::vector<std::string>& paths)
{
    const std::string first = paths.empty() ? "." : paths.front();
    auto repo = repo::open(first);
    if (!repo) return std::unexpected(repo.error());
    if (repo->bare()) return std::unexpected(_("No working directory found."));
    // Repository-relative; the root itself becomes "" (the whole project).
    std::error_code ec;
    const fs::path root = fs::weakly_canonical(rap::sys::path_from_utf8(repo->workdir()), ec);
    std::vector<std::string> relative;
    for (const std::string& p : paths.empty() ? std::vector<std::string>{"."} : paths)
    {
        fs::path abs = fs::weakly_canonical(fs::absolute(rap::sys::path_from_utf8(p), ec), ec);
        std::string rel = rap::sys::path_to_utf8(abs.lexically_relative(root).generic_string());
        if (rel == ".") rel.clear();
        relative.push_back(rel);
    }
    std::unique_ptr<changes_window> w(
        new changes_window(ui, std::move(*repo), std::move(relative)));
    w->build();
    w->reload();
    return w;
}

changes_window::changes_window(rap::ui& ui, repo r, std::vector<std::string> relative)
    : ui_(ui)
    , repo_(std::move(r))
    , workdir_(repo_.workdir())
    , worker_(std::make_unique<repo_worker>(ui, workdir_))
    , files_({.ui = ui,
              .window = [this] { return window_.get(); },
              .workdir = workdir_,
              .run =
                  [this](const std::string& title, std::vector<std::vector<std::string>> commands)
              {
                  run_progress_dialog(ui_, window_.get(), title, workdir_, std::move(commands),
                                      [this](bool) { reload(); });
              },
              .refresh = [this] { reload(); },
              .open_settings = [this](settings_page page)
              { show_settings(ui_, window_.get(), workdir_, page); },
              .selection_changed = {}})
{
    root_ = std::ranges::any_of(relative, [](const std::string& p) { return p.empty(); });
    if (!root_) paths_ = std::move(relative);
}

changes_window::~changes_window()
{
    commit_window_.reset();
    if (load_) load_->cancel();  // so the worker's thread isn't left waiting on git
}

void changes_window::build()
{
    using namespace rap::layout;
    using namespace rap::literals;
    // Show unversioned and Show ignored only make sense for one folder [spec 5.3.3].
    one_folder_ =
        root_ || (paths_.size() == 1 && fs::is_directory(rap::sys::path_from_utf8(workdir_) /
                                                         rap::sys::path_from_utf8(paths_.front())));
    const bool one_folder = one_folder_;
    auto close = [this]
    {
        settings::get().save_window("changes.window", *window_);
        files_.save_layout();
        if (closed_) closed_();
    };
    window_ = ui_.create(window{
        _("Working Tree"),
        {vbox{
            label{}.ellipsize().bind(branch_),
            files_.list("changes.files").stretch(),
            label{}.bind(note_),
            hbox{
                vbox{
                    check{_("Show un_versioned files"), flag(k_unversioned, true)}
                        .bind(unversioned_)
                        .enabled(one_folder)
                        .on_toggled(
                            [this](bool on)
                            {
                                set_flag(k_unversioned, on);
                                reload();
                            }),
                    check{_("Show ignore local changes flagged files")}
                        .bind(local_ignored_)
                        .on_toggled([this](bool) { reload(); }),
                    check{_("Show i_gnored files")}
                        .bind(ignored_)
                        .enabled(one_folder)
                        .on_toggled([this](bool) { reload(); }),
                    check{_("Show all _staged files"), flag(k_all_staged, true)}
                        .bind(all_staged_)
                        .on_toggled(
                            [this](bool on)
                            {
                                set_flag(k_all_staged, on);
                                reload();
                            }),
                    // At the repository's root it's the whole project anyway.
                    check{_("Show _Whole Project"), root_ || flag(whole_project_key(workdir_), false)}
                        .bind(whole_project_)
                        .enabled(!root_)
                        .on_toggled(
                            [this](bool on)
                            {
                                set_flag(whole_project_key(workdir_), on);
                                reload();
                            }),
                },
                spacer{},
                label{}.bind(stats_),
            },
            hbox{
                spacer{},
                button{_("Save unified diff")}.on_click([this] { show_unified_diff_of_paths(); }),
                menu_button{_("Stash")}.on_menu([this] { return stash_menu(); }),
                button{_("Commit")}.on_click([this] { open_commit(); }),
                button{_("_Refresh")}.bind(refresh_).on_click([this] { reload(); }),
                // Always OK, closing even mid-load (TortoiseGit relabels it
                // Cancel while loading, which only flickers).
                button{_("OK")}.on_click(
                    [this, close]
                    {
                        close();
                        window_->close();
                    }),
            },
        }}}.size(110_ch, 32_lines));

    window_->on_close_request(
        [close]
        {
            close();
            return true;
        });
    // F5 reloads, as Refresh [spec 5.3.4].
    window_->on_key(
        [this](const rap::key_event& e)
        {
            if (!e.press || e.key != rap::key::f5 || e.modifiers) return false;
            reload();
            return true;
        });
    settings::get().restore_window("changes.window", *window_);
    files_.restore_layout();
    files_.set_source({.what = changes_source::working_tree});
    update_title();
}

// "<path> - Working Tree - Tortilla": the repository's root for the whole
// project, else the path, or for several their common folder [spec 5.3.1].
void changes_window::update_title()
{
    std::string root = workdir_;
    while (root.size() > 1 && (root.back() == '/' || root.back() == '\\')) root.pop_back();
    fs::path shown = rap::sys::path_from_utf8(root);
    if (!whole_project_->checked() && !paths_.empty())
    {
        fs::path common = rap::sys::path_from_utf8(paths_.front());
        if (paths_.size() > 1)
            for (const std::string& p : paths_)
                while (!common.empty() && !within(p, {common.generic_string()}))
                    common = common.parent_path();
        shown /= common;
    }
    window_->set_title(rap::i18n::format(_("{0} - Working Tree - Tortilla"),
                                         rap::sys::path_to_utf8(shown.make_preferred())));
}

// The loading state (controls greyed, "Please wait...") only for a load
// that takes a while: most take milliseconds, and showing it for them is
// just flicker [unlike TortoiseGit, which always shows it].
void changes_window::set_loading(bool on)
{
    loading_ = on;
    loading_delay_ = {};
    auto show = [this](bool busy)
    {
        refresh_->set_enabled(!busy);
        unversioned_->set_enabled(!busy && one_folder_);
        ignored_->set_enabled(!busy && one_folder_);
        local_ignored_->set_enabled(!busy);
        all_staged_->set_enabled(!busy);
        if (busy) note_->set_text(_("Please wait..."));
    };
    if (on)
        loading_delay_ = ui_.timer(250, false, [show] { show(true); });
    else
        show(false);
}

void changes_window::reload()
{
    working_tree_options o{
        .paths = whole_project_->checked() ? std::vector<std::string>{} : paths_,
        .unversioned = unversioned_->checked(),
        .ignored = ignored_->checked(),
        .local_changes_ignored = local_ignored_->checked(),
        .all_staged = all_staged_->checked(),
    };
    set_loading(true);
    update_title();
    // A load still running is stopped, so this one starts at once; its
    // result would be dropped anyway, as the worker only delivers the newest.
    if (load_) load_->cancel();
    load_ = std::make_shared<git_canceller>();
    const std::string workdir = workdir_;
    worker_->submit(
        [this, o, workdir, cancel = load_](repo& r,
                                           const repo_worker::progress&) -> std::function<void()>
        {
            auto changes = load_working_tree(workdir, o, cancel.get());
            std::string branch = r.head_name();
            return [this, changes = std::move(changes), branch]() mutable
            {
                branch_->set_text(branch);
                loaded(std::move(changes));
            };
        });
}

void changes_window::loaded(working_tree_changes changes)
{
    set_loading(false);
    loaded_ = std::move(changes.files);
    files_.set_files(loaded_);
    if (!changes.error.empty())
        note_->set_text(changes.error);
    else if (loaded_.empty())
        note_->set_text(
            _("No files to show with the current setting. Check one or more settings below to see "
              "unversioned, ignored and/or unmodified files."));
    else
        note_->set_text("");
    update_stats();
}

// "line: 1(+) 2(-) files: normal=0, non-versioned=1, modified=2, ..." [spec 5.3.6]
void changes_window::update_stats()
{
    size_t added = 0, removed = 0, unversioned = 0, modified = 0, add = 0, del = 0, conflicted = 0;
    for (const file_change& f : loaded_)
    {
        added += f.added;
        removed += f.removed;
        switch (f.status)
        {
        case '?':
        case '!': ++unversioned; break;
        case 'M':
        case 'T':
        case 'R': ++modified; break;
        case 'A':
        case 'C': ++add; break;
        case 'D': ++del; break;
        case 'U': ++conflicted; break;
        }
    }
    // Two lines where TortoiseGit's box wraps it.
    stats_->set_text(rap::i18n::format(_("line: {0}(+) {1}(-)\nfiles: normal=0, non-versioned={2}, "
                                         "modified={3}, added={4}, deleted={5}, conflicted={6}"),
                                       added, removed, unversioned, modified, add, del, conflicted));
}

// Commit, for the same paths; this list reloads when it closes [spec 5.3.4].
void changes_window::open_commit()
{
    std::vector<std::string> paths;
    const fs::path root = rap::sys::path_from_utf8(workdir_);
    if (whole_project_->checked() || paths_.empty())
        paths.push_back(workdir_);
    else
        for (const std::string& p : paths_)
            paths.push_back(rap::sys::path_to_utf8(root / rap::sys::path_from_utf8(p)));
    auto w = commit_window::open(ui_, paths);
    if (!w)
    {
        ui_.message(window_.get(),
                    {.icon = rap::message_spec::icon::error, .title = _("Commit"), .text = w.error()});
        return;
    }
    commit_window_ = std::move(*w);
    commit_window_->on_closed(
        [this](bool)
        {
            ui_.post(
                [this]
                {
                    commit_window_.reset();
                    reload();
                });
        });
    commit_window_->show();
}

// Stash changes, and Pop/Apply/List once there's a stash [spec 5.3.4].
rap::menu changes_window::stash_menu()
{
    auto after = [this](bool) { reload(); };
    rap::menu m{{.label = _("Stash changes"),
                 .action = [this, after] { stash_changes(ui_, window_.get(), workdir_, after); }}};
    git_result has = run_git(workdir_, {"rev-parse", "--verify", "--quiet", "refs/stash"},
                             git_output::stdout_only);
    if (has.ok())
    {
        m.push_back({.label = _("Stash Pop"),
                     .action = [this, after]
                     {
                         run_progress_dialog(ui_, window_.get(), _("Stash Pop"), workdir_,
                                             {{"stash", "pop"}}, after);
                     }});
        m.push_back({.label = _("Stash Apply"),
                     .action = [this, after]
                     {
                         run_progress_dialog(ui_, window_.get(), _("Stash Apply"), workdir_,
                                             {{"stash", "apply"}}, after);
                     }});
        m.push_back({.label = _("Stash List"), .enabled = false});
    }
    return m;
}

// TortoiseGit's "Save unified diff" shows HEAD against the working tree for
// the paths in the unified diff viewer, which can save it [spec 5.3.4].
void changes_window::show_unified_diff_of_paths()
{
    std::vector<std::string> args{"diff", "HEAD"};
    if (!whole_project_->checked() && !paths_.empty())
    {
        args.push_back("--");
        args.insert(args.end(), paths_.begin(), paths_.end());
    }
    show_unified_diff(ui_, _("HEAD..working tree"), workdir_, args);
}

}  // namespace tortilla
