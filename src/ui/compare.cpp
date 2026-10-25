#include "compare.h"

#include "git/diff_tool.h"
#include "i18n.h"
#include "ui/diff_window.h"

namespace tortilla
{

namespace
{

// An image comparison runs on its own: kept until its tool exits, and a
// failure said in a message.
void start_image_tool(rap::ui& ui, rap::window* parent, const std::string& dir, std::vector<std::string> args,
                      std::function<void()> finished)
{
    static std::vector<std::shared_ptr<git_process>> running;
    auto output = std::make_shared<std::string>();
    auto slot = std::make_shared<std::weak_ptr<git_process>>();
    auto process = git_process::start(
        ui, dir, std::move(args), [output](const std::string& chunk) { *output += chunk; },
        [&ui, parent, output, slot, finished](int code)
        {
            // Let go of it after this callback, which it's running.
            ui.post(
                [slot]
                {
                    if (auto self = slot->lock()) std::erase(running, self);
                });
            if (code == 0) return finished();
            ui.message(parent,
                       {.icon = rap::message_spec::icon::error,
                        .title = "Tortilla",
                        .text = _("The image diff tool failed."),
                        .detail = *output},
                       [finished](rap::dialog_role) { finished(); });
        });
    if (!process) return finished();
    *slot = process;
    running.push_back(std::move(process));
}

}  // namespace

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
    const std::string mine = file + (revisions.size() > 1 ? name(revisions[1]) : std::string(_("working tree")));
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
                    .detail = _("Choose one on the Diff Viewer page of the Settings."),
                    .buttons = {rap::dialog_role::ok, rap::dialog_role::cancel}},
                   [open_settings, done](rap::dialog_role r)
                   {
                       if (r == rap::dialog_role::ok && open_settings)
                           open_settings();
                       else
                           done();
                   });
    };

    // Nothing to compare: say so. A diff tool given identical sides (git
    // difftool starts none at all) would otherwise just end, so it looked
    // like nothing happened [TortoiseGit says "no differences" too].
    {
        std::vector<std::string> quiet{"diff", "--quiet"};
        quiet.insert(quiet.end(), revisions.begin(), revisions.end());
        quiet.insert(quiet.end(), limit.begin(), limit.end());
        if (run_git(dir, quiet, git_output::stdout_only).exit_code == 0)
        {
            ui.message(parent,
                       {.icon = rap::message_spec::icon::info,
                        .title = "Tortilla",
                        .text = paths.size() == 1
                                    ? rap::i18n::format(_("“{0}” has no changes to compare."),
                                                        paths.front())
                                    : std::string(_("There are no changes to compare."))},
                       [done](rap::dialog_role) { done(); });
            return nullptr;
        }
    }

    // Image files go to the image tool, each in a window of its own
    // (IfritView compares two images); the rest to the text tool, as before.
    if (const std::string image_tool = image_diff_command(); !image_tool.empty())
    {
        std::vector<std::string> names{"diff", "--name-only", "-z"};
        names.insert(names.end(), revisions.begin(), revisions.end());
        names.insert(names.end(), limit.begin(), limit.end());
        const git_result changed = run_git(dir, names, git_output::stdout_only);
        std::vector<std::string> images, others;
        for (size_t at = 0; at < changed.output.size();)
        {
            size_t end = changed.output.find('\0', at);
            if (end == std::string::npos) end = changed.output.size();
            std::string path = changed.output.substr(at, end - at);
            at = end + 1;
            if (!path.empty()) (is_image_path(path) ? images : others).push_back(std::move(path));
        }
        if (!images.empty())
        {
            // `done` once every comparison started here has finished.
            auto remaining = std::make_shared<size_t>(images.size() + (others.empty() ? 0 : 1));
            done = [remaining, done]
            {
                if (--*remaining == 0) done();
            };
            for (const std::string& image : images)
            {
                const std::string b = image + " (" + name(revisions.front()) + ")";
                const std::string m = image + " (" +
                                      (revisions.size() > 1 ? name(revisions[1]) : std::string(_("working tree"))) +
                                      ")";
                start_image_tool(ui, parent, dir, custom_difftool_args(image_tool, revisions, b, m, {image}, false),
                                 done);
            }
            // Only images: that's all. Otherwise the rest, by name.
            if (others.empty()) return nullptr;
            paths = others;
            limit = {"--"};
            limit.insert(limit.end(), paths.begin(), paths.end());
        }
    }

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
            offer_settings(_("git has no diff tool configured (diff.tool)."));
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
            offer_settings(_("No diff program is set."));
            return nullptr;
        }
        args = custom_difftool_args(diff_command(), revisions, base, mine, paths, diff_folders());
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
                        .text = _("The diff tool failed."),
                        .detail = *output},
                       [done](rap::dialog_role) { done(); });
        });
}

}  // namespace tortilla
