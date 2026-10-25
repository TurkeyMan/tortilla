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
#include <vector>

struct git_repository;

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
};

// Whether a walk over `refs` starts at the ref named `full_name`.
bool walks_ref(walk_refs refs, std::string_view full_name);

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
    std::string head_name() const;  // branch short name, or "HEAD" when detached
    ref_map refs() const;           // labels sorted by full ref name, as TortoiseGit shows them
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
    // Short names: local branches, then remote branches, then tags.
    std::vector<std::string> branch_names(bool local, bool remote, bool tags) const;
    // A branch, tag or hash, peeled to its commit.
    std::optional<oid> resolve(const std::string& rev) const;
    // Short name of a local branch's upstream ("origin/main"), if any.
    std::string upstream_of(const std::string& branch) const;
    // Commits in branch but not in upstream, newest first (topological).
    std::expected<std::vector<commit>, std::string> range(const oid& upstream, const oid& branch,
                                                          bool include_merges) const;

    // True when `descendant` has `ancestor` in its history.
    bool is_ancestor(const oid& ancestor, const oid& descendant) const;
    // Cheap summary of changes() (no line counts): a mask of Action bits.
    uint8_t actions(const oid&) const;

    git_repository* raw() const { return repo_; }

private:
    explicit repo(git_repository* r)
        : repo_(r)
    {
    }

    git_repository* repo_ = nullptr;
};

std::string last_git_error();

}  // namespace tortilla
