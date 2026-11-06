#include "journal_tests.h"

#include "git/ref_journal.h"
#include "shell.h"
#include "test_support.h"

#include <filesystem>

namespace tortilla::test
{
namespace
{

// The log's Undo and Redo (git/ref_journal.h): steps undone and redone in
// order, a new step dropping what was undone, the limit, and a step whose
// refs moved since refused without touching anything.
void ref_journal_tests(const std::string& root)
{
    const std::string dir = root + "/journal";
    const std::string g = "git -C '" + dir + "' ";
    sh("git init -q -b main '" + dir + "' && " + g + "config user.name Tester && " + g +
       "config user.email t@example.com && " + g + "config core.autocrlf false && " + g +
       "commit -q --allow-empty -m one && " + g + "commit -q --allow-empty -m two && " + g +
       "commit -q --allow-empty -m three && " + g + "branch topic HEAD~2 && " + g +
       "tag -a -m release v1 HEAD~1");
    auto r = repo::open(dir);
    check(r.has_value(), "journal: the repository opens");
    if (!r) return;
    const oid one = *r->resolve("HEAD~2"), two = *r->resolve("HEAD~1"), three = *r->resolve("HEAD");
    auto at = [&](const std::string& ref) { return ref_target(*r, ref); };
    const std::optional<oid> tag_object = at("refs/tags/v1");
    check(tag_object && *tag_object != two, "journal: a ref's own value, not peeled");

    ref_journal j(3);
    check(!j.next_undo() && !j.next_redo() && !j.undo_edit(), "journal: empty at first");
    // A branch moved (as a label dropped on another commit), then deleted.
    history_edit move{.reflog = "tortilla: move topic",
                      .moves = {{"refs/heads/topic", one, three}}};
    check(apply_edit(*r, move).has_value(), "journal: the move applies");
    j.record("Move topic", move);
    history_edit drop{.reflog = "tortilla: delete topic",
                      .moves = {{"refs/heads/topic", three, std::nullopt}}};
    check(apply_edit(*r, drop).has_value() && !at("refs/heads/topic"),
          "journal: the delete applies");
    j.record("Delete topic", drop);
    check(j.next_undo() && j.next_undo()->label == "Delete topic" && j.undo_count() == 2,
          "journal: the newest step is undone first");

    // Undo both, newest first.
    auto u = j.undo_edit();
    check(u && !why_stale(*r, *u) && apply_edit(*r, *u) && at("refs/heads/topic") == three,
          "journal: undoing a delete makes the branch again");
    j.undone();
    u = j.undo_edit();
    check(u && apply_edit(*r, *u) && at("refs/heads/topic") == one, "journal: and the move");
    j.undone();
    check(!j.next_undo() && j.next_redo() && j.next_redo()->label == "Move topic",
          "journal: redo has them, the first first");

    // Redo the move only; a new step then drops the delete's redo.
    auto d = j.redo_edit();
    check(d && !why_stale(*r, *d) && apply_edit(*r, *d) && at("refs/heads/topic") == three,
          "journal: redo moves it again");
    j.redone();
    history_edit retag{.reflog = "tortilla: move v1", .moves = {{"refs/tags/v1", tag_object, one}}};
    check(apply_edit(*r, retag).has_value(), "journal: a tag moves");
    j.record("Move v1", retag);
    check(!j.next_redo() && j.undo_count() == 2, "journal: a new step drops what was undone");
    u = j.undo_edit();
    check(u && apply_edit(*r, *u) && at("refs/tags/v1") == tag_object,
          "journal: undoing puts the annotated tag's own object back");
    j.undone();

    // Something else moved the branch: undoing the move under it is refused,
    // and nothing changes.
    sh(g + "branch -f topic HEAD~1");
    u = j.undo_edit();
    const auto stale = u ? why_stale(*r, *u) : std::nullopt;
    check(stale && stale->find("topic") != std::string::npos,
          "journal: a branch moved since is noticed");
    check(u && !apply_edit(*r, *u) && at("refs/heads/topic") == two,
          "journal: and never overwritten");
    j.forget_undo();
    check(!j.next_undo() && j.next_redo(), "journal: the stale step is forgotten, alone");

    // A redo whose branch was made again meanwhile is refused too.
    history_edit make{.reflog = "x", .moves = {{"refs/heads/fresh", std::nullopt, one}}};
    check(apply_edit(*r, make).has_value(), "journal: a branch made");
    j.record("Create fresh", make);
    u = j.undo_edit();
    check(u && apply_edit(*r, *u) && !at("refs/heads/fresh"), "journal: undoing it deletes it");
    j.undone();
    sh(g + "branch fresh HEAD");
    d = j.redo_edit();
    check(d && why_stale(*r, *d) && !apply_edit(*r, *d) && at("refs/heads/fresh") == three,
          "journal: a redo over a branch made since is refused");

    // The limit: the oldest steps go.
    ref_journal small(2);
    for (int i = 0; i < 4; ++i) small.record("step " + std::to_string(i), move);
    check(small.undo_count() == 2 && small.next_undo()->label == "step 3",
          "journal: only the newest steps are kept");

    // A one-way step (refs a failed edit left moved, which their files never
    // followed): undoable, never redone, so Redo can't move them again
    // without their files.
    ref_journal one_way;
    one_way.record("Move topic", move);
    one_way.record("Move topic (in part)", move, true);
    check(one_way.next_undo() && one_way.next_undo()->one_way && one_way.undo_edit(),
          "journal: a one-way step can be undone");
    one_way.undone();
    check(!one_way.next_redo() && !one_way.redo_edit() && one_way.redo_count() == 0 &&
              one_way.next_undo() && one_way.next_undo()->label == "Move topic",
          "journal: undone, a one-way step isn't redoable");
    one_way.undone();
    check(one_way.next_redo() && one_way.next_redo()->label == "Move topic" &&
              one_way.redo_count() == 1,
          "journal: the steps before it still are");
}

// Undo and Redo over a branch another working tree has checked out since:
// a commit moved between two branches, one of them then checked out in a
// linked worktree, and Undo pressed in the first window. Moving it would
// leave that tree's index and files on the old tip, so it's refused there,
// and nothing changes; from the worktree that has it, its files follow.
void journal_worktree_tests(const std::string& root)
{
    const std::string dir = root + "/journal-wt", linked = root + "/journal-wt-linked";
    const std::string g = "git -C '" + dir + "' ";
    sh("git init -q -b main '" + dir + "' && " + g + "config user.name Tester && " + g +
       "config user.email t@example.com && " + g + "config core.autocrlf false && echo base > '" +
       dir + "/base.txt' && " + g + "add base.txt && " + g + "commit -q -m base && " + g +
       "branch onto && " + g + "checkout -q -b from && echo a > '" + dir + "/a.txt' && " + g +
       "add a.txt && " + g + "commit -q -m 'move me' && " + g + "checkout -q main");
    auto r = repo::open(dir);
    check(r.has_value(), "journal worktree: the repository opens");
    if (!r) return;
    auto at = [&](const std::string& ref) { return ref_target(*r, ref); };
    auto from = load_branch_history(*r, "refs/heads/from", "");
    auto onto = load_branch_history(*r, "refs/heads/onto", "");
    check(from && onto && from->worktree.empty() && onto->worktree.empty(),
          "journal worktree: (fixture) two branches checked out nowhere");
    if (!from || !onto) return;
    auto moved = transplant_commits(*r, *onto, 0, {from->line[0]}, &*from,
                                    "tortilla: move 1 commit to onto");
    check(moved && moved->moves.size() == 2 && apply_edit(*r, *moved).has_value(),
          "journal worktree: the commit moves between the branches");
    if (!moved) return;
    ref_journal j;
    j.record("Move 1 commit to onto", *moved);
    const std::optional<oid> from_now = at("refs/heads/from"), onto_now = at("refs/heads/onto");
    check(onto_now != onto->tip && from_now != from->tip, "journal worktree: (fixture) both moved");

    sh(g + "worktree add -q '" + linked + "' onto");
    auto u = j.undo_edit();
    check(u && !why_stale(*r, *u), "journal worktree: the step's refs are where it left them");
    check(u && !apply_edit(*r, *u),
          "journal worktree: Undo over a branch checked out elsewhere is refused");
    check(at("refs/heads/onto") == onto_now && at("refs/heads/from") == from_now,
          "journal worktree: and neither branch moves");
    check(git_out(linked, {"status", "--porcelain"}).empty(),
          "journal worktree: that worktree's files stay its branch's");

    // Undone from the worktree that has it: its files follow; then Redo
    // from the first one is refused the same way.
    auto there = repo::open(linked);
    check(there && u && effect_of(*there, *u) == tree_effect::follow &&
              apply_edit(*there, *u).has_value() && at("refs/heads/onto") == onto->tip &&
              at("refs/heads/from") == from->tip &&
              git_out(linked, {"status", "--porcelain"}).empty() &&
              !std::filesystem::exists(linked + "/a.txt"),
          "journal worktree: undone where it's checked out, the files follow");
    j.undone();
    auto d = j.redo_edit();
    check(d && !why_stale(*r, *d) && !apply_edit(*r, *d) && at("refs/heads/onto") == onto->tip &&
              at("refs/heads/from") == from->tip,
          "journal worktree: Redo over it is refused too");

    // A rebase paused in that worktree: its HEAD is detached, but git holds
    // the branch being rebased (git branch -f refuses it), and so does Redo,
    // from either worktree.
    const std::string lg = "git -C '" + linked + "' ";
    sh(lg + "rebase -q --exec false --root >/dev/null 2>&1; true");
    check(git_out(linked, {"rev-parse", "--abbrev-ref", "HEAD"}) == "HEAD" &&
              std::filesystem::exists(root + "/journal-wt/.git/worktrees/journal-wt-linked/"
                                             "rebase-merge/head-name"),
          "journal worktree: (fixture) a rebase of onto paused there, HEAD detached");
    check(d && !apply_edit(*r, *d) && at("refs/heads/onto") == onto->tip &&
              at("refs/heads/from") == from->tip,
          "journal worktree: Redo over a branch a paused rebase holds there is refused");
    there = repo::open(linked);
    check(there && d && !apply_edit(*there, *d) && at("refs/heads/onto") == onto->tip,
          "journal worktree: and in that worktree itself");
    sh(lg + "rebase --abort");

    // A bisect there holds the branch it started on, HEAD moved off it.
    sh(lg + "bisect start && " + lg + "checkout -q --detach");
    check(git_out(linked, {"rev-parse", "--abbrev-ref", "HEAD"}) == "HEAD" && d &&
              !why_stale(*r, *d) && !apply_edit(*r, *d) && at("refs/heads/onto") == onto->tip,
          "journal worktree: Redo over a branch a bisect holds there is refused");
    sh(lg + "bisect reset >/dev/null 2>&1");
}

// Two refs moved together where the second can't be written: libgit2
// writes a transaction's refs one by one and stops at the first failure, so
// the first has moved by then. It's put back, and nothing is left changed.
// A ref's write fails here when its reflog can't be: a folder with a file in
// it where the reflog goes. Which of the two libgit2 writes first is its own
// order, so each is broken in turn: one way round, the other has moved
// first and is put back (its reflog says so).
void partial_move_tests(const std::string& root)
{
    const std::string dir = root + "/journal-partial";
    const std::string g = "git -C '" + dir + "' ";
    sh("git init -q -b main '" + dir + "' && " + g + "config user.name Tester && " + g +
       "config user.email t@example.com && " + g + "config core.autocrlf false && " + g +
       "commit -q --allow-empty -m one && " + g + "commit -q --allow-empty -m two");
    auto r = repo::open(dir);
    check(r.has_value(), "partial move: the repository opens");
    if (!r) return;
    const oid one = *r->resolve("HEAD~1"), two = *r->resolve("HEAD");
    auto at = [&](const std::string& ref) { return ref_target(*r, ref); };
    const std::string logs = dir + "/.git/logs/refs/heads/";
    bool put_back = false;
    for (const std::string broken : {"first", "second"})
    {
        const std::string other = broken == "first" ? "second" : "first";
        sh("rm -rf '" + logs + "first' '" + logs + "second' && " + g +
           "branch -f first HEAD~1 && " + g + "branch -f second HEAD~1 && rm -f '" + logs + broken +
           "' && mkdir '" + logs + broken + "' && echo x > '" + logs + broken + "/in-the-way'");
        const history_edit e{
            .reflog = "tortilla: partial move",
            .moves = {{"refs/heads/first", one, two}, {"refs/heads/second", one, two}}};
        auto applied = apply_edit(*r, e);
        check(!applied && !applied.error().message.empty(),
              ("partial move: the edit fails, " + broken + " not written").c_str());
        check(at("refs/heads/first") == one && at("refs/heads/second") == one,
              ("partial move: neither ref is left moved, " + broken + " not written").c_str());
        check(!applied && applied.error().moved.moves.empty(),
              ("partial move: nothing left for the journal, " + broken + " not written").c_str());
        put_back = put_back || git_out(dir, {"reflog", "-1", "--format=%gs", other}) ==
                                   "tortilla: undo partial move";
    }
    check(put_back, "partial move: a ref moved before the failure was put back");
    sh("rm -rf '" + logs + "first' '" + logs + "second'");
}

}  // namespace

void run_journal_tests(const std::string& root)
{
    ref_journal_tests(root);
    journal_worktree_tests(root);
    partial_move_tests(root);
}

}  // namespace tortilla::test
