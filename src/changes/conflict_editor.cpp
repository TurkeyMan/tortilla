#include "conflict_editor.h"

#include "git/command.h"
#include "git/merge_tool.h"
#include "i18n.h"

#include <rapport/sys.h>

#include <memory>
#include <thread>

namespace tortilla
{

void edit_conflict(rap::ui& ui, rap::window* parent, const std::string& workdir,
                   const std::string& path, std::function<void(bool)> done,
                   std::function<void(settings_page)> open_settings)
{
    auto finish = [done](bool changed)
    {
        if (done) done(changed);
    };
    auto sides = write_conflict_sides(workdir, path);
    if (!sides)
    {
        ui.message(parent,
                   {.icon = rap::message_spec::icon::error,
                    .title = _("Edit conflicts"),
                    .text = sides.error()},
                   [finish](rap::dialog_role) { finish(false); });
        return;
    }
    if (sides->submodule)
    {
        ui.message(
            parent,
            {.icon = rap::message_spec::icon::info,
             .title = _("Edit conflicts"),
             .text = rap::i18n::format(_("“{0}” is a submodule: choose a side with Resolve conflict "
                                         "using “mine” or “theirs”."),
                                       path)},
            [finish](rap::dialog_role) { finish(false); });
        return;
    }
    // Deleted on one side, changed on the other: keep it or not [spec 9.1.4].
    if (sides->mine_deleted || sides->theirs_deleted)
    {
        const std::string deleted_by =
            sides->mine_deleted ? sides->mine_title : sides->theirs_title;
        const std::string changed_by =
            sides->mine_deleted ? sides->theirs_title : sides->mine_title;
        ui.message(
            parent,
            {.icon = rap::message_spec::icon::question,
             .title = _("Conflict"),
             .text = rap::i18n::format(_("“{0}” was deleted in {1} and changed in {2}."), path,
                                       deleted_by, changed_by),
             .detail = _("Keep the changed file? (No deletes it.)"),
             .buttons = {rap::dialog_role::yes, rap::dialog_role::no, rap::dialog_role::cancel}},
            [&ui, parent, workdir, path, finish](rap::dialog_role r)
            {
                if (r == rap::dialog_role::cancel) return finish(false);
                const git_result g =
                    run_git(workdir, r == rap::dialog_role::yes
                                         ? std::vector<std::string>{"add", "-f", "--", path}
                                         : std::vector<std::string>{"rm", "-f", "--", path});
                if (!g.ok())
                    ui.message(parent, {.icon = rap::message_spec::icon::error,
                                        .title = _("Conflict"),
                                        .text = g.output});
                finish(true);
            });
        return;
    }

    const std::string command = merge_command();
    if (command.empty())
    {
        remove_conflict_sides(*sides);
        ui.message(parent,
                   {.icon = rap::message_spec::icon::question,
                    .title = _("Edit conflicts"),
                    .text = _("No merge tool was found. Choose one in Settings (Diff Viewer)?"),
                    .buttons = {rap::dialog_role::yes, rap::dialog_role::no}},
                   [open_settings, finish](rap::dialog_role r)
                   {
                       if (r == rap::dialog_role::yes && open_settings)
                           open_settings(settings_page::diff_viewer);
                       finish(false);
                   });
        return;
    }
    std::vector<std::string> argv = merge_tool_command(command, *sides, workdir);
    const std::string program = argv.front();
    argv.erase(argv.begin());

    // The tool runs off the UI thread; when it exits its sides go, and
    // unless it marked the file resolved itself (TortoiseGitMerge does) the
    // user says whether the merge worked [spec 9.1.3, MergeBlockTrustBehavior 1].
    std::thread(
        [&ui, workdir, path, sides = *sides, program, argv, finish]
        {
            auto result = rap::sys::process::run(program, argv, {.merge_stderr = true});
            const std::string error = result ? "" : result.error();
            ui.post(
                [&ui, workdir, path, sides, program, error, finish]
                {
                    // (The window that asked may be gone: these stand alone.)
                    remove_conflict_sides(sides);
                    if (!error.empty())
                    {
                        ui.message(nullptr,
                                   {.icon = rap::message_spec::icon::error,
                                    .title = _("Edit conflicts"),
                                    .text = rap::i18n::format(_("Could not start {0}: {1}"),
                                                              program, error)},
                                   [finish](rap::dialog_role) { finish(false); });
                        return;
                    }
                    if (!still_conflicted(workdir, path)) return finish(true);
                    ui.message(nullptr,
                               {.icon = rap::message_spec::icon::question,
                                .title = _("Edit conflicts"),
                                .text = rap::i18n::format(_("Was the merge of “{0}” successful?"),
                                                          path),
                                .detail = _("Yes marks it resolved."),
                                .buttons = {rap::dialog_role::yes, rap::dialog_role::no}},
                               [&ui, workdir, path, finish](rap::dialog_role r)
                               {
                                   if (r != rap::dialog_role::yes) return finish(false);
                                   const git_result g = run_git(workdir, {"add", "-f", "--", path});
                                   if (!g.ok())
                                       ui.message(nullptr, {.icon = rap::message_spec::icon::error,
                                                            .title = _("Resolve"),
                                                            .text = g.output});
                                   finish(true);
                               });
                });
        })
        .detach();
}

}  // namespace tortilla
