// The log's history editing [git/history_edit.h]: a commit's menu combines,
// rewords, drops, folds and moves commits of its branch in place, without a
// rebase, and dragging commits reorders them. The edit is made on a worker
// and applied there; the status bar offers Undo for a while after.
#include "log_window.h"

#include "log/branch_filter.h"
#include "log/history_dialogs.h"

#include "git/svn.h"
#include "i18n.h"
#include "ui/menu_items.h"
#include "ui/menu_text.h"
#include "ui/progress_dialog.h"

#include <algorithm>

namespace tortilla
{
namespace
{

using rap::menu;
using rap::menu_item;

std::string short_branch(std::string_view full)
{
    for (std::string_view p : {"refs/heads/", "refs/remotes/"})
        if (full.starts_with(p)) return std::string(full.substr(p.size()));
    return std::string(full);
}

// An action greyed, with why in its label: "_Drop commit... (merged)".
menu_item maybe(const std::string& label, const std::optional<std::string>& why,
                std::function<void()> action)
{
    if (why) return item(rap::i18n::format(_("{0} ({1})"), label, *why), {}, false);
    return item(label, std::move(action));
}

// The plan replayed and made the branch's edit, on the worker.
std::function<std::expected<history_edit, replay_failure>(const repo&)>
from_plan(branch_history h, std::expected<edit_plan, std::string> plan, std::string reflog)
{
    return [h = std::move(h), plan = std::move(plan), reflog = std::move(reflog)](
               const repo& r) -> std::expected<history_edit, replay_failure>
    {
        if (!plan) return std::unexpected(replay_failure{.message = plan.error()});
        auto tip = replay(r, *plan);
        if (!tip) return std::unexpected(tip.error());
        return edit_from(r, h, *plan, *tip, reflog);
    };
}

std::optional<rebase_todo> fallback_of(const std::expected<edit_plan, std::string>& plan)
{
    if (!plan || !plan->base) return std::nullopt;
    return todo_for(*plan);
}

std::string commit_label(const repo& r, const oid& id)
{
    const auto c = r.lookup(id);
    return id.hex(8) + " " + (c ? c->summary : std::string());
}

}  // namespace

std::string log_window::mainline_ref() const
{
    return merge_base_branch(repo_, show_.pins, {});
}

std::string log_window::pushed_warning(const branch_history& h) const
{
    return rap::i18n::format(
        _("Some of these commits are on {0} already: after this, {1} needs a force push."),
        short_branch(h.upstream), h.name());
}

void log_window::history_items(menu& m, const std::vector<size_t>& sel)
{
    if (repo_.bare() || picked_) return;
    // Commits only, oldest last as the rows run.
    std::vector<oid> commits;
    for (size_t row : sel)
    {
        const commit* c = log_.commit(row);
        if (!c) return;  // the working tree row
        if (const std::vector<ref>* refs = log_.refs_of(row))
            for (const ref& r : *refs)
                if (r.kind == ref_kind::stash) return;
        commits.push_back(c->id);
    }
    if (commits.empty()) return;

    auto found = branch_holding(repo_, commits, mainline_ref());
    std::optional<std::string> why;
    if (!found)
        why = found.error();
    else if (found->checked_out)
        switch (repo_.state())
        {
        case repo::repo_state::merge: why = _("merging"); break;
        case repo::repo_state::rebase:
        case repo::repo_state::apply_mailbox: why = _("rebasing"); break;
        case repo::repo_state::cherry_pick: why = _("cherry-picking"); break;
        case repo::repo_state::revert: why = _("reverting"); break;
        default: break;
        }
    // A commit already in SVN (git svn's) can't be rewritten: the next
    // dcommit or rebase would break.
    auto in_svn_any = [&](const std::vector<oid>& which)
    {
        return std::ranges::any_of(which,
                                   [&](const oid& id)
                                   {
                                       const auto c = repo_.lookup(id);
                                       return c && in_svn(c->message);
                                   });
    };
    auto why_not = [&](const std::vector<oid>& which, bool replays) -> std::optional<std::string>
    {
        if (why) return why;
        if (in_svn_any(which)) return std::string(_("in SVN"));
        return why_not_editable(*found, which, replays);
    };
    const branch_history h = found ? *found : branch_history{};
    // Rewriting from the oldest selected up: on the upstream already?
    size_t oldest = 0;
    for (const oid& c : commits) oldest = std::max(oldest, h.index_of(c).value_or(0));
    const bool pushed = found && rewrites_pushed(repo_, h, h.line[oldest]);
    const std::string warning = pushed ? pushed_warning(h) : std::string();
    const size_t n = commits.size();

    if (n == 1)
    {
        const oid c = commits.front();
        const auto at = found ? h.index_of(c) : std::nullopt;
        const std::string hash = c.hex(8);

        m.push_back(maybe(
            _("Re_word..."), why_not({c}, false),
            [this, h, c, hash, warning]
            {
                const auto commit = repo_.lookup(c);
                history_message_dialog(
                    ui_, window_.get(), _("Reword"),
                    rap::i18n::format(_("The message of {0}:"), commit_label(repo_, c)),
                    commit ? commit->message : std::string(), warning,
                    [this, h, c, hash](std::string text)
                    {
                        text = tidy_message(text);
                        if (text.empty())
                            return ui_.message(window_.get(),
                                               {.icon = rap::message_spec::icon::error,
                                                .title = _("Reword"),
                                                .text = _("A commit needs a message.")});
                        start_history_edit(_("Reword"), from_plan(h, plan_reword(h, c, text),
                                                                  "tortilla: reword " + hash));
                    });
            }));

        // Into the commit below: its message and this one's, to edit.
        std::optional<std::string> below_why = why_not({c}, false);
        if (!below_why && (!at || *at + 1 >= h.line.size())) below_why = _("merged below");
        if (!below_why) below_why = why_not({c, h.line[*at + 1]}, false);
        m.push_back(maybe(
            _("S_quash into the commit below..."), below_why,
            [this, h, c, at, warning]
            {
                const oid into = h.line[*at + 1];
                history_message_dialog(
                    ui_, window_.get(), _("Squash into the Commit Below"),
                    rap::i18n::format(_("The message of {0} with {1} squashed in:"),
                                      commit_label(repo_, into), c.hex(8)),
                    combined_message(repo_, {into, c}), warning,
                    [this, h, c, into](std::string text)
                    {
                        text = tidy_message(strip_squash_separators(text, 2));
                        if (text.empty())
                            return ui_.message(window_.get(),
                                               {.icon = rap::message_spec::icon::error,
                                                .title = _("Squash"),
                                                .text = _("A commit needs a message.")});
                        start_history_edit(_("Squash"), from_plan(h, plan_fold(h, c, into, text),
                                                                  "tortilla: squash " + c.hex(8) +
                                                                      " into " + into.hex(8)));
                    });
            }));

        // Fixup into an earlier commit: this one's changes, that one's message.
        menu_item fixup{.label = _("Fi_xup into")};
        std::optional<std::string> fixup_why = why;
        if (!fixup_why && at && *at + 1 >= h.line.size()) fixup_why = _("merged below");
        if (fixup_why)
            fixup = maybe(fixup.label, fixup_why, {});
        else if (at)
        {
            for (size_t i = *at + 1; i < h.line.size() && i <= *at + 15; ++i)
            {
                const oid into = h.line[i];
                const auto not_here = why_not({c, into}, true);
                menu_item child =
                    maybe(menu_text(commit_label(repo_, into)), not_here,
                          [this, h, c, into, warning]
                          {
                              const auto plan = plan_fold(h, c, into);
                              confirm_history_edit(_("Fixup"), {}, !warning.empty(), h,
                                                   [this, h, c, into, plan]
                                                   {
                                                       start_history_edit(
                                                           _("Fixup"),
                                                           from_plan(h, plan,
                                                                     "tortilla: fixup " + c.hex(8) +
                                                                         " into " + into.hex(8)),
                                                           fallback_of(plan));
                                                   });
                          });
                fixup.children.push_back(std::move(child));
            }
        }
        m.push_back(std::move(fixup));

        // The index folded into this commit.
        std::optional<std::string> amend_why = why_not({c}, true);
        if (!amend_why && !h.checked_out) amend_why = _("not checked out");
        if (!amend_why) amend_why = why_nothing_staged(repo_);
        m.push_back(maybe(
            _("A_mend with staged changes"), amend_why,
            [this, h, c, hash, pushed]
            {
                confirm_history_edit(
                    _("Amend with Staged Changes"), {}, pushed, h,
                    [this, h, c, hash]
                    {
                        start_history_edit(
                            _("Amend with Staged Changes"),
                            [h, c,
                             hash](const repo& r) -> std::expected<history_edit, replay_failure>
                            {
                                auto staged = staged_commit(r);
                                if (!staged)
                                    return std::unexpected(
                                        replay_failure{.message = staged.error()});
                                auto plan = plan_amend(h, *staged, c);
                                if (!plan)
                                    return std::unexpected(replay_failure{.message = plan.error()});
                                auto tip = replay(r, *plan);
                                // The index must be what the new tip has, or
                                // it would undo or redo changes.
                                if (!tip || !same_tree(r, *tip, *staged))
                                    return std::unexpected(replay_failure{
                                        .message = _("The staged changes don't fold into that "
                                                     "commit cleanly: a later commit changes the "
                                                     "same lines. Commit them, then use Fixup "
                                                     "into.")});
                                history_edit e =
                                    edit_from(r, h, *plan, *tip,
                                              "tortilla: amend " + hash + " with staged changes");
                                e.keep_index = true;
                                return e;
                            });
                    });
            }));
    }
    else
    {
        // Several: one commit, if they're adjacent.
        std::optional<std::string> combine_why = why_not(commits, false);
        if (!combine_why)
        {
            std::vector<size_t> idx;
            for (const oid& c : commits) idx.push_back(*h.index_of(c));
            std::ranges::sort(idx);
            if (idx.back() - idx.front() + 1 != idx.size()) combine_why = _("not adjacent");
        }
        m.push_back(
            maybe(_("_Combine to one commit..."), combine_why,
                  [this, h, commits, warning, n]
                  {
                      std::vector<oid> oldest_first;
                      for (auto it = h.line.rbegin(); it != h.line.rend(); ++it)
                          if (std::ranges::contains(commits, *it)) oldest_first.push_back(*it);
                      history_message_dialog(
                          ui_, window_.get(), _("Combine to One Commit"),
                          rap::i18n::format(N_("{0} commit becomes one, with this message:",
                                               "{0} commits become one, with this message:", n),
                                            n),
                          combined_message(repo_, oldest_first), warning,
                          [this, h, commits, n](std::string text)
                          {
                              text = tidy_message(strip_squash_separators(text, n));
                              if (text.empty())
                                  return ui_.message(window_.get(),
                                                     {.icon = rap::message_spec::icon::error,
                                                      .title = _("Combine to One Commit"),
                                                      .text = _("A commit needs a message.")});
                              start_history_edit(
                                  _("Combine to One Commit"),
                                  from_plan(h, plan_combine(h, commits, text),
                                            "tortilla: combine " + std::to_string(n) + " commits"));
                          });
                  }));
    }

    // Drop, with a question.
    m.push_back(maybe(
        n == 1 ? _("_Drop commit...") : _("_Drop commits..."), why_not(commits, true),
        [this, h, commits, n, warning]
        {
            std::string question =
                n == 1 ? rap::i18n::format(_("Drop {0} from {1}? Its changes go with it."),
                                           commit_label(repo_, commits.front()), h.name())
                       : rap::i18n::format(
                             N_("Drop {0} commit from {1}? Their changes go with them.",
                                "Drop {0} commits from {1}? Their changes go with them.", n),
                             n, h.name());
            if (!warning.empty()) question += "\n\n" + warning;
            ui_.message(window_.get(),
                        {.icon = rap::message_spec::icon::question,
                         .title = _("Drop"),
                         .text = question,
                         .buttons = {rap::dialog_role::yes, rap::dialog_role::no}},
                        [this, h, commits, n](rap::dialog_role r)
                        {
                            if (r != rap::dialog_role::yes) return;
                            const auto plan = plan_drop(h, commits);
                            start_history_edit(_("Drop"),
                                               from_plan(h, plan,
                                                         "tortilla: drop " + std::to_string(n) +
                                                             (n == 1 ? " commit" : " commits")),
                                               fallback_of(plan));
                        });
        }));

    // Move to another branch (committed on the wrong one), or a new one.
    {
        std::optional<std::string> move_why = why_not(commits, true);
        if (!move_why)
        {
            std::vector<size_t> idx;
            for (const oid& c : commits) idx.push_back(*h.index_of(c));
            std::ranges::sort(idx);
            if (idx.back() - idx.front() + 1 != idx.size()) move_why = _("not adjacent");
        }
        m.push_back(maybe(
            _("Mo_ve commits to branch..."), move_why,
            [this, h, commits, n, warning]
            {
                std::vector<std::string> branches;
                for (const std::string& b : repo_.branch_names(true, false, false))
                    if (b != h.name()) branches.push_back(b);
                move_commits_dialog(
                    ui_, window_.get(),
                    rap::i18n::format(
                        N_("Move {0} commit off {1}, onto:", "Move {0} commits off {1}, onto:", n),
                        n, h.name()),
                    branches, warning,
                    [this, h, commits, n](move_target t)
                    {
                        const std::string reflog = "tortilla: move " + std::to_string(n) +
                                                   (n == 1 ? " commit" : " commits") + " to " +
                                                   t.branch;
                        start_history_edit(_("Move Commits to Branch"),
                                           [h, commits, t, reflog](const repo& r)
                                           {
                                               return move_commits(r, h, commits,
                                                                   "refs/heads/" + t.branch,
                                                                   t.create, reflog);
                                           });
                    });
            }));
    }

    // Undo last commit: HEAD's branch tip only.
    if (n == 1 && found && h.checked_out && !h.line.empty() && h.line.front() == commits.front())
    {
        std::optional<std::string> undo_why = why;
        if (!undo_why && in_svn_any({commits.front()})) undo_why = _("in SVN");
        if (!undo_why)
            if (auto e = undo_last_commit(repo_, h); !e) undo_why = e.error();
        m.push_back(maybe(
            _("_Undo last commit"), undo_why,
            [this, h, pushed]
            {
                confirm_history_edit(
                    _("Undo Last Commit"), _("Undo the last commit? Its changes stay, staged."),
                    pushed, h,
                    [this, h]
                    {
                        start_history_edit(
                            _("Undo Last Commit"),
                            [h](const repo& r) -> std::expected<history_edit, replay_failure>
                            {
                                auto e = undo_last_commit(r, h);
                                if (!e)
                                    return std::unexpected(replay_failure{.message = e.error()});
                                return *e;
                            });
                    });
            }));
    }
}

void log_window::confirm_history_edit(const std::string& title, const std::string& question,
                                      bool pushed, const branch_history& h,
                                      std::function<void()> go)
{
    std::string text = question;
    if (pushed) text += (text.empty() ? "" : "\n\n") + pushed_warning(h);
    if (text.empty()) return go();
    ui_.message(
        window_.get(),
        {.icon = pushed ? rap::message_spec::icon::warning : rap::message_spec::icon::question,
         .title = title,
         .text = text,
         .buttons = {rap::dialog_role::yes, rap::dialog_role::no}},
        [go = std::move(go)](rap::dialog_role r)
        {
            if (r == rap::dialog_role::yes) go();
        });
}

bool log_window::reorder_rows(const std::vector<size_t>& rows, size_t destination)
{
    if (picked_ || repo_.bare() || rows.empty()) return false;
    std::vector<oid> commits;
    for (size_t row : rows)
    {
        const commit* c = log_.commit(row);
        if (!c) return false;
        commits.push_back(c->id);
    }
    auto found = branch_holding(repo_, commits, mainline_ref());
    if (!found)
    {
        set_sync_notice(rap::i18n::format(_("Can't reorder: {0}"), found.error()), false);
        return false;
    }
    const branch_history h = std::move(*found);
    // Where they go, as a place in the branch's line: in front of the row
    // dropped on (the working tree row is above the tip).
    std::optional<size_t> place;
    if (destination < log_.row_count())
    {
        if (const commit* c = log_.commit(destination))
            place = c->id == h.below ? std::optional(h.line.size()) : h.index_of(c->id);
        else
            place = 0;
    }
    if (!place) return false;
    const auto plan = plan_reorder(h, commits, *place);
    if (!plan)
    {
        if (plan.error() != _("nothing moves"))
            set_sync_notice(rap::i18n::format(_("Can't reorder: {0}"), plan.error()), false);
        return false;
    }
    const size_t n = commits.size();
    const std::string where =
        *place < h.line.size()
            ? rap::i18n::format(_("above {0}"), commit_label(repo_, h.line[*place]))
            : rap::i18n::format(_("to the bottom of {0}"), h.name());
    const bool pushed =
        rewrites_pushed(repo_, h, h.line[std::min(*place, h.line.size() - 1)]) ||
        std::ranges::any_of(commits, [&](const oid& c) { return rewrites_pushed(repo_, h, c); });
    confirm_history_edit(
        _("Reorder"),
        rap::i18n::format(N_("Move {0} commit {1}?", "Move {0} commits {1}?", n), n, where), pushed,
        h,
        [this, h, plan, n]
        {
            start_history_edit(_("Reorder"),
                               from_plan(h, plan,
                                         "tortilla: reorder " + std::to_string(n) +
                                             (n == 1 ? " commit" : " commits")),
                               fallback_of(plan));
        });
    return false;  // the reload shows the new order
}

void log_window::start_history_edit(const std::string& title, edit_maker make,
                                    std::optional<rebase_todo> fallback, bool undoing)
{
    if (!edit_worker_) edit_worker_ = std::make_unique<repo_worker>(ui_, repo_.workdir());
    status_->set_text(rap::i18n::format(_("{0}..."), title));
    edit_worker_->submit(
        [this, title, make = std::move(make), fallback = std::move(fallback),
         undoing](repo& r, const repo_worker::progress&) -> std::function<void()>
        {
            auto e = make(r);
            const tree_effect effect = e ? effect_of(r, *e) : tree_effect::none;
            return [this, title, e = std::move(e), effect, fallback, undoing]
            { history_replayed(title, e, effect, fallback, undoing); };
        });
}

void log_window::history_replayed(const std::string& title,
                                  std::expected<history_edit, replay_failure> e, tree_effect effect,
                                  std::optional<rebase_todo> fallback, bool undoing)
{
    update_status();
    if (!e)
    {
        const replay_failure& f = e.error();
        if (f.conflict && fallback)
            return choose(title, f.message + "\n" + _("Nothing was changed."),
                          {{_("Continue in the _rebase window"),
                            _("The same edit a commit at a time, stopping at the conflict for you "
                              "to resolve it."),
                            [this, todo = *fallback] { open_rebase(todo.onto->hex(), &todo); }}});
        return ui_.message(
            window_.get(),
            {.icon = rap::message_spec::icon::error,
             .title = title,
             .text = f.message.empty() ? std::string(_("It can't be done.")) : f.message});
    }
    if (effect == tree_effect::stash)
        return choose(title,
                      _("This changes the checked-out branch's files, and the working tree has "
                        "local changes."),
                      {{_("_Stash them and go on"),
                        _("Your changes are put aside, the files follow the branch, and your "
                          "changes come back."),
                        [this, title, edit = *e, undoing]
                        { apply_history_edit(title, edit, true, undoing); }}});
    apply_history_edit(title, std::move(*e), false, undoing);
}

void log_window::apply_history_edit(const std::string& title, history_edit e, bool stash,
                                    bool undoing)
{
    if (!edit_worker_) edit_worker_ = std::make_unique<repo_worker>(ui_, repo_.workdir());
    edit_worker_->submit(
        [this, title, e = std::move(e), stash,
         undoing](repo& r, const repo_worker::progress&) -> std::function<void()>
        {
            auto applied = apply_edit(r, e, stash);
            return [this, title, e, applied = std::move(applied), undoing]
            {
                if (!applied)
                {
                    reload();
                    return ui_.message(window_.get(),
                                       {.icon = rap::message_spec::icon::error,
                                        .title = title,
                                        .text = rap::i18n::format(_("{0} failed."), title),
                                        .detail = applied.error()});
                }
                if (!applied->empty())
                    ui_.message(window_.get(), {.icon = rap::message_spec::icon::warning,
                                                .title = title,
                                                .text = *applied});
                if (undoing)
                {
                    last_edit_.reset();
                    undo_->set_visible(false);
                }
                else
                {
                    last_edit_ = e;
                    undo_->set_tooltip(rap::i18n::format(_("Undo: {0}"), title));
                    undo_->set_visible(true);
                    undo_timer_ = ui_.timer(60000, false,
                                            [this]
                                            {
                                                undo_->set_visible(false);
                                                last_edit_.reset();
                                            });
                    set_sync_notice(rap::i18n::format(_("{0}: done"), title), false);
                }
                reload();
                if (e.pushed && !undoing) offer_force_push(e);
            };
        });
}

void log_window::offer_force_push(const history_edit& e)
{
    const std::string branch = e.moves.front().ref;
    auto args = force_push_args(repo_, branch);
    if (args.empty()) return;
    ui_.message(window_.get(),
                {.icon = rap::message_spec::icon::question,
                 .title = _("Force Push"),
                 .text = rap::i18n::format(
                     _("{0} has been rewritten. Force-push it to {1}, if no one else has pushed "
                       "there since?"),
                     short_branch(branch), args[2]),
                 .detail = command_line(args),
                 .buttons = {rap::dialog_role::yes, rap::dialog_role::no}},
                [this, args](rap::dialog_role r)
                {
                    if (r == rap::dialog_role::yes) run_git_commands(_("Force Push"), {args});
                });
}

}  // namespace tortilla
