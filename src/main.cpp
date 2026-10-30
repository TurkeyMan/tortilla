// Tortilla: a TortoiseGit-style git GUI.
//   tortilla [log] [--all] [--submodule] [path]   log window for the repository
//                                     containing path, limited to path; --submodule:
//                                     the repository around a submodule at path
//                                     with nothing given and none here, the welcome window
//   tortilla about                    the welcome window: what Tortilla is, links
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
//   tortilla reflog [path] [ref]       the RefLog dialog, on HEAD or ref
//   tortilla stashlist [path]          the RefLog dialog on the stash
//   tortilla prevdiff [path]           the file against its previous version
//   tortilla stashsave|stashapply|stashpop [path]  Stash changes, Apply or Pop
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
//   tortilla settings [path] [diff|menu|network|remotes]  the Settings dialog (at that page)
//   tortilla --which-ui       show the detected desktop and backend order
//   tortilla --register-shell / --unregister-shell [--all-users]
//                             add or remove the file manager's context menu,
//                             for this user or (elevated, as the installer
//                             does) every user
#include "blame/blame_window.h"
#include "branch/browse_refs.h"
#include "branch/create_ref_dialog.h"
#include "branch/merge_dialog.h"
#include "branch/switch_dialog.h"
#include "changes/changed_files_window.h"
#include "changes/changes_window.h"
#include "changes/cleanup_dialog.h"
#include "changes/commit_window.h"
#include "changes/conflict_editor.h"
#include "changes/file_action_window.h"
#include "changes/ignore_dialog.h"
#include "changes/remove.h"
#include "changes/rename_dialog.h"
#include "git/repo.h"
#include "git/ssh_client.h"
#include "i18n.h"
#include "log/bisect_dialog.h"
#include "log/log_window.h"
#include "log/reflog_window.h"
#include "rebase/rebase_window.h"
#include "remote/clone_dialog.h"
#include "remote/pull_fetch_dialog.h"
#include "remote/push_dialog.h"
#include "settings.h"
#include "ui/askpass.h"
#include "ui/compare.h"
#include "ui/create_repo_dialog.h"
#include "ui/diff_window.h"
#include "ui/progress_dialog.h"
#include "ui/repo_browser.h"
#include "ui/settings_dialog.h"
#include "ui/stash_dialog.h"
#include "ui/trust_repository.h"
#include "ui/welcome_window.h"
#include "ui/windows.h"

#include <rapport/rapport.h>
#include <rapport/shell.h>
#include <rapport/sys.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <cstdio>
#include <cstring>

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

int main(int argc, char** argv)
{
    // The language first: what follows shows text, and git inherits it.
    tortilla::setup_language();
    // Started by git or ssh for an answer (a password, a passphrase): asked
    // in a window, answered on our output, so before the console is taken.
    if (tortilla::started_as_askpass(argc))
    {
        auto ui = rap::ui::load(argc, argv, {});
        if (!ui) return 1;
        return tortilla::run_askpass(**ui, argv[1]);
    }
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
    rap::load_options opts;
    std::string command = "log";
    bool named = false;  // a command was given (else the log, or the welcome window)
    bool all_branches = false;
    bool submodule = false;  // the log of a submodule's folder in the repository around it
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strncmp(argv[i], "--ui=", 5) == 0)
            opts.backend = argv[i] + 5;
        else if (std::strcmp(argv[i], "--all") == 0)
            all_branches = true;
        else if (std::strcmp(argv[i], "--submodule") == 0)
            submodule = true;
        else if (std::strcmp(argv[i], "--which-ui") == 0)
        {
            auto d = rap::detect(opts);
            std::printf("desktop: %s\nbackends:",
                        d.desktop.empty() ? "unknown" : d.desktop.c_str());
            for (auto& b : d.preference) std::printf(" %s", b.c_str());
            std::printf("\n");
            return 0;
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
                  std::strcmp(argv[i], "stashpop") == 0 ||
                  std::strcmp(argv[i], "repocreate") == 0 || std::strcmp(argv[i], "rename") == 0 ||
                  std::strcmp(argv[i], "cleanup") == 0 || std::strcmp(argv[i], "compare") == 0 ||
                  std::strcmp(argv[i], "blame") == 0 || std::strcmp(argv[i], "showcompare") == 0 ||
                  std::strcmp(argv[i], "repobrowser") == 0 ||
                  std::strcmp(argv[i], "conflicteditor") == 0 ||
                  std::strcmp(argv[i], "bisect") == 0 || std::strcmp(argv[i], "about") == 0 ||
                  std::strcmp(argv[i], "remove") == 0 || std::strcmp(argv[i], "removekeep") == 0 ||
                  std::strcmp(argv[i], "ignore") == 0))
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

    auto ui = rap::ui::load(argc, argv, opts);
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
    }

    if (command == "settings")
    {
        // tortilla settings [path] [diff|menu|network|remotes]: the page to open on.
        const std::string page = args.size() > 1 ? args[1] : "";
        tortilla::show_settings(**ui, nullptr, path,
                                page == "diff"      ? tortilla::settings_page::diff_viewer
                                : page == "menu"    ? tortilla::settings_page::context_menu
                                : page == "network" ? tortilla::settings_page::network
                                : page == "remotes" ? tortilla::settings_page::remotes
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

    // The remote dialogs, and the windows their follow-ups open: the program
    // runs until the last of them closes.
    if (command == "clone" || command == "fetch" || command == "pull" || command == "push" ||
        command == "branch" || command == "tag" || command == "switch" || command == "merge" ||
        command == "mergeabort" || command == "refbrowse" || command == "reflog" ||
        command == "stashlist" || command == "blame" || command == "repobrowser" ||
        command == "showcompare")
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
        std::error_code ec;
        const auto root =
            std::filesystem::weakly_canonical(rap::sys::path_from_utf8(repo->workdir()), ec);
        const auto file = std::filesystem::weakly_canonical(
            std::filesystem::absolute(rap::sys::path_from_utf8(path)), ec);
        const std::string rel =
            rap::sys::path_to_utf8(file.lexically_relative(root).generic_string());
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
        const std::filesystem::path root = rap::sys::path_from_utf8(repo->workdir());
        std::error_code ec;
        const std::filesystem::path where = std::filesystem::relative(
            std::filesystem::absolute(rap::sys::path_from_utf8(path)), root, ec);
        std::vector<std::string> paths;
        if (!ec && !where.empty() && where != ".")
        {
            std::string rel = rap::sys::path_to_utf8(where);
            std::ranges::replace(rel, '\\', '/');
            paths.push_back(rel);
        }
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
    // Started with nothing to open (from the Start menu, a launcher): what
    // Tortilla is and a way in, rather than nothing at all.
    auto welcome = [&]
    {
        tortilla::windows::keep(tortilla::welcome_window::open(**ui));
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
    // The log, and the dialogs it opens: until the last closes.
    tortilla::windows::keep(std::move(*log));
    return (*ui)->run();
}
