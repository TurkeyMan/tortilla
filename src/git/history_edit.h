// Editing a branch's history in place, from the log: combining, rewording,
// dropping, folding and moving commits without a rebase. The commits are
// rewritten in memory with libgit2 (three-way tree merges, never the working
// tree or the index), then the branch is moved with a compare-and-swap, so
// nothing happens unless the whole edit worked, and a branch that moved
// meanwhile isn't overwritten.
//
// libgit2 rather than git's plumbing (merge-tree --write-tree, commit-tree):
// it's linked already, needs no git version check (merge-tree's mode is git
// 2.38+), and a replay of ten commits is ten calls, not twenty processes,
// which counts on Windows over \\wsl.localhost.
#pragma once

#include "git/repo.h"

#include <expected>
#include <optional>
#include <string>
#include <vector>

namespace tortilla
{

// A local branch's own history: from its tip back along first parents to
// the first commit already in the mainline (the leading remote's default
// branch), which isn't part of it.
struct branch_history
{
    std::string branch;  // full name, refs/heads/...
    oid tip;
    std::vector<oid> line;       // newest first
    std::vector<size_t> merges;  // indexes into `line` of merge commits
    // Indexes into `line` of commits in SVN (git svn's git-svn-id line):
    // SVN's history can't be rewritten, so neither they nor anything below
    // them is (the next dcommit or rebase would break).
    std::vector<size_t> svn;
    // The first parent of the line's oldest commit: where the line leaves
    // the mainline; nullopt when that commit is a root.
    std::optional<oid> below;
    std::string mainline;  // the ref it stops at, full name; "" when there's none
    std::string upstream;  // full name of the branch's upstream; "" when it has none
    std::optional<oid> upstream_tip;
    std::string worktree;      // another working tree that has it checked out
    bool checked_out = false;  // this working tree's HEAD is on it

    std::optional<size_t> index_of(const oid&) const;
    bool is_merge(size_t index) const;
    // The first parent of line[index]: the next one down, or `below`.
    std::optional<oid> parent_of(size_t index) const;
    // The branch's short name, "main".
    std::string name() const;
};

// The longest line looked at; older commits can't be edited from the log.
constexpr size_t history_limit = 2000;

std::expected<branch_history, std::string>
load_branch_history(const repo&, const std::string& branch, const std::string& mainline);

// The local branch whose own history holds all of `commits`: the
// checked-out one if it does, else the only other one; the reason when
// none does (merged, on no branch, on several).
std::expected<branch_history, std::string>
branch_holding(const repo&, const std::vector<oid>& commits, const std::string& mainline);

// One commit of a replay, oldest first. `fold` folds it into the commit the
// step before made (fixup); `message` replaces the resulting commit's
// message (a squash's combined one, a reword).
struct edit_step
{
    oid commit;
    bool fold = false;
    std::optional<std::string> message;
};

// A branch rewritten: its commits from `base` up replayed as `steps` say.
struct edit_plan
{
    std::string branch;  // full name
    oid old_tip;
    std::optional<oid> base;  // nullopt: from nothing (the steps start at a root commit)
    std::vector<edit_step> steps;
};

// A branch moved, from what it was to what it becomes; `from` nullopt: it's
// made (must not exist), `to` nullopt: it's deleted.
struct ref_move
{
    std::string ref;
    std::optional<oid> from, to;
};

// An edit ready to apply, and what applying it undoes.
struct history_edit
{
    std::string reflog;  // "tortilla: combine 3 commits"
    std::vector<ref_move> moves;
    // The checked-out branch moves but the index and working tree stay as
    // they are, even when its tree changes: Undo last commit (reset --soft)
    // and Amend with staged changes (the index is what the new tip has).
    bool keep_index = false;
    bool pushed = false;  // rewritten commits were on the upstream already
    // The upstream's remote-tracking tip the edit was made from (the
    // branch_history's): what a force push afterwards leases on, so a push
    // by someone else, fetched since, is never overwritten.
    std::optional<oid> lease;

    history_edit undo() const;
};

// Why a replay stopped: a conflict, or something else.
struct replay_failure
{
    bool conflict = false;
    oid commit;  // the commit that didn't apply
    std::string message;
};

// The commits replayed, nothing moved yet: the new tip. A step's commit in
// SVN is refused ("in SVN"), whatever made the plan.
std::expected<oid, replay_failure> replay(const repo&, const edit_plan&);

// What applying does to this working tree.
enum class tree_effect
{
    none,    // nothing checked out moves, or its tree stays (index and files untouched)
    follow,  // the files follow the new tip; the tree is clean
    stash,   // the same, but there are local changes: stash them first, or cancel
};
tree_effect effect_of(const repo&, const history_edit&);

// Moves the refs (all or none, each only from what it was), then, if the
// checked-out branch's tree changed, brings the index and files along
// safely (git read-tree -m -u: local changes are never overwritten).
// `stash`: local changes are stashed first and restored after; if they
// don't restore cleanly they're left in the stash, and the error says so.
// Returns a warning that doesn't undo the edit, if any (changes left in the
// stash). A move that leaves a commit in SVN behind isn't made: SVN's
// history stays as it is, whatever made the edit.
std::expected<std::string, std::string> apply_edit(const repo&, const history_edit&,
                                                   bool stash = false);

// -- The log's actions, each a plan or why not --------------------------

// The messages of `commits` (oldest first) as git's squash writes them,
// for editing: the separator lines go again with strip_squash_separators.
std::string combined_message(const repo&, const std::vector<oid>& oldest_first);
std::string strip_squash_separators(const std::string& message, size_t count);
// A message as typed, made a commit's: trailing spaces and blank lines
// gone, one newline at the end.
std::string tidy_message(const std::string&);

// Combine: `commits` (any order) must be adjacent in the line, no merge.
std::expected<edit_plan, std::string>
plan_combine(const branch_history&, const std::vector<oid>& commits, const std::string& message);
std::expected<edit_plan, std::string> plan_reword(const branch_history&, const oid&,
                                                  const std::string& message);
std::expected<edit_plan, std::string> plan_drop(const branch_history&, const std::vector<oid>&);
// Fold `commit` into `into`, an older commit of the line; `message`, when
// given, is the result's (a squash), else `into`'s stays (a fixup).
std::expected<edit_plan, std::string> plan_fold(const branch_history&, const oid& commit,
                                                const oid& into,
                                                std::optional<std::string> message = {});
// The line's commits in a new order: `moved` (adjacent or not) put in
// front of line index `destination` (0 = on top; line.size() = at the
// bottom), keeping their order.
std::expected<edit_plan, std::string>
plan_reorder(const branch_history&, const std::vector<oid>& moved, size_t destination);
// Amend with staged changes: `staged` (staged_commit's) folded into
// `into`, the commits above replayed on the result.
std::expected<edit_plan, std::string> plan_amend(const branch_history&, const oid& staged,
                                                 const oid& into);
// The index's staged changes as a commit of their own on HEAD, unreferenced,
// for folding into an older commit; why not when there's nothing staged or
// a conflict.
std::expected<oid, std::string> staged_commit(const repo&);

// Why there are no staged changes to amend with ("nothing staged",
// "conflicts"), without writing anything; nullopt when there are.
std::optional<std::string> why_nothing_staged(const repo&);

// Whether two commits have the same tree (an amend's result has the
// index's, or the index wouldn't match it).
bool same_tree(const repo&, const oid&, const oid&);

// Each of these checks the line first: why not, or nullopt when it can.
// Every plan goes through it, whichever way in (a menu, a drag), with all
// the commits it rewrites: from the oldest of `commits` up to the tip, so
// none may be in SVN ("in SVN"). `replays`: commits above are replayed onto
// changed trees, so a merge among them can't be.
std::optional<std::string> why_not_editable(const branch_history&, const std::vector<oid>& commits,
                                            bool replays);
// Whether rewriting from `oldest` up rewrites commits on the upstream.
bool rewrites_pushed(const repo&, const branch_history&, const oid& oldest);

// The steps as the interactive rebase window lists them (newest first),
// for going on there after a conflict: onto `base`, each commit picked or
// squashed into the one below; commits not listed are skipped.
struct rebase_todo
{
    std::string branch;  // short name
    std::optional<oid> onto;
    std::vector<std::pair<oid, bool>> rows;  // commit, squash; newest first
};
rebase_todo todo_for(const edit_plan&);

// Finishing an edit: the plan's branch moved to `new_tip`.
history_edit edit_from(const repo&, const branch_history&, const edit_plan&, const oid& new_tip,
                       std::string reflog);

// Move to branch: `commits` (adjacent, at most up to the tip) taken off
// `from` and replayed onto `onto` (a local branch's full name), or, with
// `new_branch` (a full name), onto a new branch where they were. The edit
// moves both refs, or makes the new one.
std::expected<history_edit, replay_failure> move_commits(const repo&, const branch_history& from,
                                                         const std::vector<oid>& commits,
                                                         const std::string& onto, bool new_branch,
                                                         std::string reflog);

// After rewriting pushed commits: git push of `tip` to `branch`'s (full
// name) upstream branch, only if that is still at `expected` there
// (--force-with-lease=<its ref>:<expected>, the edit's lease, not the
// remote-tracking ref as it is when the push runs, which a fetch since may
// have moved); empty when it has no upstream on a remote.
std::vector<std::string> force_push_args(const repo&, const std::string& branch,
                                         const oid& expected, const oid& tip);

// Undo last commit: the checked-out branch back to its tip's parent, the
// index and files as they are (git reset --soft HEAD~1).
std::expected<history_edit, std::string> undo_last_commit(const repo&, const branch_history&);

}  // namespace tortilla
