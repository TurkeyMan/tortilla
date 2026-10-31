#include "badges.h"

#include "git/command.h"

#include <algorithm>

namespace tortilla
{

namespace
{

// How a change shows: conflicted over modified over added, so a folder takes
// the strongest of its contents.
int rank(badge b)
{
    switch (b)
    {
    case badge::conflicted: return 3;
    case badge::modified: return 2;
    case badge::added: return 1;
    default: return 0;
    }
}

// Each NUL-terminated field in turn.
template <class F>
void each_field(std::string_view text, F f)
{
    for (size_t at = 0; at < text.size();)
    {
        size_t end = text.find('\0', at);
        if (end == std::string_view::npos) end = text.size();
        f(text.substr(at, end - at));
        at = end + 1;
    }
}

// The folders above `path`, nearest first, then "" (the top).
template <class F>
void each_folder(std::string_view path, F f)
{
    for (size_t slash = path.rfind('/'); slash != std::string_view::npos;
         slash = slash ? path.rfind('/', slash - 1) : std::string_view::npos)
    {
        f(path.substr(0, slash));
        if (slash == 0) break;
    }
    f(std::string_view());
}

}  // namespace

repo_badges repo_badges::build(std::string_view tracked, std::string_view status)
{
    repo_badges out;
    each_field(tracked,
               [&](std::string_view path)
               {
                   if (path.empty()) return;
                   out.tracked_.emplace(path);
                   each_folder(path,
                               [&](std::string_view folder) { out.tracked_.emplace(folder); });
               });

    // "XY path", and a rename or copy's old path as the next field.
    bool skip_next = false;
    std::vector<std::pair<std::string, badge>> files;
    each_field(status,
               [&](std::string_view field)
               {
                   if (skip_next)
                   {
                       skip_next = false;
                       return;
                   }
                   if (field.size() < 4) return;
                   const char x = field[0], y = field[1];
                   std::string_view path = field.substr(3);
                   // Untracked: no badge, but noted (a folder git collapses
                   // ends in '/'), and its folders hold something untracked.
                   if (x == '?')
                   {
                       while (!path.empty() && path.back() == '/') path.remove_suffix(1);
                       out.untracked_.emplace(path);
                       each_folder(path, [&](std::string_view folder)
                                   { out.untracked_within_.emplace(folder); });
                       return;
                   }
                   badge b = badge::modified;
                   // Unmerged: both sides' letters, or either one U.
                   if (x == 'U' || y == 'U' || (x == 'A' && y == 'A') || (x == 'D' && y == 'D'))
                       b = badge::conflicted;
                   else if (x == 'A' || x == 'R' || x == 'C')
                       b = badge::added;  // new here, a rename's or copy's too
                   if (x == 'R' || x == 'C' || y == 'R' || y == 'C') skip_next = true;
                   files.emplace_back(path, b);
               });
    for (auto& [path, b] : files)
    {
        out.changed_[path] = b;
        // Its folders: anything changed in a folder changes it.
        const badge folder_badge = b == badge::conflicted ? badge::conflicted : badge::modified;
        each_folder(path,
                    [&](std::string_view folder)
                    {
                        badge& f = out.changed_[std::string(folder)];
                        if (rank(folder_badge) > rank(f)) f = folder_badge;
                    });
    }
    return out;
}

bool repo_badges::untracked(std::string_view path) const
{
    while (!path.empty() && path.back() == '/') path.remove_suffix(1);
    if (untracked_within_.contains(path) || untracked_.contains(path)) return true;
    // Inside a folder git reported whole.
    bool inside = false;
    each_folder(path, [&](std::string_view folder)
                { inside = inside || (!folder.empty() && untracked_.contains(folder)); });
    return inside;
}

badge repo_badges::of(std::string_view path) const
{
    while (!path.empty() && path.back() == '/') path.remove_suffix(1);
    if (auto it = changed_.find(path); it != changed_.end()) return it->second;
    return tracked_.contains(path) ? badge::clean : badge::none;
}

namespace
{

// No optional locks: git status may otherwise refresh the index, which a
// watcher of the repository would see as a change to load again.
// With `untracked`, untracked files too (folders of them collapsed), at
// some cost: the menu wants to know a folder has none.
git_result quiet_status(const std::string& workdir, bool untracked = false)
{
    return run_git(workdir,
                   {"--no-optional-locks", "status", "--porcelain", "-z",
                    untracked ? "--untracked-files=normal" : "--untracked-files=no"},
                   git_output::stdout_only);
}

}  // namespace

std::expected<repo_badges, std::string> load_badges(const std::string& workdir)
{
    const git_result tracked = run_git(workdir, {"ls-files", "-z"}, git_output::stdout_only);
    if (!tracked.ok()) return std::unexpected(tracked.output);
    const git_result status = quiet_status(workdir, true);
    if (!status.ok()) return std::unexpected(status.output);
    return repo_badges::build(tracked.output, status.output);
}

std::expected<badge, std::string> load_summary(const std::string& workdir)
{
    const git_result status = quiet_status(workdir);
    if (!status.ok()) return std::unexpected(status.output);
    const badge b = repo_badges::build({}, status.output).of("");
    return b == badge::none ? badge::clean : b;
}

}  // namespace tortilla
