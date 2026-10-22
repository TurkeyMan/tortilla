#include "repo.h"

#include "command.h"

#include <git2.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <utility>

namespace tortilla
{
namespace
{

void initLibgit2()
{
    static const int once = git_libgit2_init();
    (void)once;
}

Oid toOid(const git_oid* o)
{
    Oid out;
    std::memcpy(out.bytes.data(), o->id, out.bytes.size());
    return out;
}

git_oid toGit(const Oid& o)
{
    git_oid out{};
    std::memcpy(out.id, o.bytes.data(), o.bytes.size());
    return out;
}

template <class T, void (*Free)(T*)>
struct Owned
{
    T* p = nullptr;
    ~Owned()
    {
        if (p) Free(p);
    }
};

using OwnedRef = Owned<git_reference, git_reference_free>;
using OwnedObject = Owned<git_object, git_object_free>;
using OwnedIter = Owned<git_reference_iterator, git_reference_iterator_free>;
using OwnedWalk = Owned<git_revwalk, git_revwalk_free>;
using OwnedCommit = Owned<git_commit, git_commit_free>;
using OwnedTree = Owned<git_tree, git_tree_free>;
using OwnedDiff = Owned<git_diff, git_diff_free>;
using OwnedPatch = Owned<git_patch, git_patch_free>;

}  // namespace

std::string Oid::hex(size_t length) const
{
    git_oid o = toGit(*this);
    char buf[GIT_OID_MAX_HEXSIZE + 1];
    git_oid_tostr(buf, sizeof buf, &o);
    return std::string(buf, std::min(length, std::strlen(buf)));
}

std::string lastGitError()
{
    const git_error* e = git_error_last();
    return e && e->message ? e->message : "unknown libgit2 error";
}

std::expected<Repo, std::string> Repo::open(const std::string& path)
{
    initLibgit2();
    git_repository* r = nullptr;
    if (git_repository_open_ext(&r, path.c_str(), 0, nullptr) != 0)
        return std::unexpected(lastGitError());
    return Repo(r);
}

Repo::Repo(Repo&& o) noexcept
    : repo_(std::exchange(o.repo_, nullptr))
{
}

Repo& Repo::operator=(Repo&& o) noexcept
{
    std::swap(repo_, o.repo_);
    return *this;
}

Repo::~Repo()
{
    if (repo_) git_repository_free(repo_);
}

std::string Repo::workdir() const
{
    const char* w = git_repository_workdir(repo_);
    return w ? w : git_repository_path(repo_);
}

std::string Repo::gitDir() const
{
    return git_repository_path(repo_);
}

std::string Repo::headName() const
{
    OwnedRef head;
    if (git_repository_head(&head.p, repo_) != 0) return "HEAD";
    if (git_reference_is_branch(head.p)) return git_reference_shorthand(head.p);
    return "HEAD";
}

// One `git for-each-ref` gives every label with its commit, annotated-ness,
// upstream and the checked-out branch. It is ~2x faster than libgit2's ref
// iteration on slow filesystems, and replaces a config parse per branch
// for upstreams (together ~750 ms -> ~170 ms on a 250-ref repo over WSL).
RefMap Repo::refs() const
{
    RefMap out;
    GitResult r =
        runGit(workdir(),
               {"for-each-ref", "--format=%(objectname)%09%(objecttype)%09%(*objectname)%09"
                                "%(refname)%09%(upstream)%09%(HEAD)"},
               GitOutput::Stdout);
    if (!r.ok()) return out;

    for (size_t at = 0; at < r.output.size();)
    {
        size_t nl = r.output.find('\n', at);
        if (nl == std::string::npos) nl = r.output.size();
        std::string_view line(r.output.data() + at, nl - at);
        at = nl + 1;

        std::vector<std::string_view> f;
        for (size_t p = 0;;)
        {
            size_t tab = line.find('\t', p);
            f.push_back(
                line.substr(p, tab == std::string_view::npos ? std::string_view::npos : tab - p));
            if (tab == std::string_view::npos) break;
            p = tab + 1;
        }
        if (f.size() < 6) continue;
        std::string_view object = f[0], type = f[1], peeled = f[2], full = f[3];

        Ref ref;
        ref.fullName = full;
        ref.kind = RefKind::Other;
        auto strip = [&](std::string_view prefix)
        { return std::string(full.substr(prefix.size())); };
        if (full.starts_with("refs/heads/"))
        {
            ref.kind = RefKind::LocalBranch;
            ref.name = strip("refs/heads/");
        }
        else if (full.starts_with("refs/remotes/"))
        {
            if (full.ends_with("/HEAD")) continue;
            ref.kind = RefKind::RemoteBranch;
            ref.name = strip("refs/remotes/");
        }
        else if (full.starts_with("refs/tags/"))
        {
            ref.kind = RefKind::Tag;
            ref.name = strip("refs/tags/");
        }
        else if (full == "refs/stash")
        {
            ref.kind = RefKind::Stash;
            ref.name = "stash";
        }
        else
            ref.name = full.starts_with("refs/") ? strip("refs/") : std::string(full);
        ref.upstream = f[4];
        ref.current = f[5] == "*";

        // Annotated tags label the commit underneath; refs to trees or
        // blobs aren't shown.
        std::string_view commit = object;
        if (type == "tag")
        {
            ref.annotated = true;
            commit = peeled;
        }
        else if (type != "commit")
            continue;
        git_oid id;
        if (git_oid_fromstrn(&id, commit.data(), commit.size()) != 0) continue;
        out[toOid(&id)].push_back(std::move(ref));
    }

    // A detached HEAD shows as a label too.
    git_oid head;
    if (git_repository_head_detached(repo_) == 1 &&
        git_reference_name_to_id(&head, repo_, "HEAD") == 0)
        out[toOid(&head)].push_back({"HEAD", "HEAD", RefKind::Head, true});

    for (auto& [id, refs] : out) std::ranges::sort(refs, {}, &Ref::fullName);
    return out;
}

std::optional<Oid> Repo::headId() const
{
    git_oid id;
    if (git_reference_name_to_id(&id, repo_, "HEAD") != 0) return std::nullopt;
    return toOid(&id);
}

bool Repo::bare() const
{
    return git_repository_is_bare(repo_);
}

// git's --topo-order, which TortoiseGit uses [spec 1.4]. libgit2's
// topological sort is a different (valid) order that, with many branches,
// shows tips far ahead of their history and leaves lanes open for long
// stretches. git's algorithm [commit.c sort_in_topological_order]: walk
// everything newest first, count each commit's children, then pop from a
// stack seeded with the childless tips (newest on top), pushing a parent
// once all its children are out. Parents are pushed in order, so a merge's
// second parent comes next: merged branches stay together under the merge.
std::expected<void, std::string> Repo::walk(const WalkOptions& opts,
                                            const std::function<bool(Commit&&)>& sink) const
{
    OwnedWalk w;
    if (git_revwalk_new(&w.p, repo_) != 0) return std::unexpected(lastGitError());
    git_revwalk_sorting(w.p, GIT_SORT_TIME);

    if (opts.allBranches && !opts.tips.empty())
    {
        for (const Oid& tip : opts.tips)
        {
            git_oid t = toGit(tip);
            git_revwalk_push(w.p, &t);
        }
    }
    else if (opts.allBranches)
    {
        // Every ref, like git log --all (branches, remotes, tags, stash...).
        OwnedIter it;
        if (git_reference_iterator_new(&it.p, repo_) == 0)
        {
            git_reference* raw;
            while (git_reference_next(&raw, it.p) == 0)
            {
                OwnedRef ref{raw};
                OwnedObject target;
                if (git_reference_peel(&target.p, raw, GIT_OBJECT_COMMIT) == 0)
                    git_revwalk_push(w.p, git_object_id(target.p));
            }
        }
    }
    // Pushing HEAD fails in an empty repository; that's just "no commits".
    git_revwalk_push_head(w.p);
    if (opts.allBranches)
    {
        // --all also covers linked worktrees' HEADs; detached ones can be
        // the only way to reach a commit. Their HEAD files sit in the main
        // .git even when the worktree itself is gone or on another OS.
        git_strarray names{};
        if (git_worktree_list(&names, repo_) == 0)
        {
            for (size_t i = 0; i < names.count; ++i)
            {
                std::string path = gitDir() + "worktrees/" + names.strings[i] + "/HEAD";
                if (std::FILE* f = std::fopen(path.c_str(), "r"))
                {
                    char line[128] = {};
                    if (std::fgets(line, sizeof line, f))
                    {
                        git_oid head;
                        if (git_oid_fromstrn(&head, line, GIT_OID_SHA1_HEXSIZE) == 0)
                            git_revwalk_push(w.p, &head);
                    }
                    std::fclose(f);
                }
            }
            git_strarray_dispose(&names);
        }
    }
    if (opts.firstParent) git_revwalk_simplify_first_parent(w.p);

    std::vector<Commit> all;
    std::unordered_map<Oid, size_t, OidHash> index;
    git_oid id;
    while (git_revwalk_next(&id, w.p) == 0)
    {
        OwnedCommit c;
        if (git_commit_lookup(&c.p, repo_, &id) != 0) continue;

        unsigned n = git_commit_parentcount(c.p);
        Commit out;
        out.id = toOid(&id);
        out.parents.reserve(n);
        for (unsigned i = 0; i < n; ++i) out.parents.push_back(toOid(git_commit_parent_id(c.p, i)));
        if (opts.firstParent && n > 1) out.parents.resize(1);
        if (const char* s = git_commit_summary(c.p)) out.summary = s;
        if (const char* m = git_commit_message(c.p)) out.message = m;
        if (const git_signature* author = git_commit_author(c.p))
        {
            out.author = author->name ? author->name : "";
            out.email = author->email ? author->email : "";
            out.time = author->when.time;
            out.offsetMinutes = author->when.offset;
        }
        if (const git_signature* cm = git_commit_committer(c.p))
        {
            out.committer = cm->name ? cm->name : "";
            out.committerEmail = cm->email ? cm->email : "";
            out.commitTime = cm->when.time;
            out.commitOffsetMinutes = cm->when.offset;
        }
        index[out.id] = all.size();
        all.push_back(std::move(out));
    }

    std::vector<unsigned> children(all.size(), 0);
    for (auto& c : all)
        for (auto& p : c.parents)
            if (auto it = index.find(p); it != index.end()) ++children[it->second];

    std::vector<size_t> stack;
    for (size_t i = all.size(); i-- > 0;)
        if (children[i] == 0) stack.push_back(i);  // newest tip ends up on top

    while (!stack.empty())
    {
        size_t i = stack.back();
        stack.pop_back();
        for (auto& p : all[i].parents)
            if (auto it = index.find(p); it != index.end() && --children[it->second] == 0)
                stack.push_back(it->second);

        Commit& c = all[i];
        if (opts.noMerges && c.parents.size() > 1) continue;
        if ((opts.since && c.time < *opts.since) || (opts.until && c.time > *opts.until)) continue;
        if (!sink(std::move(c))) break;
    }
    return {};
}

std::expected<CommitDetails, std::string> Repo::details(const Oid& id) const
{
    git_oid gid = toGit(id);
    OwnedCommit c;
    if (git_commit_lookup(&c.p, repo_, &gid) != 0) return std::unexpected(lastGitError());

    CommitDetails d;
    if (const char* m = git_commit_message(c.p)) d.message = m;
    if (const git_signature* s = git_commit_committer(c.p))
    {
        d.committer = s->name ? s->name : "";
        d.committerEmail = s->email ? s->email : "";
        d.commitTime = s->when.time;
        d.commitOffsetMinutes = s->when.offset;
    }
    return d;
}

std::expected<std::vector<FileChange>, std::string> Repo::changes(const Oid& id) const
{
    std::vector<FileChange> out;
    auto r = changes(id, {.listed = [&](const std::vector<FileChange>& files) { out = files; },
                          .counted =
                              [&](size_t first, const std::vector<FileChange>& files)
                          { std::ranges::copy(files, out.begin() + std::ptrdiff_t(first)); }});
    if (!r) return std::unexpected(r.error());
    return out;
}

std::expected<void, std::string> Repo::changes(const Oid& id, const ChangeSteps& steps) const
{
    git_oid gid = toGit(id);
    OwnedCommit c, parent;
    OwnedTree tree, parentTree;
    if (git_commit_lookup(&c.p, repo_, &gid) != 0 || git_commit_tree(&tree.p, c.p) != 0)
        return std::unexpected(lastGitError());
    if (git_commit_parentcount(c.p) > 0)
    {
        if (git_commit_parent(&parent.p, c.p, 0) != 0 ||
            git_commit_tree(&parentTree.p, parent.p) != 0)
            return std::unexpected(lastGitError());
    }

    OwnedDiff diff;
    if (git_diff_tree_to_tree(&diff.p, repo_, parentTree.p, tree.p, nullptr) != 0)
        return std::unexpected(lastGitError());
    git_diff_find_options find = GIT_DIFF_FIND_OPTIONS_INIT;
    find.flags = GIT_DIFF_FIND_RENAMES;
    git_diff_find_similar(diff.p, &find);

    // The files first: the tree diff alone, no file contents.
    const size_t n = git_diff_num_deltas(diff.p);
    std::vector<FileChange> files;
    files.reserve(n);
    for (size_t i = 0; i < n; ++i)
    {
        const git_diff_delta* d = git_diff_get_delta(diff.p, i);
        FileChange f;
        f.path = d->new_file.path;
        if (d->status == GIT_DELTA_RENAMED || d->status == GIT_DELTA_COPIED)
            f.oldPath = d->old_file.path;
        f.status = git_diff_status_char(d->status);
        f.binary = d->flags & GIT_DIFF_FLAG_BINARY;
        f.counted = false;
        files.push_back(std::move(f));
    }
    if (steps.listed) steps.listed(files);

    // Then the line counts, which read both sides of every file: batches
    // small enough to show progress, cheap enough not to flood the UI.
    constexpr size_t kBatch = 256;
    for (size_t first = 0; first < n; first += kBatch)
    {
        const size_t last = std::min(n, first + kBatch);
        for (size_t i = first; i < last; ++i)
        {
            if (steps.stop && steps.stop()) return {};
            FileChange& f = files[i];
            OwnedPatch patch;
            if (git_patch_from_diff(&patch.p, diff.p, i) == 0 && patch.p)
            {
                size_t context;
                git_patch_line_stats(&context, &f.added, &f.removed, patch.p);
                f.binary = f.binary || (git_patch_get_delta(patch.p)->flags & GIT_DIFF_FLAG_BINARY);
            }
            f.counted = true;
        }
        if (steps.counted)
            steps.counted(first, std::vector<FileChange>(files.begin() + std::ptrdiff_t(first),
                                                         files.begin() + std::ptrdiff_t(last)));
    }
    return {};
}

namespace
{

uint8_t actionBit(git_delta_t status)
{
    switch (status)
    {
    case GIT_DELTA_ADDED:
    case GIT_DELTA_COPIED:
    case GIT_DELTA_UNTRACKED: return ActionAdded;
    case GIT_DELTA_DELETED: return ActionDeleted;
    case GIT_DELTA_RENAMED: return ActionRenamed;
    case GIT_DELTA_CONFLICTED: return ActionConflicted;
    default: return ActionModified;
    }
}

}  // namespace

uint8_t Repo::actions(const Oid& id) const
{
    git_oid gid = toGit(id);
    OwnedCommit c, parent;
    OwnedTree tree, parentTree;
    if (git_commit_lookup(&c.p, repo_, &gid) != 0 || git_commit_tree(&tree.p, c.p) != 0) return 0;
    if (git_commit_parentcount(c.p) > 0 && (git_commit_parent(&parent.p, c.p, 0) != 0 ||
                                            git_commit_tree(&parentTree.p, parent.p) != 0))
        return 0;

    OwnedDiff diff;
    if (git_diff_tree_to_tree(&diff.p, repo_, parentTree.p, tree.p, nullptr) != 0) return 0;
    git_diff_find_options find = GIT_DIFF_FIND_OPTIONS_INIT;
    find.flags = GIT_DIFF_FIND_RENAMES;
    git_diff_find_similar(diff.p, &find);

    uint8_t mask = 0;
    for (size_t i = 0, n = git_diff_num_deltas(diff.p); i < n; ++i)
        mask |= actionBit(git_diff_get_delta(diff.p, i)->status);
    return mask;
}

std::expected<std::vector<FileChange>, std::string>
Repo::workingTreeChanges(const std::atomic<bool>* stop) const
{
    OwnedObject headTree;
    // An unborn branch has no HEAD tree; diff against the empty tree then.
    git_revparse_single(&headTree.p, repo_, "HEAD^{tree}");

    git_diff_options opts = GIT_DIFF_OPTIONS_INIT;
    opts.flags = GIT_DIFF_INCLUDE_UNTRACKED;
    // Called before each file comparison; non-zero aborts the diff.
    opts.progress_cb = +[](const git_diff*, const char*, const char*, void* p) -> int
    {
        auto* stop = static_cast<const std::atomic<bool>*>(p);
        return stop && *stop ? GIT_EUSER : 0;
    };
    opts.payload = const_cast<std::atomic<bool>*>(stop);
    OwnedDiff diff;
    if (git_diff_tree_to_workdir_with_index(&diff.p, repo_, reinterpret_cast<git_tree*>(headTree.p),
                                            &opts) != 0)
        return std::unexpected(stop && *stop ? std::string("cancelled") : lastGitError());
    git_diff_find_options find = GIT_DIFF_FIND_OPTIONS_INIT;
    find.flags = GIT_DIFF_FIND_RENAMES;
    git_diff_find_similar(diff.p, &find);

    std::vector<FileChange> out;
    for (size_t i = 0, n = git_diff_num_deltas(diff.p); i < n; ++i)
    {
        if (stop && *stop) return std::unexpected("cancelled");
        const git_diff_delta* d = git_diff_get_delta(diff.p, i);
        FileChange f;
        f.path = d->new_file.path;
        if (d->status == GIT_DELTA_RENAMED) f.oldPath = d->old_file.path;
        f.status = d->status == GIT_DELTA_UNTRACKED ? '?' : git_diff_status_char(d->status);
        // Line counts read both sides of the file; untracked files and
        // directories have nothing to count against.
        OwnedPatch patch;
        if (d->status != GIT_DELTA_UNTRACKED && git_patch_from_diff(&patch.p, diff.p, i) == 0 &&
            patch.p)
        {
            size_t context;
            git_patch_line_stats(&context, &f.added, &f.removed, patch.p);
            f.binary = git_patch_get_delta(patch.p)->flags & GIT_DIFF_FLAG_BINARY;
        }
        out.push_back(std::move(f));
    }
    return out;
}

bool Repo::isAncestor(const Oid& ancestor, const Oid& descendant) const
{
    git_oid a = toGit(ancestor), d = toGit(descendant);
    return git_oid_equal(&a, &d) || git_graph_descendant_of(repo_, &d, &a) == 1;
}

Repo::State Repo::state() const
{
    switch (git_repository_state(repo_))
    {
    case GIT_REPOSITORY_STATE_NONE: return State::None;
    case GIT_REPOSITORY_STATE_MERGE: return State::Merge;
    case GIT_REPOSITORY_STATE_REVERT:
    case GIT_REPOSITORY_STATE_REVERT_SEQUENCE: return State::Revert;
    case GIT_REPOSITORY_STATE_CHERRYPICK:
    case GIT_REPOSITORY_STATE_CHERRYPICK_SEQUENCE: return State::CherryPick;
    case GIT_REPOSITORY_STATE_BISECT: return State::Bisect;
    case GIT_REPOSITORY_STATE_APPLY_MAILBOX:
    case GIT_REPOSITORY_STATE_APPLY_MAILBOX_OR_REBASE: return State::ApplyMailbox;
    default: return State::Rebase;
    }
}

std::vector<std::string> Repo::branchNames(bool local, bool remote, bool tags) const
{
    std::vector<std::string> locals, remotes, tagNames;
    OwnedIter it;
    if (git_reference_iterator_new(&it.p, repo_) != 0) return {};
    git_reference* raw;
    while (git_reference_next(&raw, it.p) == 0)
    {
        OwnedRef ref{raw};
        std::string_view full = git_reference_name(raw);
        std::string name = git_reference_shorthand(raw);
        if (local && git_reference_is_branch(raw))
            locals.push_back(name);
        else if (remote && git_reference_is_remote(raw) && !full.ends_with("/HEAD"))
            remotes.push_back(name);
        else if (tags && git_reference_is_tag(raw))
            tagNames.push_back(name);
    }
    for (auto* v : {&locals, &remotes, &tagNames}) std::ranges::sort(*v);
    locals.insert(locals.end(), remotes.begin(), remotes.end());
    locals.insert(locals.end(), tagNames.begin(), tagNames.end());
    return locals;
}

std::optional<Oid> Repo::resolve(const std::string& rev) const
{
    OwnedObject obj;
    if (git_revparse_single(&obj.p, repo_, (rev + "^{commit}").c_str()) != 0) return std::nullopt;
    return toOid(git_object_id(obj.p));
}

std::string Repo::upstreamOf(const std::string& branch) const
{
    git_buf up = GIT_BUF_INIT;
    std::string out;
    if (git_branch_upstream_name(&up, repo_, ("refs/heads/" + branch).c_str()) == 0)
    {
        out = up.ptr;
        for (const char* prefix : {"refs/remotes/", "refs/heads/"})
            if (out.starts_with(prefix)) out.erase(0, std::strlen(prefix));
    }
    git_buf_dispose(&up);
    return out;
}

std::expected<std::vector<Commit>, std::string> Repo::range(const Oid& upstream, const Oid& branch,
                                                            bool includeMerges) const
{
    OwnedWalk w;
    if (git_revwalk_new(&w.p, repo_) != 0) return std::unexpected(lastGitError());
    git_revwalk_sorting(w.p, GIT_SORT_TOPOLOGICAL);
    git_oid b = toGit(branch), u = toGit(upstream);
    if (git_revwalk_push(w.p, &b) != 0 || git_revwalk_hide(w.p, &u) != 0)
        return std::unexpected(lastGitError());

    std::vector<Commit> out;
    git_oid id;
    while (git_revwalk_next(&id, w.p) == 0)
    {
        OwnedCommit c;
        if (git_commit_lookup(&c.p, repo_, &id) != 0) continue;
        unsigned n = git_commit_parentcount(c.p);
        if (!includeMerges && n > 1) continue;
        Commit out1;
        out1.id = toOid(&id);
        for (unsigned i = 0; i < n; ++i)
            out1.parents.push_back(toOid(git_commit_parent_id(c.p, i)));
        if (const char* s = git_commit_summary(c.p)) out1.summary = s;
        if (const char* m = git_commit_message(c.p)) out1.message = m;
        if (const git_signature* a = git_commit_author(c.p))
        {
            out1.author = a->name ? a->name : "";
            out1.email = a->email ? a->email : "";
            out1.time = a->when.time;
            out1.offsetMinutes = a->when.offset;
        }
        out.push_back(std::move(out1));
    }
    return out;
}

}  // namespace tortilla
