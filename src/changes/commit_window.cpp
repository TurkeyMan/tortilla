#include "commit_window.h"

#include "i18n.h"
#include "settings.h"
#include "ui/progress_dialog.h"
#include "ui/settings_dialog.h"

#include <rapport/sys.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>

namespace tortilla
{
namespace
{

namespace fs = std::filesystem;

// Shared with Check for modifications (and TortoiseGit's Log View option).
constexpr const char* k_unversioned = "log.view.unversioned";
std::string whole_project_key(const std::string& workdir)
{
    return "changes.wholeProject." + workdir;
}

bool flag(const std::string& key, bool fallback)
{
    const std::string v = settings::get().value(key);
    return v.empty() ? fallback : v == "1";
}

std::string trimmed(std::string s)
{
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) ++start;
    return s.substr(start);
}

bool versioned(const file_change& f)
{
    return f.status != '?' && f.status != '!' && f.status != 'S' && f.status != 'h';
}

}  // namespace

std::expected<std::unique_ptr<commit_window>, std::string>
commit_window::open(rap::ui& ui, const std::vector<std::string>& paths)
{
    const std::string first = paths.empty() ? "." : paths.front();
    auto repo = repo::open(first);
    if (!repo) return std::unexpected(repo.error());
    if (repo->bare()) return std::unexpected(_("No working directory found."));
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
    std::unique_ptr<commit_window> w(new commit_window(ui, std::move(*repo), std::move(relative)));
    // TortoiseGit asks for an identity before anything else [spec 5.4.1].
    if (w->config("user.name").empty() || w->config("user.email").empty())
        return std::unexpected(
            _("git doesn't know who you are: set user.name and user.email (git config --global)."));
    w->build();
    w->reload();
    return w;
}

commit_window::commit_window(rap::ui& ui, repo r, std::vector<std::string> relative)
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
              .selection_changed = {},
              .checks_changed = [this] { update_state(); }})
{
    root_ = std::ranges::any_of(relative, [](const std::string& p) { return p.empty(); });
    if (!root_) paths_ = std::move(relative);
}

commit_window::~commit_window()
{
    if (load_) load_->cancel();
}

std::string commit_window::config(const std::string& key) const
{
    git_result r = run_git(workdir_, {"config", key}, git_output::stdout_only);
    return r.ok() ? trimmed(r.output) : "";
}

void commit_window::build()
{
    using namespace rap::layout;
    using namespace rap::literals;
    // The "Check:" links tick the rows of a kind [spec 5.4.4].
    auto check_link =
        [this](const char* text, std::function<bool(const file_change&)> which, bool on = true)
    { return label{text}.link([this, which, on] { files_.check_where(which, on); }); };
    window_ = ui_.create(window{
        _("Commit"),
        {vbox{
            hbox{
                label{_("Commit to:")},
                entry{}.bind(branch_).read_only().width(24_ch),
                check{_("new branch")}
                    .bind(new_branch_)
                    .on_toggled(
                        [this](bool on)
                        {
                            branch_->set_read_only(!on);
                            branch_->set_placeholder(on ? _("new branch name") : "");
                            branch_->set_text(on ? "" : current_branch_);
                            if (on) branch_->focus();
                        }),
                spacer{},
            },
            splitter{
                vertical,
                group{
                    _("Message:"),
                    vbox{
                        // TortoiseGit's room for a message, before any dragging.
                        text_view{}
                            .bind(message_)
                            .on_changed([this] { update_state(); })
                            .height(8_lines)
                            .stretch(),
                        check{_("Amend _Last Commit")}.bind(amend_).on_toggled([this](bool on)
                                                                            { amend_toggled(on); }),
                        hbox{
                            // Shown greyed until wanted, as the author field: the
                            // row keeps its layout when they're turned on.
                            check{_("Set author _date")}.bind(set_date_).on_toggled(
                                [this](bool on)
                                {
                                    date_->set_enabled(on);
                                    time_->set_enabled(on);
                                }),
                            date_picker{}.bind(date_).enabled(false),
                            entry{}.bind(time_).placeholder("HH:MM:SS").width(10_ch).enabled(false),
                            spacer{},
                        },
                        hbox{
                            check{_("Set au_thor")}
                                .bind(set_author_)
                                .on_toggled([this](bool on) { author_->set_enabled(on); }),
                            entry{}.bind(author_).enabled(false).width(36_ch),
                            spacer{},
                            button{_("Add _Signed-off-by")}.on_click([this] { add_signoff(); }),
                        },
                    }},
                group{
                    _("Changes made (F5: refresh, double-click on file for diff):"),
                    vbox{
                        hbox{
                            label{_("Check:")},
                            check_link(_("All"), [](const file_change&) { return true; }),
                            check_link(
                                _("None"), [](const file_change&) { return true; }, false),
                            check_link(_("Unversioned"),
                                       [](const file_change& f) { return f.status == '?'; }),
                            check_link(_("Versioned"), versioned),
                            check_link(_("Added"),
                                       [](const file_change& f) { return f.status == 'A'; }),
                            check_link(_("Deleted"),
                                       [](const file_change& f) { return f.status == 'D'; }),
                            check_link(
                                _("Modified"), [](const file_change& f)
                                { return f.status == 'M' || f.status == 'T' || f.status == 'R'; }),
                            check_link(_("Files"),
                                       [](const file_change& f) { return !f.path.ends_with('/'); }),
                            check_link(_("Submodules"),
                                       [](const file_change& f) { return f.path.ends_with('/'); }),
                            spacer{},
                        },
                        files_.list("commit.files", true).stretch(),
                        hbox{
                            vbox{
                                check{_("Show _Unversioned Files"), flag(k_unversioned, true)}
                                    .bind(unversioned_)
                                    .on_toggled(
                                        [this](bool on)
                                        {
                                            settings::get().set(k_unversioned, on ? "1" : "0");
                                            reload();
                                        }),
                                check{_("Show _Whole Project"),
                                      root_ || flag(whole_project_key(workdir_), false)}
                                    .bind(whole_project_)
                                    .enabled(!root_)
                                    .on_toggled(
                                        [this](bool on)
                                        {
                                            settings::get().set(whole_project_key(workdir_),
                                                                on ? "1" : "0");
                                            reload();
                                        }),
                                check{_("Message onl_y")}
                                    .bind(message_only_)
                                    .on_toggled([this](bool) { update_state(); }),
                            },
                            spacer{},
                            label{}.bind(stats_),
                        },
                    }}
                    .stretch(),  // the files take what the window gains, not the message
            }
                .bind(splitter_)
                .stretch(),
            hbox{
                spacer{},
                button{_("C_ommit")}.bind(commit_).enabled(false).on_click([this] { commit(); }),
                button{_("Cancel")}.on_click([this] { close(false); }),
            },
        }}}.size(100_ch, 44_lines));

    window_->on_close_request(
        [this]
        {
            close(false);
            return false;  // close() closes it
        });
    // F5 refreshes; Ctrl+Enter commits [spec 5.4.3].
    window_->on_key(
        [this](const rap::key_event& e)
        {
            if (!e.press) return false;
            if (e.key == rap::key::f5 && !e.modifiers && !committing_)
            {
                reload();
                return true;
            }
            if (e.key == rap::key::enter && (e.modifiers & rap::mod_ctrl))
            {
                if (can_commit_) commit();
                return true;
            }
            return false;
        });
    settings::get().restore_window("commit.window", *window_);
    if (auto v = settings::get().numbers("commit.panes"); v.size() == 2) splitter_->set_sizes(v);
    files_.restore_layout();
    files_.set_source({.what = changes_source::working_tree});
    author_->set_text(config("user.name") + " <" + config("user.email") + ">");
    const std::tm* now = []
    {
        static std::time_t t = std::time(nullptr);
        return std::localtime(&t);
    }();
    date_->set_date({now->tm_year + 1900, now->tm_mon + 1, now->tm_mday});
    char hms[16];
    std::snprintf(hms, sizeof hms, "%02d:%02d:%02d", now->tm_hour, now->tm_min, now->tm_sec);
    time_->set_text(hms);
    // A merge's or squash's message is waiting in the repository [spec 5.4.3].
    const fs::path git_dir = rap::sys::path_from_utf8(repo_.git_dir());
    for (const char* name : {"SQUASH_MSG", "MERGE_MSG"})
        if (std::FILE* f = rap::sys::fopen(git_dir / name, "rb"))
        {
            std::string text;
            char buf[4096];
            while (size_t n = std::fread(buf, 1, sizeof buf, f)) text.append(buf, n);
            std::fclose(f);
            message_->set_text(text);
            break;
        }
    update_title();
    update_state();
    message_->focus();
}

// "<path> - Commit - Tortilla" [spec 5.4.1].
void commit_window::update_title()
{
    std::string root = workdir_;
    while (root.size() > 1 && (root.back() == '/' || root.back() == '\\')) root.pop_back();
    fs::path shown = rap::sys::path_from_utf8(root);
    if (!whole_project_->checked() && paths_.size() == 1)
        shown /= rap::sys::path_from_utf8(paths_.front());
    window_->set_title(rap::i18n::format(_("{0} - Commit - Tortilla"),
                                         rap::sys::path_to_utf8(shown.make_preferred())));
}

void commit_window::reload()
{
    working_tree_options o{
        .paths = whole_project_->checked() ? std::vector<std::string>{} : paths_,
        .unversioned = unversioned_->checked(),
        .all_staged = true,  // everything staged is listed, as TortoiseGit's Commit does
    };
    update_title();
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
                current_branch_ = branch;
                if (!new_branch_->checked()) branch_->set_text(branch);
                loaded(std::move(changes));
            };
        });
}

void commit_window::loaded(working_tree_changes changes)
{
    files_.set_files(std::move(changes.files));
    update_state();
}

// "n files selected, n files total", and whether Commit can go: a message,
// and something to commit [spec 5.4.7].
void commit_window::update_state()
{
    const size_t checked = files_.checked_count();
    stats_->set_text(rap::i18n::format(_("{0} files selected, {1} files total"), checked,
                                       files_.files().size()));
    const bool something = checked > 0 || message_only_->checked() || amend_->checked();
    can_commit_ = !committing_ && !trimmed(message_->text()).empty() && something;
    commit_->set_enabled(can_commit_);
}

// Amend: the last commit's message to edit, and its author [spec 5.4.4].
void commit_window::amend_toggled(bool on)
{
    if (on)
    {
        pre_amend_message_ = message_->text();
        git_result m = run_git(workdir_, {"log", "-1", "--format=%B"}, git_output::stdout_only);
        if (m.ok()) message_->set_text(trimmed(m.output) + "\n");
        git_result a =
            run_git(workdir_, {"log", "-1", "--format=%an <%ae>"}, git_output::stdout_only);
        if (a.ok()) author_->set_text(trimmed(a.output));
    }
    else
    {
        message_->set_text(pre_amend_message_);
        author_->set_text(config("user.name") + " <" + config("user.email") + ">");
    }
    update_state();
}

// "Signed-off-by: name <email>", once, after a blank line [spec 5.4.4].
void commit_window::add_signoff()
{
    const std::string line =
        "Signed-off-by: " + config("user.name") + " <" + config("user.email") + ">";
    std::string text = message_->text();
    if (text.find(line) != std::string::npos) return;
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.pop_back();
    const size_t last = text.rfind('\n');
    const std::string last_line = text.substr(last == std::string::npos ? 0 : last + 1);
    if (!text.empty() && last_line.find("-by: ") == std::string::npos) text += "\n";
    message_->set_text(text + (text.empty() ? "" : "\n") + line + "\n");
    update_state();
}

// The checked files, as they are in the working tree, become the commit:
// git commit --only takes exactly those paths and leaves whatever else is
// staged alone (TortoiseGit rewrites the index to match the boxes, then
// re-stages what it unstaged) [spec 5.4.6].
void commit_window::commit()
{
    const std::vector<file_change> files =
        message_only_->checked() ? std::vector<file_change>{} : files_.checked_files();
    if (std::ranges::any_of(files, [](const file_change& f) { return f.status == 'U'; }))
    {
        ui_.message(window_.get(), {.icon = rap::message_spec::icon::warning,
                                    .title = _("Commit"),
                                    .text = _("One or more files are in a conflicted state.")});
        return;
    }
    std::string branch;
    if (new_branch_->checked())
    {
        branch = trimmed(branch_->text());
        if (branch.empty() || !run_git(workdir_, {"check-ref-format", "--branch", branch}).ok())
        {
            ui_.message(window_.get(), {.icon = rap::message_spec::icon::warning,
                                        .title = _("Commit"),
                                        .text = rap::i18n::format(_("“{0}” isn't a valid branch name."),
                                                                  branch)});
            return;
        }
    }

    // The message and the paths go through files: no command-line limits.
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / "tortilla";
    fs::create_directories(dir, ec);
    const auto stamp = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const fs::path message_file = dir / ("commit-" + stamp + ".txt");
    const fs::path paths_file = dir / ("paths-" + stamp + ".txt");
    auto write = [](const fs::path& p, const std::string& s)
    {
        std::FILE* f = rap::sys::fopen(p, "wb");
        bool ok = f && std::fwrite(s.data(), 1, s.size(), f) == s.size();
        if (f) ok = std::fclose(f) == 0 && ok;
        return ok;
    };
    std::string paths, added;
    std::vector<std::string> unversioned;
    for (const file_change& f : files)
    {
        paths += f.path + '\0';
        if (!f.old_path.empty()) paths += f.old_path + '\0';
        if (f.status == '?') unversioned.push_back(f.path);
    }
    if (!write(message_file, message_->text()) || !write(paths_file, paths))
    {
        ui_.message(window_.get(), {.icon = rap::message_spec::icon::error,
                                    .title = _("Commit"),
                                    .text = rap::i18n::format(_("Could not write the commit message to {0}."),
                                                              rap::sys::path_to_utf8(dir))});
        return;
    }

    std::vector<std::vector<std::string>> commands;
    if (!branch.empty()) commands.push_back({"switch", "-c", branch});
    if (!unversioned.empty())
    {
        std::vector<std::string> add{"add", "-f", "--"};
        add.insert(add.end(), unversioned.begin(), unversioned.end());
        commands.push_back(add);
    }
    std::vector<std::string> c{"commit", "-F", rap::sys::path_to_utf8(message_file)};
    if (amend_->checked()) c.push_back("--amend");
    if (set_author_->checked()) c.push_back("--author=" + trimmed(author_->text()));
    if (set_date_->checked())
    {
        const rap::date d = date_->date();
        char date[32];
        std::snprintf(date, sizeof date, "%04d-%02d-%02dT", d.year, d.month, d.day);
        c.push_back("--date=" + std::string(date) + trimmed(time_->text()));
    }
    if (files.empty())
        c.insert(c.end(),
                 {"--only", "--allow-empty"});  // message only (or an amend of the message)
    else
        c.insert(c.end(), {"--only", "--pathspec-from-file=" + rap::sys::path_to_utf8(paths_file),
                           "--pathspec-file-nul"});
    commands.push_back(c);

    committing_ = true;
    update_state();
    if (load_) load_->cancel();
    run_progress_dialog(ui_, window_.get(), _("Commit"), workdir_, std::move(commands),
                        [this, message_file, paths_file](bool ok)
                        {
                            std::error_code ec;
                            fs::remove(message_file, ec);
                            fs::remove(paths_file, ec);
                            committing_ = false;
                            if (ok)
                                close(true);
                            else
                            {
                                update_state();
                                reload();
                            }
                        });
}

void commit_window::close(bool committed)
{
    settings::get().save_window("commit.window", *window_);
    settings::get().set_numbers("commit.panes", splitter_->sizes());
    files_.save_layout();
    window_->close();
    if (closed_) closed_(committed);
}

}  // namespace tortilla
