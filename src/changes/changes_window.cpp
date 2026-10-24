#include "changes_window.h"

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
    if (repo->bare()) return std::unexpected("No working directory found.");
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

changes_window::~changes_window() = default;

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
        "Working Tree",
        {vbox{
            label{}.ellipsize().bind(branch_),
            files_.list("changes.files").stretch(),
            label{}.bind(note_),
            hbox{
                vbox{
                    check{"Show un_versioned files", flag(k_unversioned, true)}
                        .bind(unversioned_)
                        .enabled(one_folder)
                        .on_toggled(
                            [this](bool on)
                            {
                                set_flag(k_unversioned, on);
                                reload();
                            }),
                    check{"Show ignore local changes flagged files"}
                        .bind(local_ignored_)
                        .on_toggled([this](bool) { reload(); }),
                    check{"Show i_gnored files"}
                        .bind(ignored_)
                        .enabled(one_folder)
                        .on_toggled([this](bool) { reload(); }),
                    check{"Show all _staged files", flag(k_all_staged, true)}
                        .bind(all_staged_)
                        .on_toggled(
                            [this](bool on)
                            {
                                set_flag(k_all_staged, on);
                                reload();
                            }),
                    // At the repository's root it's the whole project anyway.
                    check{"Show _Whole Project", root_ || flag(whole_project_key(workdir_), false)}
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
                button{"Save unified diff"}.on_click([this] { show_unified_diff_of_paths(); }),
                menu_button{"Stash"}.on_menu([this] { return stash_menu(); }),
                button{"Commit"}.enabled(false).tooltip("Not yet in Tortilla"),
                button{"_Refresh"}.bind(refresh_).on_click([this] { reload(); }),
                button{"OK"}.bind(ok_).on_click(
                    [this, close]
                    {
                        // While loading it's Cancel: stop the load, stay open.
                        if (loading_)
                        {
                            worker_ = std::make_unique<repo_worker>(ui_, workdir_);
                            loaded({});
                            return;
                        }
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
    window_->set_title(rap::sys::path_to_utf8(shown.make_preferred()) +
                       " - Working Tree - Tortilla");
}

void changes_window::set_loading(bool on)
{
    loading_ = on;
    ok_->set_text(on ? "Cancel" : "OK");
    // While loading, everything but Whole Project waits [spec 5.3.3].
    refresh_->set_enabled(!on);
    unversioned_->set_enabled(!on && one_folder_);
    ignored_->set_enabled(!on && one_folder_);
    local_ignored_->set_enabled(!on);
    all_staged_->set_enabled(!on);
    if (on)
    {
        branch_->set_text("");
        note_->set_text("Please wait...");
    }
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
    const std::string workdir = workdir_;
    worker_->submit(
        [this, o, workdir](repo& r, const repo_worker::progress&) -> std::function<void()>
        {
            auto changes = load_working_tree(workdir, o);
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
            "No files to show with the current setting. Check one or more settings below to see "
            "unversioned, ignored and/or unmodified files.");
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
    auto n = [](size_t v) { return std::to_string(v); };
    // Two lines where TortoiseGit's box wraps it.
    stats_->set_text("line: " + n(added) + "(+) " + n(removed) +
                     "(-)\nfiles: normal=0, non-versioned=" + n(unversioned) +
                     ", modified=" + n(modified) + ", added=" + n(add) + ", deleted=" + n(del) +
                     ", conflicted=" + n(conflicted));
}

// Stash changes, and Pop/Apply/List once there's a stash [spec 5.3.4].
rap::menu changes_window::stash_menu()
{
    auto after = [this](bool) { reload(); };
    rap::menu m{{.label = "Stash changes",
                 .action = [this, after] { stash_changes(ui_, window_.get(), workdir_, after); }}};
    git_result has = run_git(workdir_, {"rev-parse", "--verify", "--quiet", "refs/stash"},
                             git_output::stdout_only);
    if (has.ok())
    {
        m.push_back({.label = "Stash Pop",
                     .action = [this, after]
                     {
                         run_progress_dialog(ui_, window_.get(), "Stash Pop", workdir_,
                                             {{"stash", "pop"}}, after);
                     }});
        m.push_back({.label = "Stash Apply",
                     .action = [this, after]
                     {
                         run_progress_dialog(ui_, window_.get(), "Stash Apply", workdir_,
                                             {{"stash", "apply"}}, after);
                     }});
        m.push_back({.label = "Stash List", .enabled = false});
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
    show_unified_diff(ui_, "HEAD..working tree", workdir_, args);
}

}  // namespace tortilla
