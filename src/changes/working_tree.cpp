#include "working_tree.h"

#include "git/command.h"
#include "git/status.h"

namespace tortilla
{

bool within(const std::string& path, const std::vector<std::string>& paths)
{
    if (paths.empty()) return true;
    for (std::string p : paths)
    {
        while (!p.empty() && p.back() == '/') p.pop_back();
        if (p.empty() || path == p ||
            (path.size() > p.size() && path.starts_with(p) && path[p.size()] == '/'))
            return true;
    }
    return false;
}

working_tree_changes load_working_tree(const std::string& workdir, const working_tree_options& o,
                                       git_canceller* canceller)
{
    working_tree_changes out;
    auto git = [&](const std::vector<std::string>& args)
    {
        return canceller ? run_git(workdir, args, git_output::stdout_only, *canceller)
                         : run_git(workdir, args, git_output::stdout_only);
    };
    // Staged changes elsewhere need the whole repository asked; the rest is
    // filtered to the paths here.
    const bool everywhere = o.paths.empty() || o.all_staged;
    auto limited = [&](std::vector<std::string> args)
    {
        if (!everywhere)
        {
            args.push_back("--");
            args.insert(args.end(), o.paths.begin(), o.paths.end());
        }
        return args;
    };

    std::vector<std::string> status{"status", "--porcelain", "-z",
                                    !o.unversioned          ? "--untracked-files=no"
                                    : o.each_untracked_file ? "--untracked-files=all"
                                                            : "--untracked-files=normal"};
    if (o.ignored) status.push_back("--ignored=matching");
    git_result s = git(limited(status));
    if (!s.ok())
    {
        out.error = s.output;
        return out;
    }
    // Line counts against HEAD; in a repository without commits there are none.
    git_result n = git(limited(std::vector<std::string>(k_numstat_args)));
    for (file_change& f : parse_status(s.output, n.ok() ? n.output : ""))
        if (within(f.path, o.paths) || (f.staged && o.all_staged))
            out.files.push_back(std::move(f));

    if (o.local_changes_ignored)
    {
        // ls-files -v tags assume-unchanged files in lower case, and
        // skip-worktree ones with S.
        git_result l = git(limited({"ls-files", "-v", "-z"}));
        size_t at = 0;
        while (l.ok() && at < l.output.size())
        {
            size_t end = l.output.find('\0', at);
            if (end == std::string::npos) end = l.output.size();
            std::string e = l.output.substr(at, end - at);
            at = end + 1;
            if (e.size() < 3) continue;
            const char tag = e[0];
            const char status = tag == 'S' ? 'S' : (tag >= 'a' && tag <= 'z') ? 'h' : 0;
            if (!status || !within(e.substr(2), o.paths)) continue;
            out.files.push_back({.path = e.substr(2), .status = status});
        }
    }
    return out;
}

}  // namespace tortilla
