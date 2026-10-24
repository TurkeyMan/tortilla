#include "remove.h"

#include "git/command.h"
#include "git/repo.h"
#include "i18n.h"

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
        ui.message(nullptr,
                   {.icon = rap::message_spec::icon::error,
                    .title = "Tortilla",
                    .text = text,
                    .detail = detail},
                   [done](rap::dialog_role) { done(false); });
    };
    if (paths.empty()) return done(false);
    auto repo = repo::open(paths.front());
    if (!repo) return fail(repo.error());
    const fs::path root = rap::sys::path_from_utf8(repo->workdir());

    // Repository-relative, as git takes them; absolute, for the trash.
    std::vector<std::string> relative;
    std::vector<fs::path> absolute;
    for (const std::string& p : paths)
    {
        std::error_code ec;
        const fs::path abs = fs::absolute(rap::sys::path_from_utf8(p), ec);
        const fs::path rel = fs::relative(abs, root, ec);
        if (ec || rel.empty() || rel == "." || *rel.begin() == "..") continue;  // not inside it
        std::string r = rap::sys::path_to_utf8(rel);
        std::ranges::replace(r, '\\', '/');
        relative.push_back(std::move(r));
        absolute.push_back(abs);
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
    ui.message(nullptr,
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
                           return fail(N_("Couldn't delete it.", "Couldn't delete them.", n), trashed.error());
                   // Its removal staged (already gone from disk, or kept there);
                   // untracked items git doesn't know are skipped.
                   std::vector<std::string> args{"rm", "-r", "-q", "--cached", "--ignore-unmatch",
                                                 "--"};
                   args.insert(args.end(), relative.begin(), relative.end());
                   const git_result rm = run_git(workdir, args);
                   if (!rm.ok())
                       return fail(N_("git couldn't stop tracking it.", "git couldn't stop tracking them.", n),
                                   rm.output);
                   done(true);
               });
}

}  // namespace tortilla
