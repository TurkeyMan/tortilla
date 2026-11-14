// Tortilla: a TortoiseGit-style git GUI.
//   tortilla [log] [--all] [--submodule] [path]   log window for the repository
//                                     containing path, limited to path; --submodule:
//                                     the repository around a submodule at path
//                                     with nothing given and none here, the welcome window
//   tortilla about                    the welcome window: what Tortilla is, links
//   tortilla help                     the documentation (the README online) in the browser
//   tortilla cache                    the file manager badges' status cache (no window)
//   tortilla remove|removekeep path...  Delete (to the Recycle Bin), or stop tracking
//   tortilla ignore [--delete] [--mask] path...  add to an ignore list (by extension;
//                                     and stop tracking)
//   tortilla rebase [path] [upstream]   rebase dialog
//   tortilla diff [path]              a file: its working tree changes against HEAD, in
//                                     the diff tool; a folder: Check for modifications
//   tortilla repostatus [path...]      Check for modifications (TortoiseGit's repostatus)
//   tortilla commit [path...]          the Commit dialog
//   tortilla add|revert|resolve [path...]  the Add, Revert or Resolve dialog
//   tortilla clone [folder] [url]      the Clone dialog, cloning into folder
//   tortilla fetch|pull|push [path]    the Fetch, Pull or Push dialog
//   tortilla branch|tag|switch|merge [path] [rev]  Create Branch/Tag, Switch/Checkout
//                                     or Merge, rev preselected
//   tortilla mergeabort [path]         the Abort Merge dialog
//   tortilla refbrowse [path]          Browse References
//   tortilla repobrowser [path] [rev]  the Repository Browser: the files at rev (HEAD), from path
//   tortilla export [path] [rev]       Export: rev's (HEAD's) tracked files to a .zip or a folder
//   tortilla reflog [path] [ref]       the RefLog dialog, on HEAD or ref
//   tortilla stashlist [path]          the RefLog dialog on the stash
//   tortilla worktrees [path]          the repository's worktrees: add, remove, lock, prune
//   tortilla pullrequest [path] [branch]  Create Pull Request for a branch (the current one)
//   tortilla prevdiff [path]           the file against its previous version
//   tortilla stashsave|stashapply|stashpop [path]  Stash changes, Apply or Pop
//   tortilla subadd|subupdate|subsync [path]  Submodule Add or Update (their dialogs),
//                                     or Sync, for the working tree at path
//   tortilla svndcommit|svnrebase|svnfetch|svnignore [path]  git-svn: SVN DCommit,
//                                     SVN Rebase, SVN Fetch or Import SVN Ignore
//   tortilla formatpatch [path]       Create Patch Serial: git format-patch into a folder
//   tortilla reviewpatch [patch] [path]  Review/apply single patch: its files, each
//                                     reviewed in the diff tool, then git apply
//   tortilla importpatch [path] [patch...]  Apply Patch Serial: git am, to the working
//                                     tree at path (or the patches' own)
//   tortilla repocreate [folder]       git init there, bare or not
//   tortilla rename path               git mv it to a new name
//   tortilla cleanup [path...]         Clean up: untracked files removed
//   tortilla compare file1 file2       two files, anywhere, in the diff tool
//   tortilla blame file [rev]          Blame: each line's last commit, through its history
//   tortilla showcompare [path] rev1 [rev2]  the files changed between two revisions
//                                     (or rev1 and the working tree), in a list
//   tortilla conflicteditor file       a conflicted file in the merge tool
//   tortilla bisect start|good|bad|skip|reset [path]  git bisect: start asks for the
//                                     good and bad revisions; the rest mark HEAD, or end it
//   tortilla diff [path] rev [rev2]   a commit, or rev..rev2, as a unified diff
//   tortilla settings [path] [diff|menu|network|remotes|accounts]  the Settings dialog (at that page)
//   tortilla --which-ui       show the detected desktop and backend order
//   tortilla --in-use DIR     (Windows, for the installer) the programs holding
//                             a file in DIR, a line each
//   tortilla --restart-explorer [FILE...]  (Windows, for the installer)
//                             restart Explorer as its user, deleting FILEs
//                             while it's stopped
//   tortilla --register-shell / --unregister-shell [--all-users]
//                             add or remove the file manager's context menu,
//                             for this user or (elevated, as the installer
//                             does) every user
#include "blame/blame_window.h"
#include "branch/browse_refs.h"
#include "branch/create_ref_dialog.h"
#include "branch/merge_dialog.h"
#include "branch/switch_dialog.h"
#include "cache/cache_main.h"
#include "changes/changed_files_window.h"
#include "changes/changes_window.h"
#include "changes/cleanup_dialog.h"
#include "changes/commit_window.h"
#include "changes/conflict_editor.h"
#include "changes/file_action_window.h"
#include "changes/ignore_dialog.h"
#include "changes/remove.h"
#include "changes/rename_dialog.h"
#include "git/askpass_env.h"
#include "git/git_install.h"
#include "git/repo.h"
#include "git/ssh_client.h"
#include "git/temp_files.h"
#include "i18n.h"
#include "log/bisect_dialog.h"
#include "log/log_host.h"
#include "log/log_window.h"
#include "log/reflog_window.h"
#include "log/worktree_window.h"
#include "rebase/rebase_window.h"
#include "remote/clone_dialog.h"
#include "remote/pull_fetch_dialog.h"
#include "remote/push_dialog.h"
#include "settings.h"
#include "ui/askpass.h"
#include "ui/compare.h"
#include "ui/create_repo_dialog.h"
#include "ui/diff_window.h"
#include "ui/export_dialog.h"
#include "ui/git_missing.h"
#include "ui/patch_serial_dialogs.h"
#include "ui/progress_dialog.h"
#include "ui/pull_request_dialog.h"
#include "ui/repo_browser.h"
#include "ui/repo_icon.h"
#include "ui/review_patch.h"
#include "ui/settings_dialog.h"
#include "ui/stash_dialog.h"
#include "ui/submodule_dialogs.h"
#include "ui/svn_dialogs.h"
#include "ui/trust_repository.h"
#include "ui/welcome_window.h"
#include "ui/windows.h"
#include "util/notices.h"
#include "util/repo_path.h"
#include "util/restart_explorer.h"

#include <rapport/rapport.h>
#include <rapport/shell.h>
#include <rapport/sys.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>

#ifndef _WIN32
#include <spawn.h>
#include <sys/wait.h>

extern char** environ;

namespace
{

// A desktop notification, where there's no console to read a message on
// (started from a menu); nothing if notify-send isn't there.
void notify(const std::string& text)
{
    const char* args[] = {"notify-send", "--app-name=Tortilla", "Tortilla", text.c_str(), nullptr};
    pid_t pid = 0;
    if (posix_spawnp(&pid, "notify-send", nullptr, nullptr, const_cast<char* const*>(args), environ) == 0)
        waitpid(pid, nullptr, 0);
}

// The toolkit packages are one per toolkit, picked by the user (the package
// managers can't see the running desktop). Once per toolkit, when the one
// that suits this desktop best isn't the one running (its package isn't
// installed), says which package that is. Not when a backend was chosen.
void show_toolkit_hint(rap::ui& ui, const rap::load_options& opts)
{
    if (!opts.backend.empty() || std::getenv("RAPPORT_BACKEND")) return;
    const rap::detection d = rap::detect(opts);
    if (d.preference.empty() || d.desktop.empty()) return;
    const std::string best = d.preference.front();
    if (best == ui.backend_name()) return;
    const std::string package = best == "qt6"    ? "tortilla-qt"
                                : best == "gtk4" ? "tortilla-gtk4"
                                : best == "gtk3" ? "tortilla-gtk3"
                                                 : "";
    tortilla::settings& st = tortilla::settings::get();
    const std::string key = "hint.toolkit." + best;
    if (package.empty() || st.value(key) == "1") return;
    st.set(key, "1");
    ui.post(
        [&ui, desktop = d.desktop, package]
        {
            ui.message(nullptr,
                       {.icon = rap::message_spec::icon::info,
                        .title = "Tortilla",
                        .text = rap::i18n::format(_("On {0}, {1} fits in better: its own toolkit, theme and dialogs."),
                                                  desktop, package),
                        .detail = rap::i18n::format(_("Install the {0} package for it. This is said only once."),
                                                    package)},
                       [](rap::dialog_role) {});
        });
}

}  // namespace
#endif

namespace
{

// The notices a write left for the repository at `path` (util/notices.h),
// together, or "" when there are none.
std::string notices_for(const std::string& path)
{
    std::string detail;
    if (auto r = tortilla::repo::open(path))
        for (const std::string& n : tortilla::take_notices(tortilla::notices_dir(), r->workdir()))
            detail += (detail.empty() ? "" : "\n\n") + n;
    return detail;
}

// A log another `tortilla log` handed to this one, the log windows' host
// (log/log_host.h): opened as that process would have, from trusting the
// repository to the window, except that it's a tab of the log window used
// last unless the request says a window (log.openIn), and a failure is said
// without ending the host's other windows. Held meanwhile, so the host
// doesn't end while it's asked or said.
void open_forwarded_log(rap::ui& ui, const tortilla::log_request& r)
{
    const size_t opening = tortilla::windows::hold({});
    auto open = [&ui, r, opening]
    {
        tortilla::use_repo_icon(ui, r.path, r.submodule);
        // A tab of the log window used last, or a window (log.openIn).
        auto log = tortilla::log_window::open_forwarded(ui, r,
                                                        [](std::unique_ptr<tortilla::log_window> w)
                                                        { tortilla::windows::keep(std::move(w)); });
        if (!log && r.welcome)
            tortilla::windows::keep(tortilla::welcome_window::open(ui));
        else if (!log)
        {
            const size_t said = tortilla::windows::hold({});
            ui.message(nullptr,
                       {.icon = rap::message_spec::icon::error, .title = "Tortilla", .text = log.error()},
                       [said](rap::dialog_role) { tortilla::windows::release(said); });
        }
        tortilla::windows::release(opening);
    };
    // Nothing given: no repository asked for, so none to trust.
    if (r.welcome) return open();
    tortilla::ensure_trusted(ui, r.path,
                             [&ui, r, open, opening](bool go_on)
                             {
                                 if (!go_on) return tortilla::windows::release(opening);
                                 const std::string detail = notices_for(r.path);
                                 if (detail.empty()) return open();
                                 ui.message(nullptr,
                                            {.icon = rap::message_spec::icon::warning,
                                             .title = _("Tortilla"),
                                             .text = _("A change made here ended after its window was closed:"),
                                             .detail = detail},
                                            [open](rap::dialog_role) { open(); });
                             });
}

}  // namespace

int main(int argc, char** argv)
{
    // The language first: what follows shows text, and git inherits it.
    tortilla::setup_language();
    // Started by git or ssh for an answer (a password, a passphrase): asked
    // in a window, answered on our output, so before the console is taken.
    if (tortilla::started_as_askpass(argc))
    {
        // For git that mustn't ask (a sync, a background fetch): no.
        if (const char* m = std::getenv(tortilla::k_askpass_marker);
            m && std::strcmp(m, tortilla::k_askpass_never) == 0)
            return 1;
        rap::load_options askpass;
        askpass.look.color_scheme = tortilla::appearance();
        auto ui = rap::ui::load(argc, argv, askpass);
        if (!ui) return 1;
        return tortilla::run_askpass(**ui, argv[1]);
    }
    // The badges' status cache: no window, no toolkit, started by the file
    // manager extension.
    if (argc == 2 && std::strcmp(argv[1], "cache") == 0) return tortilla::run_cache();
#ifdef _WIN32
    // A GUI program has no console; started from one, write to it, so
    // --which-ui and --register-shell can be read.
    if (AttachConsole(ATTACH_PARENT_PROCESS))
    {
        std::freopen("CONOUT$", "w", stdout);
        std::freopen("CONOUT$", "w", stderr);
    }
#endif
    // The SSH client's settings, read here before any worker runs git.
    tortilla::ssh_environment();
    // Revision files a day old and more, from a crash or a file opened in
    // another program: one listing of the temporary folder.
    tortilla::sweep_temp_dirs();
    rap::load_options opts;
    std::string command = "log";
    bool named = false;  // a command was given (else the log, or the welcome window)
    bool all_branches = false;
    bool submodule = false;  // the log of a submodule's folder in the repository around it
    std::string select;      // the log's commit to select
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strncmp(argv[i], "--ui=", 5) == 0)
            opts.backend = argv[i] + 5;
        else if (std::strcmp(argv[i], "--all") == 0)
            all_branches = true;
        else if (std::strcmp(argv[i], "--submodule") == 0)
            submodule = true;
        // The log opens on that commit, selected, rather than the top row.
        else if (std::strncmp(argv[i], "--select=", 9) == 0)
            select = argv[i] + 9;
        else if (std::strcmp(argv[i], "--which-ui") == 0)
        {
            auto d = rap::detect(opts);
            std::printf("desktop: %s\nbackends:",
                        d.desktop.empty() ? "unknown" : d.desktop.c_str());
            for (auto& b : d.preference) std::printf(" %s", b.c_str());
            std::printf("\n");
            return 0;
        }
        else if (std::strcmp(argv[i], "--in-use") == 0 && i + 1 < argc)
        {
            // For the installer: who holds a file in the folder, a line each.
            for (const std::string& name : tortilla::programs_using(argv[i + 1]))
                std::printf("%s\n", name.c_str());
            return 0;
        }
        else if (std::strcmp(argv[i], "--restart-explorer") == 0)
        {
            // For the installer: Explorer restarted to load the extension,
            // the files after it (the old extension, moved aside) deleted
            // meanwhile.
            std::vector<std::string> leftovers;
            for (int j = i + 1; j < argc; ++j)
                if (*argv[j]) leftovers.push_back(argv[j]);  // the installer's "" for none
            return tortilla::restart_explorer(leftovers) ? 0 : 1;
        }
        else if (std::strcmp(argv[i], "--register-shell") == 0 ||
                 std::strcmp(argv[i], "--unregister-shell") == 0)
        {
            // tortilla-shell sits beside tortilla; on Windows it registers
            // itself per user. Elsewhere packages install it.
            const bool add = argv[i][2] == 'r';
            bool everyone = false;
            for (int j = 1; j < argc; ++j)
                everyone = everyone || std::strcmp(argv[j], "--all-users") == 0;
            const auto scope = everyone ? rap::shell::scope::machine : rap::shell::scope::user;
#ifdef _WIN32
            const auto module = rap::sys::executable_path().parent_path() / "tortilla-shell.dll";
#else
            const auto module = rap::sys::executable_path().parent_path() / "tortilla-shell";
#endif
            const bool ok =
                add ? rap::shell::install(module, scope) : rap::shell::uninstall(module, scope);
            std::fprintf(stderr, "tortilla: %s%s the context menu\n", ok ? "" : "couldn't ",
                         add ? (ok ? "registered" : "register")
                             : (ok ? "unregistered" : "unregister"));
            return ok ? 0 : 1;
        }
        else if (args.empty() && command == "log" &&
                 (std::strcmp(argv[i], "log") == 0 || std::strcmp(argv[i], "rebase") == 0 ||
                  std::strcmp(argv[i], "diff") == 0 || std::strcmp(argv[i], "settings") == 0 ||
                  std::strcmp(argv[i], "repostatus") == 0 || std::strcmp(argv[i], "commit") == 0 ||
                  std::strcmp(argv[i], "add") == 0 || std::strcmp(argv[i], "revert") == 0 ||
                  std::strcmp(argv[i], "resolve") == 0 || std::strcmp(argv[i], "clone") == 0 ||
                  std::strcmp(argv[i], "fetch") == 0 || std::strcmp(argv[i], "pull") == 0 ||
                  std::strcmp(argv[i], "push") == 0 || std::strcmp(argv[i], "branch") == 0 ||
                  std::strcmp(argv[i], "tag") == 0 || std::strcmp(argv[i], "switch") == 0 ||
                  std::strcmp(argv[i], "merge") == 0 || std::strcmp(argv[i], "mergeabort") == 0 ||
                  std::strcmp(argv[i], "refbrowse") == 0 || std::strcmp(argv[i], "prevdiff") == 0 ||
                  std::strcmp(argv[i], "reflog") == 0 || std::strcmp(argv[i], "stashlist") == 0 ||
                  std::strcmp(argv[i], "stashsave") == 0 ||
                  std::strcmp(argv[i], "stashapply") == 0 ||
                  std::strcmp(argv[i], "stashpop") == 0 || std::strcmp(argv[i], "subadd") == 0 ||
                  std::strcmp(argv[i], "subupdate") == 0 || std::strcmp(argv[i], "subsync") == 0 ||
                  std::strcmp(argv[i], "svndcommit") == 0 ||
                  std::strcmp(argv[i], "svnrebase") == 0 || std::strcmp(argv[i], "svnfetch") == 0 ||
                  std::strcmp(argv[i], "svnignore") == 0 ||
                  std::strcmp(argv[i], "formatpatch") == 0 ||
                  std::strcmp(argv[i], "importpatch") == 0 ||
                  std::strcmp(argv[i], "reviewpatch") == 0 ||
                  std::strcmp(argv[i], "repocreate") == 0 || std::strcmp(argv[i], "rename") == 0 ||
                  std::strcmp(argv[i], "cleanup") == 0 || std::strcmp(argv[i], "compare") == 0 ||
                  std::strcmp(argv[i], "blame") == 0 || std::strcmp(argv[i], "showcompare") == 0 ||
                  std::strcmp(argv[i], "repobrowser") == 0 || std::strcmp(argv[i], "export") == 0 ||
                  std::strcmp(argv[i], "conflicteditor") == 0 ||
                  std::strcmp(argv[i], "bisect") == 0 || std::strcmp(argv[i], "about") == 0 ||
                  std::strcmp(argv[i], "help") == 0 || std::strcmp(argv[i], "worktrees") == 0 ||
                  std::strcmp(argv[i], "pullrequest") == 0 || std::strcmp(argv[i], "remove") == 0 ||
                  std::strcmp(argv[i], "removekeep") == 0 || std::strcmp(argv[i], "ignore") == 0))
        {
            command = argv[i];
            named = true;
        }
        else
            args.push_back(argv[i]);
    }
    // bisect's step comes before its path.
    const size_t path_at = command == "bisect" ? 1 : 0;
    std::string path = args.size() <= path_at ? "." : args[path_at];

    // Explorer's Help [spec 4.2, row 61]: the documentation, online; no window.
    if (command == "help") return tortilla::open_help() ? 0 : 1;

    // A log goes to the log windows' host, when one is running, and this
    // process ends (log/log_host.h): its path made absolute first, as the
    // host's own folder is another. Not with --ui=, which asks for a toolkit
    // of its own. With no host, this process claims the endpoint at once,
    // before its UI loads: of two started together one hosts, and the other
    // hands its log over once the first is ready (or opens its own, if that
    // takes too long). A host that doesn't take it leaves it to this process.
    const bool hosting = command == "log" && opts.backend.empty() && tortilla::log_host_enabled();
    std::unique_ptr<tortilla::log_host> claimed;
    if (hosting)
    {
        auto start = tortilla::forward_or_claim(
            {.path = tortilla::log_request_path(path),
             .select = select,
             .all_branches = all_branches,
             .submodule = submodule,
             .welcome = !named && args.empty(),
             .new_window = tortilla::log_open_in_setting() == tortilla::log_open_in::window});
        if (start.forwarded) return 0;
        claimed = std::move(start.host);
    }

    // Light or dark as Settings says, from the first window.
    opts.look.color_scheme = tortilla::appearance();
    // Under libadwaita (GNOME): its message boxes, and settings groups that
    // map cleanly as boxed lists. Header bars in dialogs wait until each
    // dialog's buttons have been looked at in one.
    opts.look.alert_dialogs = true;
    opts.look.boxed_lists = true;
    auto ui = rap::ui::load(argc, argv, opts);
    // The endpoint claimed above: from here on held after the ui, so it's
    // gone (its thread stopped, posting nothing more) before the ui is.
    // Until serve_logs() below, requests wait; ending before then (a
    // repository not trusted, a log that didn't open) turns them away.
    std::unique_ptr<tortilla::log_host> host = std::move(claimed);
    if (!ui)
    {
        std::fprintf(stderr, "%s", ui.error().c_str());
#ifndef _WIN32
        // Only tortilla-core installed: the toolkit is a package of its own,
        // chosen to suit the desktop. Said where a menu launch shows it too.
        const std::string text = _("Tortilla needs one of its user interfaces: install tortilla-gtk4, "
                                   "tortilla-qt or tortilla-gtk3, whichever suits your desktop.");
        std::fprintf(stderr, "tortilla: %s\n", text.c_str());
        notify(text);
#endif
        return 1;
    }
#ifndef _WIN32
    show_toolkit_hint(**ui, opts);
#endif

    // Without git Tortilla is a viewer: said once a session; a command that
    // needs git says so rather than failing as it runs.
    if (!tortilla::git_installed())
    {
        static const std::set<std::string, std::less<>> reads{
            "log",       "settings",    "repostatus", "refbrowse", "reflog",
            "stashlist", "repobrowser", "worktrees",  "about",     "help"};
        if (!reads.contains(command))
        {
            tortilla::show_git_missing(**ui, nullptr, [&] { (*ui)->quit(); });
            (*ui)->run();
            return 1;
        }
        tortilla::notice_if_git_missing(**ui);
    }

    // A repository git refuses for its folder's owner (a network share,
    // WSL's files from Windows): offered to trust it first, as TortoiseGit
    // does, for every command that opens one.
    if (command != "clone" && command != "repocreate" && command != "about" &&
        !(command == "settings" && args.empty()) && !(command == "log" && !named && args.empty()))
    {
        // Usually answered at once; when it asks, the question runs a loop
        // of its own, which the answer ends.
        bool go_on = true, decided = false, asking = false;
        tortilla::ensure_trusted(**ui, path,
                                 [&](bool ok)
                                 {
                                     go_on = ok;
                                     decided = true;
                                     if (asking) (*ui)->quit();
                                 });
        if (!decided)
        {
            asking = true;
            (*ui)->run();
        }
        if (!go_on) return 1;

        // What a write said after its window on this repository closed
        // [util/notices.h], shown once before going on.
        if (const std::string detail = notices_for(path); !detail.empty())
        {
            bool seen = false;
            (*ui)->message(nullptr,
                           {.icon = rap::message_spec::icon::warning,
                            .title = _("Tortilla"),
                            .text = _("A change made here ended after its window was closed:"),
                            .detail = detail},
                           [&](rap::dialog_role)
                           {
                               seen = true;
                               (*ui)->quit();
                           });
            if (!seen) (*ui)->run();
        }
    }

    // Windows of a repository carry its colour on the taskbar.
    if (command != "clone" && command != "repocreate" && command != "about")
        tortilla::use_repo_icon(**ui, path, command == "log" && submodule);

    if (command == "settings")
    {
        // tortilla settings [path] [diff|menu|network|remotes|accounts]: the page to open on.
        const std::string page = args.size() > 1 ? args[1] : "";
        tortilla::show_settings(**ui, nullptr, path,
                                page == "diff"       ? tortilla::settings_page::diff_viewer
                                : page == "menu"     ? tortilla::settings_page::context_menu
                                : page == "network"  ? tortilla::settings_page::network
                                : page == "remotes"  ? tortilla::settings_page::remotes
                                : page == "accounts" ? tortilla::settings_page::accounts
                                                     : tortilla::settings_page::general,
                                {}, [&] { (*ui)->quit(); });
        return (*ui)->run();
    }

    if (command == "commit")
    {
        auto w = tortilla::commit_window::open(**ui, args.empty() ? std::vector<std::string>{path}
                                                                  : args);
        if (!w)
        {
            std::fprintf(stderr, "tortilla: %s\n", w.error().c_str());
            return 1;
        }
        int code = 0;
        (*w)->on_closed(
            [&](bool committed)
            {
                code = committed ? 0 : 1;
                (*ui)->quit();
            });
        (*w)->show();
        (*ui)->run();
        return code;
    }

    // Explorer's Rename [spec 4.2, row 36].
    if (command == "rename")
    {
        int code = 1;
        tortilla::rename_item(**ui, path,
                              [&](bool renamed)
                              {
                                  code = renamed ? 0 : 1;
                                  (*ui)->quit();
                              });
        (*ui)->run();
        return code;
    }

    // Explorer's Create repository here [spec 4.2, row 47].
    if (command == "repocreate")
    {
        int code = 1;
        tortilla::create_repository(**ui, path == "." ? rap::sys::path_to_utf8(std::filesystem::current_path()) : path,
                                    [&](bool made)
                                    {
                                        code = made ? 0 : 1;
                                        (*ui)->quit();
                                    });
        (*ui)->run();
        return code;
    }

    // Explorer's Export [spec 4.2, row 46]: the whole tree at rev (HEAD),
    // whichever folder it was asked in; a bare repository's too.
    if (command == "export")
    {
        auto repo = tortilla::repo::open(path);
        if (!repo)
        {
            std::fprintf(stderr, "tortilla: %s\n", repo.error().c_str());
            (*ui)->message(
                nullptr,
                {.icon = rap::message_spec::icon::error, .title = "Tortilla", .text = repo.error()},
                [&](rap::dialog_role) { (*ui)->quit(); });
            (*ui)->run();
            return 1;
        }
        tortilla::export_revision(**ui, nullptr, repo->workdir(), args.size() > 1 ? args[1] : "",
                                  [&] { (*ui)->quit(); });
        return (*ui)->run();
    }

    // Explorer's stash commands [spec 4.2, rows 24-26]: Stash changes asks
    // for its message; Apply and Pop run in the progress dialog.
    if (command == "stashsave" || command == "stashapply" || command == "stashpop")
    {
        auto repo = tortilla::repo::open(path);
        if (!repo || repo->bare())
        {
            const std::string error = repo ? std::string(_("This needs a working tree.")) : repo.error();
            std::fprintf(stderr, "tortilla: %s\n", error.c_str());
            (*ui)->message(nullptr, {.icon = rap::message_spec::icon::error, .title = "Tortilla", .text = error},
                           [&](rap::dialog_role) { (*ui)->quit(); });
            (*ui)->run();
            return 1;
        }
        tortilla::windows::init(**ui);
        const size_t running = tortilla::windows::hold({});
        auto done = [running](bool) { tortilla::windows::release(running); };
        if (command == "stashsave")
            tortilla::stash_changes(**ui, nullptr, repo->workdir(), done);
        else
        {
            const bool pop = command == "stashpop";
            std::string root = repo->workdir();
            while (root.size() > 1 && (root.back() == '/' || root.back() == '\\')) root.pop_back();
            tortilla::run_progress_dialog(
                **ui, nullptr,
                rap::i18n::format(pop ? _("{0} - Stash Pop") : _("{0} - Stash Apply"),
                                  rap::sys::path_to_utf8(rap::sys::path_from_utf8(root).make_preferred())),
                repo->workdir(), {{"stash", pop ? "pop" : "apply"}}, done);
        }
        return (*ui)->run();
    }

    // Explorer's Bisect entries [spec 4.2, rows 28-32]: start asks for the
    // good and bad revisions; the others run on HEAD in the progress dialog.
    if (command == "bisect")
    {
        const std::optional<tortilla::bisect_step> step =
            args.empty() ? std::nullopt : tortilla::parse_bisect_step(args[0]);
        auto fail = [&ui](const std::string& error)
        {
            std::fprintf(stderr, "tortilla: %s\n", error.c_str());
            (*ui)->message(
                nullptr,
                {.icon = rap::message_spec::icon::error, .title = "Tortilla", .text = error},
                [&ui](rap::dialog_role) { (*ui)->quit(); });
            (*ui)->run();
            return 1;
        };
        if (!step) return fail("usage: tortilla bisect start|good|bad|skip|reset [path]");
        tortilla::windows::init(**ui);
        if (*step == tortilla::bisect_step::start)
        {
            auto d = tortilla::bisect_start_dialog::open(**ui, path);
            if (!d) return fail(d.error());
            tortilla::windows::keep(std::move(*d));
            return (*ui)->run();
        }
        auto repo = tortilla::repo::open(path);
        if (!repo || repo->bare())
            return fail(repo ? std::string(_("This needs a working tree.")) : repo.error());
        const size_t running = tortilla::windows::hold({});
        tortilla::run_bisect(**ui, nullptr, repo->workdir(), *step, {},
                             [running](bool) { tortilla::windows::release(running); });
        return (*ui)->run();
    }

    // Explorer's submodule commands [spec 4.2, rows 53-55]: Add and Update
    // ask first; Sync runs in the progress dialog. All from the clicked
    // folder's working tree.
    if (command == "subadd" || command == "subupdate" || command == "subsync")
    {
        auto repo = tortilla::repo::open(path);
        if (!repo || repo->bare())
        {
            const std::string error =
                repo ? std::string(_("This needs a working tree.")) : repo.error();
            std::fprintf(stderr, "tortilla: %s\n", error.c_str());
            (*ui)->message(
                nullptr,
                {.icon = rap::message_spec::icon::error, .title = "Tortilla", .text = error},
                [&](rap::dialog_role) { (*ui)->quit(); });
            (*ui)->run();
            return 1;
        }
        tortilla::windows::init(**ui);
        const size_t running = tortilla::windows::hold({});
        auto done = [running](bool) { tortilla::windows::release(running); };
        if (command == "subadd")
            tortilla::add_submodule(**ui, nullptr, repo->workdir(), path, done);
        else if (command == "subupdate")
            tortilla::update_submodules(**ui, nullptr, repo->workdir(), path, done);
        else
            tortilla::sync_submodules(**ui, nullptr, repo->workdir(), done);
        return (*ui)->run();
    }

    // Explorer's Review/apply single patch [spec 4.2, row 58; 4.4]: the
    // patch given, or one chosen (TortoiseGitMerge asks too), applied to the
    // working tree given, else the one the patch (or the item) is in, else
    // one chosen.
    if (command == "reviewpatch")
    {
        auto is_patch = [](const std::string& a)
        {
            std::string ext = rap::sys::path_from_utf8(a).extension().string();
            for (char& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
            return ext == ".patch" || ext == ".diff";
        };
        auto absolute = [](const std::string& a)
        {
            std::error_code ec;
            const auto at = std::filesystem::absolute(rap::sys::path_from_utf8(a), ec);
            return ec ? a : rap::sys::path_to_utf8(at);
        };
        std::string patch, where;
        for (const std::string& a : args)
            if (patch.empty() && is_patch(a))
                patch = absolute(a);
            else if (where.empty())
                where = a;
        if (where.empty())
            where = patch.empty()
                        ? "."
                        : rap::sys::path_to_utf8(rap::sys::path_from_utf8(patch).parent_path());
        tortilla::windows::init(**ui);
        const size_t running = tortilla::windows::hold({});
        int code = 1;
        auto done = [running, &code](bool ok)
        {
            code = ok ? 0 : 1;
            tortilla::windows::release(running);
        };
        auto review = [&ui, done](const std::string& at, const std::string& file)
        {
            auto repo = tortilla::repo::open(at);
            if (!repo || repo->bare())
            {
                const std::string error =
                    repo ? std::string(_("This needs a working tree.")) : repo.error();
                (*ui)->message(
                    nullptr,
                    {.icon = rap::message_spec::icon::error, .title = "Tortilla", .text = error},
                    [done](rap::dialog_role) { done(false); });
                return;
            }
            tortilla::review_patch(**ui, nullptr, repo->workdir(), file, done);
        };
        // The working tree, then the patch, each asked for if not known.
        auto with_tree = [&ui, review, done, where](const std::string& file)
        {
            if (tortilla::repo::open(where)) return review(where, file);
            (*ui)->choose_files(nullptr,
                                {.mode = rap::file_dialog_spec::mode::folder,
                                 .title = _("Apply the patch to the working tree in")},
                                [review, done, file](std::vector<std::string> chosen)
                                {
                                    if (chosen.empty())
                                        done(false);
                                    else
                                        review(chosen.front(), file);
                                });
        };
        if (!patch.empty())
            with_tree(patch);
        else
            (*ui)->choose_files(
                nullptr,
                {.mode = rap::file_dialog_spec::mode::open,
                 .title = _("Review a patch"),
                 .filters = {{_("Patches"), "*.patch;*.diff"}, {_("All files"), "*"}}},
                [with_tree, done](std::vector<std::string> chosen)
                {
                    if (chosen.empty())
                        done(false);
                    else
                        with_tree(chosen.front());
                });
        (*ui)->run();
        return code;
    }

    // Explorer's Apply Patch Serial [spec 4.2, row 57]: on a working tree's
    // folder, or on patch files, applied to the working tree they're in, or
    // else to one chosen, as TortoiseGit asks.
    if (command == "importpatch")
    {
        std::vector<std::string> patches;
        std::string where;
        for (const std::string& a : args)
        {
            std::string ext = rap::sys::path_from_utf8(a).extension().string();
            for (char& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
            if (ext == ".patch" || ext == ".diff")
            {
                std::error_code ec;
                const auto at = std::filesystem::absolute(rap::sys::path_from_utf8(a), ec);
                patches.push_back(ec ? a : rap::sys::path_to_utf8(at));
                if (where.empty())
                    where = rap::sys::path_to_utf8(rap::sys::path_from_utf8(a).parent_path());
            }
            else if (where.empty() || patches.empty())
                where = a;
        }
        if (where.empty()) where = ".";
        tortilla::windows::init(**ui);
        const size_t running = tortilla::windows::hold({});
        int code = 1;
        auto done = [running, &code](bool ok)
        {
            code = ok ? 0 : 1;
            tortilla::windows::release(running);
        };
        auto apply = [&ui, patches, done](const std::string& at)
        {
            auto repo = tortilla::repo::open(at);
            if (!repo || repo->bare())
            {
                const std::string error =
                    repo ? std::string(_("This needs a working tree.")) : repo.error();
                (*ui)->message(
                    nullptr,
                    {.icon = rap::message_spec::icon::error, .title = "Tortilla", .text = error},
                    [done](rap::dialog_role) { done(false); });
                return;
            }
            tortilla::apply_patch_serial(**ui, nullptr, repo->workdir(), patches, done);
        };
        if (tortilla::repo::open(where))
            apply(where);
        else
            (*ui)->choose_files(nullptr,
                                {.mode = rap::file_dialog_spec::mode::folder,
                                 .title = _("Apply the patches to the working tree in")},
                                [apply, done](std::vector<std::string> chosen)
                                {
                                    if (chosen.empty())
                                        done(false);
                                    else
                                        apply(chosen.front());
                                });
        (*ui)->run();
        return code;
    }

    // Explorer's git-svn commands [spec 4.2, rows 8-11] and Create Patch
    // Serial [row 56], for the working tree at path.
    if (command == "svndcommit" || command == "svnrebase" || command == "svnfetch" ||
        command == "svnignore" || command == "formatpatch")
    {
        auto repo = tortilla::repo::open(path);
        if (!repo || repo->bare())
        {
            const std::string error =
                repo ? std::string(_("This needs a working tree.")) : repo.error();
            std::fprintf(stderr, "tortilla: %s\n", error.c_str());
            (*ui)->message(
                nullptr,
                {.icon = rap::message_spec::icon::error, .title = "Tortilla", .text = error},
                [&](rap::dialog_role) { (*ui)->quit(); });
            (*ui)->run();
            return 1;
        }
        tortilla::windows::init(**ui);
        const size_t running = tortilla::windows::hold({});
        int code = 1;
        auto done = [running, &code](bool ok)
        {
            code = ok ? 0 : 1;
            tortilla::windows::release(running);
        };
        if (command == "formatpatch")
        {
            // Explorer's Create Patch Serial [spec 4.2, row 56]: into the
            // folder clicked, by default.
            std::error_code ec;
            const auto at = std::filesystem::absolute(rap::sys::path_from_utf8(path), ec);
            tortilla::create_patch_serial(**ui, nullptr, repo->workdir(),
                                          ec ? path : rap::sys::path_to_utf8(at), done);
        }
        else if (command == "svndcommit")
            tortilla::svn_dcommit(
                **ui, [] { return (rap::window*)nullptr; }, repo->workdir(), done);
        else if (command == "svnrebase")
            tortilla::svn_rebase(**ui, [] { return (rap::window*)nullptr; }, repo->workdir(), done);
        else if (command == "svnfetch")
            tortilla::svn_fetch(**ui, [] { return (rap::window*)nullptr; }, repo->workdir(), done);
        else
            tortilla::svn_import_ignore(
                **ui, [] { return (rap::window*)nullptr; }, repo->workdir(), done);
        (*ui)->run();
        return code;
    }

    // The remote dialogs, and the windows their follow-ups open: the program
    // runs until the last of them closes.
    if (command == "clone" || command == "fetch" || command == "pull" || command == "push" ||
        command == "branch" || command == "tag" || command == "switch" || command == "merge" ||
        command == "mergeabort" || command == "refbrowse" || command == "reflog" ||
        command == "stashlist" || command == "blame" || command == "repobrowser" ||
        command == "showcompare" || command == "worktrees" || command == "pullrequest")
    {
        tortilla::windows::init(**ui);
        // The branch dialogs: the second argument preselects a ref or hash.
        auto keep = [&ui](auto d)
        {
            if (!d)
            {
                // No console to read it on when started from a file
                // manager: said in a window too, as the log does.
                std::fprintf(stderr, "tortilla: %s\n", d.error().c_str());
                (*ui)->message(nullptr,
                               {.icon = rap::message_spec::icon::error, .title = "Tortilla", .text = d.error()},
                               [&ui](rap::dialog_role) { (*ui)->quit(); });
                (*ui)->run();
                return false;
            }
            tortilla::windows::keep(std::move(*d));
            return true;
        };
        const std::string rev = args.size() > 1 ? args[1] : "";
        if (command == "branch" || command == "tag")
            return keep(tortilla::create_ref_dialog::open(**ui, path, command == "tag", rev))
                       ? (*ui)->run()
                       : 1;
        if (command == "switch")
            return keep(tortilla::switch_dialog::open(**ui, path, rev)) ? (*ui)->run() : 1;
        if (command == "merge")
            return keep(tortilla::merge_dialog::open(**ui, path, rev)) ? (*ui)->run() : 1;
        if (command == "mergeabort")
            return keep(tortilla::abort_merge_dialog::open(**ui, path)) ? (*ui)->run() : 1;
        if (command == "refbrowse")
            return keep(tortilla::browse_refs::open(**ui, path, {})) ? (*ui)->run() : 1;
        if (command == "repobrowser")
            return keep(tortilla::repo_browser::open(**ui, path, rev)) ? (*ui)->run() : 1;
        if (command == "blame")
            return keep(tortilla::blame_window::open(**ui, path, rev)) ? (*ui)->run() : 1;
        if (command == "showcompare")
            return keep(tortilla::changed_files_window::open(**ui, path, rev,
                                                             args.size() > 2 ? args[2] : ""))
                       ? (*ui)->run()
                       : 1;
        if (command == "pullrequest")
        {
            auto repo = tortilla::repo::open(path);
            if (!repo)
                return keep(std::expected<std::unique_ptr<tortilla::pull_request_dialog>,
                                          std::string>(std::unexpected(repo.error())))
                           ? (*ui)->run()
                           : 1;
            const std::string name = rev.empty() ? repo->head_name() : rev;
            const tortilla::ref branch{.name = name,
                                       .full_name = "refs/heads/" + name,
                                       .kind = tortilla::ref_kind::local_branch};
            return keep(tortilla::pull_request_dialog::open(
                       **ui, repo->workdir(), branch, tortilla::forge_remotes(repo->workdir())))
                       ? (*ui)->run()
                       : 1;
        }
        if (command == "worktrees")
            return keep(tortilla::worktree_window::open(**ui, path)) ? (*ui)->run() : 1;
        if (command == "reflog" || command == "stashlist")
            return keep(tortilla::reflog_window::open(**ui, path,
                                                      command == "stashlist" ? "refs/stash" : rev))
                       ? (*ui)->run()
                       : 1;
        if (command == "clone")
            tortilla::windows::keep(tortilla::clone_dialog::open(**ui, args.empty() ? "" : args[0],
                                                                 args.size() > 1 ? args[1] : ""));
        else if (command == "push")
        {
            auto d = tortilla::push_dialog::open(**ui, path);
            if (!d)
            {
                std::fprintf(stderr, "tortilla: %s\n", d.error().c_str());
                return 1;
            }
            tortilla::windows::keep(std::move(*d));
        }
        else
        {
            auto d = tortilla::pull_fetch_dialog::open(**ui, path, command == "pull");
            if (!d)
            {
                std::fprintf(stderr, "tortilla: %s\n", d.error().c_str());
                return 1;
            }
            tortilla::windows::keep(std::move(*d));
        }
        return (*ui)->run();
    }

    // A conflicted file in the merge tool [spec 9.3.1].
    if (command == "conflicteditor")
    {
        auto repo = tortilla::repo::open(path);
        if (!repo)
        {
            std::fprintf(stderr, "tortilla: %s\n", repo.error().c_str());
            return 1;
        }
        const std::string rel = tortilla::repo_relative(repo->workdir(), path).value_or("");
        if (rel.empty())
        {
            std::fprintf(stderr, "tortilla: %s isn't a file in the repository\n", path.c_str());
            return 1;
        }
        tortilla::windows::init(**ui);
        const size_t running = tortilla::windows::hold({});
        const std::string workdir = repo->workdir();
        tortilla::edit_conflict(
            **ui, nullptr, workdir, rel, [running](bool) { tortilla::windows::release(running); },
            [&ui, workdir](tortilla::settings_page page)
            {
                const size_t open = tortilla::windows::hold({});
                tortilla::show_settings(**ui, nullptr, workdir, page, {},
                                        [open] { tortilla::windows::release(open); });
            });
        return (*ui)->run();
    }

    if (command == "ignore")
    {
        // tortilla ignore [--delete] [--mask] path...
        bool untrack = false, by_extension = false;
        std::vector<std::string> paths;
        for (const std::string& a : args)
            if (a == "--delete")
                untrack = true;
            else if (a == "--mask")
                by_extension = true;
            else
                paths.push_back(a);
        int code = 1;
        tortilla::show_ignore_dialog(**ui, paths.empty() ? std::vector<std::string>{path} : paths,
                                     by_extension, untrack,
                                     [&](bool done)
                                     {
                                         code = done ? 0 : 1;
                                         (*ui)->quit();
                                     });
        (*ui)->run();
        return code;
    }

    if (command == "cleanup")
    {
        tortilla::windows::init(**ui);
        const size_t running = tortilla::windows::hold({});
        tortilla::clean_up(**ui, nullptr, args.empty() ? std::vector<std::string>{path} : args,
                           [running](bool) { tortilla::windows::release(running); });
        return (*ui)->run();
    }

    if (command == "remove" || command == "removekeep")
    {
        int code = 1;
        tortilla::remove_files(**ui, args.empty() ? std::vector<std::string>{path} : args,
                               command == "removekeep",
                               [&](bool done)
                               {
                                   code = done ? 0 : 1;
                                   (*ui)->quit();
                               });
        (*ui)->run();
        return code;
    }

    if (command == "add" || command == "revert" || command == "resolve")
    {
        const auto action = command == "add"      ? tortilla::file_action::add
                            : command == "revert" ? tortilla::file_action::revert
                                                  : tortilla::file_action::resolve;
        auto w = tortilla::file_action_window::open(
            **ui, action, args.empty() ? std::vector<std::string>{path} : args);
        if (!w)
        {
            std::fprintf(stderr, "tortilla: %s\n", w.error().c_str());
            return 1;
        }
        int code = 0;
        (*w)->on_closed(
            [&](bool done)
            {
                code = done ? 0 : 1;
                (*ui)->quit();
            });
        (*w)->show();
        (*ui)->run();
        return code;
    }

    // A folder's Diff is Check for modifications, as TortoiseGit's [spec 5.1].
    std::error_code ec;
    if (command == "repostatus" ||
        (command == "diff" && args.size() < 2 &&
         std::filesystem::is_directory(rap::sys::path_from_utf8(path), ec)))
    {
        auto w = tortilla::changes_window::open(
            **ui, command == "repostatus" && !args.empty() ? args : std::vector<std::string>{path});
        if (!w)
        {
            std::fprintf(stderr, "tortilla: %s\n", w.error().c_str());
            return 1;
        }
        (*w)->on_closed([&] { (*ui)->quit(); });
        (*w)->show();
        return (*ui)->run();
    }

    // Explorer's Diff of two files, and Diff later [spec 4.4].
    if (command == "compare")
    {
        if (args.size() != 2)
        {
            std::fprintf(stderr, "tortilla: compare needs two files\n");
            return 2;
        }
        auto quit = [&] { (*ui)->quit(); };
        auto tool = tortilla::compare_files(
            **ui, nullptr, args[0], args[1],
            [&]
            {
                tortilla::show_settings(**ui, nullptr, args[1],
                                        tortilla::settings_page::diff_viewer, {}, quit);
            },
            quit);
        return (*ui)->run();
    }

    if ((command == "diff" && args.size() < 2) || command == "prevdiff")
    {
        // The working tree against HEAD, under path, with the chosen diff
        // tool: Explorer's Diff. prevdiff: against the version before the
        // last commit that changed the file, or HEAD~1 for a folder
        // [spec 4.2, row 14; PrevDiffCommand].
        auto repo = tortilla::repo::open(path);
        if (!repo)
        {
            std::fprintf(stderr, "tortilla: %s\n", repo.error().c_str());
            return 1;
        }
        std::vector<std::string> paths;
        if (auto rel = tortilla::repo_relative(repo->workdir(), path); rel && !rel->empty())
            paths.push_back(std::move(*rel));
        auto quit = [&] { (*ui)->quit(); };
        std::string against = "HEAD";
        if (command == "prevdiff")
        {
            if (std::filesystem::is_directory(rap::sys::path_from_utf8(path), ec))
                against = "HEAD~1";
            else
            {
                std::vector<std::string> log_args{"log", "-2", "--format=%H", "--"};
                log_args.insert(log_args.end(), paths.begin(), paths.end());
                const tortilla::git_result r =
                    tortilla::run_git(repo->workdir(), log_args, tortilla::git_output::stdout_only);
                const size_t nl = r.output.find('\n');
                against = r.ok() && nl != std::string::npos ? r.output.substr(nl + 1, 40) : "";
                if (against.size() != 40)
                {
                    (*ui)->message(nullptr,
                                   {.icon = rap::message_spec::icon::error,
                                    .title = "Tortilla",
                                    .text = _("Could not determine the last committed revision!")},
                                   [&](rap::dialog_role) { quit(); });
                    (*ui)->run();
                    return 1;
                }
            }
        }
        auto tool = tortilla::compare_revisions(
            **ui, nullptr, repo->workdir(), {against}, paths,
            [&]
            {
                tortilla::show_settings(**ui, nullptr, repo->workdir(),
                                        tortilla::settings_page::diff_viewer, {}, quit);
            },
            quit);
        return (*ui)->run();
    }

    if (command == "diff")
    {
        // One revision: that commit; two: git diff between them.
        const std::string rev = args[1];
        std::vector<std::string> git_args{"show", "--format=fuller", rev};
        std::string title = rev;
        if (args.size() > 2)
        {
            git_args = {"diff", rev, args[2]};
            title = rev + ".." + args[2];
        }
        tortilla::show_unified_diff(**ui, title, path, git_args, [&] { (*ui)->quit(); });
        return (*ui)->run();
    }

    if (command == "rebase")
    {
        auto rebase = tortilla::rebase_window::open(**ui, path, args.size() > 1 ? args[1] : "");
        if (!rebase)
        {
            std::fprintf(stderr, "tortilla: %s\n", rebase.error().c_str());
            return 1;
        }
        (*rebase)->on_closed([&] { (*ui)->quit(); });
        (*rebase)->show();
        return (*ui)->run();
    }

    tortilla::windows::init(**ui);
    // Once this process's own log (or welcome window) is open, it hosts the
    // log windows later `tortilla log`s ask for, until the last window of
    // all closes. A request taken holds the process (it mustn't end before
    // its window opens) until it's opened, or dropped when its sender gave
    // up and opened its own; requests come one at a time, so one hold.
    size_t reserved = 0;
    auto serve_logs = [&]
    {
        if (!host) return;
        host->serve([&ui](std::function<void()> f) { (*ui)->post(std::move(f)); },
                    [&reserved]
                    {
                        if (tortilla::windows::ending()) return false;
                        reserved = tortilla::windows::hold({});
                        return true;
                    },
                    [&ui, &reserved](const tortilla::log_request& r)
                    {
                        open_forwarded_log(**ui, r);
                        tortilla::windows::release(reserved);
                    },
                    [&reserved] { tortilla::windows::release(reserved); });
    };
    // Started with nothing to open (from the Start menu, a launcher): what
    // Tortilla is and a way in, rather than nothing at all.
    auto welcome = [&]
    {
        tortilla::windows::keep(tortilla::welcome_window::open(**ui));
        if (command == "log") serve_logs();
        return (*ui)->run();
    };
    if (command == "about") return welcome();
    auto log = tortilla::log_window::open(**ui, path, all_branches, submodule);
    if (!log)
    {
        if (!named && args.empty()) return welcome();
        // No console to read it on: said in a window too.
        std::fprintf(stderr, "tortilla: %s\n", log.error().c_str());
        (*ui)->message(nullptr,
                       {.icon = rap::message_spec::icon::error, .title = "Tortilla",
                        .text = log.error()},
                       [&](rap::dialog_role) { (*ui)->quit(); });
        (*ui)->run();
        return 1;
    }
    if (!select.empty()) (*log)->select_commit(select);
    // The log, and the dialogs it opens: until the last closes.
    tortilla::windows::keep(std::move(*log));
    serve_logs();
    return (*ui)->run();
}
