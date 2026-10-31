#include "file_action_window.h"

#include "changes/commit_window.h"
#include "i18n.h"
#include "settings.h"
#include "ui/progress_dialog.h"
#include "ui/repo_icon.h"
#include "ui/settings_dialog.h"

#include <rapport/sys.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>

namespace tortilla
{
namespace
{

namespace fs = std::filesystem;

// The paths of a git command, in a file it reads them from
// (--pathspec-from-file): no command-line limits. Removed when the last
// copy goes, after the commands have run.
class pathspec_file
{
public:
    static std::shared_ptr<pathspec_file> write(const std::vector<std::string>& paths)
    {
        std::error_code ec;
        const fs::path dir = fs::temp_directory_path(ec) / "tortilla";
        fs::create_directories(dir, ec);
        static int serial = 0;
        const auto stamp =
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        auto out = std::shared_ptr<pathspec_file>(
            new pathspec_file(dir / ("paths-" + stamp + "-" + std::to_string(++serial) + ".txt")));
        std::string data;
        for (const std::string& p : paths) data += p + '\0';
        std::FILE* f = rap::sys::fopen(out->path_, "wb");
        bool ok = f && std::fwrite(data.data(), 1, data.size(), f) == data.size();
        if (f) ok = std::fclose(f) == 0 && ok;
        return ok ? out : nullptr;
    }
    ~pathspec_file()
    {
        std::error_code ec;
        fs::remove(path_, ec);
    }
    // The arguments that read it, NUL-separated.
    std::vector<std::string> arguments() const
    {
        return {"--pathspec-from-file=" + rap::sys::path_to_utf8(path_), "--pathspec-file-nul"};
    }

private:
    explicit pathspec_file(fs::path p)
        : path_(std::move(p))
    {
    }
    fs::path path_;
};

bool is_versioned_change(char status)
{
    return std::string_view("MADRCTU").find(status) != std::string_view::npos;
}

}  // namespace

std::expected<std::unique_ptr<file_action_window>, std::string>
file_action_window::open(rap::ui& ui, file_action action, const std::vector<std::string>& paths)
{
    const std::string first = paths.empty() ? "." : paths.front();
    auto repo = repo::open(first);
    if (!repo) return std::unexpected(repo.error());
    if (repo->bare()) return std::unexpected(_("No working directory found."));
    std::error_code ec;
    const fs::path root = fs::weakly_canonical(rap::sys::path_from_utf8(repo->workdir()), ec);
    std::vector<std::string> relative;
    std::set<std::string> files;
    for (const std::string& p : paths.empty() ? std::vector<std::string>{"."} : paths)
    {
        fs::path abs = fs::weakly_canonical(fs::absolute(rap::sys::path_from_utf8(p), ec), ec);
        std::string rel = rap::sys::path_to_utf8(abs.lexically_relative(root).generic_string());
        if (rel == ".") rel.clear();
        if (!rel.empty() && !fs::is_directory(abs, ec)) files.insert(rel);
        relative.push_back(rel);
    }
    std::unique_ptr<file_action_window> w(
        new file_action_window(ui, action, std::move(*repo), std::move(relative)));
    w->explicit_files_ = std::move(files);
    w->build();
    return w;
}

file_action_window::file_action_window(rap::ui& ui, file_action action, repo r,
                                       std::vector<std::string> relative)
    : ui_(ui)
    , action_(action)
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
              .selection_changed = {},
              .checks_changed = [this] { update_state(); }})
{
    if (std::ranges::none_of(relative, [](const std::string& p) { return p.empty(); }))
        paths_ = std::move(relative);
}

file_action_window::~file_action_window()
{
    if (load_) load_->cancel();
}

std::string file_action_window::name() const
{
    switch (action_)
    {
    case file_action::add: return T_("Add");
    case file_action::revert: return T_("Revert");
    case file_action::resolve: return T_("Resolve");
    }
    return {};
}

std::string file_action_window::title() const
{
    return _(name().c_str());
}

// Which rows the list shows [spec 6.2.3, 6.3.3, 6.5.3].
// Where its window and list settings are kept: "add", "revert", "resolve".
std::string file_action_window::settings_key() const
{
    std::string key = name();
    key[0] = char(std::tolower(static_cast<unsigned char>(key[0])));
    return key;
}

bool file_action_window::shows(const file_change& f) const
{
    switch (action_)
    {
    case file_action::add: return f.status == '?' || (f.status == '!' && ignored_->checked());
    case file_action::revert: return is_versioned_change(f.status);
    case file_action::resolve: return f.status == 'U';
    }
    return false;
}

// A row's check when it first appears: Add checks the unversioned files,
// Revert only additions and the files it was given (so reverting a folder
// asks which changes to drop), Resolve everything [spec 6.2.3, 6.3.3, 6.5.3].
bool file_action_window::starts_checked(const file_change& f) const
{
    if (explicit_files_.contains(f.path)) return true;
    switch (action_)
    {
    case file_action::add: return f.status == '?';
    case file_action::revert: return f.status == 'A';
    case file_action::resolve: return true;
    }
    return false;
}

void file_action_window::build()
{
    using namespace rap::layout;
    using namespace rap::literals;
    rap::nodes options;
    options.push_back(check{_("Select/deselect _all")}
                          .bind(select_all_)
                          .on_toggled(
                              [this](bool on)
                              {
                                  // From some checked, a click checks none [spec 6.1].
                                  files_.check_where([](const file_change&) { return true; },
                                                     on && all_state_ !=
                                                               rap::check_state::indeterminate);
                              }));
    if (action_ == file_action::add)
        options.push_back(
            check{_("Include _ignored files")}.bind(ignored_).on_toggled([this](bool) { reload(); }));
    if (action_ == file_action::resolve)
        options.push_back(label{_("Reminder: Commit your change after resolve")});
    options.push_back(spacer{});
    options.push_back(label{}.bind(note_));

    window_ = create_window(
        ui_,
        window{title(),
               {vbox{
                   files_.list(settings_key() + ".files", true).stretch(),
                   rap::node{rap::spec::box{rap::layout::horizontal, std::move(options), false, {}},
                             {}},
                   hbox{
                       spacer{},
                       button{_("OK")}.bind(ok_).enabled(false).on_click(
                           [this] { run(files_.checked_files()); }),
                       button{_("Cancel")}.on_click([this] { close(false); }),
                   },
               }}}
            .size(action_ == file_action::revert ? 90_ch : 80_ch,
                  action_ == file_action::revert ? 32_lines : 24_lines));

    window_->on_close_request(
        [this]
        {
            close(false);
            return false;  // close() closes it
        });
    // F5 reloads; Ctrl+Enter is OK [spec 6.1].
    window_->on_key(
        [this](const rap::key_event& e)
        {
            if (!e.press || running_) return false;
            if (e.key == rap::key::f5 && !e.modifiers)
            {
                reload();
                return true;
            }
            if (e.key == rap::key::enter && (e.modifiers & rap::mod_ctrl))
            {
                if (files_.checked_count()) run(files_.checked_files());
                return true;
            }
            return false;
        });
    settings::get().restore_window(settings_key() + ".window", *window_);
    files_.restore_layout();
    files_.set_source({.what = changes_source::working_tree});

    // "<path> - Revert - Tortilla"; Resolve always names the repository
    // [spec 6.2.2, 6.3.2, 6.5.2].
    std::string root = workdir_;
    while (root.size() > 1 && (root.back() == '/' || root.back() == '\\')) root.pop_back();
    fs::path shown = rap::sys::path_from_utf8(root);
    if (action_ != file_action::resolve && paths_.size() == 1)
        shown /= rap::sys::path_from_utf8(paths_.front());
    window_->set_title(rap::i18n::format(_("{0} - {1} - Tortilla"),
                                         rap::sys::path_to_utf8(shown.make_preferred()), title()));
}

void file_action_window::show()
{
    // Add given only files: no list to choose from [spec 6.2.1].
    if (action_ == file_action::add && !paths_.empty() && explicit_files_.size() == paths_.size())
    {
        std::vector<file_change> files;
        for (const std::string& p : paths_) files.push_back({.path = p, .status = '?'});
        run(files);
        return;
    }
    shown_ = true;
    window_->show();
    reload();
}

void file_action_window::reload()
{
    working_tree_options o{
        .paths = paths_,
        .unversioned = action_ == file_action::add,
        .each_untracked_file = true,
        .ignored = action_ == file_action::add && ignored_->checked(),
        .all_staged = false,
    };
    note_->set_text(_("Please wait..."));
    if (load_) load_->cancel();
    load_ = std::make_shared<git_canceller>();
    const std::string workdir = workdir_;
    worker_->submit(
        [this, o, workdir, cancel = load_](repo&,
                                           const repo_worker::progress&) -> std::function<void()>
        {
            auto changes = load_working_tree(workdir, o, cancel.get());
            return [this, changes = std::move(changes)]() mutable { loaded(std::move(changes)); };
        });
}

void file_action_window::loaded(working_tree_changes changes)
{
    std::vector<file_change> rows;
    for (file_change& f : changes.files)
        if (shows(f)) rows.push_back(std::move(f));
    // New rows get their first check; rows seen before keep the user's.
    std::set<std::string> fresh, fresh_checked;
    for (const file_change& f : rows)
        if (seen_.insert(f.path).second)
        {
            fresh.insert(f.path);
            if (starts_checked(f)) fresh_checked.insert(f.path);
        }
    files_.set_files(std::move(rows));
    files_.check_where([&](const file_change& f)
                       { return fresh.contains(f.path) && !fresh_checked.contains(f.path); },
                       false);
    files_.check_where([&](const file_change& f) { return fresh_checked.contains(f.path); }, true);
    note_->set_text(!changes.error.empty()           ? changes.error
                    : !files_.files().empty()        ? ""
                    : action_ == file_action::add    ? _("There is nothing to add.")
                    : action_ == file_action::revert ? _("There is nothing to revert.")
                                                     : _("There are no conflicts."));
    update_state();
}

// Select/deselect all follows the checks; OK needs one [spec 6.1].
void file_action_window::update_state()
{
    const size_t checked = files_.checked_count(), total = files_.files().size();
    all_state_ = checked == 0       ? rap::check_state::unchecked
                 : checked == total ? rap::check_state::checked
                                    : rap::check_state::indeterminate;
    select_all_->set_state(all_state_);
    select_all_->set_enabled(total > 0);
    ok_->set_enabled(checked > 0 && !running_);
}

// The git work of OK, in the progress dialog [spec 6.2.4, 6.3.4, 6.5.4].
void file_action_window::run(const std::vector<file_change>& files)
{
    if (files.empty()) return;
    std::vector<std::shared_ptr<pathspec_file>> lists;
    std::vector<std::vector<std::string>> commands;
    // `args`, the paths, then `after` (checkout's revision).
    auto command = [&](std::vector<std::string> args, const std::vector<std::string>& paths,
                       std::vector<std::string> after = {})
    {
        if (paths.empty()) return true;
        auto list = pathspec_file::write(paths);
        if (!list) return false;
        for (std::string& a : list->arguments()) args.push_back(std::move(a));
        args.insert(args.end(), after.begin(), after.end());
        lists.push_back(list);
        commands.push_back(std::move(args));
        return true;
    };
    std::vector<std::string> all;
    for (const file_change& f : files) all.push_back(f.path);

    bool written = true;
    if (action_ == file_action::revert)
    {
        // By kind: additions are unstaged (the file stays); a rename's new
        // file goes and its old one comes back from HEAD; the rest come
        // back from HEAD, index and working tree.
        std::vector<std::string> unstage, remove, restore;
        std::error_code ec;
        for (const file_change& f : files)
        {
            if (f.status == 'R' && !f.old_path.empty())
            {
                const fs::path old_path =
                    rap::sys::path_from_utf8(workdir_) / rap::sys::path_from_utf8(f.old_path);
                if (fs::is_directory(old_path, ec))
                {
                    ui_.message(window_.get(), {.icon = rap::message_spec::icon::error,
                                                .title = _("Revert"),
                                                .text = rap::i18n::format(
                                                    _("Cannot revert renaming of “{0}”. A directory "
                                                      "with the old name “{1}” exists."),
                                                    f.path, f.old_path)});
                    return;
                }
                const bool there = fs::exists(
                    rap::sys::path_from_utf8(workdir_) / rap::sys::path_from_utf8(f.path), ec);
                (there ? remove : unstage).push_back(f.path);
                restore.push_back(f.old_path);
            }
            else if (f.status == 'A')
                unstage.push_back(f.path);
            else
                restore.push_back(f.path);
        }
        written = command({"rm", "-f", "--cached"}, unstage) &&
                  command({"rm", "-f", "--ignore-unmatch"}, remove) &&
                  command({"checkout", "-f"}, restore, {"--end-of-options", "HEAD"});
    }
    else
        // Add, and Resolve (marking a conflict resolved is adding it).
        written = command({"add", "-f"}, all);
    if (!written)
    {
        ui_.message(window_.get(),
                    {.icon = rap::message_spec::icon::error,
                     .title = title(),
                     .text = _("Could not write the list of files to the temporary folder.")});
        return;
    }

    // After Add and Resolve: Commit, and for Add the file modes TortoiseGit
    // offers [spec 6.2.4, 6.5.4].
    std::vector<progress_follow_up> follow_ups;
    if (action_ != file_action::revert)
        follow_ups.push_back({.label = _("_Commit..."), .after_close = [this] { open_commit(); }});
    if (action_ == file_action::add)
    {
        std::vector<std::string> paths;
        std::error_code ec;
        for (const std::string& p : all)
            if (!fs::is_directory(rap::sys::path_from_utf8(workdir_) / rap::sys::path_from_utf8(p),
                                  ec))
                paths.push_back(p);
        follow_ups.push_back({.label = _("Add as _Executable (+x)"),
                              .commands = [paths]
                              {
                                  std::vector<std::string> c{"update-index", "--chmod=+x", "--"};
                                  c.insert(c.end(), paths.begin(), paths.end());
                                  return std::vector<git_command>{c};
                              }});
        // A symlink's blob is its target: the file as added, with the mode
        // of a link.
        follow_ups.push_back(
            {.label = _("Add as _Symlink"),
             .commands = [paths, workdir = workdir_]
             {
                 std::vector<std::string> ls{"ls-files", "-s", "-z", "--"};
                 ls.insert(ls.end(), paths.begin(), paths.end());
                 const git_result r = run_git(workdir, ls, git_output::stdout_only);
                 std::vector<std::string> c{"update-index"};
                 // "<mode> <hash> <stage>\t<path>\0" each.
                 for (size_t at = 0; r.ok() && at < r.output.size();)
                 {
                     size_t end = r.output.find('\0', at);
                     if (end == std::string::npos) end = r.output.size();
                     const std::string e = r.output.substr(at, end - at);
                     at = end + 1;
                     const size_t hash = e.find(' '), stage = e.find(' ', hash + 1),
                                  tab = e.find('\t');
                     if (hash == std::string::npos || stage == std::string::npos ||
                         tab == std::string::npos)
                         continue;
                     c.insert(c.end(),
                              {"--cacheinfo", "120000," + e.substr(hash + 1, stage - hash - 1) +
                                                  "," + e.substr(tab + 1)});
                 }
                 return c.size() > 1 ? std::vector<git_command>{c} : std::vector<git_command>{};
             }});
    }
    auto start =
        [this, commands = std::move(commands), lists, follow_ups = std::move(follow_ups)]() mutable
    {
        running_ = true;
        if (load_) load_->cancel();
        update_state();
        run_progress_dialog(
            ui_, shown_ ? window_.get() : nullptr, title(), workdir_, std::move(commands),
            [this, lists](bool ok)
            {
                running_ = false;
                if (ok)
                    close(true);
                else
                {
                    update_state();
                    reload();
                }
            },
            std::move(follow_ups));
    };
    if (action_ != file_action::revert)
    {
        start();
        return;
    }
    // Revert first moves what it overwrites to the Recycle Bin (the trash),
    // so it can be undone: every file but the added ones, which it leaves on
    // disk anyway [spec 6.3.4].
    std::vector<fs::path> keep;
    std::error_code ec;
    for (const file_change& f : files)
    {
        const fs::path p = rap::sys::path_from_utf8(workdir_) / rap::sys::path_from_utf8(f.path);
        if (f.status != 'A' && (fs::is_regular_file(fs::symlink_status(p, ec)) ||
                                fs::is_symlink(fs::symlink_status(p, ec))))
            keep.push_back(p);
    }
    if (auto kept = rap::sys::move_to_trash(keep); !kept)
        ui_.message(window_.get(),
                    {.icon = rap::message_spec::icon::warning,
                     .title = _("Revert"),
                     .text = _("The files couldn't be moved to the Recycle Bin first, so reverting "
                               "can't be undone. Revert anyway?"),
                     .detail = kept.error(),
                     .buttons = {rap::dialog_role::yes, rap::dialog_role::no}},
                    [start](rap::dialog_role r) mutable
                    {
                        if (r == rap::dialog_role::yes) start();
                    });
    else
        start();
}

// The follow-up Commit: the Commit dialog on the whole repository, which
// keeps this one's caller waiting until it closes [spec 6.2.4].
void file_action_window::open_commit()
{
    auto w = commit_window::open(ui_, {workdir_});
    if (!w)
    {
        ui_.message(nullptr,
                    {.icon = rap::message_spec::icon::error, .title = _("Commit"), .text = w.error()});
        return;
    }
    commit_ = std::move(*w);
    commit_->on_closed(
        [this](bool)
        {
            if (closed_) closed_(true);
        });
    commit_->show();
}

void file_action_window::close(bool done)
{
    if (shown_)
    {
        settings::get().save_window(settings_key() + ".window", *window_);
        files_.save_layout();
    }
    window_->close();
    if (closed_ && !commit_) closed_(done);
}

}  // namespace tortilla
