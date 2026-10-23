// Tortilla's entries in the file manager's context menu: TortoiseGit's
// Explorer menu [spec 4], built into tortilla-shell by rapport. This runs
// inside the file manager, so it looks only at the filesystem: no libgit2,
// no git processes, no toolkit.
//
// Until the status cache exists, an item inside a working tree counts as
// tracked and unmodified: TortoiseGit's own fallback when status fails
// [spec 4.3 step 3]. So IGN, CONFL, ADDED and DELETED are never set.
#include <rapport/shell.h>

#include <array>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>

namespace
{

namespace fs = std::filesystem;
using rap::shell::menu_item;
using rap::shell::selection;

// -- item states ----------------------------------------------------------------

// TortoiseGit's ITEMIS_* flags [spec 4.2, 4.3]; only those it tests.
enum : uint32_t
{
    ONE = 1u << 0,
    TWO = 1u << 1,
    EXT = 1u << 2,     // Shift held
    GIT = 1u << 3,     // tracked (here: inside a working tree)
    IVF = 1u << 4,     // the item's parent is inside a working tree
    FOLD = 1u << 5,    // a directory
    FIG = 1u << 6,     // a directory inside a working tree
    ROOT = 1u << 7,    // the top of a working tree
    SUB = 1u << 8,     // a submodule's top
    SUBC = 1u << 9,    // the working tree has a .gitmodules
    BARE = 1u << 10,   // a bare repository
    MERGE = 1u << 11,  // a merge in progress
    BISECT = 1u << 12,
    STASH = 1u << 13,
    SVN = 1u << 14,  // git-svn
    CONFL = 1u << 15,
    NORMAL = 1u << 16,
    ADDED = 1u << 17,
    DELETED = 1u << 18,
    IGN = 1u << 19,
    PATCH = 1u << 20,  // a .diff or .patch file
    INACC = 1u << 21,  // a repository git can't open
};

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

// -- the menu table -------------------------------------------------------------

// A condition pair: every `yes` bit set and every `no` bit clear; {0,0}
// never matches [spec 4.2].
struct when
{
    uint32_t yes = 0, no = 0;
    bool matches(uint32_t f) const { return (yes || no) && (f & yes) == yes && (f & no) == 0; }
};

enum class place
{
    sub,
    top,    // in TortoiseGit's default top-level mask
    shift,  // in its default extended mask: only with Shift held
};

// What Tortilla runs for an entry; none yet means shown disabled.
enum class run
{
    none,
    log,       // tortilla log <first>
    diff,      // tortilla diff <first>: the working tree against HEAD
    rebase,    // tortilla rebase <folder>
    settings,  // tortilla settings <first>
};

struct row
{
    const char* label;  // nullptr: a separator row
    std::array<when, 4> conditions{};
    place where = place::sub;
    run command = run::none;
    const char* id = "";
};

constexpr row separator{nullptr};

// TortoiseGit's menuInfo[] in order, labels as its resources have them
// [spec 4.2]. Rows it builds specially (Diff later, LFS, ignore) are listed
// for their position and shown disabled.
const row k_rows[] = {
    {"Inaccessible...", {{{INACC, GIT | FIG | BARE}}}, place::top, run::none, "inaccessible"},
    {"Clone...",
     {{{FOLD, GIT | FIG | BARE | INACC}, {FOLD | IGN, 0}, {FOLD | EXT, 0}}},
     place::top,
     run::none,
     "clone"},
    {"&Pull...",
     {{{FIG | ONE, BISECT | MERGE}, {ROOT, BISECT | MERGE}}},
     place::sub,
     run::none,
     "pull"},
    {"F&etch...", {{{FIG | ONE, 0}, {BARE, 0}, {ROOT, 0}}}, place::sub, run::none, "fetch"},
    {"Pus&h...", {{{FIG | ONE, 0}, {BARE, 0}, {ROOT, 0}}}, place::sub, run::none, "push"},
    {"Sync...", {{{FIG | ONE, BISECT | MERGE}}}, place::top, run::none, "sync"},
    separator,
    {"&Commit...", {{{GIT, 0}, {FIG, 0}}}, place::top, run::none, "commit"},
    {"SVN DCommit...",
     {{{GIT | SVN, BISECT | MERGE}, {FIG | SVN, BISECT | MERGE}}},
     place::sub,
     run::none,
     "svndcommit"},
    {"SVN Rebase", {{{FIG | SVN | ONE, BISECT | MERGE}}}, place::sub, run::none, "svnrebase"},
    {"SVN Fetch", {{{FIG | SVN | ONE, 0}}}, place::sub, run::none, "svnfetch"},
    {"Import SVN Ignore ...", {{{FIG | SVN | ONE, 0}}}, place::shift, run::none, "svnignore"},
    separator,
    {"&Diff", {{{GIT | ONE, 0}, {TWO, FOLD}}}, place::sub, run::diff, "diff"},
    {"Diff later", {{{ONE, FOLD}}}, place::sub, run::none, "difflater"},
    {"&Diff with previous version", {{{GIT | ONE, ADDED}}}, place::sub, run::none, "prevdiff"},
    separator,
    {"Show &log",
     {{{GIT | ONE, ADDED}, {FOLD | FIG | ONE, ADDED}, {FIG | ONE, ADDED}, {BARE, 0}}},
     place::sub,
     run::log,
     "log"},
    {"Show log of this folder",
     {{{FIG | ONE | ROOT | SUB, 0}}},
     place::sub,
     run::none,
     "logsubmodule"},
    {"Show &RefLog", {{{FIG | ONE, 0}, {BARE, 0}}}, place::sub, run::none, "reflog"},
    {"Browse References", {{{FIG | ONE, 0}, {BARE, 0}}}, place::sub, run::none, "refbrowse"},
    {"Daemon",
     {{{GIT | ONE, ADDED}, {FOLD | FIG | ONE, ADDED}, {FIG | ONE, ADDED}, {BARE, 0}}},
     place::sub,
     run::none,
     "daemon"},
    {"Revision &graph",
     {{{GIT | ONE, ADDED}, {FOLD | FIG | ONE, ADDED}, {FIG | ONE, ADDED}, {BARE, 0}}},
     place::sub,
     run::none,
     "revisiongraph"},
    {"&Repo-browser", {{{FIG | ONE, 0}, {BARE | ONE, 0}}}, place::sub, run::none, "repobrowser"},
    {"Check for modi&fications",
     {{{GIT, 0}, {FOLD | FIG, 0}}},
     place::sub,
     run::none,
     "repostatus"},
    {"Rebase...", {{{FIG | ONE, BISECT | MERGE}}}, place::sub, run::rebase, "rebase"},
    {"Stash changes", {{{GIT | ONE, MERGE}}}, place::sub, run::none, "stashsave"},
    {"Stash Apply", {{{FIG | ONE | STASH, 0}}}, place::shift, run::none, "stashapply"},
    {"Stash Pop", {{{FIG | ONE | STASH, 0}}}, place::sub, run::none, "stashpop"},
    {"Stash List", {{{FIG | ONE | STASH, 0}}}, place::sub, run::none, "stashlist"},
    separator,
    {"Bisect start", {{{FIG | ONE, BISECT | MERGE}}}, place::sub, run::none, "bisectstart"},
    {"Bisect good", {{{FIG | ONE | BISECT, 0}}}, place::sub, run::none, "bisectgood"},
    {"Bisect bad", {{{FIG | ONE | BISECT, 0}}}, place::sub, run::none, "bisectbad"},
    {"Bisect skip", {{{FIG | ONE | BISECT, 0}}}, place::sub, run::none, "bisectskip"},
    {"Bisect reset", {{{FIG | ONE | BISECT, 0}}}, place::sub, run::none, "bisectreset"},
    separator,
    {"&Edit conflicts", {{{GIT | CONFL, FOLD}}}, place::sub, run::none, "conflicteditor"},
    {"Res&olve...",
     {{{GIT | CONFL, 0}, {GIT | FOLD, 0}, {FIG, 0}}},
     place::sub,
     run::none,
     "resolve"},
    {"Abort &Merge", {{{GIT | MERGE, 0}, {FIG | MERGE, 0}}}, place::sub, run::none, "mergeabort"},
    {"Re&name...", {{{GIT | ONE | IVF, ROOT}, {ROOT | SUB, 0}}}, place::sub, run::none, "rename"},
    {"&Delete",
     {{{GIT | IVF, ADDED | ROOT}, {FIG | ROOT | SUB, 0}}},
     place::sub,
     run::none,
     "remove"},
    {"&Delete (keep local)", {{{GIT | IVF, ADDED | ROOT}}}, place::sub, run::none, "removekeep"},
    {"Re&vert...", {{{GIT, NORMAL}, {FIG, 0}}}, place::sub, run::none, "revert"},
    {"&Clean up...", {{{FIG | FOLD, 0}}}, place::sub, run::none, "cleanup"},
    separator,
    // (LFS: a submenu only when the repository has <gitdir>/lfs [spec 4.4].)
    separator,
    {"S&witch/Checkout...", {{{FIG | ONE, 0}}}, place::sub, run::none, "switch"},
    {"&Merge...", {{{FIG | ONE, BISECT | MERGE}}}, place::sub, run::none, "merge"},
    {"Create &Branch...", {{{FIG | ONE, 0}}}, place::sub, run::none, "branch"},
    {"Create &Tag...", {{{FIG | ONE, 0}}}, place::sub, run::none, "tag"},
    {"E&xport...", {{{FIG | ONE, 0}, {BARE, 0}}}, place::sub, run::none, "export"},
    separator,
    {"Create repositor&y here...",
     {{{FOLD, GIT | FIG | BARE | INACC}, {FOLD | IGN, 0}, {FOLD | EXT, GIT}}},
     place::top,
     run::none,
     "repocreate"},
    {"&Add...",
     {{{IVF, GIT}, {GIT | FOLD, 0}, {IGN, 0}, {DELETED, FOLD | ONE}}},
     place::sub,
     run::none,
     "add"},
    {"&Blame", {{{GIT | ONE, FOLD | ADDED}}}, place::sub, run::none, "blame"},
    {"Add to &ignore list", {{{IVF, IGN | GIT | ROOT}}}, place::sub, run::none, "ignore"},
    {"Delete and add to &ignore list",
     {{{IVF | GIT, IGN | ROOT}}},
     place::sub,
     run::none,
     "deleteignore"},
    separator,
    {"Worktrees", {{{FIG | ONE, 0}, {BARE, 0}}}, place::sub, run::none, "worktreelist"},
    {"Submodule Add...", {{{FIG | ONE, 0}}}, place::sub, run::none, "subadd"},
    {"Submodule &Update...", {{{FIG | SUBC, 0}}}, place::sub, run::none, "subupdate"},
    {"Submodule Sync", {{{FIG | SUBC, 0}}}, place::shift, run::none, "subsync"},
    separator,
    {"Create Patch Serial...", {{{FIG | ONE, 0}}}, place::sub, run::none, "formatpatch"},
    {"Apply Patch Serial...", {{{PATCH, 0}, {FIG | ONE, 0}}}, place::sub, run::none, "importpatch"},
    {"Review/apply single &patch...",
     {{{PATCH | ONE, 0}, {EXT | ONE, FOLD}}},
     place::sub,
     run::none,
     "patch"},
    {"Send Mail...", {{{PATCH, 0}, {EXT, FOLD}}}, place::sub, run::none, "sendmail"},
    separator,
    {"&Settings", {{{FOLD, 0}, {0, FOLD}}}, place::sub, run::settings, "settings"},
    {"&Help", {{{FOLD, 0}, {0, FOLD}}}, place::sub, run::none, "help"},
    {"A&bout", {{{FOLD, 0}, {0, FOLD}}}, place::sub, run::none, "about"},
};

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

std::string utf8(const fs::path& p)
{
    const auto s = p.u8string();
    return {s.begin(), s.end()};
}

class tortilla_provider : public rap::shell::provider
{
public:
    std::vector<menu_item> menu(const selection& sel) override
    {
        state_ = compute(sel);
        const uint32_t f = state_.flags;

        std::vector<menu_item> top, sub;
        bool armed = false, added_since = false;
        for (const row& r : k_rows)
        {
            if (!r.label)
            {
                armed = true;
                continue;
            }
            if (r.where == place::shift && !(f & EXT)) continue;
            if (!matches(r, f)) continue;
            std::string label = r.label;
            if (std::string_view(r.id) == "commit") label = commit_label(state_);
            // Diff of two files isn't Tortilla's yet, only of one item.
            const bool runs = r.command != run::none && (r.command != run::diff || (f & ONE));
            menu_item item{.id = r.id, .label = label, .enabled = runs};
            if (r.where == place::top || std::string_view(r.id) == "inaccessible")
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

    void invoke(std::string_view id, const selection&) override
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
        }
    }

private:
    state state_;
};

}  // namespace

RAPPORT_SHELL_PROVIDER(tortilla_provider)
