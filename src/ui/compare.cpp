#include "compare.h"

#include "git/diff_tool.h"
#include "ui/diff_window.h"

namespace tortilla
{

std::shared_ptr<git_process>
compare_revisions(rap::ui& ui, rap::window* parent, const std::string& dir,
                  std::vector<std::string> revisions, std::vector<std::string> paths,
                  std::function<void()> open_settings, std::function<void()> done)
{
    if (!done) done = [] {};
    auto name = [](const std::string& rev)
    {
        // Full hashes as TortoiseGit shows them: 8 characters.
        const bool hex = rev.size() >= 40 && rev.find_first_not_of("0123456789abcdef") >= 40;
        return hex ? rev.substr(0, 8) + rev.substr(40) : rev;
    };
    // One file: its name too, as TortoiseGit titles a file comparison.
    const std::string file = paths.size() == 1 ? paths.front() + " : " : "";
    const std::string base = file + name(revisions.front());
    const std::string mine = file + (revisions.size() > 1 ? name(revisions[1]) : "working tree");
    // Renames are only found with both paths given (rename detection is
    // on by default in git diff).
    std::vector<std::string> limit;
    if (!paths.empty())
    {
        limit.push_back("--");
        limit.insert(limit.end(), paths.begin(), paths.end());
    }
    auto offer_settings = [&ui, parent, open_settings, done](const std::string& text)
    {
        ui.message(parent,
                   {.icon = rap::message_spec::icon::question,
                    .title = "Tortilla",
                    .text = text,
                    .detail = "Choose one on the Diff Viewer page of the Settings.",
                    .buttons = {rap::dialog_role::ok, rap::dialog_role::cancel}},
                   [open_settings, done](rap::dialog_role r)
                   {
                       if (r == rap::dialog_role::ok && open_settings)
                           open_settings();
                       else
                           done();
                   });
    };

    std::vector<std::string> args;
    switch (saved_diff_mode(dir))
    {
    case diff_mode::builtin:
    {
        std::vector<std::string> diff{"diff"};
        diff.insert(diff.end(), revisions.begin(), revisions.end());
        diff.insert(diff.end(), limit.begin(), limit.end());
        show_unified_diff(ui, base + ".." + mine, dir, diff, done);
        return nullptr;
    }
    case diff_mode::git:
        if (git_diff_tool(dir).empty())
        {
            offer_settings("git has no diff tool configured (diff.tool).");
            return nullptr;
        }
        args = {"difftool", "--no-prompt"};
        if (paths.size() != 1)
        {
            args.insert(args.begin() + 1, "--dir-diff");
#ifdef _WIN32
            args.insert(args.begin() + 2, "--no-symlinks");  // see custom_difftool_args
#endif
        }
        args.insert(args.end(), revisions.begin(), revisions.end());
        args.insert(args.end(), limit.begin(), limit.end());
        break;
    case diff_mode::custom:
        if (diff_command().empty())
        {
            offer_settings("No diff program is set.");
            return nullptr;
        }
        args = custom_difftool_args(diff_command(), revisions, base, mine, paths);
        break;
    }
    auto output = std::make_shared<std::string>();
    return git_process::start(
        ui, dir, args, [output](const std::string& chunk) { *output += chunk; },
        [&ui, parent, output, done](int code)
        {
            if (code == 0) return done();
            ui.message(parent,
                       {.icon = rap::message_spec::icon::error,
                        .title = "Tortilla",
                        .text = "The diff tool failed.",
                        .detail = *output},
                       [done](rap::dialog_role) { done(); });
        });
}

}  // namespace tortilla
