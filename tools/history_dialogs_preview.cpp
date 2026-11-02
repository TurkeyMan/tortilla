// The log's history editing dialogs on a repository, for judging them
// without a log window: Combine to one commit's message for its newest
// three commits, or Move commits to branch (with --move).
//   tortilla-history-dialogs-preview <repository> [--move] [--ui=gtk3]
#include "git/history_edit.h"
#include "log/history_dialogs.h"

#include <rapport/rapport.h>

#include <cstring>

namespace tortilla
{

// The dialogs' windows, without Tortilla's icon (ui/repo_icon.cpp's).
std::unique_ptr<rap::window> create_window(rap::ui& ui, const rap::window_spec& spec,
                                           rap::window* parent)
{
    return ui.create(spec, parent);
}

}  // namespace tortilla

int main(int argc, char** argv)
{
    rap::load_options opts;
    std::string path = ".";
    bool move = false;
    for (int i = 1; i < argc; ++i)
        if (std::strncmp(argv[i], "--ui=", 5) == 0)
            opts.backend = argv[i] + 5;
        else if (std::strcmp(argv[i], "--move") == 0)
            move = true;
        else
            path = argv[i];
    auto ui = rap::ui::load(argc, argv, opts);
    if (!ui) return 1;
    auto repo = tortilla::repo::open(path);
    if (!repo) return 1;
    auto quit = [&] { (*ui)->quit(); };
    const std::string warning = "Some of these commits are on origin/feature already: after this, "
                                "feature needs a force push.";
    if (move)
        tortilla::move_commits_dialog(
            **ui, nullptr, "Move 2 commits off main, onto:", repo->branch_names(true, false, false),
            warning, [&](tortilla::move_target) { quit(); });
    else
    {
        std::vector<tortilla::oid> commits;
        for (const char* rev : {"HEAD~2", "HEAD~1", "HEAD"})
            if (auto id = repo->resolve(rev)) commits.push_back(*id);
        tortilla::history_message_dialog(
            **ui, nullptr, "Combine to One Commit",
            "3 commits become one, with this message:", tortilla::combined_message(*repo, commits),
            warning, [&](std::string) { quit(); });
    }
    return (*ui)->run();
}
