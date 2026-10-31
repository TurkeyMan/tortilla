#include "cleanup_dialog.h"

#include "git/command.h"
#include "git/repo.h"
#include "i18n.h"
#include "settings.h"
#include "ui/progress_dialog.h"
#include "ui/repo_icon.h"

#include <rapport/sys.h>

#include <algorithm>
#include <filesystem>
#include <memory>

namespace tortilla
{

namespace fs = std::filesystem;

void clean_up(rap::ui& ui, rap::window* parent, std::vector<std::string> paths,
              std::function<void(bool done)> done)
{
    auto fail = [&ui, parent, done](const std::string& text)
    {
        ui.message(parent,
                   {.icon = rap::message_spec::icon::error, .title = _("Clean up"), .text = text},
                   [done](rap::dialog_role) { done(false); });
    };
    if (paths.empty()) paths.push_back(".");
    auto repo = repo::open(paths.front());
    if (!repo) return fail(repo.error());
    if (repo->bare()) return fail(_("This needs a working tree."));
    const std::string workdir = repo->workdir();
    const fs::path root = rap::sys::path_from_utf8(workdir);

    // Repository-relative folders, each cleaned from inside it (the top as
    // "."), and files chosen one by one.
    std::vector<std::string> folders, files;
    for (const std::string& p : paths)
    {
        std::error_code ec;
        const fs::path abs = fs::absolute(rap::sys::path_from_utf8(p), ec);
        const fs::path rel = fs::relative(abs, root, ec);
        if (ec || rel.empty() || *rel.begin() == "..") continue;  // not inside it
        std::string r = rap::sys::path_to_utf8(rel);
        std::ranges::replace(r, '\\', '/');
        if (r.empty()) r = ".";
        (fs::is_directory(abs, ec) ? folders : files).push_back(r);
    }
    if (folders.empty() && files.empty())
        return fail(_("Only something inside the repository can be cleaned up."));
    // The top covers everything.
    if (std::ranges::contains(folders, std::string("."))) folders = {"."}, files.clear();

    struct state
    {
        std::unique_ptr<rap::window> window;
        rap::drop_down* files = nullptr;
        rap::check* directories = nullptr;
        rap::check* trash = nullptr;
        rap::check* submodules = nullptr;
        rap::check* dry_run = nullptr;
    };
    auto st = std::make_shared<state>();
    settings& set = settings::get();

    using namespace rap::layout;
    st->window = create_window(
        ui,
        dialog{
            _("Clean up"),
            {vbox{
                form{
                    {_("Remove"),
                     drop_down{{_("Untracked files that git doesn't ignore"),
                                _("Only ignored files"),
                                _("All untracked files, ignored ones too")},
                               std::clamp(std::atoi(set.value("cleanup.files", "0").c_str()), 0, 2)}
                         .bind(st->files)},
                },
                check{_("Remove untracked _directories"),
                      set.value("cleanup.directories", "1") == "1"}
                    .bind(st->directories),
#ifdef _WIN32
                check{_("Use the _Recycle Bin"), set.value("cleanup.trash", "1") == "1"}.bind(
                    st->trash),
#else
                check{_("Move to the _Trash"), set.value("cleanup.trash", "1") == "1"}.bind(
                    st->trash),
#endif
                check{_("Include _submodules"), set.value("cleanup.submodules", "0") == "1"}.bind(
                    st->submodules),
                check{_("Dry _run (only list what would be removed)")}.bind(st->dry_run),
                buttons{ok, cancel},
            }}}
            .resizable(false),
        parent);
    st->window->on_finished(
        [st, &ui, parent, workdir, folders, files, done](rap::dialog_role role) mutable
        {
            auto keep = std::move(st);
            ui.post([keep] {});  // destroy after this callback returns
            if (role != rap::dialog_role::ok) return done(false);

            clean_options o;
            o.files = clean_options::what(std::clamp(keep->files->current(), 0, 2));
            o.directories = keep->directories->checked();
            o.submodules = keep->submodules->checked();
            const bool trash = keep->trash->checked();
            const bool dry_run = keep->dry_run->checked();
            settings& set = settings::get();
            set.set("cleanup.files", std::to_string(int(o.files)));
            set.set("cleanup.directories", o.directories ? "1" : "0");
            set.set("cleanup.trash", trash ? "1" : "0");
            set.set("cleanup.submodules", o.submodules ? "1" : "0");

            // A dry run, or for good: git clean in the progress dialog.
            if (dry_run || !trash)
            {
                std::vector<git_command> commands;
                for (const std::string& f : folders) commands.push_back(clean_args(o, dry_run, f));
                if (!files.empty()) commands.push_back(clean_args(o, dry_run, {}, files));
                if (o.submodules) commands.push_back(submodule_clean_args(o, dry_run));
                run_progress_dialog(ui, parent, _("Clean up"), workdir, std::move(commands), done);
                return;
            }

            // To the Recycle Bin: what git clean would remove, moved there.
            // Each command's listing is relative to where it ran.
            std::vector<std::pair<std::string, git_command>> listings;
            for (const std::string& f : folders) listings.push_back({f, clean_args(o, true, f)});
            if (!files.empty()) listings.push_back({".", clean_args(o, true, {}, files)});
            if (o.submodules) listings.push_back({".", submodule_clean_args(o, true)});
            std::vector<std::string> found;
            git_result list{.exit_code = 0};
            for (const auto& [where, args] : listings)
            {
                list = run_git(workdir, args, git_output::stdout_only);
                if (!list.ok()) break;
                // "./" is the folder itself, when it's untracked as a whole.
                for (const std::string& f : parse_clean_listing(list.output))
                    found.push_back(where == "." ? f : f == "./" ? where + "/" : where + "/" + f);
            }
            if (!list.ok())
            {
                ui.message(parent,
                           {.icon = rap::message_spec::icon::error,
                            .title = _("Clean up"),
                            .text = _("git couldn't list the untracked files."),
                            .detail = list.output},
                           [done](rap::dialog_role) { done(false); });
                return;
            }
            if (found.empty())
            {
                ui.message(parent,
                           {.icon = rap::message_spec::icon::info,
                            .title = _("Clean up"),
                            .text = _("Nothing to clean up.")},
                           [done](rap::dialog_role) { done(true); });
                return;
            }
            std::vector<fs::path> absolute;
            std::string names;
            const fs::path root = rap::sys::path_from_utf8(workdir);
            for (const std::string& f : found)
            {
                std::string trimmed = f;
                while (trimmed.size() > 1 && trimmed.back() == '/')
                    trimmed.pop_back();  // folders end in /
                absolute.push_back((root / rap::sys::path_from_utf8(trimmed)).lexically_normal());
                names += f + "\n";
            }
            const auto trashed = rap::sys::move_to_trash(absolute);
            const size_t n = found.size();
            ui.message(
                parent,
                {.icon = trashed ? rap::message_spec::icon::info : rap::message_spec::icon::error,
                 .title = _("Clean up"),
#ifdef _WIN32
                 .text = trashed ? rap::i18n::format(N_("{0} item moved to the Recycle Bin.",
                                                        "{0} items moved to the Recycle Bin.", n),
                                                     std::to_string(n))
#else
                 .text = trashed ? rap::i18n::format(N_("{0} item moved to the Trash.",
                                                        "{0} items moved to the Trash.", n),
                                                     std::to_string(n))
#endif
                                 : std::string(_("Some of them couldn't be removed.")),
                 .detail = trashed ? names : trashed.error() + "\n\n" + names},
                [done](rap::dialog_role) { done(true); });
        });
    st->window->show();
}

}  // namespace tortilla
