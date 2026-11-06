#include "repo.h"

#include "command.h"
#include "libgit2_oid.h"
#include "remotes.h"
#include "worktrees.h"

#include <git2.h>
#include <rapport/sys.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace tortilla
{
namespace
{

// A name libgit2 gave, or "" for none: a reference's name or target is
// never null in the versions tested, but a std::string from null is a crash.
const char* or_empty(const char* s)
{
    return s ? s : "";
}

void init_libgit2()
{
    static const int once = git_libgit2_init();
    (void)once;
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

// Keeps the history `full` reaches as it is and, of the rest, only the
// `anchors` and the forks and merges between them, as git log
// --simplify-by-decoration does: each kept commit's parents become the
// nearest kept ones. A fork is kept where lines reaching different kept
// commits part, a merge where lines from different ones join; so every
// commit dropped leads to at most one kept commit each way, which is the
// one its kept neighbours join through it.
void simplify_walk(std::vector<commit>& all, const std::vector<oid>& full,
                   const std::vector<oid>& anchors)
{
    const size_t n = all.size();
    std::unordered_map<oid, size_t, oid_hash> index;
    for (size_t i = 0; i < n; ++i) index.emplace(all[i].id, i);
    std::vector<std::vector<size_t>> parents(n), children(n);
    for (size_t i = 0; i < n; ++i)
        for (const oid& p : all[i].parents)
            if (auto it = index.find(p); it != index.end())
            {
                parents[i].push_back(it->second);
                children[it->second].push_back(i);
            }

    std::vector<char> keep(n, 0);
    std::vector<size_t> stack;
    for (const oid& id : full)
        if (auto it = index.find(id); it != index.end() && !keep[it->second])
        {
            keep[it->second] = 1;
            stack.push_back(it->second);
        }
    while (!stack.empty())
    {
        const size_t i = stack.back();
        stack.pop_back();
        for (size_t p : parents[i])
            if (!keep[p])
            {
                keep[p] = 1;
                stack.push_back(p);
            }
    }
    for (const oid& id : anchors)
        if (auto it = index.find(id); it != index.end()) keep[it->second] = 1;

    // Children before parents.
    std::vector<size_t> order;
    order.reserve(n);
    std::vector<size_t> waiting(n);
    for (size_t i = 0; i < n; ++i)
        if (!(waiting[i] = children[i].size())) stack.push_back(i);
    while (!stack.empty())
    {
        const size_t i = stack.back();
        stack.pop_back();
        order.push_back(i);
        for (size_t p : parents[i])
            if (--waiting[p] == 0) stack.push_back(p);
    }

    constexpr size_t none = SIZE_MAX;
    // The one kept commit each neighbour leads to, or a second: a fork or
    // a merge, kept.
    auto nearest = [&](const std::vector<size_t>& next, const std::vector<size_t>& rep, size_t self)
    {
        size_t found = none;
        for (size_t j : next)
            if (rep[j] != none)
            {
                if (found != none && rep[j] != found) return self;
                found = rep[j];
            }
        return found;
    };
    std::vector<size_t> up(n, none), down(n, none);
    for (size_t i : order)
    {
        up[i] = keep[i] ? i : nearest(children[i], up, i);
        if (up[i] == i) keep[i] = 1;
    }
    for (size_t k = order.size(); k-- > 0;)
    {
        const size_t i = order[k];
        down[i] = keep[i] ? i : nearest(parents[i], down, i);
        if (down[i] == i) keep[i] = 1;
    }

    std::vector<commit> out;
    for (size_t i = 0; i < n; ++i)
    {
        if (!keep[i]) continue;
        std::vector<oid> rewritten;
        for (size_t p : parents[i])
            if (const size_t to = keep[p] ? p : down[p];
                to != none && std::ranges::find(rewritten, all[to].id) == rewritten.end())
                rewritten.push_back(all[to].id);
        out.push_back(std::move(all[i]));
        out.back().parents = std::move(rewritten);
    }
    all = std::move(out);
}

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

int config_snapshot(git_config** out, git_repository* r)
{
    *out = nullptr;
    if (const int error = git_repository_config_snapshot(out, r); error != 0)
    {
        *out = nullptr;
        return error;
    }
    git_config_iterator* it = nullptr;
    bool unknown = false;
    if (git_config_iterator_glob_new(&it, *out, "^includeif\\.hasconfig:.*\\.path$") == 0)
    {
        git_config_entry* e = nullptr;
        unknown = git_config_next(&e, it) == 0;
        git_config_iterator_free(it);
    }
#ifdef _WIN32
    // A gitdir: pattern written with backslashes (C:\x\): git for Windows
    // and libgit2 may read the \ differently (an escape, or a separator).
    if (!unknown && git_config_iterator_glob_new(&it, *out, "^includeif\\.gitdir.*\\.path$") == 0)
    {
        git_config_entry* e = nullptr;
        while (!unknown && git_config_next(&e, it) == 0)
            unknown =
                e && e->name && std::string_view(e->name).find('\\') != std::string_view::npos;
        git_config_iterator_free(it);
    }
#endif
    if (!unknown) return 0;
    git_config_free(*out);
    *out = nullptr;
    return GIT_ERROR;
}

std::string commit_body(std::string_view message)
{
    // Line by line: blank ones (only spaces) end the subject's paragraph,
    // and those after it are skipped too.
    auto blank = [](std::string_view line)
    { return line.find_first_not_of(" \t\r") == std::string_view::npos; };
    auto next_line = [&](std::string_view& rest)
    {
        const size_t nl = rest.find('\n');
        const std::string_view line = rest.substr(0, nl);
        rest.remove_prefix(nl == std::string_view::npos ? rest.size() : nl + 1);
        return line;
    };
    std::string_view rest = message;
    while (!rest.empty() && blank(rest.substr(0, rest.find('\n')))) next_line(rest);
    while (!rest.empty() && !blank(rest.substr(0, rest.find('\n')))) next_line(rest);
    while (!rest.empty() && blank(rest.substr(0, rest.find('\n')))) next_line(rest);
    return std::string(rest);
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
    , git_strict_refs_(o.git_strict_refs_)
{
}

repo& repo::operator=(repo&& o) noexcept
{
    std::swap(repo_, o.repo_);
    std::swap(git_strict_refs_, o.git_strict_refs_);
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

std::string repo::common_dir() const
{
    return git_repository_commondir(repo_);
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

namespace
{

using remote_head_map = std::unordered_map<std::string, std::string>;

// One ref as a label: `commit` is what it shows on (an annotated tag's
// commit), null for refs to trees or blobs, which aren't shown.
void add_label(ref_map& out, remote_head_map& remote_heads, std::string_view full,
               std::string_view symref, const git_oid* commit, bool annotated,
               std::string_view upstream, bool current, std::string_view worktree)
{
    ref ref;
    ref.full_name = full;
    ref.kind = ref_kind::other;
    auto strip = [&](std::string_view prefix) { return std::string(full.substr(prefix.size())); };
    if (full.starts_with("refs/heads/"))
    {
        ref.kind = ref_kind::local_branch;
        ref.name = strip("refs/heads/");
    }
    else if (full.starts_with("refs/remotes/"))
    {
        // <remote>/HEAD isn't a label: it says which is the default.
        if (full.ends_with("/HEAD"))
        {
            if (!symref.empty())
                remote_heads.emplace(std::string(full.substr(0, full.size() - 5)),
                                     std::string(symref));
            return;
        }
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
    else if (is_prefetch_ref(full))
        return;
    else
        ref.name = full.starts_with("refs/") ? strip("refs/") : std::string(full);
    if (!commit) return;
    ref.upstream = upstream;
    ref.current = current;
    ref.annotated = annotated;
    if (ref.kind == ref_kind::local_branch && !current) ref.worktree = worktree;
    out[to_oid(commit)].push_back(std::move(ref));
}

// Every label through git for-each-ref, when libgit2 can't list them.
bool git_labels(const repo& r, ref_map& out, remote_head_map& remote_heads)
{
    git_result listed = run_git(
        r.workdir(),
        {"for-each-ref", "--format=%(objectname)%09%(objecttype)%09%(*objectname)%09"
                         "%(refname)%09%(upstream)%09%(HEAD)%09%(worktreepath)%09%(symref)"},
        git_output::stdout_only);
    if (!listed.ok()) return false;
    for (size_t at = 0; at < listed.output.size();)
    {
        size_t nl = listed.output.find('\n', at);
        if (nl == std::string::npos) nl = listed.output.size();
        std::string_view line(listed.output.data() + at, nl - at);
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
        if (f.size() < 8) continue;
        const std::string_view type = f[1];
        // Annotated tags label the commit underneath.
        const std::string_view commit = type == "tag" ? f[2] : f[0];
        git_oid id;
        const bool shown = (type == "tag" || type == "commit") &&
                           git_oid_fromstrn(&id, commit.data(), commit.size()) == 0;
        add_label(out, remote_heads, f[3], f[7], shown ? &id : nullptr, type == "tag", f[4],
                  f[5] == "*", f[6]);
    }
    return true;
}

// branch.<b>.merge through the remote's fetch refspecs (the first that
// maps it), as git's %(upstream); as is for the remote "."; "" when it has
// none or no refspec maps it.
std::string tracked_ref(const std::string& remote, const std::string& merge,
                        const std::vector<std::string>& specs)
{
    if (remote.empty() || merge.empty()) return {};
    if (remote == ".") return merge;
    for (const std::string& text : specs)
    {
        git_refspec* spec = nullptr;
        if (git_refspec_parse(&spec, text.c_str(), true) != 0) continue;
        std::string name;
        if (git_refspec_src_matches(spec, merge.c_str()))
        {
            git_buf out = GIT_BUF_INIT;
            if (git_refspec_transform(&out, spec, merge.c_str()) == 0 && out.ptr) name = out.ptr;
            git_buf_dispose(&out);
        }
        git_refspec_free(spec);
        if (!name.empty()) return name;
    }
    return {};
}

// Local branches' tracking from one config snapshot, as repo::tracking_of
// has it. Unknown when libgit2 can't read the config as git does: git
// answers then.
class upstreams
{
public:
    explicit upstreams(git_repository* r)
    {
        if (config_snapshot(&config_.p, r) != 0) config_.p = nullptr;
    }

    bool known() const { return config_.p != nullptr; }

    // Only when known().
    repo::tracking of(std::string_view branch)
    {
        repo::tracking t;
        if (!config_.p) return t;
        const std::string key = "branch." + std::string(branch);
        t.remote = get(key + ".remote");
        const std::vector<std::string> merges = all(key + ".merge");
        t.merges = merges.size();
        if (!merges.empty()) t.merge = merges.front();
        auto [it, added] = specs_.try_emplace(t.remote);
        if (added && !t.remote.empty() && t.remote != ".")
            it->second = all("remote." + t.remote + ".fetch");
        t.upstream = tracked_ref(t.remote, t.merge, it->second);
        return t;
    }

private:
    // The last value given, as git config --get.
    std::string get(const std::string& key) const
    {
        git_buf value = GIT_BUF_INIT;
        std::string out;
        if (git_config_get_string_buf(&value, config_.p, key.c_str()) == 0 && value.ptr)
            out = value.ptr;
        git_buf_dispose(&value);
        return out;
    }

    // Every value given, in git's order.
    std::vector<std::string> all(const std::string& key) const
    {
        std::vector<std::string> out;
        git_config_get_multivar_foreach(
            config_.p, key.c_str(), nullptr,
            [](const git_config_entry* e, void* values)
            {
                if (e->value)
                    static_cast<std::vector<std::string>*>(values)->emplace_back(e->value);
                return 0;
            },
            &out);
        return out;
    }

    owned<git_config, git_config_free> config_;
    std::unordered_map<std::string, std::vector<std::string>> specs_;  // by remote
};

// Every label through libgit2, as git_labels has them; false when the refs
// can't be read (git answers then).
bool libgit2_labels(const repo& r, ref_map& out, remote_head_map& remote_heads)
{
    git_repository* raw = r.raw();
    // The branch HEAD names here, and those other worktrees have out.
    std::string head;
    {
        owned_ref h;
        if (git_reference_lookup(&h.p, raw, "HEAD") == 0 &&
            git_reference_type(h.p) == GIT_REFERENCE_SYMBOLIC)
            head = or_empty(git_reference_symbolic_target(h.p));
    }
    std::unordered_map<std::string, std::string> elsewhere;  // full branch name -> worktree
    if (auto trees = list_worktrees(r))
        for (const worktree_info& w : *trees)
        {
            if (!w.branch.empty())
                elsewhere.emplace(
                    w.branch.starts_with("refs/") ? w.branch : "refs/heads/" + w.branch, w.path);
            // A rebase or bisect there holds its branch as if checked out.
            if (!w.current)
                for (const std::string& held : w.held) elsewhere.emplace(held, w.path);
        }
    // A config libgit2 can't read as git does (includeIf hasconfig:, say,
    // setting branch.<b>.merge): every label from git, upstreams with them.
    upstream_list upstream(r);
    owned<git_odb, git_odb_free> odb;
    if (git_repository_odb(&odb.p, raw) != 0) return false;

    owned_iter it;
    if (git_reference_iterator_new(&it.p, raw) != 0) return false;
    for (;;)
    {
        owned_ref ref;
        const int next = git_reference_next(&ref.p, it.p);
        if (next == GIT_ITEROVER) break;
        if (next != 0) return false;
        const std::string_view full = or_empty(git_reference_name(ref.p));
        std::string symref;
        owned_ref resolved;
        const git_reference* direct = ref.p;
        if (git_reference_type(ref.p) == GIT_REFERENCE_SYMBOLIC)
        {
            symref = or_empty(git_reference_symbolic_target(ref.p));
            // Dangling (origin/HEAD naming a branch that's gone): left
            // out, as for-each-ref does.
            if (git_reference_resolve(&resolved.p, ref.p) != 0) continue;
            direct = resolved.p;
        }
        const git_oid* target = git_reference_target(direct);
        if (!target) continue;
        // Branches hold commits; a tag may be annotated (a tag object over
        // its commit), and other refs may hold anything.
        git_oid commit = *target;
        bool annotated = false, shown = true;
        if (!full.starts_with("refs/heads/") && !full.starts_with("refs/remotes/"))
        {
            if (const git_oid* peeled = git_reference_target_peel(direct))
            {
                annotated = true;  // packed-refs knows it's a tag, and its commit
                commit = *peeled;
            }
            else
            {
                size_t size = 0;
                git_object_t type = GIT_OBJECT_INVALID;
                if (git_odb_read_header(&size, &type, odb.p, target) != 0) continue;
                if (type == GIT_OBJECT_TAG)
                {
                    annotated = true;
                    owned_object peeled_object;
                    owned_object tag;
                    if (git_object_lookup(&tag.p, raw, target, GIT_OBJECT_TAG) != 0 ||
                        git_object_peel(&peeled_object.p, tag.p, GIT_OBJECT_COMMIT) != 0)
                        shown = false;
                    else
                        commit = *git_object_id(peeled_object.p);
                }
                else
                    shown = type == GIT_OBJECT_COMMIT;
            }
        }
        const bool branch = full.starts_with("refs/heads/");
        const auto there = branch ? elsewhere.find(std::string(full)) : elsewhere.end();
        add_label(out, remote_heads, full, symref, shown ? &commit : nullptr, annotated,
                  branch ? upstream.of(full.substr(11)).value_or("") : std::string(),
                  branch && full == head,
                  there != elsewhere.end() ? std::string_view(there->second) : std::string_view());
    }
    return true;
}

// The labels as listed, with a detached HEAD and each remote's default.
ref_map with_heads(git_repository* repo_, ref_map out, const remote_head_map& remote_heads)
{
    // A detached HEAD shows as a label too.
    git_oid head;
    if (git_repository_head_detached(repo_) == 1 &&
        git_reference_name_to_id(&head, repo_, "HEAD") == 0)
        out[to_oid(&head)].push_back({"HEAD", "HEAD", ref_kind::head, true});

    // Each remote's default branch: what <remote>/HEAD names, else its main,
    // else its master.
    std::unordered_map<std::string, ref*> remote_branches;  // by full name
    for (auto& [id, refs] : out)
        for (ref& r : refs)
            if (r.kind == ref_kind::remote_branch) remote_branches.emplace(r.full_name, &r);
    std::unordered_set<std::string> remotes;  // "refs/remotes/origin"
    for (auto& [full, r] : remote_branches)
    {
        const size_t slash = full.find('/', 13);
        if (slash != std::string::npos) remotes.insert(full.substr(0, slash));
    }
    for (auto& [prefix, h] : remote_heads) remotes.insert(prefix);
    for (const std::string& prefix : remotes)
    {
        std::vector<std::string> candidates{prefix + "/main", prefix + "/master"};
        if (auto h = remote_heads.find(prefix); h != remote_heads.end())
            candidates.insert(candidates.begin(), h->second);
        for (const std::string& c : candidates)
            if (auto it = remote_branches.find(c); it != remote_branches.end())
            {
                it->second->remote_default = true;
                break;
            }
    }

    for (auto& [id, refs] : out) std::ranges::sort(refs, {}, &ref::full_name);
    return out;
}

}  // namespace

// Every label with its commit, annotated-ness, upstream and the checked-out
// branch, through libgit2: one ref listing and one config snapshot, where
// git for-each-ref costs a process (tens of ms on Windows); git when libgit2
// can't read the refs.
ref_map repo::refs() const
{
    ref_map out;
    // "refs/remotes/origin" -> what origin/HEAD names.
    remote_head_map remote_heads;
    if (!libgit2_labels(*this, out, remote_heads)) return refs_by_git();
    return with_heads(repo_, std::move(out), remote_heads);
}

ref_map repo::refs_by_git() const
{
    ref_map out;
    remote_head_map remote_heads;
    if (!git_labels(*this, out, remote_heads)) return {};
    return with_heads(repo_, std::move(out), remote_heads);
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
    case walk_refs::all: return !is_prefetch_ref(full_name);
    case walk_refs::basic:
        return full_name.starts_with("refs/heads/") || full_name.starts_with("refs/remotes/") ||
               full_name.starts_with("refs/tags/");
    case walk_refs::local: return full_name.starts_with("refs/heads/");
    case walk_refs::head:
    case walk_refs::tips: break;
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
    const bool only_tips = opts.refs == walk_refs::tips;
    // What the walk starts from in full, for sparse_tips: the rest is theirs.
    std::vector<oid> dense;
    auto push = [&](const git_oid* id)
    {
        git_revwalk_push(w.p, id);
        dense.push_back(to_oid(id));
    };
    if (only_tips && opts.tips.empty() && opts.sparse_tips.empty()) return {};
    if (many && (!opts.tips.empty() || only_tips))
    {
        for (const oid& tip : opts.tips)
        {
            git_oid t = to_git(tip);
            push(&t);
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
                    push(git_object_id(target.p));
            }
        }
    }
    if (!many && !opts.rev.empty())
    {
        // One rev, or a range: a..b is b's history less a's, a...b both
        // less what they share (their merge bases and before).
        git_revspec spec{};
        if (git_revparse(&spec, repo_, opts.rev.c_str()) != 0)
            return std::unexpected(last_git_error());
        owned_object from{spec.from}, to{spec.to};
        auto peeled = [&](git_object* o, owned_object& out) -> const git_oid*
        {
            return o && git_object_peel(&out.p, o, GIT_OBJECT_COMMIT) == 0 ? git_object_id(out.p)
                                                                           : nullptr;
        };
        owned_object a, b;
        const git_oid* left = peeled(from.p, a);
        if (!left) return std::unexpected(last_git_error());
        if (spec.flags & GIT_REVSPEC_SINGLE)
            push(left);
        else
        {
            const git_oid* right = peeled(to.p, b);
            if (!right) return std::unexpected(last_git_error());
            push(right);
            if (spec.flags & GIT_REVSPEC_MERGE_BASE)
            {
                push(left);
                git_oidarray bases{};
                if (git_merge_bases(&bases, repo_, left, right) == 0)
                {
                    for (size_t i = 0; i < bases.count; ++i) git_revwalk_hide(w.p, &bases.ids[i]);
                    git_oidarray_dispose(&bases);
                }
            }
            else
                git_revwalk_hide(w.p, left);
        }
    }
    else if (!only_tips)
    {
        // Pushing HEAD fails in an empty repository; that's just "no commits".
        git_oid head;
        if (git_reference_name_to_id(&head, repo_, "HEAD") == 0) push(&head);
    }
    for (const oid& tip : opts.sparse_tips)
    {
        git_oid t = to_git(tip);
        git_revwalk_push(w.p, &t);
    }
    if (opts.unmerged_into)
    {
        git_oid base = to_git(*opts.unmerged_into);
        git_revwalk_hide(w.p, &base);
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
                if (std::FILE* f = rap::sys::fopen(rap::sys::path_from_utf8(path), "rb"))
                {
                    char line[128] = {};
                    if (std::fgets(line, sizeof line, f))
                    {
                        git_oid head;
                        if (git_oid_fromstrn(&head, line, GIT_OID_SHA1_HEXSIZE) == 0) push(&head);
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
        // Following renames is git log's alone; its history of one file is
        // a line, each commit's parent the one listed after it.
        // TODO(perf): this starts git and waits for all of it, ~1.5 s on
        // Windows over \\wsl.localhost where the plain walk is instant. In
        // process instead: compare the path's tree entry with the parents'
        // per commit, rebuild the default simplification (merges too), find
        // renames only where the path vanishes, and stream the rows.
        const bool follow = opts.follow && opts.paths.size() == 1;
        std::vector<std::string> args{"rev-list", "--parents"};
        if (follow) args = {"log", "--follow", "--format=%H"};
        if (opts.full_history && !follow) args.push_back("--full-history");
        if (opts.first_parent) args.push_back("--first-parent");
        if (many && (!opts.tips.empty() || only_tips))
            for (const oid& tip : opts.tips) args.push_back(tip.hex());
        else if (opts.refs == walk_refs::all)
            args.push_back("--all");
        else if (opts.refs == walk_refs::basic)
            args.insert(args.end(), {"--branches", "--remotes", "--tags"});
        else if (opts.refs == walk_refs::local)
            args.push_back("--branches");
        else
            args.push_back(opts.rev.empty() ? std::string("HEAD") : opts.rev);
        for (const oid& tip : opts.sparse_tips) args.push_back(tip.hex());
        if (opts.unmerged_into) args.push_back("^" + opts.unmerged_into->hex());
        args.push_back("--");
        args.insert(args.end(), opts.paths.begin(), opts.paths.end());
        const git_result r =
            run_git(workdir().empty() ? git_dir() : workdir(), args, git_output::stdout_only);
        // An empty repository has no HEAD: no commits, as below.
        if (!r.ok() && head_id()) return std::unexpected(r.output);
        std::vector<std::vector<oid>> lines;
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
            if (!ids.empty()) lines.push_back(std::move(ids));
        }
        for (size_t i = 0; i < lines.size(); ++i)
        {
            if (follow)
            {
                lines[i].resize(1);
                if (i + 1 < lines.size()) lines[i].push_back(lines[i + 1].front());
            }
            const git_oid first = to_git(lines[i].front());
            owned_commit c;
            if (git_commit_lookup(&c.p, repo_, &first) == 0)
                add(c, std::vector<oid>(lines[i].begin() + 1, lines[i].end()));
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

    if (opts.tips_only || !opts.sparse_tips.empty() || opts.unmerged_into)
    {
        std::vector<oid> anchors = dense;
        anchors.insert(anchors.end(), opts.sparse_tips.begin(), opts.sparse_tips.end());
        if (opts.unmerged_into)
        {
            // Where each branch joins the merged history, and tips already
            // in it: shown, without their parents.
            std::vector<oid> missing;
            for (const commit& c : all)
                for (const oid& p : c.parents)
                    if (!index.contains(p)) missing.push_back(p);
            for (const oid& t : anchors)
                if (!index.contains(t)) missing.push_back(t);
            for (const oid& id : missing)
            {
                if (index.contains(id)) continue;
                const git_oid g = to_git(id);
                owned_commit c;
                if (git_commit_lookup(&c.p, repo_, &g) != 0) continue;
                add(c, {});
                anchors.push_back(id);
            }
            for (commit& c : all)
                std::erase_if(c.parents, [&](const oid& p) { return !index.contains(p); });
            // In the walk's order (newest first), for the sort below.
            std::ranges::stable_sort(all, std::greater<>{}, &commit::commit_time);
            index.clear();
            for (size_t i = 0; i < all.size(); ++i) index[all[i].id] = i;
        }
        if (opts.tips_only || !opts.sparse_tips.empty())
        {
            anchors.insert(anchors.end(), opts.decorated.begin(), opts.decorated.end());
            simplify_walk(all, opts.tips_only ? std::vector<oid>{} : dense, anchors);
            index.clear();
            for (size_t i = 0; i < all.size(); ++i) index[all[i].id] = i;
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

// A diff's files, renames paired, as change_steps want them: the list
// first (the tree diff alone), then the line counts in batches.
std::expected<void, std::string> repo::report_changes(git_diff* diff,
                                                      const change_steps& steps) const
{
    git_diff_find_options find = GIT_DIFF_FIND_OPTIONS_INIT;
    find.flags = GIT_DIFF_FIND_RENAMES;
    git_diff_find_similar(diff, &find);

    // The files first: the tree diff alone, no file contents.
    const size_t n = git_diff_num_deltas(diff);
    std::vector<file_change> files;
    files.reserve(n);
    for (size_t i = 0; i < n; ++i)
    {
        const git_diff_delta* d = git_diff_get_delta(diff, i);
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
            if (git_patch_from_diff(&patch.p, diff, i) == 0 && patch.p)
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

// `from` against `to` (commits, refs or trees; git's empty tree is none),
// or against the working tree and index when `to` is "".
std::expected<void, std::string> repo::changes_between(const std::string& from,
                                                       const std::string& to,
                                                       const change_steps& steps) const
{
    auto tree_of = [this](const std::string& rev, owned_tree& out) -> bool
    {
        if (rev == "4b825dc642cb6eb9a060e54bf8d69288fbee4904") return true;  // stays null: empty
        owned_object obj;
        if (git_revparse_single(&obj.p, repo_, rev.c_str()) != 0) return false;
        return git_object_peel(reinterpret_cast<git_object**>(&out.p), obj.p, GIT_OBJECT_TREE) == 0;
    };
    owned_tree a, b;
    if (!tree_of(from, a) || (!to.empty() && !tree_of(to, b)))
        return std::unexpected(last_git_error());
    owned_diff diff;
    const int made = to.empty() ? git_diff_tree_to_workdir_with_index(&diff.p, repo_, a.p, nullptr)
                                : git_diff_tree_to_tree(&diff.p, repo_, a.p, b.p, nullptr);
    if (made != 0) return std::unexpected(last_git_error());
    return report_changes(diff.p, steps);
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
    return report_changes(diff.p, steps);
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

std::string repo::note(const oid& id) const
{
    git_oid gid = to_git(id);
    git_note* n = nullptr;
    if (git_note_read(&n, repo_, nullptr, &gid) != 0) return {};
    const char* message = git_note_message(n);
    std::string text = message ? message : "";
    git_note_free(n);
    return text;
}

std::vector<repo::stash_entry> repo::stashes() const
{
    std::vector<stash_entry> out;
    git_reflog* log = nullptr;
    if (git_reflog_read(&log, repo_, "refs/stash") != 0) return out;
    for (size_t i = 0, n = git_reflog_entrycount(log); i < n; ++i)
    {
        const git_reflog_entry* e = git_reflog_entry_byindex(log, i);  // 0: the newest
        stash_entry s{.selector = "stash@{" + std::to_string(i) + "}",
                      .id = to_oid(git_reflog_entry_id_new(e))};
        if (const char* m = git_reflog_entry_message(e)) s.subject = m;
        while (!s.subject.empty() && (s.subject.back() == '\n' || s.subject.back() == '\r'))
            s.subject.pop_back();
        owned_commit c;
        const git_oid id = to_git(s.id);
        if (git_commit_lookup(&c.p, repo_, &id) == 0 && git_commit_parentcount(c.p) > 0)
            s.base = to_oid(git_commit_parent_id(c.p, 0));
        out.push_back(std::move(s));
    }
    git_reflog_free(log);
    return out;
}

std::string repo::short_id(const oid& id) const
{
    const git_oid g = to_git(id);
    owned_object obj;
    git_buf buf = GIT_BUF_INIT;
    std::string out;
    if (git_object_lookup(&obj.p, repo_, &g, GIT_OBJECT_ANY) == 0 &&
        git_object_short_id(&buf, obj.p) == 0 && buf.ptr)
        out = buf.ptr;
    git_buf_dispose(&buf);
    return out.empty() ? id.hex(7) : out;
}

std::optional<std::string> repo::tag_message(const std::string& tag) const
{
    git_oid id;
    if (git_reference_name_to_id(&id, repo_, ("refs/tags/" + tag).c_str()) != 0)
        return std::nullopt;
    owned<git_tag, git_tag_free> t;
    if (git_tag_lookup(&t.p, repo_, &id) != 0) return std::nullopt;
    const char* m = git_tag_message(t.p);
    return std::string(m ? m : "");
}

std::string repo::branches_containing(const oid& id) const
{
    std::string out;
    if (git_repository_head_detached(repo_) == 1)
        if (const auto head = head_id(); head && is_ancestor(id, *head))
            out += "* (HEAD detached at " + short_id(*head) + ")\n";
    std::string current;
    {
        owned_ref h;
        if (git_reference_lookup(&h.p, repo_, "HEAD") == 0 &&
            git_reference_type(h.p) == GIT_REFERENCE_SYMBOLIC)
            current = or_empty(git_reference_symbolic_target(h.p));
    }
    for (const char* prefix : {"refs/heads/", "refs/remotes/"})
        if (const auto refs = refs_under({prefix}))
            for (const listed_ref& l : *refs)
            {
                if (!is_ancestor(id, l.target)) continue;
                const bool local = std::string_view(prefix) == "refs/heads/";
                out += l.full_name == current ? "* " : "  ";
                out += local ? l.full_name.substr(11) : l.full_name.substr(5);
                if (!local && !l.symref.empty())
                    out += " -> " +
                           (l.symref.starts_with("refs/remotes/") ? l.symref.substr(13) : l.symref);
                out += '\n';
            }
    return out;
}

std::optional<oid> repo::merge_base(const oid& a, const oid& b) const
{
    git_oid x = to_git(a), y = to_git(b), base;
    if (git_merge_base(&base, repo_, &x, &y) != 0) return std::nullopt;
    return to_oid(&base);
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
    std::string out = upstream_ref(branch);
    for (const char* prefix : {"refs/remotes/", "refs/heads/"})
        if (out.starts_with(prefix))
        {
            out.erase(0, std::strlen(prefix));
            break;
        }
    return out;
}

bool repo::revision_exists(const std::string& rev) const
{
    if (rev.find("..") == std::string::npos) return resolve(rev).has_value();
    git_revspec spec{};
    if (git_revparse(&spec, repo_, rev.c_str()) != 0) return false;
    git_object_free(spec.from);
    git_object_free(spec.to);
    return true;
}

std::optional<std::string> repo::upstream_in_process(std::string_view branch) const
{
    upstreams up(repo_);
    if (!up.known()) return std::nullopt;
    return up.of(branch).upstream;
}

repo::tracking repo::tracking_of(std::string_view branch) const
{
    upstreams up(repo_);
    if (up.known()) return up.of(branch);
    // git's values (config --get, the last; --get-all, every one), read the
    // same way.
    const std::string dir = workdir(), key = "branch." + std::string(branch);
    tracking t;
    t.remote = config_value(dir, key + ".remote");
    const std::vector<std::string> merges = config_values(dir, key + ".merge");
    t.merges = merges.size();
    if (!merges.empty()) t.merge = merges.front();
    std::vector<std::string> specs;
    if (!t.remote.empty() && t.remote != ".")
        specs = config_values(dir, "remote." + t.remote + ".fetch");
    t.upstream = tracked_ref(t.remote, t.merge, specs);
    return t;
}

struct upstream_list::impl
{
    explicit impl(git_repository* r)
        : in_process(r)
    {
    }
    upstreams in_process;
    bool unknown = false;
    std::unordered_map<std::string, std::string> by_git;  // short name -> upstream
};

upstream_list::upstream_list(const repo& r)
    : p_(std::make_unique<impl>(r.raw()))
{
    if (p_->in_process.known()) return;
    const git_result listed =
        run_git(r.workdir(), {"for-each-ref", "--format=%(refname)%09%(upstream)", "refs/heads/"},
                git_output::stdout_only);
    if (!listed.ok())
    {
        p_->unknown = true;
        return;
    }
    for (std::string_view rest = listed.output; !rest.empty();)
    {
        const size_t nl = rest.find('\n');
        std::string_view line = rest.substr(0, nl);
        rest.remove_prefix(nl == std::string_view::npos ? rest.size() : nl + 1);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        const size_t tab = line.find('\t');
        if (tab == std::string_view::npos || !line.starts_with("refs/heads/")) continue;
        p_->by_git.emplace(line.substr(11, tab - 11), line.substr(tab + 1));
    }
}

upstream_list::~upstream_list() = default;

std::optional<std::string> upstream_list::of(std::string_view branch)
{
    if (p_->in_process.known()) return p_->in_process.of(branch).upstream;
    if (p_->unknown) return std::nullopt;
    const auto it = p_->by_git.find(std::string(branch));
    return it == p_->by_git.end() ? std::string() : it->second;
}

std::string repo::upstream_ref(std::string_view branch) const
{
    return tracking_of(branch).upstream;
}

std::optional<std::vector<repo::listed_ref>>
repo::refs_under(const std::vector<std::string>& prefixes) const
{
    std::vector<listed_ref> out;
    auto list = [&](const char* glob) -> bool
    {
        owned_iter it;
        if ((glob ? git_reference_iterator_glob_new(&it.p, repo_, glob)
                  : git_reference_iterator_new(&it.p, repo_)) != 0)
            return false;
        for (;;)
        {
            owned_ref ref;
            const int next = git_reference_next(&ref.p, it.p);
            if (next == GIT_ITEROVER) return true;
            if (next != 0) return false;
            listed_ref l{.full_name = or_empty(git_reference_name(ref.p))};
            // (A glob's * crosses slashes; the prefix must still hold.)
            if (glob && !std::string_view(l.full_name)
                             .starts_with(std::string_view(glob, std::strlen(glob) - 1)))
                continue;
            owned_ref resolved;
            const git_reference* direct = ref.p;
            if (git_reference_type(ref.p) == GIT_REFERENCE_SYMBOLIC)
            {
                l.symref = or_empty(git_reference_symbolic_target(ref.p));
                if (git_reference_resolve(&resolved.p, ref.p) != 0) continue;
                direct = resolved.p;
            }
            const git_oid* target = git_reference_target(direct);
            if (!target) continue;
            l.target = to_oid(target);
            out.push_back(std::move(l));
        }
    };
    if (prefixes.empty())
    {
        if (!list(nullptr)) return std::nullopt;
    }
    else
        for (const std::string& p : prefixes)
            if (!list((p + "*").c_str())) return std::nullopt;
    std::ranges::sort(out, {}, &listed_ref::full_name);
    // Prefixes that overlap list a ref once.
    out.erase(std::ranges::unique(out, {}, &listed_ref::full_name).begin(), out.end());
    return out;
}

std::string repo::full_ref_name(const std::string& name) const
{
    owned_ref ref, resolved;
    if (git_reference_dwim(&ref.p, repo_, name.c_str()) != 0) return {};
    if (git_reference_type(ref.p) != GIT_REFERENCE_SYMBOLIC)
        return or_empty(git_reference_name(ref.p));
    // HEAD and other symbolic refs: what they end at, even unborn.
    std::string target = or_empty(git_reference_symbolic_target(ref.p));
    if (git_reference_resolve(&resolved.p, ref.p) == 0)
        target = or_empty(git_reference_name(resolved.p));
    return target;
}

std::string repo::short_ref_name(const std::string& full) const
{
    // git's shorten_unambiguous_ref: the shortest form by its rules that
    // no rule before it would take for another ref.
    static constexpr std::pair<std::string_view, std::string_view> rules[] = {
        {"", ""},
        {"refs/", ""},
        {"refs/tags/", ""},
        {"refs/heads/", ""},
        {"refs/remotes/", ""},
        {"refs/remotes/", "/HEAD"},
    };
    auto exists = [&](const std::string& name)
    {
        // A name outside refs/ is a root ref (HEAD, ORIG_HEAD): capitals.
        if (!name.starts_with("refs/") &&
            !std::ranges::all_of(name, [](char c) { return (c >= 'A' && c <= 'Z') || c == '_'; }))
            return false;
        owned_ref ref, resolved;
        return git_reference_lookup(&ref.p, repo_, name.c_str()) == 0 &&
               git_reference_resolve(&resolved.p, ref.p) == 0;
    };
    const std::string_view name = full;
    bool strict = true;
    {
        owned<git_config, git_config_free> config;
        int value = 1;
        if (config_snapshot(&config.p, repo_) != 0)
        {
            // git's word, kept: the reference dialogs ask for every ref.
            if (!git_strict_refs_)
                git_strict_refs_ = config_bool(workdir(), "core.warnambiguousrefs").value_or(true);
            strict = *git_strict_refs_;
        }
        else if (git_config_get_bool(&value, config.p, "core.warnambiguousrefs") == 0)
            strict = value != 0;
    }
    for (size_t i = std::size(rules) - 1; i > 0; --i)
    {
        const auto [prefix, suffix] = rules[i];
        if (name.size() <= prefix.size() + suffix.size() || !name.starts_with(prefix) ||
            !name.ends_with(suffix))
            continue;
        const std::string shortened(
            name.substr(prefix.size(), name.size() - prefix.size() - suffix.size()));
        // With core.warnAmbiguousRefs (on unless turned off) no other rule
        // may take it; without, only those before it.
        bool taken = false;
        for (size_t j = 0; j < (strict ? std::size(rules) : i) && !taken; ++j)
            taken = j != i &&
                    exists(std::string(rules[j].first) + shortened + std::string(rules[j].second));
        if (!taken) return shortened;
    }
    return full;
}

std::vector<oid> repo::rev_list(const std::vector<oid>& include, const std::vector<oid>& exclude,
                                size_t limit) const
{
    owned_walk w;
    if (git_revwalk_new(&w.p, repo_) != 0) return {};
    git_revwalk_sorting(w.p, GIT_SORT_TIME);
    for (const oid& o : include)
        if (git_oid id = to_git(o); git_revwalk_push(w.p, &id) != 0) return {};
    for (const oid& o : exclude)
        if (git_oid id = to_git(o); git_revwalk_hide(w.p, &id) != 0) return {};
    std::vector<oid> out;
    git_oid id;
    while ((limit == 0 || out.size() < limit) && git_revwalk_next(&id, w.p) == 0)
        out.push_back(to_oid(&id));
    return out;
}

std::optional<std::pair<size_t, size_t>> repo::ahead_behind(const oid& local,
                                                            const oid& upstream) const
{
    git_oid l = to_git(local), u = to_git(upstream);
    size_t ahead = 0, behind = 0;
    if (git_graph_ahead_behind(&ahead, &behind, repo_, &l, &u) != 0) return std::nullopt;
    return std::pair{ahead, behind};
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

std::optional<commit> repo::lookup(const oid& id) const
{
    git_oid gid = to_git(id);
    owned_commit c;
    if (git_commit_lookup(&c.p, repo_, &gid) != 0) return std::nullopt;
    commit out;
    out.id = id;
    for (unsigned i = 0, n = git_commit_parentcount(c.p); i < n; ++i)
        out.parents.push_back(to_oid(git_commit_parent_id(c.p, i)));
    if (const char* s = git_commit_summary(c.p)) out.summary = s;
    if (const char* m = git_commit_message(c.p)) out.message = m;
    if (const git_signature* a = git_commit_author(c.p))
    {
        out.author = a->name ? a->name : "";
        out.email = a->email ? a->email : "";
        out.time = a->when.time;
        out.offset_minutes = a->when.offset;
    }
    if (const git_signature* cm = git_commit_committer(c.p))
    {
        out.committer = cm->name ? cm->name : "";
        out.committer_email = cm->email ? cm->email : "";
        out.commit_time = cm->when.time;
        out.commit_offset_minutes = cm->when.offset;
    }
    return out;
}

}  // namespace tortilla
