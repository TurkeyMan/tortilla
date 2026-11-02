#include "revert.h"

#include "git/command.h"
#include "i18n.h"

#include <rapport/sys.h>

#include <set>
#include <string_view>
#include <system_error>

namespace fs = std::filesystem;

namespace tortilla
{
namespace
{

// A file or a link on disk, not a folder: what the trash should keep.
bool on_disk(const fs::path& p)
{
    std::error_code ec;
    const fs::file_status s = fs::symlink_status(p, ec);
    return fs::is_regular_file(s) || fs::is_symlink(s);
}

void add_nul_list(std::string_view out, std::set<std::string, std::less<>>& to)
{
    for (size_t at = 0; at < out.size();)
    {
        size_t end = out.find('\0', at);
        if (end == std::string_view::npos) end = out.size();
        if (end > at) to.emplace(out.substr(at, end - at));
        at = end + 1;
    }
}

}  // namespace

revert_plan plan_revert(const std::string& workdir, const std::vector<file_change>& files)
{
    revert_plan plan;
    const fs::path root = rap::sys::path_from_utf8(workdir);
    std::error_code ec;
    for (const file_change& f : files)
    {
        const fs::path p = root / rap::sys::path_from_utf8(f.path);
        if (f.status == 'R' && !f.old_path.empty())
        {
            if (fs::is_directory(root / rap::sys::path_from_utf8(f.old_path), ec))
            {
                plan.error = rap::i18n::format(_("Cannot revert renaming of “{0}”. A directory "
                                                 "with the old name “{1}” exists."),
                                               f.path, f.old_path);
                return plan;
            }
            (fs::exists(fs::symlink_status(p, ec)) ? plan.remove : plan.unstage).push_back(f.path);
            plan.restore.push_back(f.old_path);
        }
        else if (f.status == 'A')
            plan.unstage.push_back(f.path);
        else
            plan.restore.push_back(f.path);
        // Every file but the added ones, which stay on disk anyway.
        if (f.status != 'A' && on_disk(p)) plan.trash.push_back(p);
    }
    return plan;
}

std::vector<std::vector<std::string>> revert_commands(const revert_plan& plan)
{
    std::vector<std::vector<std::string>> out;
    auto command = [&](std::vector<std::string> args, const std::vector<std::string>& paths,
                       std::vector<std::string> after = {})
    {
        if (paths.empty()) return;
        args.insert(args.end(), after.begin(), after.end());
        args.push_back("--");
        args.insert(args.end(), paths.begin(), paths.end());
        out.push_back(literal_paths(std::move(args)));
    };
    command({"rm", "-f", "-q", "--cached"}, plan.unstage);
    command({"rm", "-f", "-q", "--ignore-unmatch"}, plan.remove);
    command({"checkout", "-f"}, plan.restore, {"--end-of-options", "HEAD"});
    return out;
}

std::vector<fs::path> with_local_changes(const std::string& workdir,
                                         const std::vector<std::string>& paths)
{
    if (paths.empty()) return {};
    std::set<std::string, std::less<>> changed;
    std::vector<std::string> diff{"diff", "HEAD", "--name-only", "--no-renames", "-z", "--"};
    diff.insert(diff.end(), paths.begin(), paths.end());
    add_nul_list(run_git(workdir, literal_paths(diff), git_output::stdout_only).output, changed);
    std::vector<std::string> others{"ls-files", "--others", "-z", "--"};
    others.insert(others.end(), paths.begin(), paths.end());
    add_nul_list(run_git(workdir, literal_paths(others), git_output::stdout_only).output, changed);
    std::vector<fs::path> out;
    for (const std::string& p : changed)
        if (fs::path abs = rap::sys::path_from_utf8(workdir) / rap::sys::path_from_utf8(p);
            on_disk(abs))
            out.push_back(std::move(abs));
    return out;
}

void trash_first(rap::ui& ui, rap::window* parent, const std::string& title,
                 const std::string& question, const std::vector<fs::path>& files,
                 std::function<void()> go)
{
    auto kept = rap::sys::move_to_trash(files);
    if (kept) return go();
    ui.message(parent,
               {.icon = rap::message_spec::icon::warning,
                .title = title,
                .text = question,
                .detail = kept.error(),
                .buttons = {rap::dialog_role::yes, rap::dialog_role::no}},
               [go = std::move(go)](rap::dialog_role r)
               {
                   if (r == rap::dialog_role::yes) go();
               });
}

}  // namespace tortilla
