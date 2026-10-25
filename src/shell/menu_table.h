// TortoiseGit's Explorer context menu as data [spec 4.2]: every entry, its
// conditions and where it goes. Shared by the shell extension, which builds
// the menu from it, and the Settings dialog, which chooses the entries shown
// at the top level. Plain data: the extension links no more than this.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>

namespace tortilla::shell_menu
{

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
    repostatus,  // tortilla repostatus <every selected path>
    commit,      // tortilla commit <every selected path>
    add,         // tortilla add <every selected path>
    revert,      // tortilla revert <every selected path>
    resolve,     // tortilla resolve <every selected path>
    clone,       // tortilla clone <folder>
    fetch,       // tortilla fetch <folder>
    pull,        // tortilla pull <folder>
    push,        // tortilla push <folder>
    branch,      // tortilla branch <folder>
    tag,         // tortilla tag <folder>
    switch_to,   // tortilla switch <folder>
    merge,       // tortilla merge <folder>
    merge_abort,  // tortilla mergeabort <folder>
    refbrowse,    // tortilla refbrowse <folder>
    prevdiff,     // tortilla prevdiff <first>
    stash_save,   // tortilla stashsave <first>
    stash_apply,  // tortilla stashapply <folder>
    stash_pop,    // tortilla stashpop <folder>
    repo_create,  // tortilla repocreate <folder>
    rename,       // tortilla rename <first>
    edit_conflicts,  // tortilla conflicteditor <file>
    about,           // tortilla about
    remove,          // tortilla remove <paths...>
    remove_keep,     // tortilla removekeep <paths...>
    ignore,          // a submenu: tortilla ignore [--delete] [--mask] <paths...>
};

struct row
{
    const char* label;  // nullptr: a separator row
    std::array<when, 4> conditions{};
    place where = place::sub;
    run command = run::none;
    const char* id = "";
};

inline constexpr row separator{nullptr};

// TortoiseGit's menuInfo[] in order, labels as its resources have them
// [spec 4.2]. Rows it builds specially (Diff later, LFS, ignore) are listed
// for their position and shown disabled.
inline constexpr row k_rows[] = {
    {"Inaccessible...", {{{INACC, GIT | FIG | BARE}}}, place::top, run::none, "inaccessible"},
    {"Clone...",
     {{{FOLD, GIT | FIG | BARE | INACC}, {FOLD | IGN, 0}, {FOLD | EXT, 0}}},
     place::top,
     run::clone,
     "clone"},
    {"&Pull...",
     {{{FIG | ONE, BISECT | MERGE}, {ROOT, BISECT | MERGE}}},
     place::sub,
     run::pull,
     "pull"},
    {"F&etch...", {{{FIG | ONE, 0}, {BARE, 0}, {ROOT, 0}}}, place::sub, run::fetch, "fetch"},
    {"Pus&h...", {{{FIG | ONE, 0}, {BARE, 0}, {ROOT, 0}}}, place::sub, run::push, "push"},
    {"Sync...", {{{FIG | ONE, BISECT | MERGE}}}, place::top, run::none, "sync"},
    separator,
    {"&Commit...", {{{GIT, 0}, {FIG, 0}}}, place::top, run::commit, "commit"},
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
    {"&Diff with previous version", {{{GIT | ONE, ADDED}}}, place::sub, run::prevdiff, "prevdiff"},
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
    {"Browse References", {{{FIG | ONE, 0}, {BARE, 0}}}, place::sub, run::refbrowse, "refbrowse"},
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
     run::repostatus,
     "repostatus"},
    {"Rebase...", {{{FIG | ONE, BISECT | MERGE}}}, place::sub, run::rebase, "rebase"},
    {"Stash changes", {{{GIT | ONE, MERGE}}}, place::sub, run::stash_save, "stashsave"},
    {"Stash Apply", {{{FIG | ONE | STASH, 0}}}, place::shift, run::stash_apply, "stashapply"},
    {"Stash Pop", {{{FIG | ONE | STASH, 0}}}, place::sub, run::stash_pop, "stashpop"},
    {"Stash List", {{{FIG | ONE | STASH, 0}}}, place::sub, run::none, "stashlist"},
    separator,
    {"Bisect start", {{{FIG | ONE, BISECT | MERGE}}}, place::sub, run::none, "bisectstart"},
    {"Bisect good", {{{FIG | ONE | BISECT, 0}}}, place::sub, run::none, "bisectgood"},
    {"Bisect bad", {{{FIG | ONE | BISECT, 0}}}, place::sub, run::none, "bisectbad"},
    {"Bisect skip", {{{FIG | ONE | BISECT, 0}}}, place::sub, run::none, "bisectskip"},
    {"Bisect reset", {{{FIG | ONE | BISECT, 0}}}, place::sub, run::none, "bisectreset"},
    separator,
    {"&Edit conflicts", {{{GIT | CONFL, FOLD}}}, place::sub, run::edit_conflicts, "conflicteditor"},
    {"Res&olve...",
     {{{GIT | CONFL, 0}, {GIT | FOLD, 0}, {FIG, 0}}},
     place::sub,
     run::resolve,
     "resolve"},
    {"Abort &Merge", {{{GIT | MERGE, 0}, {FIG | MERGE, 0}}}, place::sub, run::merge_abort, "mergeabort"},
    {"Re&name...", {{{GIT | ONE | IVF, ROOT}, {ROOT | SUB, 0}}}, place::sub, run::rename, "rename"},
    {"&Delete",
     {{{GIT | IVF, ADDED | ROOT}, {FIG | ROOT | SUB, 0}}},
     place::sub,
     run::remove,
     "remove"},
    {"&Delete (keep local)", {{{GIT | IVF, ADDED | ROOT}}}, place::sub, run::remove_keep, "removekeep"},
    {"Re&vert...", {{{GIT, NORMAL}, {FIG, 0}}}, place::sub, run::revert, "revert"},
    {"&Clean up...", {{{FIG | FOLD, 0}}}, place::sub, run::none, "cleanup"},
    separator,
    // (LFS: a submenu only when the repository has <gitdir>/lfs [spec 4.4].)
    separator,
    {"S&witch/Checkout...", {{{FIG | ONE, 0}}}, place::sub, run::switch_to, "switch"},
    {"&Merge...", {{{FIG | ONE, BISECT | MERGE}}}, place::sub, run::merge, "merge"},
    {"Create &Branch...", {{{FIG | ONE, 0}}}, place::sub, run::branch, "branch"},
    {"Create &Tag...", {{{FIG | ONE, 0}}}, place::sub, run::tag, "tag"},
    {"E&xport...", {{{FIG | ONE, 0}, {BARE, 0}}}, place::sub, run::none, "export"},
    separator,
    {"Create repositor&y here...",
     {{{FOLD, GIT | FIG | BARE | INACC}, {FOLD | IGN, 0}, {FOLD | EXT, GIT}}},
     place::top,
     run::repo_create,
     "repocreate"},
    {"&Add...",
     {{{IVF, GIT}, {GIT | FOLD, 0}, {IGN, 0}, {DELETED, FOLD | ONE}}},
     place::sub,
     run::add,
     "add"},
    {"&Blame", {{{GIT | ONE, FOLD | ADDED}}}, place::sub, run::none, "blame"},
    {"Add to &ignore list", {{{IVF, IGN | GIT | ROOT}}}, place::sub, run::ignore, "ignore"},
    {"Delete and add to &ignore list",
     {{{IVF | GIT, IGN | ROOT}}},
     place::sub,
     run::ignore,
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
    {"A&bout", {{{FOLD, 0}, {0, FOLD}}}, place::sub, run::about, "about"},
};

// Where each entry shows, as the Settings page chooses it: TortoiseGit's
// top-level and Shift-only masks and its administrators' hidden mask
// [spec 4.6], one choice per entry.
enum class shows
{
    top,      // directly in the file manager's menu, as "Git <entry>"
    submenu,  // in the Tortilla submenu
    shift,    // in the submenu, only while Shift is held
    hidden,   // never
};

// The entries TortoiseGit sets as one (one bit of its masks [spec 4.6]):
// the five bisect rows, Merge with Abort Merge, and the two ignore entries.
constexpr std::string_view group_of(std::string_view id)
{
    if (id.starts_with("bisect")) return "bisect";
    if (id == "mergeabort") return "merge";
    if (id == "deleteignore") return "ignore";
    return id;
}

// Tortilla's settings file keeps the choices as lists of group ids,
// comma-separated; an unset list means the default (the rows marked top or
// shift; nothing hidden).
constexpr std::string_view k_top_setting = "shell.top";
constexpr std::string_view k_shift_setting = "shell.shift";
constexpr std::string_view k_hidden_setting = "shell.hidden";
// "1": no menu outside working trees unless Shift is held [spec 4.5, 4.6].
constexpr std::string_view k_hide_outside_setting = "shell.hideOutside";
// Paths with no menu, tab-separated: "dir\" a folder and all in it, "pre*"
// a prefix, else that path exactly; case ignored [spec 4.6].
constexpr std::string_view k_no_menu_setting = "shell.noMenu";

using id_set = std::set<std::string, std::less<>>;

inline id_set parse_ids(std::string_view list)
{
    id_set out;
    for (size_t at = 0; at <= list.size();)
    {
        size_t comma = list.find(',', at);
        if (comma == std::string_view::npos) comma = list.size();
        if (comma > at) out.emplace(list.substr(at, comma - at));
        at = comma + 1;
    }
    return out;
}

// The three lists as saved; nullopt: never saved, the default.
struct placements
{
    std::optional<id_set> top, shift, hidden;
};

inline shows where_shown(const row& r, const placements& p)
{
    const std::string_view g = group_of(r.id);
    if (g == "inaccessible") return shows::top;  // always [spec 4.2]
    if (p.hidden ? p.hidden->contains(g) : false) return shows::hidden;
    if (p.shift ? p.shift->contains(g) : r.where == place::shift) return shows::shift;
    if (p.top ? p.top->contains(g) : r.where == place::top) return shows::top;
    return shows::submenu;
}

}  // namespace tortilla::shell_menu
