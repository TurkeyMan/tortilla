#include "compare.h"

#include "git/diff_tool.h"
#include "i18n.h"
#include "ui/diff_window.h"

#include <rapport/sys.h>

#include <git2.h>

#include <filesystem>
#include <optional>

namespace tortilla
{

namespace
{

// An image comparison runs on its own: kept until its tool exits, and a
// failure said in a message.
void start_image_tool(rap::ui& ui, rap::window* parent, const std::string& dir,
                      std::vector<std::string> args, std::function<void()> finished,
                      bool no_index = false)
{
    static std::vector<std::shared_ptr<git_process>> running;
    auto output = std::make_shared<std::string>();
    auto slot = std::make_shared<std::weak_ptr<git_process>>();
    auto process = git_process::start(
        ui, dir, std::move(args), [output](const std::string& chunk) { *output += chunk; },
        [&ui, parent, output, slot, finished, no_index](int code)
        {
            // Let go of it after this callback, which it's running.
            ui.post(
                [slot]
                {
                    if (auto self = slot->lock()) std::erase(running, self);
                });
            // With --no-index, git exits 1 for files that differ.
            if (code == 0 || (no_index && code == 1)) return finished();
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

// The paths that differ between `revisions` (one: against the working
// tree) under `paths`, through libgit2: these checks run before the tool
// starts, where starting git costs most (near a second on Windows over
// \\wsl.localhost). nullopt where git should answer: no paths (a whole
// tree, where git's index caches are faster) or a revision libgit2 can't
// resolve.
std::optional<std::vector<std::string>> changed_paths(const std::string& dir,
                                                      const std::vector<std::string>& revisions,
                                                      const std::vector<std::string>& paths)
{
    if (paths.empty() || revisions.empty() || revisions.size() > 2) return std::nullopt;
    static const int init = git_libgit2_init();
    (void)init;
    git_repository* raw = nullptr;
    if (git_repository_open_ext(&raw, dir.c_str(), 0, nullptr) != 0) return std::nullopt;
    std::unique_ptr<git_repository, decltype(&git_repository_free)> repo(raw, &git_repository_free);
    std::vector<std::unique_ptr<git_tree, decltype(&git_tree_free)>> trees;
    for (const std::string& rev : revisions)
    {
        // git's empty tree needn't be stored: no tree, to libgit2.
        if (rev == "4b825dc642cb6eb9a060e54bf8d69288fbee4904")
        {
            trees.emplace_back(nullptr, &git_tree_free);
            continue;
        }
        git_object* obj = nullptr;
        if (git_revparse_single(&obj, repo.get(), rev.c_str()) != 0) return std::nullopt;
        git_object* tree = nullptr;
        const int peeled = git_object_peel(&tree, obj, GIT_OBJECT_TREE);
        git_object_free(obj);
        if (peeled != 0) return std::nullopt;
        trees.emplace_back(reinterpret_cast<git_tree*>(tree), &git_tree_free);
    }
    std::vector<char*> specs;
    for (const std::string& p : paths) specs.push_back(const_cast<char*>(p.c_str()));
    git_diff_options o = GIT_DIFF_OPTIONS_INIT;
    o.pathspec = {specs.data(), specs.size()};
    git_diff* diff = nullptr;
    const int made =
        trees.size() == 2
            ? git_diff_tree_to_tree(&diff, repo.get(), trees[0].get(), trees[1].get(), &o)
            : git_diff_tree_to_workdir_with_index(&diff, repo.get(), trees[0].get(), &o);
    if (made != 0) return std::nullopt;
    std::vector<std::string> out;
    for (size_t i = 0, n = git_diff_num_deltas(diff); i < n; ++i)
    {
        const git_diff_delta* d = git_diff_get_delta(diff, i);
        out.emplace_back(d->new_file.path ? d->new_file.path : d->old_file.path);
    }
    git_diff_free(diff);
    return out;
}

void offer_diff_settings(rap::ui& ui, rap::window* parent, const std::string& text,
                         std::function<void()> open_settings, std::function<void()> done)
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
}

}  // namespace

std::shared_ptr<git_process> compare_files(rap::ui& ui, rap::window* parent,
                                           const std::string& base, const std::string& mine,
                                           std::function<void()> open_settings,
                                           std::function<void()> done)
{
    if (!done) done = [] {};
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path a = fs::absolute(rap::sys::path_from_utf8(base), ec);
    const fs::path b = fs::absolute(rap::sys::path_from_utf8(mine), ec);
    // git runs beside the second; --no-index compares any two files.
    const std::string dir = rap::sys::path_to_utf8(b.parent_path());
    const std::string pa = rap::sys::path_to_utf8(a), pb = rap::sys::path_to_utf8(b);
    const std::string na = rap::sys::path_to_utf8(a.filename()),
                      nb = rap::sys::path_to_utf8(b.filename());
    const std::vector<std::string> sides{"--", pa, pb};

    std::vector<std::string> quiet{"diff", "--no-index", "--quiet"};
    quiet.insert(quiet.end(), sides.begin(), sides.end());
    if (run_git(dir, quiet, git_output::stdout_only).exit_code == 0)
    {
        ui.message(parent,
                   {.icon = rap::message_spec::icon::info,
                    .title = "Tortilla",
                    .text = rap::i18n::format(_("“{0}” and “{1}” are the same."), na, nb)},
                   [done](rap::dialog_role) { done(); });
        return nullptr;
    }

    if (const std::string image_tool = image_diff_command();
        !image_tool.empty() && is_image_path(pa) && is_image_path(pb))
    {
        start_image_tool(ui, parent, dir,
                         custom_difftool_args(image_tool, {"--no-index"}, na, nb, {pa, pb}, false),
                         done, true);
        return nullptr;
    }

    std::vector<std::string> args;
    switch (saved_diff_mode(dir))
    {
    case diff_mode::builtin:
    {
        std::vector<std::string> diff{"diff", "--no-index"};
        diff.insert(diff.end(), sides.begin(), sides.end());
        show_unified_diff(ui, na + " : " + nb, dir, diff, done);
        return nullptr;
    }
    case diff_mode::git:
        if (git_diff_tool(dir).empty())
        {
            offer_diff_settings(ui, parent, _("git has no diff tool configured (diff.tool)."),
                                open_settings, done);
            return nullptr;
        }
        args = {"difftool", "--no-prompt", "--no-index"};
        args.insert(args.end(), sides.begin(), sides.end());
        break;
    case diff_mode::custom:
        if (diff_command().empty())
        {
            offer_diff_settings(ui, parent, _("No diff program is set."), open_settings, done);
            return nullptr;
        }
        args = custom_difftool_args(diff_command(), {"--no-index"}, na, nb, {pa, pb}, false);
        break;
    }
    auto output = std::make_shared<std::string>();
    return git_process::start(
        ui, dir, args, [output](const std::string& chunk) { *output += chunk; },
        [&ui, parent, output, done](int code)
        {
            // Exit 1: the files differ, as --no-index says it.
            if (code == 0 || code == 1) return done();
            ui.message(parent,
                       {.icon = rap::message_spec::icon::error,
                        .title = "Tortilla",
                        .text = _("The diff tool failed."),
                        .detail = *output},
                       [done](rap::dialog_role) { done(); });
        });
}

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
    auto offer_settings = [&ui, parent, open_settings, &done](const std::string& text)
    { offer_diff_settings(ui, parent, text, open_settings, done); };

    // Nothing to compare: say so. A diff tool given identical sides (git
    // difftool starts none at all) would otherwise just end, so it looked
    // like nothing happened [TortoiseGit says "no differences" too].
    const auto changed = changed_paths(dir, revisions, paths);
    {
        std::vector<std::string> quiet{"diff", "--quiet"};
        quiet.insert(quiet.end(), revisions.begin(), revisions.end());
        quiet.insert(quiet.end(), limit.begin(), limit.end());
        if (changed ? changed->empty()
                    : run_git(dir, quiet, git_output::stdout_only).exit_code == 0)
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
        std::vector<std::string> listed;
        if (changed)
            listed = *changed;
        else
        {
            std::vector<std::string> names{"diff", "--name-only", "-z"};
            names.insert(names.end(), revisions.begin(), revisions.end());
            names.insert(names.end(), limit.begin(), limit.end());
            const git_result r = run_git(dir, names, git_output::stdout_only);
            for (size_t at = 0; at < r.output.size();)
            {
                size_t end = r.output.find('\0', at);
                if (end == std::string::npos) end = r.output.size();
                listed.push_back(r.output.substr(at, end - at));
                at = end + 1;
            }
        }
        std::vector<std::string> images, others;
        for (std::string& path : listed)
            if (!path.empty()) (is_image_path(path) ? images : others).push_back(std::move(path));
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
