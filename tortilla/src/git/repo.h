// Thin libgit2 wrapper: repository discovery, refs and the revision walk.
// A Repo is not thread-safe; worker threads open their own.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

struct git_repository;

namespace tortilla
{

struct Oid
{
    std::array<uint8_t, 20> bytes{};

    bool operator==(const Oid&) const = default;
    std::string hex(size_t length = 40) const;
};

struct OidHash
{
    size_t operator()(const Oid& o) const
    {
        size_t h;
        std::memcpy(&h, o.bytes.data(), sizeof h);
        return h;
    }
};

enum class RefKind
{
    Head,  // detached HEAD
    LocalBranch,
    RemoteBranch,
    Tag,
    Stash,
    Other,
};

struct Ref
{
    std::string name;      // short name, e.g. "main", "origin/main", "v1.0"
    std::string fullName;  // e.g. "refs/heads/main"
    RefKind kind;
    bool current = false;    // the branch HEAD points at
    bool annotated = false;  // annotated tag
    std::string upstream;    // local branch: full name of its upstream, if any
};

using RefMap = std::unordered_map<Oid, std::vector<Ref>, OidHash>;

struct Commit
{
    Oid id;
    std::vector<Oid> parents;
    std::string summary;
    std::string message;  // full message, for searching
    std::string author, email;
    int64_t time = 0;       // author time, seconds since epoch
    int offsetMinutes = 0;  // author's UTC offset
    std::string committer, committerEmail;
    int64_t commitTime = 0;
    int commitOffsetMinutes = 0;
};

struct CommitDetails
{
    std::string message;  // full message
    std::string committer, committerEmail;
    int64_t commitTime = 0;
    int commitOffsetMinutes = 0;
};

// Which kinds of change a commit makes, for the log's Actions column.
enum Action : uint8_t
{
    ActionModified = 1,
    ActionAdded = 2,
    ActionDeleted = 4,
    ActionRenamed = 8,
    ActionConflicted = 16,
};

struct FileChange
{
    std::string path, oldPath;  // oldPath set for renames and copies
    char status = 'M';          // A, D, M, R, C, T
    size_t added = 0, removed = 0;
    bool binary = false;
    bool counted = true;  // false: added/removed not known yet
};

struct WalkOptions
{
    bool allBranches = false;             // every local/remote branch and tag, else HEAD only
    bool firstParent = false;             // --first-parent
    bool noMerges = false;                // --no-merges
    std::optional<int64_t> since, until;  // author time bounds, inclusive
    // With allBranches: the ref tips to start from, when the caller already
    // has them (from refs()); empty = list the refs here.
    std::vector<Oid> tips;
};

class Repo
{
public:
    // Finds the repository containing `path` (walks up like git does).
    static std::expected<Repo, std::string> open(const std::string& path);

    Repo(Repo&&) noexcept;
    Repo& operator=(Repo&&) noexcept;
    ~Repo();

    std::string workdir() const;
    std::string gitDir() const;    // the .git directory
    std::string headName() const;  // branch short name, or "HEAD" when detached
    RefMap refs() const;           // labels sorted by full ref name, as TortoiseGit shows them
    std::optional<Oid> headId() const;
    bool bare() const;

    // An operation git is in the middle of (MERGE_HEAD, rebase-merge, ...).
    enum class State
    {
        None,
        Merge,
        Revert,
        CherryPick,
        Bisect,
        Rebase,
        ApplyMailbox,
    };
    State state() const;

    // Topological + time order, newest first. `sink` returns false to stop.
    std::expected<void, std::string> walk(const WalkOptions&,
                                          const std::function<bool(Commit&&)>& sink) const;

    std::expected<CommitDetails, std::string> details(const Oid&) const;
    // Changes against the first parent (or the empty tree for a root commit).
    std::expected<std::vector<FileChange>, std::string> changes(const Oid&) const;
    // The same in two steps, for commits that may be huge: `listed` gets the
    // files at once (without line counts, cheap), then `counted` gets them
    // again in batches as their lines are counted, which reads every file.
    // `stop` is asked between files; true abandons the counting.
    struct ChangeSteps
    {
        std::function<void(const std::vector<FileChange>&)> listed;
        std::function<void(size_t first, const std::vector<FileChange>&)> counted;
        std::function<bool()> stop;
    };
    std::expected<void, std::string> changes(const Oid&, const ChangeSteps&) const;
    // Short names: local branches, then remote branches, then tags.
    std::vector<std::string> branchNames(bool local, bool remote, bool tags) const;
    // A branch, tag or hash, peeled to its commit.
    std::optional<Oid> resolve(const std::string& rev) const;
    // Short name of a local branch's upstream ("origin/main"), if any.
    std::string upstreamOf(const std::string& branch) const;
    // Commits in branch but not in upstream, newest first (topological).
    std::expected<std::vector<Commit>, std::string> range(const Oid& upstream, const Oid& branch,
                                                          bool includeMerges) const;

    // True when `descendant` has `ancestor` in its history.
    bool isAncestor(const Oid& ancestor, const Oid& descendant) const;
    // Cheap summary of changes() (no line counts): a mask of Action bits.
    uint8_t actions(const Oid&) const;
    // HEAD against the index and working tree. Untracked directories are
    // listed as one entry each (not recursed: build trees can hold tens of
    // thousands of files). `stop` aborts it from another thread.
    std::expected<std::vector<FileChange>, std::string>
    workingTreeChanges(const std::atomic<bool>* stop = nullptr) const;

    git_repository* raw() const { return repo_; }

private:
    explicit Repo(git_repository* r)
        : repo_(r)
    {
    }

    git_repository* repo_ = nullptr;
};

std::string lastGitError();

}  // namespace tortilla
