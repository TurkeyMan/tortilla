#include "journal_tests.h"

#include "git/history_writer.h"
#include "git/ref_journal.h"
#include "shell.h"
#include "test_support.h"
#include "util/notices.h"

#include <rapport/sys.h>

#include <chrono>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <thread>

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

// The history editor's writes (git/history_writer.h): an edit written, and
// held at a barrier after its refs moved but before its outcome reaches the
// UI thread; meanwhile a second edit and an Undo are refused and touch
// nothing, and the first's outcome still lands in the journal exactly once,
// a partial failure's too. A writer gone mid-write (its window closed)
// finishes the write and never calls the window.
void history_writer_tests(rap::ui& ui, const std::string& root)
{
    const std::string dir = root + "/journal-writer";
    const std::string g = "git -C '" + dir + "' ";
    sh("git init -q -b main '" + dir + "' && " + g + "config user.name Tester && " + g +
       "config user.email t@example.com && " + g + "config core.autocrlf false && " + g +
       "commit -q --allow-empty -m one && " + g + "commit -q --allow-empty -m two && " + g +
       "commit -q --allow-empty -m three && " + g + "branch topic HEAD~2 && " + g +
       "branch other HEAD~2");
    auto r = repo::open(dir);
    check(r.has_value(), "writer: the repository opens");
    if (!r) return;
    const oid one = *r->resolve("HEAD~2"), two = *r->resolve("HEAD~1"), three = *r->resolve("HEAD");
    auto at = [&](const std::string& ref) { return ref_target(*r, ref); };
    auto refs = [&] { return git_out(dir, {"for-each-ref", "--format=%(refname) %(objectname)"}); };
    // The UI thread's loop, until `done` (or a few seconds).
    auto pump = [&](const std::function<bool()>& done)
    {
        for (int i = 0; i < 2000 && !done(); ++i)
        {
            ui.run();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return done();
    };
    // A little longer, for anything delivered twice.
    auto drain = [&]
    {
        for (int i = 0; i < 50; ++i)
        {
            ui.run();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    };
    // The barrier: the worker stops after the write, before the post, until
    // released.
    struct barrier
    {
        std::promise<void> reached, release;
        std::shared_future<void> released = release.get_future().share();
        std::future<void> arrived = reached.get_future();
        bool wait()
        {
            return arrived.wait_for(std::chrono::seconds(20)) == std::future_status::ready;
        }
    };

    {
        ref_journal j;
        history_writer w(ui, dir, j);
        auto b = std::make_shared<barrier>();
        w.after_write = [b]
        {
            b->reached.set_value();
            b->released.wait();
        };
        int delivered = 0;
        const history_edit move{.reflog = "tortilla: move topic",
                                .moves = {{"refs/heads/topic", one, three}}};
        check(w.begin(), "writer: the first edit is accepted");
        w.apply("Move", "Move topic", move, journal_use::record, false,
                [&](const history_outcome&) { ++delivered; });
        check(b->wait() && at("refs/heads/topic") == three,
              "writer: (barrier) the refs moved, the outcome not yet posted");
        const std::string before = refs();
        // A second edit, and an Undo, meanwhile: both refused.
        check(w.busy() && !w.begin(), "writer: a second edit is refused while one is outstanding");
        check(!w.begin(), "writer: so is an Undo");
        check(refs() == before && at("refs/heads/other") == one,
              "writer: and the refused requests touch no ref");
        b->release.set_value();
        check(pump([&] { return delivered > 0; }), "writer: the outcome is delivered");
        drain();
        check(delivered == 1, "writer: exactly once");
        check(j.undo_count() == 1 && j.next_undo()->label == "Move topic" && !w.busy(),
              "writer: the edit is in the journal, once, and the writer is free");

        // Now the Undo is accepted, and its outcome recorded too.
        w.after_write = nullptr;
        auto u = j.undo_edit();
        check(u && w.begin(), "writer: Undo is accepted once the edit is handled");
        if (u)
            w.apply("Undo Move topic", "Move topic", *u, journal_use::undo, false,
                    [&](const history_outcome&) { ++delivered; });
        check(pump([&] { return delivered == 2; }) && at("refs/heads/topic") == one &&
                  j.undo_count() == 0 && j.redo_count() == 1,
              "writer: Undo's outcome is recorded");

        // A replay given up (failed, or the user said no) frees the writer.
        check(w.begin(), "writer: a new edit is accepted");
        const std::string unchanged = refs();
        bool replayed = false;
        w.prepare(
            [&](const repo&) -> std::function<void()>
            {
                return [&]
                {
                    replayed = true;
                    w.cancel();
                };
            });
        check(pump([&] { return replayed; }) && !w.busy() && refs() == unchanged,
              "writer: a replay given up frees it, nothing written");
    }

    // A partial failure: two refs to move, the first moved and left there
    // (as apply_edit reports refs it couldn't put back). Held at the barrier,
    // a second edit refused; the moved ref is a one-way journal step, once.
    {
        sh(g + "branch -f topic HEAD~2 && " + g + "branch -f other HEAD~2");
        ref_journal j;
        history_writer w(ui, dir, j);
        w.write = [](const repo& rp, const history_edit& e,
                     bool) -> std::expected<std::string, apply_failure>
        {
            history_edit first{.reflog = e.reflog, .moves = {e.moves.front()}};
            if (!apply_edit(rp, first)) return std::unexpected(apply_failure{"first failed"});
            return std::unexpected(apply_failure{"other can't be written", first});
        };
        auto b = std::make_shared<barrier>();
        w.after_write = [b]
        {
            b->reached.set_value();
            b->released.wait();
        };
        int delivered = 0;
        bool failed = false;
        const history_edit both{
            .reflog = "tortilla: move both",
            .moves = {{"refs/heads/topic", one, two}, {"refs/heads/other", one, two}}};
        check(w.begin(), "writer partial: the edit is accepted");
        w.apply("Move", "Move both", both, journal_use::record, false,
                [&](const history_outcome& o)
                {
                    ++delivered;
                    failed = !o.applied;
                });
        check(b->wait() && at("refs/heads/topic") == two && at("refs/heads/other") == one,
              "writer partial: (barrier) one ref moved, the other not");
        const std::string before = refs();
        check(!w.begin() && refs() == before,
              "writer partial: a second edit is refused, untouched");
        b->release.set_value();
        pump([&] { return delivered > 0; });
        drain();
        check(delivered == 1 && failed, "writer partial: the failure is delivered, once");
        check(j.undo_count() == 1 && j.next_undo()->one_way &&
                  j.next_undo()->label == "Move both (in part)" && !w.busy(),
              "writer partial: what moved is a one-way journal step");
        w.write = [](const repo& rp, const history_edit& e, bool s)
        { return apply_edit(rp, e, s); };
        w.after_write = nullptr;
        auto u = j.undo_edit();
        check(u && w.begin(), "writer partial: its Undo is accepted");
        if (u)
            w.apply("Undo", "Move both (in part)", *u, journal_use::undo, false,
                    [&](const history_outcome&) { ++delivered; });
        check(pump([&] { return delivered == 2; }) && at("refs/heads/topic") == one &&
                  j.undo_count() == 0 && j.redo_count() == 0,
              "writer partial: undone, the ref is back, and nothing to redo");
    }

    // The other way round: a journaled ref command (a label dragged) first,
    // held at the barrier with its ref moved; meanwhile an Undo, an edit and
    // a second ref command are all refused, untouched. The command's step is
    // recorded once, on top, above the step before it, and Undo takes it.
    {
        sh(g + "branch -f topic HEAD~2 && " + g + "branch -f other HEAD~2");
        ref_journal j;
        history_writer w(ui, dir, j);
        int delivered = 0;
        auto count = [&](const history_outcome&) { ++delivered; };
        check(w.begin(), "writer commands: an earlier edit is accepted");
        w.apply("Move", "Move other",
                {.reflog = "tortilla: move other", .moves = {{"refs/heads/other", one, two}}},
                journal_use::record, false, count);
        check(pump([&] { return delivered == 1; }) && j.undo_count() == 1,
              "writer commands: (fixture) a step in the journal first");
        auto b = std::make_shared<barrier>();
        w.after_write = [b]
        {
            b->reached.set_value();
            b->released.wait();
        };
        bool failed = true;
        check(w.begin(), "writer commands: the ref command is accepted");
        w.run_commands("Move Branch", "Move topic", {{"branch", "-f", "topic", three.hex()}},
                       {"refs/heads/topic"},
                       [&](const history_outcome& o)
                       {
                           ++delivered;
                           failed = !o.applied;
                       });
        check(b->wait() && at("refs/heads/topic") == three,
              "writer commands: (barrier) the command moved its ref, not yet posted");
        const std::string before = refs();
        check(!w.begin(), "writer commands: an Undo meanwhile is refused");
        check(!w.begin(), "writer commands: so is an edit");
        check(!w.begin(), "writer commands: and a second ref command");
        check(refs() == before && at("refs/heads/other") == two,
              "writer commands: the refused requests touch no ref");
        b->release.set_value();
        pump([&] { return delivered == 2; });
        drain();
        check(delivered == 2 && !failed, "writer commands: its outcome is delivered once");
        check(j.undo_count() == 2 && j.next_undo()->label == "Move topic" &&
                  !j.next_undo()->one_way,
              "writer commands: its step is recorded once, on top");
        w.after_write = nullptr;
        auto u = j.undo_edit();
        check(u && w.begin(), "writer commands: Undo is accepted after");
        if (u) w.apply("Undo", "Move topic", *u, journal_use::undo, false, count);
        check(pump([&] { return delivered == 3; }) && at("refs/heads/topic") == one &&
                  at("refs/heads/other") == two && j.next_undo() &&
                  j.next_undo()->label == "Move other",
              "writer commands: Undo takes the command's step, the one before stays");

        // A command that fails part way: what moved before it is the step.
        check(w.begin(), "writer commands: a failing series is accepted");
        w.run_commands(
            "Delete", "Move and fail",
            {{"branch", "-f", "topic", three.hex()}, {"branch", "-f", "other", "no-such-revision"}},
            {"refs/heads/topic", "refs/heads/other"},
            [&](const history_outcome& o)
            {
                ++delivered;
                failed = !o.applied;
            });
        check(pump([&] { return delivered == 4; }) && failed && j.next_undo() &&
                  j.next_undo()->label == "Move and fail" && j.next_undo()->edit.moves.size() == 1,
              "writer commands: a failure part way records what moved");
    }

    // The window closed mid-write: the writer goes while its write is held
    // at the barrier. The write finishes, and the window is never called. A
    // plain success needs no notice; a warning (local changes left in the
    // stash) or a failure (refs left moved) is saved, in full, for the next
    // window on the repository, which takes it once.
    const std::filesystem::path notes = rap::sys::path_from_utf8(root + "/journal-notices");
    std::vector<std::string> unsaved;  // notices the writer couldn't save
    auto close_mid_write = [&](const std::string& label, history_writer::write_fn write)
    {
        sh(g + "branch -f topic HEAD~2 && " + g + "branch -f other HEAD~2");
        int delivered = 0;
        {
            ref_journal j;
            auto w = std::make_unique<history_writer>(ui, dir, j);
            w->notices = notes;
            w->unsaved = [&](const std::string& text) { unsaved.push_back(text); };
            if (write) w->write = std::move(write);
            auto b = std::make_shared<barrier>();
            w->after_write = [b]
            {
                b->reached.set_value();
                b->released.wait();
            };
            check(w->begin(), ("writer closed: the edit is accepted, " + label).c_str());
            w->apply("Move", label,
                     {.reflog = "tortilla: move",
                      .moves = {{"refs/heads/topic", one, three}, {"refs/heads/other", one, two}}},
                     journal_use::record, false, [&](const history_outcome&) { ++delivered; });
            check(b->wait(), ("writer closed: (barrier) the write is under way, " + label).c_str());
            std::thread release(
                [b]
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    b->release.set_value();
                });
            w.reset();  // waits for the write
            release.join();
        }
        drain();
        check(
            delivered == 0 && at("refs/heads/topic") == three,
            ("writer closed: the write finished, the gone window never called, " + label).c_str());
    };
    // `r->workdir()` ends with a separator, `dir` doesn't: the same repository.
    auto take = [&] { return take_notices(notes, r->workdir()); };

    close_mid_write("Move plainly", nullptr);
    check(take().empty(), "writer closed: a plain success leaves no notice");

    const std::string warning = "Your local changes didn't go back cleanly; they're kept in the "
                                "stash (1a2b3c4d).\nerror: conflict in a.txt";
    close_mid_write("Move with a warning",
                    [&](const repo& rp, const history_edit& e,
                        bool s) -> std::expected<std::string, apply_failure>
                    {
                        auto applied = apply_edit(rp, e, s);
                        if (!applied) return applied;
                        return warning;
                    });
    check(take_notices(notes, root + "/elsewhere").empty(),
          "writer closed: another repository's window doesn't take the notice");
    auto got = take();
    check(got.size() == 1 && got[0].find("Move with a warning") != std::string::npos &&
              got[0].find(warning) != std::string::npos &&
              got[0].find(" UTC\n") != std::string::npos,
          "writer closed: a success with a warning leaves its full text, with the time");
    check(take().empty(), "writer closed: taken once, the notice is gone");

    close_mid_write("Move part way",
                    [&](const repo& rp, const history_edit& e,
                        bool) -> std::expected<std::string, apply_failure>
                    {
                        history_edit first{.reflog = e.reflog, .moves = {e.moves.front()}};
                        if (!apply_edit(rp, first))
                            return std::unexpected(apply_failure{"first failed"});
                        return std::unexpected(apply_failure{"other can't be written", first});
                    });
    got = take();
    check(got.size() == 1 && got[0].find("Move part way") != std::string::npos &&
              got[0].find("other can't be written") != std::string::npos &&
              got[0].find("refs/heads/topic") != std::string::npos &&
              got[0].find(three.hex()) != std::string::npos &&
              got[0].find(one.hex()) != std::string::npos,
          "writer closed: a failure leaves the refs it left moved, with both ends");
    check(take().empty(), "writer closed: and is taken once");
    check(unsaved.empty(), "writer closed: every notice so far was saved");

    // The notice can't be moved in (a rename refused, as by EACCES), though
    // tidying its half-written file away works: that's still a failure, and
    // the writer says so; nothing is left behind.
    auto files_in = [&]
    {
        size_t n = 0;
        std::error_code ec;
        for (std::filesystem::directory_iterator it(notes, ec), end; !ec && it != end;
             it.increment(ec))
            ++n;
        return n;
    };
    const auto rename = notice_rename;
    notice_rename =
        [](const std::filesystem::path&, const std::filesystem::path&, std::error_code& ec)
    { ec = std::make_error_code(std::errc::permission_denied); };
    check(!save_notice(notes, dir, "refused") && files_in() == 0,
          "notices: a refused rename is a failure, and nothing is left");
    close_mid_write("Move unsaved",
                    [&](const repo& rp, const history_edit& e,
                        bool) -> std::expected<std::string, apply_failure>
                    {
                        history_edit first{.reflog = e.reflog, .moves = {e.moves.front()}};
                        if (!apply_edit(rp, first))
                            return std::unexpected(apply_failure{"first failed"});
                        return std::unexpected(apply_failure{"other can't be written", first});
                    });
    notice_rename = rename;
    check(unsaved.size() == 1 && unsaved[0].find("Move unsaved") != std::string::npos &&
              unsaved[0].find("refs/heads/topic") != std::string::npos,
          "notices: the writer reports a notice it couldn't save, in full");
    check(take().empty() && files_in() == 0, "notices: and none is left half-saved");
    check(save_notice(notes, dir, "saved again") && take().size() == 1,
          "notices: with rename working again, it saves");
}

}  // namespace

void run_journal_tests(rap::ui& ui, const std::string& root)
{
    ref_journal_tests(root);
    journal_worktree_tests(root);
    partial_move_tests(root);
    history_writer_tests(ui, root);
}

}  // namespace tortilla::test
