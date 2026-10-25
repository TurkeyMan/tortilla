#include "lfs.h"

#include "git/command.h"
#include "i18n.h"

#include <rapport/sys.h>

#include <filesystem>
#include <set>

namespace tortilla
{

bool uses_lfs(const std::string& workdir)
{
    // A pathspec on the attribute covers every .gitattributes, nested ones
    // too; <gitdir>/lfs only appears once git-lfs has run.
    const git_result r =
        run_git(workdir, {"ls-files", "--", ":(attr:filter=lfs)"}, git_output::stdout_only);
    return r.ok() && !r.output.empty();
}

bool lfs_installed()
{
    static const bool installed = run_git(".", {"lfs", "version"}).ok();
    return installed;
}

bool lfs_missing(const std::string& workdir)
{
    // The cached check first: with git-lfs installed nothing else runs.
    return !workdir.empty() && !lfs_installed() && uses_lfs(workdir);
}

void warn_if_lfs_missing(rap::ui& ui, rap::window* parent, const std::string& workdir,
                         std::function<void()> done)
{
    // One folder however it's spelled (a trailing separator, "..").
    std::error_code ec;
    std::string key = rap::sys::path_to_utf8(
        std::filesystem::weakly_canonical(rap::sys::path_from_utf8(workdir), ec));
    if (ec || key.empty()) key = workdir;
    while (key.size() > 1 && (key.back() == '/' || key.back() == '\\')) key.pop_back();

    static std::set<std::string> warned;
    if (warned.contains(key) || !lfs_missing(workdir))
    {
        if (done) done();
        return;
    }
    warned.insert(key);
    ui.message(parent,
               {.icon = rap::message_spec::icon::info,
                .title = _("Git LFS"),
                .text = rap::i18n::format(
                    _("{0} stores some files with Git LFS, but git-lfs isn't installed, so "
                      "those files are checked out as small pointer files.\n\nInstall Git LFS "
                      "(https://git-lfs.com), then run “git lfs pull” in the repository."),
                    key)},
               [done = std::move(done)](rap::dialog_role)
               {
                   if (done) done();
               });
}

}  // namespace tortilla
