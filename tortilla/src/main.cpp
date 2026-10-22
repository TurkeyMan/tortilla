// Tortilla: a TortoiseGit-style git GUI.
//   tortilla [log] [--all] [path]   log window for the repository containing path
//   tortilla rebase [path] [upstream]   rebase dialog
//   tortilla diff [path] [rev] [rev2]   a commit (default HEAD), or rev..rev2, as a unified diff
//   tortilla settings [path] [diff]     the Settings dialog (at Diff Viewer)
//   tortilla --which-ui       show the detected desktop and backend order
#include "log/log_window.h"
#include "rebase/rebase_window.h"
#include "ui/diff_window.h"
#include "ui/settings_dialog.h"

#include <rapport/rapport.h>

#include <cstdio>
#include <cstring>

int main(int argc, char** argv)
{
    rap::LoadOptions opts;
    std::string command = "log";
    bool allBranches = false;
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strncmp(argv[i], "--ui=", 5) == 0)
            opts.backend = argv[i] + 5;
        else if (std::strcmp(argv[i], "--all") == 0)
            allBranches = true;
        else if (std::strcmp(argv[i], "--which-ui") == 0)
        {
            auto d = rap::detect(opts);
            std::printf("desktop: %s\nbackends:",
                        d.desktop.empty() ? "unknown" : d.desktop.c_str());
            for (auto& b : d.preference) std::printf(" %s", b.c_str());
            std::printf("\n");
            return 0;
        }
        else if (args.empty() && command == "log" &&
                 (std::strcmp(argv[i], "log") == 0 || std::strcmp(argv[i], "rebase") == 0 ||
                  std::strcmp(argv[i], "diff") == 0 || std::strcmp(argv[i], "settings") == 0))
            command = argv[i];
        else
            args.push_back(argv[i]);
    }
    std::string path = args.empty() ? "." : args[0];

    auto ui = rap::Ui::load(argc, argv, opts);
    if (!ui)
    {
        std::fprintf(stderr, "%s", ui.error().c_str());
        return 1;
    }

    if (command == "settings")
    {
        const bool diff = args.size() > 1 && args[1] == "diff";
        tortilla::showSettings(**ui, nullptr, path,
                               diff ? tortilla::SettingsPage::DiffViewer
                                    : tortilla::SettingsPage::General,
                               {}, [&] { (*ui)->quit(); });
        return (*ui)->run();
    }

    if (command == "diff")
    {
        // One revision: that commit; two: git diff between them.
        const std::string rev = args.size() > 1 ? args[1] : "HEAD";
        std::vector<std::string> gitArgs{"show", "--format=fuller", rev};
        std::string title = rev;
        if (args.size() > 2)
        {
            gitArgs = {"diff", rev, args[2]};
            title = rev + ".." + args[2];
        }
        tortilla::showUnifiedDiff(**ui, title, path, gitArgs, [&] { (*ui)->quit(); });
        return (*ui)->run();
    }

    if (command == "rebase")
    {
        auto rebase = tortilla::RebaseWindow::open(**ui, path, args.size() > 1 ? args[1] : "");
        if (!rebase)
        {
            std::fprintf(stderr, "tortilla: %s\n", rebase.error().c_str());
            return 1;
        }
        (*rebase)->onClosed([&] { (*ui)->quit(); });
        (*rebase)->show();
        return (*ui)->run();
    }

    auto log = tortilla::LogWindow::open(**ui, path, allBranches);
    if (!log)
    {
        std::fprintf(stderr, "tortilla: %s\n", log.error().c_str());
        return 1;
    }
    (*log)->onClosed([&] { (*ui)->quit(); });
    (*log)->show();
    return (*ui)->run();
}
