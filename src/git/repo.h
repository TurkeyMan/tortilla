// Thin libgit2 wrapper: repository discovery, refs and the revision walk.
// A Repo is not thread-safe; worker threads open their own.
#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

struct git_repository;
struct git_diff;
struct git_config;

namespace tortilla
{

struct oid
{
    std::array<uint8_t, 20> bytes{};

    bool operator==(const oid&) const = default;
    std::string hex(size_t length = 40) const;
};

struct oid_hash
{
    size_t operator()(const oid& o) const
    {
        size_t h;
        std::memcpy(&h, o.bytes.data(), sizeof h);
        return h;
    }
};

enum class ref_kind
{
    head,  // detached HEAD
    local_branch,
    remote_branch,
    tag,
    stash,
    other,
};

struct ref
{
    std::string name;      // short name, e.g. "main", "origin/main", "v1.0"
    std::string full_name;  // e.g. "refs/heads/main"
    ref_kind kind;
    bool current = false;    // the branch HEAD points at
    bool annotated = false;  // annotated tag
    std::string upstream;    // local branch: full name of its upstream, if any
    // Local branch: the other working tree it's checked out in, if any (not
    // this one); changing it here would change that tree's files.
    std::string worktree;
    // Remote branch: its remote's default (<remote>/HEAD's target, else
    // main, else master).
    bool remote_default = false;
};

using ref_map = std::unordered_map<oid, std::vector<ref>, oid_hash>;

struct commit
{
    oid id;
    std::vector<oid> parents;
    std::string summary;
    std::string message;  // full message, for searching
    std::string author, email;
    int64_t time = 0;       // author time, seconds since epoch
    int offset_minutes = 0;  // author's UTC offset
    std::string committer, committer_email;
    int64_t commit_time = 0;
    int commit_offset_minutes = 0;
};

struct commit_details
{
    std::string message;  // full message
    std::string committer, committer_email;
    int64_t commit_time = 0;
    int commit_offset_minutes = 0;
};

// Which kinds of change a commit makes, for the log's Actions column.
enum action_bits : uint8_t
{
    action_modified = 1,
    action_added = 2,
    action_deleted = 4,
    action_renamed = 8,
    action_conflicted = 16,
};

struct file_change
{
    std::string path, old_path;  // old_path set for renames and copies
    // A D M R C T; working-tree lists also '?' unversioned, 'U' conflicted,
    // '!' ignored, 'S' skip-worktree and 'h' assume-unchanged (TortoiseGit's
    // "local changes ignored").
    char status = 'M';
    bool staged = false;  // working tree: the change is (also) in the index
    size_t added = 0, removed = 0;
    bool binary = false;
    bool counted = true;  // false: added/removed not known yet
};

// Where a log walk starts [spec 1.9.1].
enum class walk_refs
{
    head,   // HEAD, or walk_options::rev when set
    all,    // every ref and worktree HEAD, like git log --all
    basic,  // local and remote branches and tags, and HEAD ("All basic refs")
    local,  // local branches and HEAD ("All local branches")
    tips,   // walk_options::tips alone, not HEAD
};

// Whether a walk over `refs` starts at the ref named `full_name`.
bool walks_ref(walk_refs refs, std::string_view full_name);

// The log's sync downloads remotes' branches ahead of time into
// refs/prefetch/ (git/sync.h): not the user's refs, so never labels, walked
// or listed.
inline bool is_prefetch_ref(std::string_view full_name)
{
    return full_name.starts_with("refs/prefetch/");
}

struct walk_options
{
    walk_refs refs = walk_refs::head;
    std::string rev;                       // with head: start here instead (a ref or commit)
    bool first_parent = false;             // --first-parent
    bool no_merges = false;                // --no-merges
    std::optional<int64_t> since, until;  // author time bounds, inclusive
    // With refs other than head: the ref tips to start from, when the caller already
    // has them (from refs()); empty = list the refs here.
    std::vector<oid> tips;
    // Repository-relative paths the history is limited to, as git log -- <paths>
    // has it (only commits that changed them, parents rewritten to join those
    // up); empty = the whole repository.
    std::vector<std::string> paths;
    bool follow = false;        // with one file in paths: across its renames (git log --follow)
    bool full_history = false;  // with paths: --full-history, no history simplification

    // Tips shown sparsely (pinned branches): of their history, only what
    // the rest needs to join them, as tips_only keeps it.
    std::vector<oid> sparse_tips;
    // Branch tips only, git log --simplify-by-decoration: the tips, the
    // `decorated` commits (those wearing a label on show), and the merges
    // and forks between them, each one's parents the nearest of those.
    bool tips_only = false;
    std::vector<oid> decorated;
    // Only what isn't merged into this commit: each branch stops at its
    // merge base with it, which is shown without its parents (as git log
    // --boundary does), and so is a tip already merged.
    std::optional<oid> unmerged_into;
};

class repo
{
public:
    // Finds the repository containing `path` (walks up like git does).
    static std::expected<repo, std::string> open(const std::string& path);

    repo(repo&&) noexcept;
    repo& operator=(repo&&) noexcept;
    ~repo();

    std::string workdir() const;
    std::string git_dir() const;    // the .git directory
    // Where the repository's worktrees share refs and config: the main
    // .git, from a linked worktree too.
    std::string common_dir() const;
    std::string head_name() const;  // branch short name, or "HEAD" when detached
    ref_map refs() const;           // labels sorted by full ref name, as TortoiseGit shows them
    // The same through git for-each-ref: what refs() falls back to when
    // libgit2 can't read the refs (and what the tests hold it to).
    ref_map refs_by_git() const;
    std::optional<oid> head_id() const;
    bool bare() const;

    // An operation git is in the middle of (MERGE_HEAD, rebase-merge, ...).
    enum class repo_state
    {
        none,
        merge,
        revert,
        cherry_pick,
        bisect,
        rebase,
        apply_mailbox,
    };
    repo_state state() const;

    // Topological + time order, newest first. `sink` returns false to stop.
    std::expected<void, std::string> walk(const walk_options&,
                                          const std::function<bool(commit&&)>& sink) const;

    std::expected<commit_details, std::string> details(const oid&) const;
    // The commit's note on the default notes ref (core.notesRef, else
    // refs/notes/commits), as git log shows it; "" when it has none.
    std::string note(const oid&) const;
    // Changes against the first parent (or the empty tree for a root commit).
    std::expected<std::vector<file_change>, std::string> changes(const oid&) const;
    // The same in two steps, for commits that may be huge: `listed` gets the
    // files at once (without line counts, cheap), then `counted` gets them
    // again in batches as their lines are counted, which reads every file.
    // `stop` is asked between files; true abandons the counting.
    struct change_steps
    {
        std::function<void(const std::vector<file_change>&)> listed;
        std::function<void(size_t first, const std::vector<file_change>&)> counted;
        std::function<bool()> stop;
    };
    std::expected<void, std::string> changes(const oid&, const change_steps&) const;
    // Any two revisions (commits, refs, trees), or one against the working
    // tree and index when `to` is "": Compare with working tree, Compare
    // revisions.
    std::expected<void, std::string> changes_between(const std::string& from, const std::string& to,
                                                     const change_steps&) const;
    // Short names: local branches, then remote branches, then tags.
    std::vector<std::string> branch_names(bool local, bool remote, bool tags) const;
    // A branch, tag or hash, peeled to its commit.
    std::optional<oid> resolve(const std::string& rev) const;
    // Whether `rev` names a commit, or with ".." or "..." in it, a range of
    // two that resolve (git rev-parse --verify rev^{commit}, rev-parse
    // a..b). False for syntax libgit2 doesn't know (git may).
    bool revision_exists(const std::string& rev) const;
    // Short name of a local branch's upstream ("origin/main"), if any.
    std::string upstream_of(const std::string& branch) const;
    // The same as a full name (refs/remotes/origin/main), as for-each-ref's
    // %(upstream) gives it: branch.<b>.merge through branch.<b>.remote's
    // fetch refspecs, whether or not that ref exists; "" for none. Git
    // answers when libgit2 can't read the config as git does.
    std::string upstream_ref(std::string_view branch) const;
    // upstream_ref through libgit2 alone: nullopt (where "" is none) when
    // it can't read the config as git does, and git must be asked.
    std::optional<std::string> upstream_in_process(std::string_view branch) const;
    // How a local branch tracks, from one read of its config, as git reads
    // it: branch.<b>.remote (the last given), branch.<b>.merge (the first
    // given, which @{u} and %(upstream) take) and how many .merge entries
    // there are, and the upstream they make. Git's config values when
    // libgit2 can't read the config as git does.
    struct tracking
    {
        std::string remote;    // "origin"; "." for a local branch; "" for none
        std::string merge;     // the branch there: refs/heads/main
        size_t merges = 0;     // .merge entries (git push refuses more than one)
        std::string upstream;  // refs/remotes/origin/main; "" when none maps
    };
    tracking tracking_of(std::string_view branch) const;

    // The refs whose full names start with one of `prefixes` (every ref
    // when it's empty), in name order, as git for-each-ref <prefix>/ lists
    // them: a symbolic ref by what it resolves to, `symref` naming it, and
    // a dangling one left out. nullopt when the refs can't be read.
    struct listed_ref
    {
        std::string full_name;
        oid target;
        std::string symref;
    };
    std::optional<std::vector<listed_ref>>
    refs_under(const std::vector<std::string>& prefixes) const;
    // The ref `name` means, as git rev-parse --symbolic-full-name has it
    // (main -> refs/heads/main, HEAD -> the branch it's on); "" when it's
    // no ref (a hash, HEAD~1).
    std::string full_ref_name(const std::string& name) const;
    // A ref's short name as for-each-ref's %(refname:short) gives it: main,
    // origin/main, v1; heads/v1 when a tag v1 would take "v1".
    std::string short_ref_name(const std::string& full) const;
    // git rev-list <include> ^<exclude> --max-count=<limit>: newest first by
    // commit time (0: no limit); empty when a commit can't be read.
    std::vector<oid> rev_list(const std::vector<oid>& include, const std::vector<oid>& exclude,
                              size_t limit = 0) const;
    // Commits in `local` but not `upstream`, and the other way round (git
    // rev-list --left-right --count local...upstream).
    std::optional<std::pair<size_t, size_t>> ahead_behind(const oid& local,
                                                          const oid& upstream) const;
    // Commits in branch but not in upstream, newest first (topological).
    std::expected<std::vector<commit>, std::string> range(const oid& upstream, const oid& branch,
                                                          bool include_merges) const;

    // One commit's fields, as a walk gives them; nullopt when it isn't here.
    std::optional<commit> lookup(const oid&) const;

    // The stashes, newest first, as git stash list has them: stash@{n}, the
    // stash commit, its first parent (the commit it was made on) and its
    // reflog subject (%gs: "WIP on main: 1a2b3c4 subject").
    struct stash_entry
    {
        std::string selector;
        oid id;
        std::optional<oid> base;
        std::string subject;
    };
    std::vector<stash_entry> stashes() const;
    // A commit's hash abbreviated as git's %h does: core.abbrev long, or
    // longer where that's ambiguous.
    std::string short_id(const oid&) const;
    // An annotated tag's message (for-each-ref's %(contents)); nullopt for
    // a lightweight tag or none.
    std::optional<std::string> tag_message(const std::string& tag) const;
    // git branch -a --contains <id>'s lines: "* main", "  topic",
    // "  remotes/origin/main", "  remotes/origin/HEAD -> origin/main".
    std::string branches_containing(const oid&) const;
    // Their best common ancestor (git merge-base a b); nullopt for
    // unrelated histories.
    std::optional<oid> merge_base(const oid& a, const oid& b) const;
    // True when `descendant` has `ancestor` in its history.
    bool is_ancestor(const oid& ancestor, const oid& descendant) const;
    // Cheap summary of changes() (no line counts): a mask of Action bits.
    uint8_t actions(const oid&) const;

    git_repository* raw() const { return repo_; }

private:
    std::expected<void, std::string> report_changes(git_diff*, const change_steps&) const;
    explicit repo(git_repository* r)
        : repo_(r)
    {
    }

    git_repository* repo_ = nullptr;
    // core.warnAmbiguousRefs as git read it, when libgit2 couldn't.
    mutable std::optional<bool> git_strict_refs_;
};

std::string last_git_error();

// Every local branch's upstream for a listing (the log's labels, Browse
// References, the sync): libgit2's, or when it can't read the config as
// git does, one git for-each-ref for them all. Unknown (nullopt) only when
// git can't answer either (it isn't installed): then a listing leaves the
// upstreams out and keeps the rest.
class upstream_list
{
public:
    explicit upstream_list(const repo&);
    ~upstream_list();
    upstream_list(const upstream_list&) = delete;
    upstream_list& operator=(const upstream_list&) = delete;
    // "" when the branch (short name) has none.
    std::optional<std::string> of(std::string_view branch);

private:
    struct impl;
    std::unique_ptr<impl> p_;
};

// The repository's config as git reads it, through libgit2 (a snapshot):
// nonzero, and *out null, when it holds what libgit2 doesn't evaluate as
// git does: an includeIf hasconfig: (libgit2 skips it, so what it sets
// reads as unset), or on Windows a gitdir: pattern written with
// backslashes. Config reads in process go through this, and ask git when
// it fails.
int config_snapshot(git_config** out, git_repository* r);

// A commit message's body, as git's %b: what follows the subject's
// paragraph and the blank lines after it.
std::string commit_body(std::string_view message);

}  // namespace tortilla
