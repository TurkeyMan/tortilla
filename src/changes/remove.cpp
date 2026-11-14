#include "remove.h"

#include "git/command.h"
#include "git/pathspec_file.h"
#include "git/repo.h"
#include "i18n.h"
#include "ui/modal_dialogs.h"
#include "util/repo_path.h"

#include <rapport/sys.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <system_error>

namespace tortilla
{

void remove_files(rap::ui& ui, const std::vector<std::string>& paths, bool keep_local,
                  std::function<void(bool)> done)
{
    namespace fs = std::filesystem;
    auto fail = [&ui, done](const std::string& text, const std::string& detail = {})
    {
        show_message(ui, nullptr,
                     {.icon = rap::message_spec::icon::error,
                      .title = "Tortilla",
                      .text = text,
                      .detail = detail},
                     [done](rap::dialog_role) { done(false); });
    };
    if (paths.empty()) return done(false);
    auto repo = repo::open(paths.front());
    if (!repo) return fail(repo.error());

    // Repository-relative, as git takes them; absolute, for the trash: the
    // same entry both, the one under the working tree git names (a link
    // itself, never its target).
    std::vector<std::string> relative;
    std::vector<fs::path> absolute;
    for (const std::string& p : paths)
    {
        auto rel = repo_relative(repo->workdir(), p);
        if (!rel || rel->empty()) continue;  // not inside it, or the top itself
        absolute.push_back(
            (rap::sys::path_from_utf8(repo->workdir()) / rap::sys::path_from_utf8(*rel))
                .make_preferred());
        relative.push_back(std::move(*rel));
    }
    if (relative.empty()) return done(false);

    const std::string what =
        relative.size() == 1
            ? rap::i18n::format(_("“{0}”"), rap::sys::path_to_utf8(absolute.front().filename()))
            : rap::i18n::format(N_("{0} item", "{0} items", relative.size()), relative.size());
    const size_t n = relative.size();
#ifdef _WIN32
    const char* bin = N_("It goes to the Recycle Bin, and git stops tracking it.",
                         "They go to the Recycle Bin, and git stops tracking them.", n);
#else
    const char* bin = N_("It goes to the Trash, and git stops tracking it.",
                         "They go to the Trash, and git stops tracking them.", n);
#endif
    const std::string question =
        keep_local ? rap::i18n::format(_("Stop tracking {0}?"), what)
                   : rap::i18n::format(_("Delete {0}?"), what);
    const std::string detail =
        keep_local ? N_("It stays on disk, untracked.", "They stay on disk, untracked.", n) : bin;
    const std::string workdir = repo->workdir();
    show_message(
        ui, nullptr,
        {.icon = rap::message_spec::icon::question,
         .title = keep_local ? _("Delete (keep local)") : _("Delete"),
         .text = question,
         .detail = detail,
         .buttons = {rap::dialog_role::ok, rap::dialog_role::cancel}},
        [=, &ui](rap::dialog_role r)
        {
            if (r != rap::dialog_role::ok) return done(false);
            if (!keep_local)
                if (auto trashed = rap::sys::move_to_trash(absolute); !trashed)
                    return fail(N_("Couldn't delete it.", "Couldn't delete them.", n),
                                trashed.error());
            // Its removal staged (already gone from disk, or kept there);
            // untracked items git doesn't know are skipped.
            // The paths through a file: a big selection would pass
            // Windows' command-line limit.
            const auto list = pathspec_file::write(relative);
            if (!list) return fail(_("Could not write the list of files to the temporary folder."));
            std::vector<std::string> args{"rm", "-r", "-q", "--cached", "--ignore-unmatch"};
            for (std::string& a : list->arguments()) args.push_back(std::move(a));
            const git_result rm = run_git(workdir, literal_paths(args));
            if (!rm.ok())
                return fail(
                    N_("git couldn't stop tracking it.", "git couldn't stop tracking them.", n),
                    rm.output);
            done(true);
        });
}

}  // namespace tortilla
