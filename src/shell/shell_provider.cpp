// Tortilla's entries in the file manager's context menu: TortoiseGit's
// Explorer menu [spec 4], built into tortilla-shell by rapport. This runs
// inside the file manager, so it looks only at the filesystem: no libgit2,
// no git processes, no toolkit.
//
// Until the status cache exists, an item inside a working tree counts as
// tracked and unmodified: TortoiseGit's own fallback when status fails
// [spec 4.3 step 3]. So IGN, CONFL, ADDED and DELETED are never set.
#include "menu_table.h"

#include <rapport/shell.h>

#ifdef _WIN32
#include <shlobj.h>
#include <windows.h>
#endif

#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <set>
#include <string>
#include <system_error>

namespace
{

using namespace tortilla::shell_menu;
namespace fs = std::filesystem;
using rap::shell::menu_item;
using rap::shell::selection;

bool is_dir(const fs::path& p)
{
    std::error_code ec;
    return fs::is_directory(p, ec);
}

bool present(const fs::path& p)
{
    std::error_code ec;
    return fs::exists(p, ec);
}

std::string read_file(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::string trim(std::string s)
{
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}

// A working tree found by walking up from a directory: where its .git is,
// as git finds it (a directory, or a file pointing at one).
struct work_tree
{
    fs::path top;
    fs::path git_dir;     // this worktree's admin dir (HEAD, MERGE_HEAD, ...)
    fs::path common_dir;  // shared refs (differs for linked worktrees)
};

std::optional<work_tree> find_work_tree(fs::path dir)
{
    for (;;)
    {
        const fs::path dot = dir / ".git";
        if (is_dir(dot)) return work_tree{dir, dot, dot};
        if (present(dot))
        {
            std::string line = trim(read_file(dot));
            if (line.starts_with("gitdir: "))
            {
                fs::path git = fs::path(line.substr(8));
                if (git.is_relative()) git = dir / git;
                work_tree wt{dir, git.lexically_normal(), git.lexically_normal()};
                const std::string common = trim(read_file(wt.git_dir / "commondir"));
                if (!common.empty())
                {
                    fs::path c(common);
                    wt.common_dir = (c.is_relative() ? wt.git_dir / c : c).lexically_normal();
                }
                return wt;
            }
        }
        if (!dir.has_relative_path() || dir.parent_path() == dir) return std::nullopt;
        dir = dir.parent_path();
    }
}

bool is_bare_repo(const fs::path& p)
{
    return present(p / "HEAD") && present(p / "config") && is_dir(p / "objects") &&
           is_dir(p / "refs/heads");
}

// Whether `top` is a submodule of the working tree above it: a path in its
// .gitmodules [spec 4.3, TGP:733-754].
bool is_submodule(const fs::path& top)
{
    if (!top.has_relative_path() || top.parent_path() == top) return false;
    auto outer = find_work_tree(top.parent_path());
    if (!outer) return false;
    const std::string rel = top.lexically_relative(outer->top).generic_string();
    std::ifstream in(outer->top / ".gitmodules");
    for (std::string line; std::getline(in, line);)
    {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq), value = trim(line.substr(eq + 1));
        key.erase(0, key.find_first_not_of(" \t"));
        value.erase(0, value.find_first_not_of(" \t"));
        if (trim(key) == "path" && value == rel) return true;
    }
    return false;
}

// GetAdminDirMask [spec 4.3 step 5]: what the filesystem says about `path`.
uint32_t admin_dir_mask(const fs::path& path)
{
    const bool dir = is_dir(path);
    auto wt = find_work_tree(dir ? path : path.parent_path());
    if (!wt) return 0;
    uint32_t m = GIT | IVF;
    if (dir)
    {
        m |= FIG;
        std::error_code ec;
        if (fs::equivalent(path, wt->top, ec))
        {
            m |= ROOT;
            if (is_submodule(wt->top)) m |= SUB;
        }
    }
    if (present(wt->common_dir / "refs/stash") ||
        read_file(wt->common_dir / "packed-refs").find(" refs/stash\n") != std::string::npos)
        m |= STASH;
    if (present(wt->git_dir / "svn/.metadata")) m |= SVN;
    if (present(wt->git_dir / "BISECT_START")) m |= BISECT;
    if (present(wt->git_dir / "MERGE_HEAD")) m |= MERGE;
    if (present(wt->top / ".gitmodules")) m |= SUBC;
    return m;
}

bool is_patch(const fs::path& p)
{
    std::string ext = p.extension().string();
    for (char& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".diff" || ext == ".patch";
}

// The flags for a selection, and the folder commands run on [spec 4.3].
struct state
{
    uint32_t flags = 0;
    fs::path folder;  // the background, or the one selected directory
    fs::path first;   // the first selected item, else the folder
};

state compute(const selection& sel)
{
    state s;
    s.folder = sel.background ? sel.folder : fs::path();
    if (sel.extended) s.flags |= EXT;
    bool fetched = false;
    for (const fs::path& p : sel.paths)
    {
        if (is_patch(p)) s.flags |= PATCH;
        if (fetched) continue;
        fetched = true;
        uint32_t m = admin_dir_mask(p);
        if (is_dir(p)) m |= FOLD;
        if (m & GIT) m |= NORMAL;  // status fallback, see the top
        s.flags |= m;
        // IVF: the first item's parent is inside a working tree.
        s.flags &= ~IVF;
        if (find_work_tree(p.parent_path())) s.flags |= IVF;
        if (is_bare_repo(p)) s.flags = (s.flags & (EXT | PATCH)) | BARE;
    }
    if (sel.background && !sel.folder.empty())
    {
        uint32_t m = admin_dir_mask(sel.folder) | FOLD;
        if (m & GIT) m |= NORMAL;
        if (is_bare_repo(sel.folder)) m = BARE | FOLD;
        s.flags |= m;
    }
    if (sel.paths.empty()) s.flags |= ONE;
    if (sel.paths.size() == 2) s.flags |= TWO;
    if (sel.paths.size() == 1)
    {
        s.flags |= ONE;
        if (is_dir(sel.paths.front()))
        {
            s.folder = sel.paths.front();
            s.flags |= admin_dir_mask(s.folder) | FOLD;
        }
    }
    s.first = sel.paths.empty() ? s.folder : sel.paths.front();
    return s;
}

bool matches(const row& r, uint32_t flags)
{
    for (const when& w : r.conditions)
        if (w.matches(flags)) return true;
    return false;
}

const row* find_row(std::string_view id)
{
    for (const row& r : k_rows)
        if (r.label && id == r.id) return &r;
    return nullptr;
}

// The branch HEAD names, for the Commit label; empty when detached.
std::string head_branch(const fs::path& path)
{
    auto wt = find_work_tree(is_dir(path) ? path : path.parent_path());
    if (!wt) return {};
    const std::string head = trim(read_file(wt->git_dir / "HEAD"));
    constexpr std::string_view prefix = "ref: refs/heads/";
    return head.starts_with(prefix) ? head.substr(prefix.size()) : std::string();
}

// Commit's label names the branch [spec 4.4]: &Commit -> "main"...
std::string commit_label(const state& s)
{
    const fs::path& at = s.folder.empty() ? s.first : s.folder;
    std::string label = (admin_dir_mask(at) & SUB) ? "&Commit submodule" : "&Commit";
    std::string branch = head_branch(at);
    if (branch.empty()) return label + "...";
    if (branch.size() > 64) branch = branch.substr(0, 64) + "...";
    std::string escaped;
    for (char c : branch) escaped += c == '&' ? "&&" : std::string(1, c);
    return label + " -> \"" + escaped + "\"...";
}

std::string utf8(const fs::path& p)
{
    const auto s = p.u8string();
    return {s.begin(), s.end()};
}

// Tortilla's settings file, where rap::sys::config_dir("tortilla") puts it;
// read directly, since the extension links no rapport core.
fs::path settings_file()
{
#ifdef _WIN32
    PWSTR dir = nullptr;
    fs::path p;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &dir))) p = dir;
    CoTaskMemFree(dir);
    return p.empty() ? p : p / "tortilla" / "settings";
#else
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    const char* home = std::getenv("HOME");
    fs::path base = xdg && *xdg ? fs::path(xdg) : home ? fs::path(home) / ".config" : fs::path();
    return base.empty() ? base : base / "tortilla" / "settings";
#endif
}

// What Settings chose for the menu. Read for every menu: it's one small
// file, and a change applies at once.
struct menu_settings
{
    placements where;
    bool hide_outside = false;
    std::vector<std::string> no_menu;
};

menu_settings read_settings()
{
    menu_settings out;
    std::ifstream in(settings_file());
    for (std::string line; std::getline(in, line);)
    {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string_view key = std::string_view(line).substr(0, eq);
        const std::string value = trim(line.substr(eq + 1));
        if (key == k_top_setting) out.where.top = parse_ids(value);
        else if (key == k_shift_setting) out.where.shift = parse_ids(value);
        else if (key == k_hidden_setting) out.where.hidden = parse_ids(value);
        else if (key == k_hide_outside_setting) out.hide_outside = value == "1";
        else if (key == k_no_menu_setting)
            for (size_t at = 0; at <= value.size();)
            {
                size_t tab = value.find('\t', at);
                if (tab == std::string::npos) tab = value.size();
                if (tab > at) out.no_menu.push_back(value.substr(at, tab - at));
                at = tab + 1;
            }
    }
    return out;
}

// TortoiseGit's exclusion patterns [spec 4.6]: "dir\\" a folder and all in
// it, "pre*" a prefix, else the path itself; case ignored.
bool excluded(const fs::path& path, const std::vector<std::string>& patterns)
{
    auto lower = [](std::string s)
    {
        for (char& c : s)
        {
            c = char(std::tolower(static_cast<unsigned char>(c)));
            if (c == '\\') c = '/';
        }
        return s;
    };
    const std::string p = lower(utf8(path));
    for (std::string pattern : patterns)
    {
        pattern = lower(pattern);
        if (pattern.empty()) continue;
        if (pattern.back() == '*')
        {
            if (p.starts_with(std::string_view(pattern).substr(0, pattern.size() - 1))) return true;
        }
        else if (pattern.back() == '/')
        {
            if (p.starts_with(pattern) || p + '/' == pattern) return true;
        }
        else if (p == pattern)
            return true;
    }
    return false;
}

std::string top_label(std::string label)
{
    return "Git " + label;
}

fs::path tortilla_exe()
{
#ifdef _WIN32
    return rap::shell::module_dir() / "tortilla.exe";
#else
    return rap::shell::module_dir() / "tortilla";
#endif
}

class tortilla_provider : public rap::shell::provider
{
public:
    std::vector<menu_item> menu(const selection& sel) override
    {
        state_ = compute(sel);
        const uint32_t f = state_.flags;

        const menu_settings settings = read_settings();
        if (excluded(state_.first, settings.no_menu)) return {};
        if (settings.hide_outside && !(f & (EXT | GIT | FIG | IVF | BARE | INACC))) return {};
        std::vector<menu_item> top, sub;
        bool armed = false, added_since = false;
        for (const row& r : k_rows)
        {
            if (!r.label)
            {
                armed = true;
                continue;
            }
            const shows where = where_shown(r, settings.where);
            if (where == shows::hidden || (where == shows::shift && !(f & EXT))) continue;
            if (!matches(r, f)) continue;
            std::string label = r.label;
            if (std::string_view(r.id) == "commit") label = commit_label(state_);
            // Diff of two files isn't Tortilla's yet, only of one item.
            const bool runs = r.command != run::none && (r.command != run::diff || (f & ONE));
            // Drawn by src/shell/menu_icons.cpp, one per Settings entry.
            menu_item item{.id = r.id,
                           .label = label,
                           .icon = "menu/" + std::string(group_of(r.id)) + ".ico",
                           .enabled = runs};
            if (where == shows::top)
            {
                item.label = top_label(item.label);
                top.push_back(std::move(item));
                continue;
            }
            // A separator only between two groups that both added entries.
            if (armed && added_since)
            {
                sub.push_back(menu_item::separator());
                added_since = false;
            }
            armed = false;
            sub.push_back(std::move(item));
            added_since = true;
        }

        std::vector<menu_item> out{menu_item::separator()};
        out.insert(out.end(), top.begin(), top.end());
        if (!sub.empty())
            out.push_back(
                {.label = "&Tortilla", .icon = "tortilla.ico", .submenu = std::move(sub)});
        out.push_back(menu_item::separator());
        return out;
    }

    void invoke(std::string_view id, const selection& sel) override
    {
        const row* r = find_row(id);
        if (!r) return;
        const std::string first = utf8(state_.first),
                          folder = utf8(state_.folder.empty() ? state_.first : state_.folder);
        switch (r->command)
        {
        case run::none: return;
        case run::log: rap::shell::launch(tortilla_exe(), {"log", first}); return;
        case run::diff: rap::shell::launch(tortilla_exe(), {"diff", first}); return;
        case run::rebase: rap::shell::launch(tortilla_exe(), {"rebase", folder}); return;
        case run::settings: rap::shell::launch(tortilla_exe(), {"settings", first}); return;
        case run::clone: rap::shell::launch(tortilla_exe(), {"clone", folder}); return;
        case run::fetch: rap::shell::launch(tortilla_exe(), {"fetch", folder}); return;
        case run::pull: rap::shell::launch(tortilla_exe(), {"pull", folder}); return;
        case run::push: rap::shell::launch(tortilla_exe(), {"push", folder}); return;
        case run::branch: rap::shell::launch(tortilla_exe(), {"branch", folder}); return;
        case run::tag: rap::shell::launch(tortilla_exe(), {"tag", folder}); return;
        case run::switch_to: rap::shell::launch(tortilla_exe(), {"switch", folder}); return;
        case run::merge: rap::shell::launch(tortilla_exe(), {"merge", folder}); return;
        case run::merge_abort: rap::shell::launch(tortilla_exe(), {"mergeabort", folder}); return;
        case run::edit_conflicts: rap::shell::launch(tortilla_exe(), {"conflicteditor", first}); return;
        case run::commit:
        case run::repostatus:
        case run::add:
        case run::revert:
        case run::resolve:
        {
            std::vector<std::string> args{r->command == run::commit       ? "commit"
                                          : r->command == run::repostatus ? "repostatus"
                                          : r->command == run::add        ? "add"
                                          : r->command == run::revert     ? "revert"
                                                                          : "resolve"};
            if (sel.paths.empty())
                args.push_back(folder);
            else
                for (const fs::path& p : sel.paths) args.push_back(utf8(p));
            rap::shell::launch(tortilla_exe(), args);
            return;
        }
        }
    }

private:
    state state_;
};

}  // namespace

RAPPORT_SHELL_PROVIDER(tortilla_provider)
