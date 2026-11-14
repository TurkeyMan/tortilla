#include "lfs.h"

#include "git/command.h"
#include "git/git_install.h"
#include "i18n.h"
#include "ui/modal_dialogs.h"

#include <git2.h>
#include <rapport/sys.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <set>

namespace tortilla
{
namespace
{

bool says_lfs(std::string_view attributes)
{
    return attributes.find("filter=lfs") != std::string_view::npos;
}

// In process: the repository's attributes files that say filter=lfs, each
// .gitattributes in the index and info/attributes. nullopt when libgit2
// can't read the index (git answers then).
std::optional<bool> lfs_attributes(const std::string& workdir)
{
    static const int init = git_libgit2_init();
    (void)init;
    git_repository* r = nullptr;
    if (git_repository_open_ext(&r, workdir.c_str(), 0, nullptr) != 0) return std::nullopt;
    std::unique_ptr<git_repository, decltype(&git_repository_free)> repo(r, git_repository_free);
    if (git_repository_is_bare(r)) return false;
    {
        std::ifstream in(rap::sys::path_from_utf8(git_repository_commondir(r)) / "info" /
                             "attributes",
                         std::ios::binary);
        if (in && says_lfs(std::string(std::istreambuf_iterator<char>(in), {}))) return true;
    }
    git_index* index = nullptr;
    if (git_repository_index(&index, r) != 0) return std::nullopt;
    std::unique_ptr<git_index, decltype(&git_index_free)> owned(index, git_index_free);
    const size_t n = git_index_entrycount(index);
    for (size_t i = 0; i < n; ++i)
    {
        const git_index_entry* e = git_index_get_byindex(index, i);
        const std::string_view path = e->path;
        if (path != ".gitattributes" && !path.ends_with("/.gitattributes")) continue;
        git_blob* blob = nullptr;
        if (git_blob_lookup(&blob, r, &e->id) != 0) continue;
        const bool lfs = says_lfs(std::string_view(
            static_cast<const char*>(git_blob_rawcontent(blob)), size_t(git_blob_rawsize(blob))));
        git_blob_free(blob);
        if (lfs) return true;
    }
    return false;
}

}  // namespace

bool uses_lfs(const std::string& workdir)
{
    // Kept per repository while its index (and info/attributes) stay as they
    // were: the log asks as it opens.
    std::string key = workdir;
    {
        std::error_code ec;
        for (const char* file : {".git/index", ".git/info/attributes"})
        {
            const auto p = rap::sys::path_from_utf8(workdir) / file;
            const auto when = std::filesystem::last_write_time(p, ec);
            key += '|' + (ec ? std::string() : std::to_string(when.time_since_epoch().count())) +
                   ':' + std::to_string(ec ? 0 : std::filesystem::file_size(p, ec));
        }
    }
    static std::mutex mutex;
    static std::map<std::string, bool, std::less<>> known;
    {
        std::lock_guard lock(mutex);
        if (auto it = known.find(key); it != known.end()) return it->second;
    }
    bool uses = false;
    if (const auto found = lfs_attributes(workdir))
        uses = *found;
    else
    {
        // A pathspec on the attribute covers every .gitattributes, nested ones
        // too; <gitdir>/lfs only appears once git-lfs has run.
        const git_result r =
            run_git(workdir, {"ls-files", "--", ":(attr:filter=lfs)"}, git_output::stdout_only);
        uses = r.ok() && !r.output.empty();
    }
    std::lock_guard lock(mutex);
    known[key] = uses;
    return uses;
}

bool lfs_installed()
{
    // Looked for rather than run: `git lfs version` is five processes.
    static const bool installed = !find_git_program("git-lfs").empty();
    return installed;
}

bool lfs_missing(const std::string& workdir)
{
    // Only a repository using LFS asks whether it's installed.
    return !workdir.empty() && git_installed() && uses_lfs(workdir) && !lfs_installed();
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
    show_message(ui, parent,
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
