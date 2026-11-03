#include "history_edit.h"

#include "git/command.h"
#include "git/libgit2_oid.h"
#include "git/svn.h"
#include "git/worktrees.h"
#include "i18n.h"

#include <git2.h>

#include <algorithm>
#include <cstring>
#include <unordered_set>

namespace tortilla
{
namespace
{

template <class t, void (*Free)(t*)>
struct owned
{
    t* p = nullptr;
    owned() = default;
    owned(const owned&) = delete;
    owned& operator=(const owned&) = delete;
    ~owned()
    {
        if (p) Free(p);
    }
};

using owned_ref = owned<git_reference, git_reference_free>;
using owned_walk = owned<git_revwalk, git_revwalk_free>;
using owned_commit = owned<git_commit, git_commit_free>;
using owned_tree = owned<git_tree, git_tree_free>;
using owned_index = owned<git_index, git_index_free>;
using owned_signature = owned<git_signature, git_signature_free>;
using owned_transaction = owned<git_transaction, git_transaction_free>;
using owned_status = owned<git_status_list, git_status_list_free>;
using owned_diff = owned<git_diff, git_diff_free>;

std::string short_name(std::string_view full)
{
    for (std::string_view p : {"refs/heads/", "refs/remotes/", "refs/tags/"})
        if (full.starts_with(p)) return std::string(full.substr(p.size()));
    return std::string(full);
}

std::optional<oid> resolve_ref(git_repository* r, const std::string& name)
{
    git_oid id;
    if (git_reference_name_to_id(&id, r, name.c_str()) != 0) return std::nullopt;
    // A tag's target peeled to its commit.
    git_object* obj = nullptr;
    if (git_object_lookup(&obj, r, &id, GIT_OBJECT_ANY) != 0) return std::nullopt;
    git_object* peeled = nullptr;
    const int error = git_object_peel(&peeled, obj, GIT_OBJECT_COMMIT);
    git_object_free(obj);
    if (error != 0) return std::nullopt;
    oid out = to_oid(git_object_id(peeled));
    git_object_free(peeled);
    return out;
}

std::optional<oid> tree_of(git_repository* r, const oid& commit)
{
    git_oid id = to_git(commit);
    owned_commit c;
    if (git_commit_lookup(&c.p, r, &id) != 0) return std::nullopt;
    return to_oid(git_commit_tree_id(c.p));
}

std::string no_identity()
{
    return _("Git doesn't know who you are, so it can't create commits. Set your name and email "
             "(git config --global user.name and user.email).");
}

// Messages without their trailing newlines, for joining.
std::string_view trimmed(std::string_view s)
{
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
        s.remove_suffix(1);
    return s;
}

// Git's squash message lines: the first, then one before each message.
std::vector<std::string> squash_separators(size_t count)
{
    std::vector<std::string> out;
    out.push_back(rap::i18n::format(_("# This is a combination of {0} commits."), count));
    out.push_back(_("# This is the 1st commit message:"));
    for (size_t i = 2; i <= count; ++i)
        out.push_back(rap::i18n::format(_("# This is the commit message #{0}:"), i));
    return out;
}

}  // namespace

// -- The branch's line ----------------------------------------------------

std::optional<size_t> branch_history::index_of(const oid& id) const
{
    auto it = std::ranges::find(line, id);
    if (it == line.end()) return std::nullopt;
    return size_t(it - line.begin());
}

bool branch_history::is_merge(size_t index) const
{
    return std::ranges::contains(merges, index);
}

std::optional<oid> branch_history::parent_of(size_t index) const
{
    return index + 1 < line.size() ? std::optional(line[index + 1]) : below;
}

std::string branch_history::name() const
{
    return short_name(branch);
}

std::expected<branch_history, std::string>
load_branch_history(const repo& repository, const std::string& branch, const std::string& mainline)
{
    git_repository* r = repository.raw();
    branch_history h;
    h.branch = branch;
    const auto tip = resolve_ref(r, branch);
    if (!tip || !branch.starts_with("refs/heads/"))
        return std::unexpected(
            rap::i18n::format(_("{0} is not a local branch"), short_name(branch)));
    h.tip = *tip;
    // A repository without remotes has its own main as the mainline: that
    // branch itself is still editable.
    if (mainline != branch) h.mainline = mainline;
    const std::optional<oid> main_tip =
        h.mainline.empty() ? std::nullopt : resolve_ref(r, h.mainline);

    owned_walk walk;
    if (git_revwalk_new(&walk.p, r) != 0) return std::unexpected(last_git_error());
    git_revwalk_sorting(walk.p, GIT_SORT_TOPOLOGICAL);
    git_revwalk_simplify_first_parent(walk.p);
    git_oid start = to_git(h.tip);
    if (git_revwalk_push(walk.p, &start) != 0) return std::unexpected(last_git_error());
    if (main_tip)
    {
        git_oid hide = to_git(*main_tip);
        git_revwalk_hide(walk.p, &hide);
    }
    // First parents only: a merged side branch's commits aren't the line's.
    git_oid id;
    std::optional<oid> expected = h.tip;
    while (h.line.size() < history_limit && git_revwalk_next(&id, walk.p) == 0)
    {
        const oid o = to_oid(&id);
        if (expected && o != *expected) continue;  // a hidden walk's side commits
        owned_commit c;
        if (git_commit_lookup(&c.p, r, &id) != 0) return std::unexpected(last_git_error());
        if (git_commit_parentcount(c.p) > 1) h.merges.push_back(h.line.size());
        if (const char* m = git_commit_message(c.p); m && in_svn(m)) h.svn.push_back(h.line.size());
        h.line.push_back(o);
        expected = git_commit_parentcount(c.p) ? std::optional(to_oid(git_commit_parent_id(c.p, 0)))
                                               : std::nullopt;
        if (!expected) break;
    }
    h.below = expected;

    git_buf up{};
    if (git_branch_upstream_name(&up, r, branch.c_str()) == 0)
    {
        h.upstream.assign(up.ptr, up.size);
        h.upstream_tip = resolve_ref(r, h.upstream);
    }
    git_buf_dispose(&up);

    owned_ref head;
    if (git_repository_head(&head.p, r) == 0 && git_reference_is_branch(head.p))
        h.checked_out = git_reference_name(head.p) == branch;
    if (auto trees = list_worktrees(repository))
        for (const worktree_info& w : *trees)
            if (!w.current && !w.prunable && w.branch == h.name()) h.worktree = w.path;
    return h;
}

std::expected<branch_history, std::string>
branch_holding(const repo& repository, const std::vector<oid>& commits, const std::string& mainline)
{
    if (commits.empty()) return std::unexpected(std::string(_("no commits")));
    auto holds = [&](const branch_history& h)
    {
        return std::ranges::all_of(commits,
                                   [&](const oid& c) { return h.index_of(c).has_value(); });
    };
    git_repository* r = repository.raw();
    std::string current;
    {
        owned_ref head;
        if (git_repository_head(&head.p, r) == 0 && git_reference_is_branch(head.p))
            current = git_reference_name(head.p);
    }
    if (!current.empty())
        if (auto h = load_branch_history(repository, current, mainline); h && holds(*h)) return h;

    // Every other branch's line stops at the mainline, so a commit already on
    // it can only be the mainline's own (a local main): one ancestry check
    // answers the commonest right-click, an old commit, instead of one per branch.
    const auto main_tip = mainline.empty() ? std::nullopt : resolve_ref(r, mainline);
    if (main_tip && std::ranges::any_of(commits, [&](const oid& c)
                                        { return repository.is_ancestor(c, *main_tip); }))
    {
        if (mainline.starts_with("refs/heads/") && mainline != current)
            if (auto h = load_branch_history(repository, mainline, mainline); h && holds(*h))
                return h;
        return std::unexpected(std::string(_("merged")));
    }

    std::vector<branch_history> found;
    git_branch_iterator* it = nullptr;
    if (git_branch_iterator_new(&it, r, GIT_BRANCH_LOCAL) == 0)
    {
        git_reference* ref = nullptr;
        git_branch_t type;
        while (git_branch_next(&ref, &type, it) == 0)
        {
            const std::string name = git_reference_name(ref);
            const git_oid* target = git_reference_target(ref);
            const bool candidate = name != current && target &&
                                   repository.is_ancestor(commits.front(), to_oid(target));
            git_reference_free(ref);
            if (!candidate) continue;
            if (auto h = load_branch_history(repository, name, mainline); h && holds(*h))
                found.push_back(std::move(*h));
        }
        git_branch_iterator_free(it);
    }
    if (found.size() == 1) return std::move(found.front());
    if (found.size() > 1) return std::unexpected(std::string(_("on several branches")));
    return std::unexpected(std::string(_("not on a local branch")));
}

std::optional<std::string> why_not_editable(const branch_history& h,
                                            const std::vector<oid>& commits, bool replays)
{
    size_t oldest = 0;
    for (const oid& c : commits)
    {
        const auto i = h.index_of(c);
        if (!i) return rap::i18n::format(_("not on {0}"), h.name());
        if (h.is_merge(*i)) return std::string(_("a merge"));
        oldest = std::max(oldest, *i);
    }
    // Rewriting from the oldest up rewrites everything above it too.
    for (size_t s : h.svn)
        if (s <= oldest) return std::string(_("in SVN"));
    if (!h.worktree.empty()) return std::string(_("in another worktree"));
    if (replays)
        for (size_t m : h.merges)
            if (m < oldest) return std::string(_("a merge follows"));
    return std::nullopt;
}

std::optional<std::string> why_nothing_staged(const repo& repository)
{
    git_repository* r = repository.raw();
    owned_index index;
    if (git_repository_index(&index.p, r) != 0 || git_index_read(index.p, false) != 0)
        return last_git_error();
    if (git_index_has_conflicts(index.p)) return std::string(_("conflicts"));
    const auto head = repository.head_id();
    if (!head) return std::string(_("no commits"));
    git_oid head_id = to_git(*head);
    owned_commit c;
    owned_tree tree;
    if (git_commit_lookup(&c.p, r, &head_id) != 0 || git_commit_tree(&tree.p, c.p) != 0)
        return last_git_error();
    owned_diff diff;
    if (git_diff_tree_to_index(&diff.p, r, tree.p, index.p, nullptr) != 0) return last_git_error();
    if (git_diff_num_deltas(diff.p) == 0) return std::string(_("nothing staged"));
    return std::nullopt;
}

bool same_tree(const repo& repository, const oid& a, const oid& b)
{
    const auto ta = tree_of(repository.raw(), a);
    return ta && ta == tree_of(repository.raw(), b);
}

bool rewrites_pushed(const repo& repository, const branch_history& h, const oid& oldest)
{
    return h.upstream_tip && repository.is_ancestor(oldest, *h.upstream_tip);
}

// -- Replaying ------------------------------------------------------------

std::expected<oid, replay_failure> replay(const repo& repository, const edit_plan& plan)
{
    git_repository* r = repository.raw();
    auto fail = [](std::string message, oid at = {}) -> std::unexpected<replay_failure>
    {
        return std::unexpected(
            replay_failure{.conflict = false, .commit = at, .message = std::move(message)});
    };

    // Nothing in SVN is rewritten, before anything's made: a conflict
    // further down mustn't offer the rebase window instead.
    for (const edit_step& step : plan.steps)
    {
        git_oid cid = to_git(step.commit);
        owned_commit c;
        if (git_commit_lookup(&c.p, r, &cid) != 0) return fail(last_git_error(), step.commit);
        if (const char* m = git_commit_message(c.p); m && in_svn(m))
            return fail(_("in SVN"), step.commit);
    }

    owned_signature committer;
    if (git_signature_default(&committer.p, r) != 0) return fail(no_identity());

    // Where the replay stands: the last commit made (or kept), and its tree.
    std::optional<oid> cur = plan.base;
    std::optional<oid> cur_tree;
    if (cur)
    {
        cur_tree = tree_of(r, *cur);
        if (!cur_tree) return fail(last_git_error(), *cur);
    }

    for (const edit_step& step : plan.steps)
    {
        git_oid cid = to_git(step.commit);
        owned_commit c;
        if (git_commit_lookup(&c.p, r, &cid) != 0) return fail(last_git_error(), step.commit);
        const unsigned parents = git_commit_parentcount(c.p);
        const std::optional<oid> parent0 =
            parents ? std::optional(to_oid(git_commit_parent_id(c.p, 0))) : std::nullopt;
        std::optional<oid> ancestor_tree;
        if (parent0)
        {
            ancestor_tree = tree_of(r, *parent0);
            if (!ancestor_tree) return fail(last_git_error(), step.commit);
        }
        const oid their_tree = to_oid(git_commit_tree_id(c.p));
        if (step.fold && !cur) return fail(_("nothing to fold into"), step.commit);

        // The commit's change on the tree so far. Where that tree is the
        // commit's parent's, the result is the commit's own tree: combining
        // and rewording never merge, so never conflict.
        oid new_tree;
        if (ancestor_tree == cur_tree)
            new_tree = their_tree;
        else if (ancestor_tree && their_tree == *ancestor_tree)
            new_tree = *cur_tree;  // an empty change
        else if (parents > 1)
            return fail(_("A merge can't be replayed here; use the rebase window."), step.commit);
        else
        {
            owned_tree ancestor, ours, theirs;
            if (ancestor_tree)
            {
                git_oid t = to_git(*ancestor_tree);
                if (git_tree_lookup(&ancestor.p, r, &t) != 0)
                    return fail(last_git_error(), step.commit);
            }
            if (cur_tree)
            {
                git_oid t = to_git(*cur_tree);
                if (git_tree_lookup(&ours.p, r, &t) != 0)
                    return fail(last_git_error(), step.commit);
            }
            if (git_commit_tree(&theirs.p, c.p) != 0) return fail(last_git_error(), step.commit);
            git_merge_options opts = GIT_MERGE_OPTIONS_INIT;
            owned_index merged;
            if (git_merge_trees(&merged.p, r, ancestor.p, ours.p, theirs.p, &opts) != 0)
                return fail(last_git_error(), step.commit);
            if (git_index_has_conflicts(merged.p))
            {
                const char* summary = git_commit_summary(c.p);
                return std::unexpected(replay_failure{
                    .conflict = true,
                    .commit = step.commit,
                    .message =
                        rap::i18n::format(_("{0} \"{1}\" conflicts with the commits below it."),
                                          step.commit.hex(8), summary ? summary : "")});
            }
            git_oid written;
            if (git_index_write_tree_to(&written, merged.p, r) != 0)
                return fail(last_git_error(), step.commit);
            new_tree = to_oid(&written);
        }

        // The commit made: picked onto the last one, or folded into it.
        owned_commit base_commit;  // what the new commit's author and parents come from
        git_commit* from = c.p;
        std::vector<oid> new_parents;
        if (step.fold)
        {
            git_oid last = to_git(*cur);
            if (git_commit_lookup(&base_commit.p, r, &last) != 0)
                return fail(last_git_error(), *cur);
            from = base_commit.p;
            for (unsigned i = 0, n = git_commit_parentcount(from); i < n; ++i)
                new_parents.push_back(to_oid(git_commit_parent_id(from, i)));
        }
        else
        {
            if (cur) new_parents.push_back(*cur);
            for (unsigned i = 1; i < parents; ++i)
                new_parents.push_back(to_oid(git_commit_parent_id(c.p, i)));
            // Unchanged below and kept as it is: the same commit, not a copy.
            if (!step.message && cur == parent0)
            {
                cur = step.commit;
                cur_tree = their_tree;
                continue;
            }
        }
        const std::string message = step.message ? *step.message : git_commit_message(from);
        git_oid tree_id = to_git(new_tree);
        owned_tree tree;
        if (git_tree_lookup(&tree.p, r, &tree_id) != 0) return fail(last_git_error(), step.commit);
        std::vector<owned_commit> parent_objects(new_parents.size());
        std::vector<const git_commit*> parent_ptrs;
        for (size_t i = 0; i < new_parents.size(); ++i)
        {
            git_oid p = to_git(new_parents[i]);
            if (git_commit_lookup(&parent_objects[i].p, r, &p) != 0)
                return fail(last_git_error(), step.commit);
            parent_ptrs.push_back(parent_objects[i].p);
        }
        git_oid made;
        if (git_commit_create(&made, r, nullptr, git_commit_author(from), committer.p,
                              git_commit_message_encoding(from), message.c_str(), tree.p,
                              parent_ptrs.size(), parent_ptrs.data()) != 0)
            return fail(last_git_error(), step.commit);
        cur = to_oid(&made);
        cur_tree = new_tree;
    }
    if (!cur) return fail(_("Nothing would be left on the branch."));
    return *cur;
}

// -- Applying -------------------------------------------------------------

history_edit history_edit::undo() const
{
    history_edit u = *this;
    u.reflog = rap::i18n::format("tortilla: undo {0}",
                                 reflog.starts_with("tortilla: ") ? reflog.substr(10) : reflog);
    u.pushed = false;
    std::ranges::reverse(u.moves);
    for (ref_move& m : u.moves) std::swap(m.from, m.to);
    return u;
}

namespace
{

// The move of the branch this working tree has checked out, if any.
const ref_move* head_move(git_repository* r, const history_edit& e)
{
    owned_ref head;
    if (git_repository_head(&head.p, r) != 0 || !git_reference_is_branch(head.p)) return nullptr;
    const std::string name = git_reference_name(head.p);
    for (const ref_move& m : e.moves)
        if (m.ref == name) return &m;
    return nullptr;
}

// Tracked files changed, staged or not (untracked ones don't count: a
// checkout leaves them, or refuses to overwrite them).
bool has_local_changes(git_repository* r)
{
    git_status_options opts = GIT_STATUS_OPTIONS_INIT;
    opts.show = GIT_STATUS_SHOW_INDEX_AND_WORKDIR;
    opts.flags = GIT_STATUS_OPT_EXCLUDE_SUBMODULES;
    owned_status list;
    if (git_status_list_new(&list.p, r, &opts) != 0) return true;
    return git_status_list_entrycount(list.p) > 0;
}

// Every move, or none: each ref locked and checked against what it was.
std::expected<void, std::string> move_refs(git_repository* r, const history_edit& e)
{
    owned_transaction tx;
    if (git_transaction_new(&tx.p, r) != 0) return std::unexpected(last_git_error());
    for (const ref_move& m : e.moves)
        if (git_transaction_lock_ref(tx.p, m.ref.c_str()) != 0)
            return std::unexpected(last_git_error());
    for (const ref_move& m : e.moves)
    {
        git_oid now;
        const bool exists = git_reference_name_to_id(&now, r, m.ref.c_str()) == 0;
        const bool same = m.from ? exists && to_oid(&now) == *m.from : !exists;
        if (!same)
            return std::unexpected(
                rap::i18n::format(m.from ? _("{0} has moved since; nothing was changed.")
                                         : _("{0} exists already; nothing was changed."),
                                  short_name(m.ref)));
        if (m.to)
        {
            git_oid to = to_git(*m.to);
            if (git_transaction_set_target(tx.p, m.ref.c_str(), &to, nullptr, e.reflog.c_str()) !=
                0)
                return std::unexpected(last_git_error());
        }
        else if (git_transaction_remove(tx.p, m.ref.c_str()) != 0)
            return std::unexpected(last_git_error());
    }
    if (git_transaction_commit(tx.p) != 0) return std::unexpected(last_git_error());
    return {};
}

std::string stash_index(const repo& repository, const std::string& sha)
{
    for (const repo::stash_entry& s : repository.stashes())
        if (s.id.hex() == sha) return s.selector;
    return {};
}

}  // namespace

tree_effect effect_of(const repo& repository, const history_edit& e)
{
    if (e.keep_index) return tree_effect::none;
    git_repository* r = repository.raw();
    const ref_move* m = head_move(r, e);
    if (!m || !m->from || !m->to) return tree_effect::none;
    if (tree_of(r, *m->from) == tree_of(r, *m->to)) return tree_effect::none;
    return has_local_changes(r) ? tree_effect::stash : tree_effect::follow;
}

namespace
{

// A commit in SVN that a move leaves behind (on `from`, not on `to`), if
// any: moving past it would rewrite SVN's history.
std::optional<oid> svn_left_behind(git_repository* r, const ref_move& m)
{
    if (!m.from || !m.to) return std::nullopt;  // made or deleted: nothing rewritten
    owned_walk walk;
    git_oid from = to_git(*m.from), to = to_git(*m.to);
    if (git_revwalk_new(&walk.p, r) != 0 || git_revwalk_push(walk.p, &from) != 0)
        return std::nullopt;
    git_revwalk_hide(walk.p, &to);
    git_oid id;
    for (size_t n = 0; n < history_limit && git_revwalk_next(&id, walk.p) == 0; ++n)
    {
        owned_commit c;
        if (git_commit_lookup(&c.p, r, &id) != 0) continue;
        if (const char* msg = git_commit_message(c.p); msg && in_svn(msg)) return to_oid(&id);
    }
    return std::nullopt;
}

}  // namespace

std::expected<std::string, std::string> apply_edit(const repo& repository, const history_edit& e,
                                                   bool stash)
{
    git_repository* r = repository.raw();
    for (const ref_move& m : e.moves)
        if (const auto at = svn_left_behind(r, m))
            return std::unexpected(rap::i18n::format(
                _("{0} is in SVN: SVN's history can't be rewritten."), at->hex(8)));
    const tree_effect effect = effect_of(repository, e);
    if (effect == tree_effect::stash && !stash)
        return std::unexpected(std::string(_("The working tree has local changes.")));
    if (effect != tree_effect::none && repository.state() != repo::repo_state::none)
        return std::unexpected(std::string(_("Git is in the middle of another operation here.")));
    const std::string workdir = repository.workdir();

    std::string stashed;
    auto restore = [&]() -> std::string
    {
        if (stashed.empty()) return {};
        const git_result applied = run_git(workdir, {"stash", "apply", "--index", stashed});
        if (!applied.ok())
            return rap::i18n::format(_("Your local changes didn't go back cleanly; they're kept in "
                                       "the stash ({0}).\n{1}"),
                                     stashed.substr(0, 8), applied.output);
        if (const std::string at = stash_index(repository, stashed); !at.empty())
            run_git(workdir, {"stash", "drop", "-q", at});
        return {};
    };
    if (effect == tree_effect::stash)
    {
        const git_result pushed = run_git(workdir, {"stash", "push", "-m", e.reflog});
        if (!pushed.ok()) return std::unexpected(pushed.output);
        // The stash just made, in process.
        if (const auto made = repository.resolve("refs/stash")) stashed = made->hex();
    }

    if (auto moved = move_refs(r, e); !moved)
    {
        const std::string warning = restore();
        return std::unexpected(moved.error() + (warning.empty() ? "" : "\n" + warning));
    }
    if (effect != tree_effect::none)
    {
        // The index and files from the old tip's tree to the new one's;
        // git refuses where that would lose a change (an untracked file in
        // the way), and then the refs go back.
        const ref_move* m = head_move(r, e);
        const git_result read =
            run_git(workdir, {"read-tree", "-m", "-u", m->from->hex(), m->to->hex()});
        if (!read.ok())
        {
            (void)move_refs(r, e.undo());
            const std::string warning = restore();
            return std::unexpected(read.output + (warning.empty() ? "" : "\n" + warning));
        }
    }
    return restore();
}

// -- Messages -------------------------------------------------------------

std::string combined_message(const repo& repository, const std::vector<oid>& oldest_first)
{
    const std::vector<std::string> separators = squash_separators(oldest_first.size());
    std::string out = separators[0] + "\n";
    for (size_t i = 0; i < oldest_first.size(); ++i)
    {
        const auto c = repository.lookup(oldest_first[i]);
        out += separators[i + 1] + "\n\n";
        out += trimmed(c ? std::string_view(c->message) : std::string_view());
        out += i + 1 < oldest_first.size() ? "\n\n" : "\n";
    }
    return out;
}

std::string strip_squash_separators(const std::string& message, size_t count)
{
    const std::vector<std::string> separators = squash_separators(count);
    std::string out;
    size_t start = 0;
    while (start < message.size())
    {
        size_t end = message.find('\n', start);
        const size_t next = end == std::string::npos ? message.size() : end + 1;
        std::string_view line = std::string_view(message).substr(start, next - start);
        if (!std::ranges::contains(separators, trimmed(line))) out += line;
        start = next;
    }
    return out;
}

std::string tidy_message(const std::string& message)
{
    std::string out;
    size_t start = 0;
    while (start <= message.size())
    {
        size_t end = message.find('\n', start);
        if (end == std::string::npos) end = message.size();
        std::string_view line = std::string_view(message).substr(start, end - start);
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r'))
            line.remove_suffix(1);
        // As git's stripspace: no leading blank lines, and a run of blank
        // lines (where the squash separators were, say) only one.
        if (!line.empty() || (!out.empty() && !out.ends_with("\n\n")))
            out.append(line).push_back('\n');
        start = end + 1;
    }
    while (out.size() > 1 && out.ends_with("\n\n")) out.pop_back();
    return out;
}

// -- Plans ----------------------------------------------------------------

namespace
{

// The line's commits from index `from` up to the tip, picked, oldest first,
// leaving out `skip`.
void pick_above(const branch_history& h, size_t from, std::vector<edit_step>& steps,
                const std::unordered_set<oid, oid_hash>& skip = {})
{
    for (size_t i = from; i-- > 0;)
        if (!skip.contains(h.line[i])) steps.push_back({.commit = h.line[i]});
}

std::expected<std::vector<size_t>, std::string> indexes(const branch_history& h,
                                                        const std::vector<oid>& commits)
{
    std::vector<size_t> out;
    for (const oid& c : commits)
    {
        const auto i = h.index_of(c);
        if (!i) return std::unexpected(rap::i18n::format(_("not on {0}"), h.name()));
        out.push_back(*i);
    }
    std::ranges::sort(out);
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

}  // namespace

std::expected<edit_plan, std::string>
plan_combine(const branch_history& h, const std::vector<oid>& commits, const std::string& message)
{
    if (auto why = why_not_editable(h, commits, false)) return std::unexpected(*why);
    auto idx = indexes(h, commits);
    if (!idx) return std::unexpected(idx.error());
    if (idx->size() < 2) return std::unexpected(std::string(_("needs two or more commits")));
    const size_t newest = idx->front(), oldest = idx->back();
    if (oldest - newest + 1 != idx->size()) return std::unexpected(std::string(_("not adjacent")));
    edit_plan p{.branch = h.branch, .old_tip = h.tip, .base = h.parent_of(oldest)};
    p.steps.push_back({.commit = h.line[oldest], .message = message});
    for (size_t i = oldest; i-- > newest;) p.steps.push_back({.commit = h.line[i], .fold = true});
    pick_above(h, newest, p.steps);
    return p;
}

std::expected<edit_plan, std::string> plan_reword(const branch_history& h, const oid& commit,
                                                  const std::string& message)
{
    if (auto why = why_not_editable(h, {commit}, false)) return std::unexpected(*why);
    const size_t i = *h.index_of(commit);
    edit_plan p{.branch = h.branch, .old_tip = h.tip, .base = h.parent_of(i)};
    p.steps.push_back({.commit = commit, .message = message});
    pick_above(h, i, p.steps);
    return p;
}

std::expected<edit_plan, std::string> plan_drop(const branch_history& h,
                                                const std::vector<oid>& commits)
{
    if (auto why = why_not_editable(h, commits, true)) return std::unexpected(*why);
    auto idx = indexes(h, commits);
    if (!idx || idx->empty())
        return std::unexpected(idx ? std::string(_("no commits")) : idx.error());
    const size_t oldest = idx->back();
    if (oldest + 1 == h.line.size() && !h.below && idx->size() == h.line.size())
        return std::unexpected(std::string(_("nothing would be left")));
    edit_plan p{.branch = h.branch, .old_tip = h.tip, .base = h.parent_of(oldest)};
    pick_above(h, oldest, p.steps, {commits.begin(), commits.end()});
    return p;
}

std::expected<edit_plan, std::string> plan_fold(const branch_history& h, const oid& commit,
                                                const oid& into, std::optional<std::string> message)
{
    if (auto why = why_not_editable(h, {commit, into}, true)) return std::unexpected(*why);
    const size_t ic = *h.index_of(commit), ii = *h.index_of(into);
    if (ii <= ic) return std::unexpected(std::string(_("not an earlier commit")));
    edit_plan p{.branch = h.branch, .old_tip = h.tip, .base = h.parent_of(ii)};
    p.steps.push_back({.commit = into, .message = std::move(message)});
    p.steps.push_back({.commit = commit, .fold = true});
    pick_above(h, ii, p.steps, {commit});
    return p;
}

std::expected<edit_plan, std::string> plan_amend(const branch_history& h, const oid& staged,
                                                 const oid& into)
{
    if (auto why = why_not_editable(h, {into}, true)) return std::unexpected(*why);
    if (!h.checked_out) return std::unexpected(std::string(_("not checked out")));
    const size_t ii = *h.index_of(into);
    edit_plan p{.branch = h.branch, .old_tip = h.tip, .base = h.parent_of(ii)};
    p.steps.push_back({.commit = into});
    p.steps.push_back({.commit = staged, .fold = true});
    pick_above(h, ii, p.steps);
    return p;
}

std::expected<edit_plan, std::string>
plan_reorder(const branch_history& h, const std::vector<oid>& moved, size_t destination)
{
    auto idx = indexes(h, moved);
    if (!idx || idx->empty())
        return std::unexpected(idx ? std::string(_("no commits")) : idx.error());
    if (destination > h.line.size())
        return std::unexpected(rap::i18n::format(_("not on {0}"), h.name()));
    // The new line, newest first: the rest with the moved ones in front of
    // `destination`.
    std::vector<oid> order;
    std::vector<oid> block;
    for (size_t i : *idx) block.push_back(h.line[i]);
    for (size_t i = 0; i <= h.line.size(); ++i)
    {
        if (i == destination) order.insert(order.end(), block.begin(), block.end());
        if (i < h.line.size() && !std::ranges::binary_search(*idx, i)) order.push_back(h.line[i]);
    }
    // The deepest place it differs: what's below stays as it is.
    size_t deepest = h.line.size();
    for (size_t i = h.line.size(); i-- > 0;)
        if (order[i] != h.line[i])
        {
            deepest = i;
            break;
        }
    if (deepest == h.line.size()) return std::unexpected(std::string(_("nothing moves")));
    std::vector<oid> rewritten(h.line.begin(), h.line.begin() + deepest + 1);
    if (auto why = why_not_editable(h, rewritten, true)) return std::unexpected(*why);
    edit_plan p{.branch = h.branch, .old_tip = h.tip, .base = h.parent_of(deepest)};
    for (size_t i = deepest + 1; i-- > 0;) p.steps.push_back({.commit = order[i]});
    return p;
}

std::expected<oid, std::string> staged_commit(const repo& repository)
{
    git_repository* r = repository.raw();
    owned_index index;
    if (git_repository_index(&index.p, r) != 0) return std::unexpected(last_git_error());
    // The repository keeps its index loaded: read again what git wrote since.
    if (git_index_read(index.p, true) != 0) return std::unexpected(last_git_error());
    if (git_index_has_conflicts(index.p)) return std::unexpected(std::string(_("conflicts")));
    git_oid tree_id;
    if (git_index_write_tree(&tree_id, index.p) != 0) return std::unexpected(last_git_error());
    const auto head = repository.head_id();
    if (!head) return std::unexpected(std::string(_("no commits")));
    if (tree_of(r, *head) == to_oid(&tree_id))
        return std::unexpected(std::string(_("nothing staged")));
    owned_signature me;
    if (git_signature_default(&me.p, r) != 0) return std::unexpected(no_identity());
    owned_tree tree;
    owned_commit parent;
    git_oid head_id = to_git(*head);
    if (git_tree_lookup(&tree.p, r, &tree_id) != 0 ||
        git_commit_lookup(&parent.p, r, &head_id) != 0)
        return std::unexpected(last_git_error());
    const git_commit* parents[] = {parent.p};
    git_oid made;
    if (git_commit_create(&made, r, nullptr, me.p, me.p, nullptr, "Staged changes\n", tree.p, 1,
                          parents) != 0)
        return std::unexpected(last_git_error());
    return to_oid(&made);
}

rebase_todo todo_for(const edit_plan& p)
{
    rebase_todo t{.branch = short_name(p.branch), .onto = p.base};
    for (auto it = p.steps.rbegin(); it != p.steps.rend(); ++it)
        t.rows.emplace_back(it->commit, it->fold);
    return t;
}

history_edit edit_from(const repo& repository, const branch_history& h, const edit_plan& p,
                       const oid& new_tip, std::string reflog)
{
    history_edit e{.reflog = std::move(reflog), .moves = {{h.branch, h.tip, new_tip}}};
    // The oldest commit rewritten: the one above the base.
    std::optional<size_t> above;
    if (p.base)
    {
        if (auto b = h.index_of(*p.base); b && *b > 0) above = *b - 1;
    }
    if (!above && !h.line.empty() && (!p.base || p.base == h.below)) above = h.line.size() - 1;
    e.pushed = above && rewrites_pushed(repository, h, h.line[*above]);
    if (e.pushed) e.lease = h.upstream_tip;
    return e;
}

std::expected<history_edit, replay_failure>
move_commits(const repo& repository, const branch_history& from, const std::vector<oid>& commits,
             const std::string& onto, bool new_branch, std::string reflog)
{
    auto refuse = [](std::string why) -> std::unexpected<replay_failure>
    { return std::unexpected(replay_failure{.message = std::move(why)}); };
    git_repository* r = repository.raw();
    if (auto why = why_not_editable(from, commits, true)) return refuse(*why);
    auto idx = indexes(from, commits);
    if (!idx || idx->empty()) return refuse(idx ? std::string(_("no commits")) : idx.error());
    const size_t newest = idx->front(), oldest = idx->back();
    if (oldest - newest + 1 != idx->size()) return refuse(_("not adjacent"));
    if (onto == from.branch) return refuse(_("the same branch"));

    edit_plan target{.branch = onto};
    ref_move target_move{.ref = onto};
    if (new_branch)
    {
        if (!git_reference_is_valid_name(onto.c_str()) || !onto.starts_with("refs/heads/"))
            return refuse(
                rap::i18n::format(_("\"{0}\" is not a valid branch name."), short_name(onto)));
        if (resolve_ref(r, onto))
            return refuse(rap::i18n::format(_("{0} exists already."), short_name(onto)));
        target.base = from.parent_of(oldest);
    }
    else
    {
        auto onto_history = load_branch_history(repository, onto, from.mainline);
        if (!onto_history) return refuse(onto_history.error());
        if (!onto_history->worktree.empty())
            return refuse(
                rap::i18n::format(_("{0} is checked out in another worktree."), short_name(onto)));
        target.base = target.old_tip = onto_history->tip;
        target_move.from = onto_history->tip;
    }
    for (size_t i = oldest + 1; i-- > newest;) target.steps.push_back({.commit = from.line[i]});
    auto moved = replay(repository, target);
    if (!moved) return std::unexpected(moved.error());
    target_move.to = *moved;

    auto left = plan_drop(from, commits);
    if (!left) return refuse(left.error());
    auto source = replay(repository, *left);
    if (!source) return std::unexpected(source.error());
    history_edit e = edit_from(repository, from, *left, *source, std::move(reflog));
    e.moves.push_back(target_move);
    return e;
}

std::expected<drop_place, std::string> drop_place_of(const repo& repository, const oid& row,
                                                     const std::vector<ref>& refs,
                                                     const std::string& mainline)
{
    std::vector<const ref*> tips;
    bool remote = false;
    for (const ref& r : refs)
    {
        if (r.kind == ref_kind::local_branch) tips.push_back(&r);
        remote = remote || r.kind == ref_kind::remote_branch;
    }
    std::ranges::stable_partition(tips, [](const ref* r) { return r->current; });
    if (tips.size() > 1 && !tips.front()->current)
        return std::unexpected(std::string(_("several branches end there")));
    if (!tips.empty())
    {
        auto h = load_branch_history(repository, tips.front()->full_name, mainline);
        if (!h) return std::unexpected(h.error());
        return drop_place{.onto = std::move(*h)};
    }
    auto h = branch_holding(repository, {row}, mainline);
    // The mainline's own, or a remote's: not a branch of ours to change.
    if (!h) return std::unexpected(remote ? std::string(_("not a local branch")) : h.error());
    const size_t place = *h->index_of(row);
    return drop_place{.onto = std::move(*h), .place = place};
}

std::expected<edit_plan, std::string> plan_insert(const repo& repository,
                                                  const branch_history& onto,
                                                  const std::vector<oid>& commits, size_t place)
{
    if (commits.empty()) return std::unexpected(std::string(_("no commits")));
    if (place > onto.line.size())
        return std::unexpected(rap::i18n::format(_("not on {0}"), onto.name()));
    if (!onto.worktree.empty()) return std::unexpected(std::string(_("in another worktree")));
    // Where they go: on line[place], or, at the bottom, where the line
    // leaves the mainline (an empty line: its tip).
    const std::optional<oid> base =
        place < onto.line.size() ? std::optional(onto.line[place]) : onto.below;
    for (const oid& c : commits)
    {
        const auto commit = repository.lookup(c);
        if (!commit) return std::unexpected(rap::i18n::format(_("{0} not found"), c.hex(8)));
        if (commit->parents.size() > 1) return std::unexpected(std::string(_("a merge")));
        if (onto.index_of(c) || (base && repository.is_ancestor(c, *base)))
            return std::unexpected(rap::i18n::format(_("on {0} already"), onto.name()));
    }
    // The commits above are replayed onto changed trees.
    if (place > 0)
        if (auto why = why_not_editable(
                onto, std::vector<oid>(onto.line.begin(), onto.line.begin() + place), true))
            return std::unexpected(*why);
    edit_plan p{.branch = onto.branch, .old_tip = onto.tip, .base = base};
    for (const oid& c : commits) p.steps.push_back({.commit = c});
    pick_above(onto, place, p.steps);
    return p;
}

std::expected<history_edit, replay_failure>
transplant_commits(const repo& repository, const branch_history& onto, size_t place,
                   const std::vector<oid>& commits, const branch_history* from, std::string reflog)
{
    auto refuse = [](std::string why) -> std::unexpected<replay_failure>
    { return std::unexpected(replay_failure{.message = std::move(why)}); };
    if (from && from->branch == onto.branch) return refuse(_("the same branch"));
    auto plan = plan_insert(repository, onto, commits, place);
    if (!plan) return refuse(plan.error());
    auto tip = replay(repository, *plan);
    if (!tip) return std::unexpected(tip.error());
    history_edit target = edit_from(repository, onto, *plan, *tip, reflog);
    if (!from) return target;

    auto left = plan_drop(*from, commits);
    if (!left) return refuse(left.error());
    auto source = replay(repository, *left);
    if (!source) return std::unexpected(source.error());
    history_edit e = edit_from(repository, *from, *left, *source, std::move(reflog));
    // The pushed branch first: it's the one offered a force push, leasing
    // on its upstream.
    if (target.pushed && !e.pushed)
    {
        e.moves.insert(e.moves.begin(), target.moves.front());
        e.lease = target.lease;
    }
    else
        e.moves.push_back(target.moves.front());
    e.pushed = e.pushed || target.pushed;
    return e;
}

std::vector<std::string> force_push_args(const repo& repository, const std::string& branch,
                                         const oid& expected, const oid& tip)
{
    git_repository* r = repository.raw();
    git_buf remote{}, merge{};
    std::vector<std::string> out;
    if (git_branch_upstream_remote(&remote, r, branch.c_str()) == 0 &&
        git_branch_upstream_merge(&merge, r, branch.c_str()) == 0 && remote.size &&
        std::string_view(remote.ptr, remote.size) != ".")
    {
        const std::string there(merge.ptr, merge.size);
        out = {"push", "--force-with-lease=" + there + ":" + expected.hex(),
               std::string(remote.ptr, remote.size), tip.hex() + ":" + there};
    }
    git_buf_dispose(&remote);
    git_buf_dispose(&merge);
    return out;
}

std::expected<history_edit, std::string> undo_last_commit(const repo& repository,
                                                          const branch_history& h)
{
    if (!h.checked_out) return std::unexpected(std::string(_("not checked out")));
    if (h.line.empty()) return std::unexpected(std::string(_("merged")));
    if (h.is_merge(0)) return std::unexpected(std::string(_("a merge")));
    if (!h.worktree.empty()) return std::unexpected(std::string(_("in another worktree")));
    if (!h.svn.empty() && h.svn.front() == 0) return std::unexpected(std::string(_("in SVN")));
    const auto parent = h.parent_of(0);
    if (!parent) return std::unexpected(std::string(_("the first commit")));
    const bool pushed = rewrites_pushed(repository, h, h.tip);
    return history_edit{.reflog = "tortilla: undo last commit",
                        .moves = {{h.branch, h.tip, *parent}},
                        .keep_index = true,
                        .pushed = pushed,
                        .lease = pushed ? h.upstream_tip : std::nullopt};
}

}  // namespace tortilla
