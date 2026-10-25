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

void init_libgit2()
{
    static const int once = git_libgit2_init();
    (void)once;
}

oid to_oid(const git_oid* o)
{
    oid out;
    std::memcpy(out.bytes.data(), o->id, out.bytes.size());
    return out;
}

git_oid to_git(const oid& o)
{
    git_oid out{};
    std::memcpy(out.id, o.bytes.data(), o.bytes.size());
    return out;
}

template <class t, void (*Free)(t*)>
struct owned
{
    t* p = nullptr;
    ~owned()
    {
        if (p) Free(p);
    }
};

using owned_ref = owned<git_reference, git_reference_free>;
using owned_object = owned<git_object, git_object_free>;
using owned_iter = owned<git_reference_iterator, git_reference_iterator_free>;
using owned_walk = owned<git_revwalk, git_revwalk_free>;
using owned_commit = owned<git_commit, git_commit_free>;
using owned_tree = owned<git_tree, git_tree_free>;
using owned_diff = owned<git_diff, git_diff_free>;
using owned_patch = owned<git_patch, git_patch_free>;

}  // namespace

std::string oid::hex(size_t length) const
{
    git_oid o = to_git(*this);
    char buf[GIT_OID_MAX_HEXSIZE + 1];
    git_oid_tostr(buf, sizeof buf, &o);
    return std::string(buf, std::min(length, std::strlen(buf)));
}

std::string last_git_error()
{
    const git_error* e = git_error_last();
    return e && e->message ? e->message : "unknown libgit2 error";
}

std::expected<repo, std::string> repo::open(const std::string& path)
{
    init_libgit2();
    git_repository* r = nullptr;
    if (git_repository_open_ext(&r, path.c_str(), 0, nullptr) != 0)
        return std::unexpected(last_git_error());
    return repo(r);
}

repo::repo(repo&& o) noexcept
    : repo_(std::exchange(o.repo_, nullptr))
{
}

repo& repo::operator=(repo&& o) noexcept
{
    std::swap(repo_, o.repo_);
    return *this;
}

repo::~repo()
{
    if (repo_) git_repository_free(repo_);
}

std::string repo::workdir() const
{
    const char* w = git_repository_workdir(repo_);
    return w ? w : git_repository_path(repo_);
}

std::string repo::git_dir() const
{
    return git_repository_path(repo_);
}

std::string repo::head_name() const
{
    owned_ref head;
    if (git_repository_head(&head.p, repo_) != 0) return "HEAD";
    if (git_reference_is_branch(head.p)) return git_reference_shorthand(head.p);
    return "HEAD";
}

// One `git for-each-ref` gives every label with its commit, annotated-ness,
// upstream and the checked-out branch. It is ~2x faster than libgit2's ref
// iteration on slow filesystems, and replaces a config parse per branch
// for upstreams (together ~750 ms -> ~170 ms on a 250-ref repo over WSL).
ref_map repo::refs() const
{
    ref_map out;
    git_result r =
        run_git(workdir(),
               {"for-each-ref", "--format=%(objectname)%09%(objecttype)%09%(*objectname)%09"
                                "%(refname)%09%(upstream)%09%(HEAD)"},
               git_output::stdout_only);
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

        ref ref;
        ref.full_name = full;
        ref.kind = ref_kind::other;
        auto strip = [&](std::string_view prefix)
        { return std::string(full.substr(prefix.size())); };
        if (full.starts_with("refs/heads/"))
        {
            ref.kind = ref_kind::local_branch;
            ref.name = strip("refs/heads/");
        }
        else if (full.starts_with("refs/remotes/"))
        {
            if (full.ends_with("/HEAD")) continue;
            ref.kind = ref_kind::remote_branch;
            ref.name = strip("refs/remotes/");
        }
        else if (full.starts_with("refs/tags/"))
        {
            ref.kind = ref_kind::tag;
            ref.name = strip("refs/tags/");
        }
        else if (full == "refs/stash")
        {
            ref.kind = ref_kind::stash;
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
        out[to_oid(&id)].push_back(std::move(ref));
    }

    // A detached HEAD shows as a label too.
    git_oid head;
    if (git_repository_head_detached(repo_) == 1 &&
        git_reference_name_to_id(&head, repo_, "HEAD") == 0)
        out[to_oid(&head)].push_back({"HEAD", "HEAD", ref_kind::head, true});

    for (auto& [id, refs] : out) std::ranges::sort(refs, {}, &ref::full_name);
    return out;
}

std::optional<oid> repo::head_id() const
{
    git_oid id;
    if (git_reference_name_to_id(&id, repo_, "HEAD") != 0) return std::nullopt;
    return to_oid(&id);
}

bool repo::bare() const
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
bool walks_ref(walk_refs refs, std::string_view full_name)
{
    switch (refs)
    {
    case walk_refs::all: return true;
    case walk_refs::basic:
        return full_name.starts_with("refs/heads/") || full_name.starts_with("refs/remotes/") ||
               full_name.starts_with("refs/tags/");
    case walk_refs::local: return full_name.starts_with("refs/heads/");
    case walk_refs::head: break;
    }
    return false;
}

std::expected<void, std::string> repo::walk(const walk_options& opts,
                                            const std::function<bool(commit&&)>& sink) const
{
    owned_walk w;
    if (git_revwalk_new(&w.p, repo_) != 0) return std::unexpected(last_git_error());
    git_revwalk_sorting(w.p, GIT_SORT_TIME);

    const bool many = opts.refs != walk_refs::head;
    if (many && !opts.tips.empty())
    {
        for (const oid& tip : opts.tips)
        {
            git_oid t = to_git(tip);
            git_revwalk_push(w.p, &t);
        }
    }
    else if (many)
    {
        // Every ref in scope; for all, like git log --all (branches,
        // remotes, tags, stash...).
        owned_iter it;
        if (git_reference_iterator_new(&it.p, repo_) == 0)
        {
            git_reference* raw;
            while (git_reference_next(&raw, it.p) == 0)
            {
                owned_ref ref{raw};
                owned_object target;
                if (walks_ref(opts.refs, git_reference_name(raw)) &&
                    git_reference_peel(&target.p, raw, GIT_OBJECT_COMMIT) == 0)
                    git_revwalk_push(w.p, git_object_id(target.p));
            }
        }
    }
    if (!many && !opts.rev.empty())
    {
        owned_object target;
        if (git_revparse_single(&target.p, repo_, opts.rev.c_str()) != 0)
            return std::unexpected(last_git_error());
        owned_object commit;
        if (git_object_peel(&commit.p, target.p, GIT_OBJECT_COMMIT) != 0)
            return std::unexpected(last_git_error());
        git_revwalk_push(w.p, git_object_id(commit.p));
    }
    else
    {
        // Pushing HEAD fails in an empty repository; that's just "no commits".
        git_revwalk_push_head(w.p);
    }
    if (opts.refs == walk_refs::all)
    {
        // --all also covers linked worktrees' HEADs; detached ones can be
        // the only way to reach a commit. Their HEAD files sit in the main
        // .git even when the worktree itself is gone or on another OS.
        git_strarray names{};
        if (git_worktree_list(&names, repo_) == 0)
        {
            for (size_t i = 0; i < names.count; ++i)
            {
                std::string path = git_dir() + "worktrees/" + names.strings[i] + "/HEAD";
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
    if (opts.first_parent) git_revwalk_simplify_first_parent(w.p);

    std::vector<commit> all;
    std::unordered_map<oid, size_t, oid_hash> index;
    // A commit's fields; `parents` as the walk found them.
    auto add = [&](const owned_commit& c, std::vector<oid> parents)
    {
        commit out;
        out.id = to_oid(git_commit_id(c.p));
        out.parents = std::move(parents);
        if (const char* s = git_commit_summary(c.p)) out.summary = s;
        if (const char* m = git_commit_message(c.p)) out.message = m;
        if (const git_signature* author = git_commit_author(c.p))
        {
            out.author = author->name ? author->name : "";
            out.email = author->email ? author->email : "";
            out.time = author->when.time;
            out.offset_minutes = author->when.offset;
        }
        if (const git_signature* cm = git_commit_committer(c.p))
        {
            out.committer = cm->name ? cm->name : "";
            out.committer_email = cm->email ? cm->email : "";
            out.commit_time = cm->when.time;
            out.commit_offset_minutes = cm->when.offset;
        }
        index[out.id] = all.size();
        all.push_back(std::move(out));
    };

    if (!opts.paths.empty())
    {
        // Limited to paths: git's history simplification, which libgit2
        // lacks. rev-list --parents gives each commit that changed them with
        // its parents rewritten to the nearest such, so the graph joins up.
        std::vector<std::string> args{"rev-list", "--parents"};
        if (opts.first_parent) args.push_back("--first-parent");
        if (many && !opts.tips.empty())
            for (const oid& tip : opts.tips) args.push_back(tip.hex());
        else if (opts.refs == walk_refs::all)
            args.push_back("--all");
        else if (opts.refs == walk_refs::basic)
            args.insert(args.end(), {"--branches", "--remotes", "--tags"});
        else if (opts.refs == walk_refs::local)
            args.push_back("--branches");
        else
            args.push_back(opts.rev.empty() ? std::string("HEAD") : opts.rev);
        args.push_back("--");
        args.insert(args.end(), opts.paths.begin(), opts.paths.end());
        const git_result r =
            run_git(workdir().empty() ? git_dir() : workdir(), args, git_output::stdout_only);
        // An empty repository has no HEAD: no commits, as below.
        if (!r.ok() && head_id()) return std::unexpected(r.output);
        for (size_t at = 0; at < r.output.size();)
        {
            size_t end = r.output.find('\n', at);
            if (end == std::string::npos) end = r.output.size();
            std::string_view line(r.output.data() + at, end - at);
            at = end + 1;
            std::vector<oid> ids;
            for (size_t p = 0; p + GIT_OID_SHA1_HEXSIZE <= line.size();
                 p += GIT_OID_SHA1_HEXSIZE + 1)
            {
                git_oid o;
                if (git_oid_fromstrn(&o, line.data() + p, GIT_OID_SHA1_HEXSIZE) != 0) break;
                ids.push_back(to_oid(&o));
            }
            if (ids.empty()) continue;
            const git_oid first = to_git(ids.front());
            owned_commit c;
            if (git_commit_lookup(&c.p, repo_, &first) == 0)
                add(c, std::vector<oid>(ids.begin() + 1, ids.end()));
        }
    }
    else
    {
        git_oid id;
        while (git_revwalk_next(&id, w.p) == 0)
        {
            owned_commit c;
            if (git_commit_lookup(&c.p, repo_, &id) != 0) continue;
            const unsigned n = git_commit_parentcount(c.p);
            std::vector<oid> parents;
            parents.reserve(n);
            for (unsigned i = 0; i < n; ++i)
                parents.push_back(to_oid(git_commit_parent_id(c.p, i)));
            if (opts.first_parent && n > 1) parents.resize(1);
            add(c, std::move(parents));
        }
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

        commit& c = all[i];
        if (opts.no_merges && c.parents.size() > 1) continue;
        if ((opts.since && c.time < *opts.since) || (opts.until && c.time > *opts.until)) continue;
        if (!sink(std::move(c))) break;
    }
    return {};
}

std::expected<commit_details, std::string> repo::details(const oid& id) const
{
    git_oid gid = to_git(id);
    owned_commit c;
    if (git_commit_lookup(&c.p, repo_, &gid) != 0) return std::unexpected(last_git_error());

    commit_details d;
    if (const char* m = git_commit_message(c.p)) d.message = m;
    if (const git_signature* s = git_commit_committer(c.p))
    {
        d.committer = s->name ? s->name : "";
        d.committer_email = s->email ? s->email : "";
        d.commit_time = s->when.time;
        d.commit_offset_minutes = s->when.offset;
    }
    return d;
}

std::expected<std::vector<file_change>, std::string> repo::changes(const oid& id) const
{
    std::vector<file_change> out;
    auto r = changes(id, {.listed = [&](const std::vector<file_change>& files) { out = files; },
                          .counted =
                              [&](size_t first, const std::vector<file_change>& files)
                          { std::ranges::copy(files, out.begin() + std::ptrdiff_t(first)); }});
    if (!r) return std::unexpected(r.error());
    return out;
}

std::expected<void, std::string> repo::changes(const oid& id, const change_steps& steps) const
{
    git_oid gid = to_git(id);
    owned_commit c, parent;
    owned_tree tree, parent_tree;
    if (git_commit_lookup(&c.p, repo_, &gid) != 0 || git_commit_tree(&tree.p, c.p) != 0)
        return std::unexpected(last_git_error());
    if (git_commit_parentcount(c.p) > 0)
    {
        if (git_commit_parent(&parent.p, c.p, 0) != 0 ||
            git_commit_tree(&parent_tree.p, parent.p) != 0)
            return std::unexpected(last_git_error());
    }

    owned_diff diff;
    if (git_diff_tree_to_tree(&diff.p, repo_, parent_tree.p, tree.p, nullptr) != 0)
        return std::unexpected(last_git_error());
    git_diff_find_options find = GIT_DIFF_FIND_OPTIONS_INIT;
    find.flags = GIT_DIFF_FIND_RENAMES;
    git_diff_find_similar(diff.p, &find);

    // The files first: the tree diff alone, no file contents.
    const size_t n = git_diff_num_deltas(diff.p);
    std::vector<file_change> files;
    files.reserve(n);
    for (size_t i = 0; i < n; ++i)
    {
        const git_diff_delta* d = git_diff_get_delta(diff.p, i);
        file_change f;
        f.path = d->new_file.path;
        if (d->status == GIT_DELTA_RENAMED || d->status == GIT_DELTA_COPIED)
            f.old_path = d->old_file.path;
        f.status = git_diff_status_char(d->status);
        f.binary = d->flags & GIT_DIFF_FLAG_BINARY;
        f.counted = false;
        files.push_back(std::move(f));
    }
    if (steps.listed) steps.listed(files);

    // Then the line counts, which read both sides of every file: batches
    // small enough to show progress, cheap enough not to flood the UI.
    constexpr size_t k_batch_size = 256;
    for (size_t first = 0; first < n; first += k_batch_size)
    {
        const size_t last = std::min(n, first + k_batch_size);
        for (size_t i = first; i < last; ++i)
        {
            if (steps.stop && steps.stop()) return {};
            file_change& f = files[i];
            owned_patch patch;
            if (git_patch_from_diff(&patch.p, diff.p, i) == 0 && patch.p)
            {
                size_t context;
                git_patch_line_stats(&context, &f.added, &f.removed, patch.p);
                f.binary = f.binary || (git_patch_get_delta(patch.p)->flags & GIT_DIFF_FLAG_BINARY);
            }
            f.counted = true;
        }
        if (steps.counted)
            steps.counted(first, std::vector<file_change>(files.begin() + std::ptrdiff_t(first),
                                                         files.begin() + std::ptrdiff_t(last)));
    }
    return {};
}

namespace
{

uint8_t action_bit(git_delta_t status)
{
    switch (status)
    {
    case GIT_DELTA_ADDED:
    case GIT_DELTA_COPIED:
    case GIT_DELTA_UNTRACKED: return action_added;
    case GIT_DELTA_DELETED: return action_deleted;
    case GIT_DELTA_RENAMED: return action_renamed;
    case GIT_DELTA_CONFLICTED: return action_conflicted;
    default: return action_modified;
    }
}

}  // namespace

uint8_t repo::actions(const oid& id) const
{
    git_oid gid = to_git(id);
    owned_commit c, parent;
    owned_tree tree, parent_tree;
    if (git_commit_lookup(&c.p, repo_, &gid) != 0 || git_commit_tree(&tree.p, c.p) != 0) return 0;
    if (git_commit_parentcount(c.p) > 0 && (git_commit_parent(&parent.p, c.p, 0) != 0 ||
                                            git_commit_tree(&parent_tree.p, parent.p) != 0))
        return 0;

    owned_diff diff;
    if (git_diff_tree_to_tree(&diff.p, repo_, parent_tree.p, tree.p, nullptr) != 0) return 0;
    git_diff_find_options find = GIT_DIFF_FIND_OPTIONS_INIT;
    find.flags = GIT_DIFF_FIND_RENAMES;
    git_diff_find_similar(diff.p, &find);

    uint8_t mask = 0;
    for (size_t i = 0, n = git_diff_num_deltas(diff.p); i < n; ++i)
        mask |= action_bit(git_diff_get_delta(diff.p, i)->status);
    return mask;
}

bool repo::is_ancestor(const oid& ancestor, const oid& descendant) const
{
    git_oid a = to_git(ancestor), d = to_git(descendant);
    return git_oid_equal(&a, &d) || git_graph_descendant_of(repo_, &d, &a) == 1;
}

repo::repo_state repo::state() const
{
    switch (git_repository_state(repo_))
    {
    case GIT_REPOSITORY_STATE_NONE: return repo_state::none;
    case GIT_REPOSITORY_STATE_MERGE: return repo_state::merge;
    case GIT_REPOSITORY_STATE_REVERT:
    case GIT_REPOSITORY_STATE_REVERT_SEQUENCE: return repo_state::revert;
    case GIT_REPOSITORY_STATE_CHERRYPICK:
    case GIT_REPOSITORY_STATE_CHERRYPICK_SEQUENCE: return repo_state::cherry_pick;
    case GIT_REPOSITORY_STATE_BISECT: return repo_state::bisect;
    case GIT_REPOSITORY_STATE_APPLY_MAILBOX:
    case GIT_REPOSITORY_STATE_APPLY_MAILBOX_OR_REBASE: return repo_state::apply_mailbox;
    default: return repo_state::rebase;
    }
}

std::vector<std::string> repo::branch_names(bool local, bool remote, bool tags) const
{
    std::vector<std::string> locals, remotes, tag_names;
    owned_iter it;
    if (git_reference_iterator_new(&it.p, repo_) != 0) return {};
    git_reference* raw;
    while (git_reference_next(&raw, it.p) == 0)
    {
        owned_ref ref{raw};
        std::string_view full = git_reference_name(raw);
        std::string name = git_reference_shorthand(raw);
        if (local && git_reference_is_branch(raw))
            locals.push_back(name);
        else if (remote && git_reference_is_remote(raw) && !full.ends_with("/HEAD"))
            remotes.push_back(name);
        else if (tags && git_reference_is_tag(raw))
            tag_names.push_back(name);
    }
    for (auto* v : {&locals, &remotes, &tag_names}) std::ranges::sort(*v);
    locals.insert(locals.end(), remotes.begin(), remotes.end());
    locals.insert(locals.end(), tag_names.begin(), tag_names.end());
    return locals;
}

std::optional<oid> repo::resolve(const std::string& rev) const
{
    owned_object obj;
    if (git_revparse_single(&obj.p, repo_, (rev + "^{commit}").c_str()) != 0) return std::nullopt;
    return to_oid(git_object_id(obj.p));
}

std::string repo::upstream_of(const std::string& branch) const
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

std::expected<std::vector<commit>, std::string> repo::range(const oid& upstream, const oid& branch,
                                                            bool include_merges) const
{
    owned_walk w;
    if (git_revwalk_new(&w.p, repo_) != 0) return std::unexpected(last_git_error());
    git_revwalk_sorting(w.p, GIT_SORT_TOPOLOGICAL);
    git_oid b = to_git(branch), u = to_git(upstream);
    if (git_revwalk_push(w.p, &b) != 0 || git_revwalk_hide(w.p, &u) != 0)
        return std::unexpected(last_git_error());

    std::vector<commit> out;
    git_oid id;
    while (git_revwalk_next(&id, w.p) == 0)
    {
        owned_commit c;
        if (git_commit_lookup(&c.p, repo_, &id) != 0) continue;
        unsigned n = git_commit_parentcount(c.p);
        if (!include_merges && n > 1) continue;
        commit out1;
        out1.id = to_oid(&id);
        for (unsigned i = 0; i < n; ++i)
            out1.parents.push_back(to_oid(git_commit_parent_id(c.p, i)));
        if (const char* s = git_commit_summary(c.p)) out1.summary = s;
        if (const char* m = git_commit_message(c.p)) out1.message = m;
        if (const git_signature* a = git_commit_author(c.p))
        {
            out1.author = a->name ? a->name : "";
            out1.email = a->email ? a->email : "";
            out1.time = a->when.time;
            out1.offset_minutes = a->when.offset;
        }
        out.push_back(std::move(out1));
    }
    return out;
}

}  // namespace tortilla
