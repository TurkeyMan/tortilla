#include "compare.h"

#include "git/diff_sides.h"
#include "git/diff_tool.h"
#include "i18n.h"
#include "ui/busy_pointer.h"
#include "ui/diff_window.h"
#include "ui/modal_dialogs.h"

#include <rapport/sys.h>

#include <cstdio>
#include <cstring>
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
            show_message(ui, parent,
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

void offer_diff_settings(rap::ui& ui, rap::window* parent, const std::string& text,
                         std::function<void()> open_settings, std::function<void()> done)
{
    show_message(ui, parent,
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

// Whether two files hold the same bytes, read here: no git diff --no-index
// --quiet to start. nullopt if either can't be read.
std::optional<bool> same_bytes(const std::filesystem::path& a, const std::filesystem::path& b)
{
    std::error_code ec;
    const auto size_a = std::filesystem::file_size(a, ec);
    if (ec) return std::nullopt;
    const auto size_b = std::filesystem::file_size(b, ec);
    if (ec) return std::nullopt;
    if (size_a != size_b) return false;
    std::unique_ptr<std::FILE, decltype(&std::fclose)> fa(rap::sys::fopen(a, "rb"), &std::fclose),
        fb(rap::sys::fopen(b, "rb"), &std::fclose);
    if (!fa || !fb) return std::nullopt;
    static char x[65536], y[65536];  // the UI thread's
    for (;;)
    {
        const size_t n = std::fread(x, 1, sizeof x, fa.get());
        if (std::fread(y, 1, sizeof y, fb.get()) != n || std::memcmp(x, y, n) != 0) return false;
        if (n < sizeof x) return true;
    }
}

// The custom tool started on two files itself, without git difftool and
// its sh (TortoiseGit's way): null where it needs those after all (a
// command for sh, a script), and git difftool runs it. `cleanup` goes with
// the last of the callbacks, once the tool has exited or been stopped.
std::shared_ptr<git_process> start_direct(rap::ui& ui, rap::window* parent, const std::string& dir,
                                          const std::string& base, const std::string& mine,
                                          const std::string& base_name,
                                          const std::string& mine_name, std::function<void()> done,
                                          std::shared_ptr<void> cleanup = {})
{
    auto words = diff_tool_words(diff_command(), base, mine, base_name, mine_name);
    if (!words) return nullptr;
    const std::string program = words->front();
    words->erase(words->begin());
    auto output = std::make_shared<std::string>();
    return git_process::start_program(
        ui, dir, program, std::move(*words),
        [output](const std::string& chunk) { *output += chunk; },
        [&ui, parent, output, done, cleanup](int code)
        {
            // Its exit code isn't the comparison's: as git difftool (without
            // difftool.trustExitCode), only a tool that didn't start failed.
            if (code != -1) return done();
            show_message(ui, parent,
                         {.icon = rap::message_spec::icon::error,
                          .title = "Tortilla",
                          .text = _("The diff tool failed."),
                          .detail = *output},
                         [done](rap::dialog_role) { done(); });
        });
}

}  // namespace

std::shared_ptr<git_process> compare_files(rap::ui& ui, rap::window* parent,
                                           const std::string& base, const std::string& mine,
                                           std::function<void()> open_settings,
                                           std::function<void()> done)
{
    if (!done) done = [] {};
    // The busy pointer until the tool is up (or a message or window shows).
    const auto idle = show_busy_pointer(ui);
    done = [idle, done]
    {
        idle();
        done();
    };
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
    const std::optional<bool> same = same_bytes(a, b);
    if (same ? *same : run_git(dir, quiet, git_output::stdout_only).exit_code == 0)
    {
        idle();
        show_message(ui, parent,
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
        idle();
        show_unified_diff(ui, na + " : " + nb, dir, diff, done);
        return nullptr;
    }
    case diff_mode::git:
        if (git_diff_tool(dir).empty())
        {
            idle();
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
            idle();
            offer_diff_settings(ui, parent, _("No diff program is set."), open_settings, done);
            return nullptr;
        }
        if (auto tool =
                start_direct(ui, parent, dir, rap::sys::path_to_utf8(fs::path(a).make_preferred()),
                             rap::sys::path_to_utf8(fs::path(b).make_preferred()), na, nb, done))
            return tool;
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
            show_message(ui, parent,
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
    // The busy pointer until the tool is up (or a message or window shows).
    const auto idle = show_busy_pointer(ui);
    done = [idle, done]
    {
        idle();
        done();
    };
    auto name = [](const std::string& rev)
    {
        // Full hashes as TortoiseGit shows them: 8 characters.
        const bool hex = rev.size() >= 40 && rev.find_first_not_of("0123456789abcdef") >= 40;
        return hex ? rev.substr(0, 8) + rev.substr(40) : rev;
    };
    const std::string base_rev = name(revisions.front());
    const std::string mine_rev =
        revisions.size() > 1 ? name(revisions[1]) : std::string(_("working tree"));
    // One file: its name too, as TortoiseGit titles a file comparison.
    const std::string file = paths.size() == 1 ? paths.front() + " : " : "";
    const std::string base = file + base_rev;
    const std::string mine = file + mine_rev;
    // Renames are only found with both paths given (rename detection is
    // on by default in git diff).
    std::vector<std::string> limit;
    if (!paths.empty())
    {
        limit.push_back("--");
        limit.insert(limit.end(), paths.begin(), paths.end());
    }
    auto offer_settings = [&ui, parent, open_settings, &done, &idle](const std::string& text)
    {
        idle();
        offer_diff_settings(ui, parent, text, open_settings, done);
    };

    // Nothing to compare: say so. A diff tool given identical sides (git
    // difftool starts none at all) would otherwise just end, so it looked
    // like nothing happened [TortoiseGit says "no differences" too].
    // libgit2 answers where it can (no git to start), its renames paired.
    std::optional<std::vector<file_diff>> changed = diff_files(dir, revisions, paths);
    {
        std::vector<std::string> quiet{"diff", "--quiet"};
        quiet.insert(quiet.end(), revisions.begin(), revisions.end());
        quiet.insert(quiet.end(), limit.begin(), limit.end());
        if (changed ? changed->empty()
                    : run_git(dir, quiet, git_output::stdout_only).exit_code == 0)
        {
            idle();
            show_message(ui, parent,
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
            for (const file_diff& f : *changed) listed.push_back(f.path);
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
            // Only images: that's all. Otherwise the rest, by name (a
            // rename's old one too, so git pairs them).
            if (others.empty()) return nullptr;
            paths.clear();
            if (changed)
            {
                std::erase_if(*changed, [](const file_diff& f) { return is_image_path(f.path); });
                for (const file_diff& f : *changed)
                {
                    paths.push_back(f.path);
                    if (f.old_path != f.path) paths.push_back(f.old_path);
                }
            }
            else
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
        idle();
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
        // One file (a rename's two names are one): its sides written here
        // and the tool started with them, as TortoiseGit does, where git
        // difftool takes git, git diff and sh before the tool starts.
        if (changed && changed->size() == 1 && changed->front().regular &&
            diff_tool_words(diff_command(), "", "", "", ""))
        {
            const file_diff& f = changed->front();
            if (auto sides = write_diff_sides(dir, revisions, f))
            {
                // Its folder goes with the last holder: the tool's callbacks,
                // dropped once it exits or is stopped, a callback skipped too.
                auto written = std::make_shared<diff_sides>(std::move(*sides));
                if (auto tool = start_direct(ui, parent, dir, written->base, written->mine,
                                             f.old_path + " : " + base_rev,
                                             f.path + " : " + mine_rev, done, written))
                    return tool;
            }
            // Otherwise git difftool, which says what went wrong.
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
            show_message(ui, parent,
                         {.icon = rap::message_spec::icon::error,
                          .title = "Tortilla",
                          .text = _("The diff tool failed."),
                          .detail = *output},
                         [done](rap::dialog_role) { done(); });
        });
}

}  // namespace tortilla
