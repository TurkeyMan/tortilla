// Tortilla: a TortoiseGit-style git GUI.
//   tortilla [log] [--all] [path]   log window for the repository containing path
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
//   tortilla diff [path] rev [rev2]   a commit, or rev..rev2, as a unified diff
//   tortilla settings [path] [diff|menu|network]  the Settings dialog (at that page)
//   tortilla --which-ui       show the detected desktop and backend order
//   tortilla --register-shell / --unregister-shell [--all-users]
//                             add or remove the file manager's context menu,
//                             for this user or (elevated, as the installer
//                             does) every user
#include "branch/create_ref_dialog.h"
#include "branch/merge_dialog.h"
#include "branch/switch_dialog.h"
#include "changes/changes_window.h"
#include "changes/commit_window.h"
#include "changes/file_action_window.h"
#include "git/repo.h"
#include "git/ssh_client.h"
#include "log/log_window.h"
#include "rebase/rebase_window.h"
#include "remote/clone_dialog.h"
#include "remote/pull_fetch_dialog.h"
#include "remote/push_dialog.h"
#include "ui/askpass.h"
#include "ui/compare.h"
#include "ui/diff_window.h"
#include "ui/settings_dialog.h"
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

int main(int argc, char** argv)
{
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
    bool all_branches = false;
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strncmp(argv[i], "--ui=", 5) == 0)
            opts.backend = argv[i] + 5;
        else if (std::strcmp(argv[i], "--all") == 0)
            all_branches = true;
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
                  std::strcmp(argv[i], "merge") == 0 || std::strcmp(argv[i], "mergeabort") == 0))
            command = argv[i];
        else
            args.push_back(argv[i]);
    }
    std::string path = args.empty() ? "." : args[0];

    auto ui = rap::ui::load(argc, argv, opts);
    if (!ui)
    {
        std::fprintf(stderr, "%s", ui.error().c_str());
        return 1;
    }

    if (command == "settings")
    {
        // tortilla settings [path] [diff|menu|network]: the page to open on.
        const std::string page = args.size() > 1 ? args[1] : "";
        tortilla::show_settings(**ui, nullptr, path,
                                page == "diff"      ? tortilla::settings_page::diff_viewer
                                : page == "menu"    ? tortilla::settings_page::context_menu
                                : page == "network" ? tortilla::settings_page::network
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

    // The remote dialogs, and the windows their follow-ups open: the program
    // runs until the last of them closes.
    if (command == "clone" || command == "fetch" || command == "pull" || command == "push" ||
        command == "branch" || command == "tag" || command == "switch" || command == "merge" ||
        command == "mergeabort")
    {
        tortilla::windows::init(**ui);
        // The branch dialogs: the second argument preselects a ref or hash.
        auto keep = [](auto d)
        {
            if (!d)
            {
                std::fprintf(stderr, "tortilla: %s\n", d.error().c_str());
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

    if (command == "diff" && args.size() < 2)
    {
        // The working tree against HEAD, under path, with the chosen diff
        // tool: Explorer's Diff.
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
        auto tool = tortilla::compare_revisions(
            **ui, nullptr, repo->workdir(), {"HEAD"}, paths,
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

    auto log = tortilla::log_window::open(**ui, path, all_branches);
    if (!log)
    {
        std::fprintf(stderr, "tortilla: %s\n", log.error().c_str());
        return 1;
    }
    // The log, and the dialogs it opens: until the last closes.
    tortilla::windows::init(**ui);
    tortilla::windows::keep(std::move(*log));
    return (*ui)->run();
}
