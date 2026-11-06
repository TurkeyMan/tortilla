// Headless checks of the rebase engine against real repositories:
//   tortilla-rebase-tests <scratch dir>
// Each case builds a repo with the git CLI, runs rebase_engine on the null
// UI backend, and checks the resulting history.
#include "git/command.h"
#include "git/history_edit.h"
#include "git/repo.h"
#include "git/repo_operation.h"
#include "git/worktrees.h"
#include "rebase/rebase_engine.h"
#include "shell.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <thread>
#include <utility>

namespace
{

using namespace tortilla;
using rebase_stage = rebase_engine::rebase_stage;

int failures = 0;

void check(bool ok, const std::string& what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) ++failures;
}

using tortilla::test::sh;

std::string git(const std::string& repo, std::vector<std::string> args)
{
    git_result r = run_git(repo, args);
    return r.output;
}

// base - m1 - m2 (main);  m1 - f1 - f2 - f3 (feature). Each commit adds its
// own file; `conflict` makes f2 and m2 edit the same line of shared.txt.
std::string make_repo(const std::string& root, const std::string& name, bool conflict = false,
                      const std::string& shared = "shared.txt")
{
    std::string r = root + "/" + name;
    std::filesystem::create_directories(r);
    std::string g = "git -C '" + r + "' -c user.name=Ada -c user.email=ada@x ";
    auto commit = [&](const std::string& file, const std::string& text, const std::string& msg)
    {
        sh("printf '%s\\n' '" + text + "' > '" + r + "/" + file + "'");
        sh(g + "add . && " + g + "commit -qm '" + msg + "'");
    };
    sh("git init -q -b main '" + r + "'");
    // The engine's own commits need an identity; this machine may have none.
    sh("git -C '" + r + "' config user.name Ada && git -C '" + r + "' config user.email ada@x");
    commit("base.txt", "base", "Base");
    commit(shared, "original", "Main one");
    sh(g + "checkout -qb feature");
    commit("f1.txt", "f1", "F1");
    if (conflict)
        commit(shared, "feature edit", "F2");
    else
        commit("f2.txt", "f2", "F2");
    commit("f3.txt", "f3", "F3");
    sh(g + "checkout -q main");
    if (conflict)
        commit(shared, "main edit", "Main two");
    else
        commit("m2.txt", "m2", "Main two");
    sh(g + "checkout -q feature");
    return r;
}

struct harness
{
    rap::ui& ui;
    std::string repo;
    rebase_model model;
    std::unique_ptr<rebase_engine> engine;
    std::string log;
    bool skip_answer = true;
    // Run once in the finish's gap: the branch moved, HEAD not attached yet.
    std::function<void()> between;
    bool worktrees_fail = false;  // the finish's worktree read fails
    // Run as each recovery branch is made, before HEAD is attached to it.
    std::function<void(const std::string&)> on_kept;

    harness(rap::ui& u, const std::string& r)
        : ui(u)
        , repo(r)
    {
        auto opened = tortilla::repo::open(r);
        tortilla::repo& rp = *opened;
        auto rows = rp.range(*rp.resolve("main"), *rp.resolve("feature"), false);
        std::vector<rebase_row> v;
        for (auto& c : *rows) v.push_back({.commit = c});
        model.set(std::move(v), {});
        rebase_engine::hooks hooks{
            .log = [this](const std::string& t, bool) { log += t; },
            .stage_changed = [] {},
            .progress = [](size_t, size_t) {},
            .message = [](const std::string&) {},
            .warn = [this](const std::string& m) { log += "WARN " + m; },
            .ask_skip = [this](const std::string&, std::function<void(bool)> a) { a(skip_answer); },
            .branch_moved =
                [this]
            {
                if (auto f = std::exchange(between, nullptr)) f();
            },
            .result_kept =
                [this](const std::string& name)
            {
                if (on_kept) on_kept(name);
            },
            .worktrees = [this](const tortilla::repo& r)
                -> std::expected<std::vector<worktree_info>, std::string>
            {
                if (worktrees_fail) return std::unexpected(std::string("no worktrees here"));
                return list_worktrees(r);
            },
        };
        engine = std::make_unique<rebase_engine>(ui, r, rp.git_dir(), model, hooks);
    }

    // Pumps the null backend's loop until the engine stops.
    void settle()
    {
        for (int i = 0; i < 2000; ++i)
        {
            ui.run();
            if (!engine->busy() && engine->stage() != rebase_stage::running) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        std::fprintf(stderr, "engine never settled; log:\n%s\n", log.c_str());
    }

    void start()
    {
        auto opened = repo::open(repo);
        engine->start({.branch = "feature",
                       .branch_head = *opened->resolve("feature"),
                       .onto = *opened->resolve("main"),
                       .checked_out = "feature"});
        settle();
    }

    // Row index by subject (rows are newest first: F3, F2, F1).
    size_t row(const std::string& subject)
    {
        for (size_t i = 0; i < model.size(); ++i)
            if (model.rows()[i].commit.summary == subject) return i;
        return size_t(-1);
    }

    std::string subjects() { return git(repo, {"log", "--format=%s", "main~1..feature"}); }
};

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: %s <empty scratch dir>\n", argv[0]);
        return 2;
    }
    std::string root = argv[1];
    int ui_argc = 1;
    auto ui = rap::ui::load(ui_argc, argv, {.backend = "null"});
    if (!ui) return 2;

    {
        // Drag-reordering in the model: rows are F3, F2, F1 (top to bottom).
        harness h(**ui, make_repo(root, "reorder"));
        auto names = [&]
        {
            std::string s;
            for (auto& r : h.model.rows()) s += r.commit.summary;
            return s;
        };
        auto sel = h.model.move_to({0}, 3);  // F3 to the bottom
        check(names() == "F2F1F3" && sel == std::vector<size_t>{2}, "moveTo: top row to the end");
        sel = h.model.move_to({1, 2}, 0);  // F1, F3 to the top
        check(names() == "F1F3F2" && sel == std::vector<size_t>{0, 1}, "moveTo: block to the top");
        sel = h.model.move_to({0, 2}, 2);  // F1, F2 gathered before F2's old place
        check(names() == "F3F1F2" && sel == std::vector<size_t>{1, 2},
              "moveTo: scattered rows gathered at the drop point");
        sel = h.model.move_to({1, 2}, 0);  // back to F1F2F3 order for the next check
        check(names() == "F1F2F3", "moveTo: gathered block moves on");
        h.model.move_to({1}, 0);        // F2F1F3
        h.model.move_to({2}, 1);        // F2F3F1
        sel = h.model.move_to({1}, 0);  // F3F2F1: the original order
        check(names() == "F3F2F1", "moveTo: original order restored");
        h.model.set_action(1, rebase_action::squash);  // F2 squashes into f1
        h.model.move_to({1}, 3);                      // ...then lands on the bottom
        check(h.model.rows().back().action == rebase_action::pick,
              "moveTo: no squash on the bottom row");
    }
    // Add: a commit from another branch, picked on top, so applied last.
    {
        harness h(**ui, make_repo(root, "add"));
        const std::string g = "git -C '" + h.repo + "' ";
        sh(g + "checkout -qb side main && printf 'x\\n' > '" + h.repo + "/x.txt' && " + g +
           "add . && " + g + "commit -qm X1 && " + g + "checkout -q feature");
        auto opened = repo::open(h.repo);
        auto side = opened->range(*opened->resolve("main"), *opened->resolve("side"), false);
        check(side && h.model.add(0, *side) == 1 && h.row("X1") == 0, "add: on top");
        check(h.model.add(2, *side) == 1 && h.model.size() == 5 && h.row("X1") == 0,
              "add: one already listed goes in again, where it's put");
        h.model.set_action(2, rebase_action::skip);
        h.start();
        check(h.engine->stage() == rebase_stage::done &&
                  h.subjects() == "X1\nF3\nF2\nF1\nMain two\n",
              "add: applied last, on top of the branch's own");
    }
    {
        harness h(**ui, make_repo(root, "plain"));
        h.model.set_action(h.row("F2"), rebase_action::skip);
        h.start();
        check(h.engine->stage() == rebase_stage::done, "pick/skip: finishes");
        check(h.subjects() == "F3\nF1\nMain two\n", "pick/skip: F2 dropped, rest on top of main");
        check(git(h.repo, {"rev-parse", "--abbrev-ref", "HEAD"}) == "feature\n",
              "pick/skip: branch checked out");
    }
    {
        harness h(**ui, make_repo(root, "squash"));
        h.model.set_action(h.row("F2"), rebase_action::squash);
        h.start();
        check(h.engine->stage() == rebase_stage::squash_edit, "squash: stops for the message");
        h.engine->resume("F1 and F2 together\n");
        h.settle();
        check(h.engine->stage() == rebase_stage::done, "squash: finishes");
        check(h.subjects() == "F3\nF1 and F2 together\nMain two\n", "squash: one commit for F1+F2");
        check(git(h.repo, {"show", "--format=", "--name-only", "HEAD~1"}) == "f1.txt\nf2.txt\n",
              "squash: combined changes");
    }
    {
        harness h(**ui, make_repo(root, "edit"));
        h.model.set_action(h.row("F1"), rebase_action::edit);
        h.start();
        check(h.engine->stage() == rebase_stage::edit, "edit: stops after F1");
        h.engine->resume("F1 reworded\n");
        h.settle();
        check(h.engine->stage() == rebase_stage::done, "edit: finishes");
        check(h.subjects() == "F3\nF2\nF1 reworded\nMain two\n", "edit: amended message");
    }
    {
        harness h(**ui, make_repo(root, "conflict", true));
        h.start();
        check(h.engine->stage() == rebase_stage::conflict, "conflict: stops at F2");
        h.engine->resume("F2\n");
        h.settle();
        check(h.engine->stage() == rebase_stage::conflict && h.log.find("WARN") != std::string::npos,
              "conflict: Commit refused while unmerged");
        sh("printf 'resolved\\n' > '" + h.repo + "/shared.txt'");
        h.engine->resume("F2 (resolved)\n");
        h.settle();
        check(h.engine->stage() == rebase_stage::done, "conflict: finishes after resolving");
        if (h.engine->stage() != rebase_stage::done) std::printf("engine log:\n%s\n", h.log.c_str());
        check(h.subjects() == "F3\nF2 (resolved)\nF1\nMain two\n",
              "conflict: resolved commit kept");
        check(git(h.repo, {"show", "HEAD~1:shared.txt"}) == "resolved\n", "conflict: resolution");
    }
    {
        harness h(**ui, make_repo(root, "abort", true));
        std::string before = git(h.repo, {"rev-parse", "feature"});
        h.start();
        bool aborted = false;
        h.engine->abort([&] { aborted = true; });
        for (int i = 0; i < 2000 && !aborted; ++i)
        {
            (*ui)->run();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        check(aborted, "abort: completes");
        check(git(h.repo, {"rev-parse", "feature"}) == before, "abort: branch untouched");
        check(git(h.repo, {"rev-parse", "--abbrev-ref", "HEAD"}) == "feature\n",
              "abort: back on the branch");
        check(git(h.repo, {"status", "--porcelain"}).empty(), "abort: clean working tree");
    }

    // A conflicted file with a space and an accent: git would quote its
    // name in a plain listing; the engine reads it as it is.
    {
        const std::string name = "caf\xc3\xa9 file.txt";
        harness h(**ui, make_repo(root, "conflict-name", true, name));
        h.start();
        check(h.engine->stage() == rebase_stage::conflict, "quoted name: stops at F2");
        h.engine->resume("F2\n");
        h.settle();
        check(h.engine->stage() == rebase_stage::conflict &&
                  h.log.find("WARN") != std::string::npos && h.log.find(name) != std::string::npos,
              "quoted name: Commit refused while it has markers, named as it is");
        sh("printf 'resolved\\n' > '" + h.repo + "/" + name + "'");
        h.engine->resume("F2 (resolved)\n");
        h.settle();
        check(h.engine->stage() == rebase_stage::done &&
                  git(h.repo, {"show", "HEAD~1:" + name}) == "resolved\n",
              "quoted name: resolved and committed");
    }
    // A repository under a non-ASCII folder: the engine's own files (the
    // marker, the message) and the markers check open by UTF-8 path, which
    // on Windows the narrow C calls couldn't.
    {
        harness h(**ui, make_repo(root, "r\xc3\xa9po \xe2\x82\xac", true));
        h.start();
        check(h.engine->stage() == rebase_stage::conflict &&
                  std::filesystem::exists(rap::sys::path_from_utf8(h.repo) / ".git" /
                                          "tortilla-rebase-active"),
              "non-ASCII folder: stops at F2, marked as rebasing");
        h.engine->resume("F2\n");
        h.settle();
        check(h.engine->stage() == rebase_stage::conflict &&
                  h.log.find("WARN") != std::string::npos,
              "non-ASCII folder: the markers seen");
        sh("printf 'resolved\\n' > '" + h.repo + "/shared.txt'");
        h.engine->resume("F2 (resolved)\n");
        h.settle();
        check(h.engine->stage() == rebase_stage::done &&
                  git(h.repo, {"log", "-1", "--format=%s", "HEAD~1"}) == "F2 (resolved)\n" &&
                  !std::filesystem::exists(rap::sys::path_from_utf8(h.repo) / ".git" /
                                           "tortilla-rebase-active"),
              "non-ASCII folder: the message written, the marker removed");
    }
    // Fast-forward: the branch moves to main, and no rebase is left marked
    // as going on (a later merge's conflicts would take the sides swapped).
    {
        harness h(**ui, make_repo(root, "ff"));
        const std::string g = "git -C '" + h.repo + "' ";
        sh(g + "checkout -q -B feature main~1");
        auto opened = repo::open(h.repo);
        h.engine->start({.branch = "feature",
                         .branch_head = *opened->resolve("feature"),
                         .onto = *opened->resolve("main"),
                         .onto_name = "main",
                         .checked_out = "feature",
                         .fast_forward = true});
        h.settle();
        check(h.engine->stage() == rebase_stage::done &&
                  git(h.repo, {"rev-parse", "feature"}) == git(h.repo, {"rev-parse", "main"}),
              "fast-forward: the branch at main");
        check(!std::filesystem::exists(h.repo + "/.git/tortilla-rebase-active"),
              "fast-forward: no rebase marker left");
    }
    // An untracked file where the upstream has one: git refuses to start,
    // and the file is as it was.
    {
        harness h(**ui, make_repo(root, "untracked"));
        // main gains a folder, sub/, for a file in its place below, and
        // ign.txt, for an ignored one.
        const std::string g = "git -C '" + h.repo + "' ";
        sh(g + "checkout -q main && mkdir -p '" + h.repo + "/sub' && printf 's\\n' > '" + h.repo +
           "/sub/in.txt' && printf 'i\\n' > '" + h.repo + "/ign.txt' && " + g +
           "add sub ign.txt && " + g + "commit -qm Sub && " + g + "checkout -q feature");
        sh("printf 'mine\\n' > '" + h.repo + "/m2.txt'");
        std::string before = git(h.repo, {"rev-parse", "feature"});
        h.start();
        check(h.engine->stage() == rebase_stage::error, "untracked: the start refused");
        check(git(h.repo, {"rev-parse", "--abbrev-ref", "HEAD"}) == "feature\n" &&
                  git(h.repo, {"rev-parse", "feature"}) == before,
              "untracked: still on the branch, unmoved");
        std::FILE* f = std::fopen((h.repo + "/m2.txt").c_str(), "r");
        char text[16] = {};
        if (f) std::fclose((std::fgets(text, sizeof text, f), f));
        check(std::string(text) == "mine\n", "untracked: the file kept");
        // The window's preflight names it first, and folder clashes both
        // ways; an ignored file and an unrelated one aren't named. (The
        // ignored one isn't m2.txt in other case: on Windows' and macOS'
        // file systems, that's m2.txt itself.)
        sh("printf 'x\\n' > '" + h.repo + "/f1.txt.bak' && printf 'ign\\n' > '" + h.repo +
           "/ign.txt' && printf 'ign.txt\\n' > '" + h.repo + "/.git/info/exclude'");
        sh("printf 'file\\n' > '" + h.repo + "/sub'");  // a file where main has a folder
        auto opened = repo::open(h.repo);
        auto names = untracked_in_the_way(h.repo, *opened->resolve("main"));
        std::string listed;
        for (auto& n : names) listed += n + ";";
        check(listed == "m2.txt;sub;", "untracked: the preflight names them (" + listed + ")");
        sh(g + "config core.ignorecase true && rm '" + h.repo + "/m2.txt' && printf 'mine\\n' > '" +
           h.repo + "/M2.txt' && printf '' > '" + h.repo + "/.git/info/exclude' && rm '" + h.repo +
           "/ign.txt'");
        names = untracked_in_the_way(h.repo, *opened->resolve("main"));
        listed.clear();
        for (auto& n : names) listed += n + ";";
        check(listed == "M2.txt;sub;", "untracked: without case when git says so (" + listed + ")");
    }
    // Aborting goes back to the branch without overwriting an untracked
    // file made meanwhile where the branch has one (f3.txt, not yet picked).
    {
        harness h(**ui, make_repo(root, "abort-untracked", true));
        h.start();
        check(h.engine->stage() == rebase_stage::conflict, "abort, untracked: stops at F2");
        sh("printf 'new work\\n' > '" + h.repo + "/f3.txt'");
        bool aborted = false;
        h.engine->abort([&] { aborted = true; });
        for (int i = 0; i < 400 && !aborted && h.engine->stage() != rebase_stage::error; ++i)
        {
            (*ui)->run();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        std::FILE* f = std::fopen((h.repo + "/f3.txt").c_str(), "r");
        char text[16] = {};
        if (f) std::fclose((std::fgets(text, sizeof text, f), f));
        check(std::string(text) == "new work\n", "abort, untracked: the file kept");
        check(!aborted && h.engine->stage() == rebase_stage::error,
              "abort, untracked: stops with an error instead");
    }

    // Finishing moves the branch from the tip it had at the start (an
    // expected-value update) and attaches HEAD to it, the files untouched.
    {
        harness h(**ui, make_repo(root, "finish"));
        h.start();
        const std::string head = git(h.repo, {"rev-parse", "HEAD"});
        check(h.engine->stage() == rebase_stage::done && h.subjects() == "F3\nF2\nF1\nMain two\n" &&
                  git(h.repo, {"symbolic-ref", "HEAD"}) == "refs/heads/feature\n" &&
                  git(h.repo, {"rev-parse", "feature"}) == head,
              "finish: the branch at the result, checked out");
        check(git(h.repo, {"status", "--porcelain"}).empty() &&
                  !std::filesystem::exists(h.repo + "/.git/tortilla-rebase-active") &&
                  git(h.repo, {"reflog", "-1", "--format=%gs", "refs/heads/feature"})
                      .starts_with("rebase (finish)") &&
                  git(h.repo, {"branch", "--list", "feature-rebased"}).empty(),
              "finish: clean, unmarked, the branch's reflog says so, no extra branch");
    }
    // While it's paused another program (git itself) moves the branch: the
    // finish leaves that, and the result goes on a branch of its own.
    {
        harness h(**ui, make_repo(root, "moved"));
        h.model.set_action(h.row("F1"), rebase_action::edit);
        h.start();
        check(h.engine->stage() == rebase_stage::edit, "moved meanwhile: stops at F1");
        std::string tree = git(h.repo, {"rev-parse", "feature^{tree}"});
        tree.pop_back();
        std::string external =
            git(h.repo, {"commit-tree", tree, "-p", "feature", "-m", "External"});
        external.pop_back();
        git(h.repo, {"update-ref", "refs/heads/feature", external});
        h.engine->resume("F1\n");
        h.settle();
        check(h.engine->stage() == rebase_stage::done &&
                  git(h.repo, {"rev-parse", "feature"}) == external + "\n",
              "moved meanwhile: the branch keeps the other program's commit");
        check(git(h.repo, {"symbolic-ref", "HEAD"}) == "refs/heads/feature-rebased\n" &&
                  git(h.repo, {"log", "--format=%s", "main~1..feature-rebased"}) ==
                      "F3\nF2\nF1\nMain two\n",
              "moved meanwhile: the result on feature-rebased, checked out");
        check(h.log.find("WARN") != std::string::npos &&
                  h.log.find("feature-rebased") != std::string::npos &&
                  h.log.find("git log --graph --oneline feature feature-rebased") !=
                      std::string::npos,
              "moved meanwhile: says where each is and how to compare");
        check(git(h.repo, {"status", "--porcelain"}).empty() &&
                  !std::filesystem::exists(h.repo + "/.git/tortilla-rebase-active"),
              "moved meanwhile: clean, no rebase left marked");
    }
    // A second worktree while it's paused: Tortilla's checks there see the
    // branch held, as git's rebase holds one; the operation shown here and
    // what's held read the same marker.
    {
        harness h(**ui, make_repo(root, "held"));
        h.model.set_action(h.row("F2"), rebase_action::edit);
        h.start();
        check(h.engine->stage() == rebase_stage::edit, "held: stops at F2");
        const std::string linked = root + "/held-linked";
        sh("git -C '" + h.repo + "' worktree add -q --detach '" + linked + "' main");
        const operation_state op = operation_in_progress(h.repo);
        check(op.operation == repo_operation::tortilla_rebase && op.branch == "feature",
              "held: shown as Tortilla's rebase of feature");
        auto other = repo::open(linked);
        auto here = repo::open(h.repo);
        const history_edit move{.moves = {{.ref = "refs/heads/feature",
                                           .from = here->resolve("feature"),
                                           .to = here->resolve("main")}}};
        std::expected<std::vector<worktree_info>, std::string> trees =
            std::unexpected(std::string("no repository"));
        if (other) trees = list_worktrees(*other);
        check(trees && !trees->empty() && trees->front().main && !trees->front().current &&
                  trees->front().holds("refs/heads/feature"),
              "held: the other worktree sees the branch held here");
        check(other && why_checked_out_elsewhere(*other, move).has_value(),
              "held: a history edit there refuses to move it");
        bool labelled = false;
        if (other)
            for (auto& [id, labels] : other->refs())
                for (const ref& l : labels)
                    if (l.full_name == "refs/heads/feature") labelled = !l.worktree.empty();
        check(labelled, "held: its label there names this worktree (no checkout, no delete)");
        check(here && why_checked_out_elsewhere(*here, move).has_value(),
              "held: here too, a history edit refuses to move it");

        // git itself doesn't know: it checks the branch out there anyway.
        // The finish then leaves it, rather than move it under that tree.
        sh("git -C '" + linked + "' checkout -q feature");
        const std::string before = git(h.repo, {"rev-parse", "feature"});
        h.engine->resume("F2\n");
        h.settle();
        check(h.engine->stage() == rebase_stage::done &&
                  git(h.repo, {"rev-parse", "feature"}) == before &&
                  git(h.repo, {"symbolic-ref", "HEAD"}) == "refs/heads/feature-rebased\n" &&
                  git(h.repo, {"log", "--format=%s", "main~1..feature-rebased"}) ==
                      "F3\nF2\nF1\nMain two\n",
              "held: checked out there meanwhile, left; the result on feature-rebased");
        check(h.log.find("another worktree") != std::string::npos &&
                  git(linked, {"status", "--porcelain"}).empty(),
              "held: says so; that worktree's files untouched");
        // Done, the hold goes with the marker.
        sh("git -C '" + linked + "' checkout -q --detach");
        check(here && !why_checked_out_elsewhere(*here, move).has_value(),
              "held: released once finished");
    }
    // Moved again in the gap between the branch taking the result and HEAD
    // attached to it (git branch -f, say): HEAD goes back to the result and
    // the result is kept on a branch of its own, as when it moved before.
    {
        harness h(**ui, make_repo(root, "gap"));
        std::string main_tip = git(h.repo, {"rev-parse", "main"});
        main_tip.pop_back();
        bool moved = false;
        h.between = [&]
        {
            git(h.repo, {"update-ref", "refs/heads/feature", main_tip});
            moved = true;
        };
        h.start();
        check(moved && h.engine->stage() == rebase_stage::done &&
                  git(h.repo, {"rev-parse", "feature"}) == main_tip + "\n",
              "moved in the gap: the branch keeps where it was moved to");
        check(git(h.repo, {"symbolic-ref", "HEAD"}) == "refs/heads/feature-rebased\n" &&
                  git(h.repo, {"log", "--format=%s", "main~1..feature-rebased"}) ==
                      "F3\nF2\nF1\nMain two\n" &&
                  git(h.repo, {"status", "--porcelain"}).empty(),
              "moved in the gap: the result on feature-rebased, checked out, clean");
        check(h.log.find("WARN") != std::string::npos &&
                  h.log.find("feature-rebased") != std::string::npos &&
                  !std::filesystem::exists(h.repo + "/.git/tortilla-rebase-active"),
              "moved in the gap: says so, no rebase left marked");
    }
    // The worktree list can't be read (the read made to fail): which
    // worktree has the branch can't be told, so it's left alone.
    {
        harness h(**ui, make_repo(root, "unreadable"));
        h.model.set_action(h.row("F2"), rebase_action::edit);
        h.start();
        check(h.engine->stage() == rebase_stage::edit, "worktrees unreadable: stops at F2");
        const std::string before = git(h.repo, {"rev-parse", "feature"});
        h.worktrees_fail = true;
        h.engine->resume("F2\n");
        h.settle();
        check(h.engine->stage() == rebase_stage::done &&
                  git(h.repo, {"rev-parse", "feature"}) == before &&
                  git(h.repo, {"symbolic-ref", "HEAD"}) == "refs/heads/feature-rebased\n" &&
                  git(h.repo, {"log", "--format=%s", "main~1..feature-rebased"}) ==
                      "F3\nF2\nF1\nMain two\n" &&
                  git(h.repo, {"status", "--porcelain"}).empty(),
              "worktrees unreadable: the branch untouched, the result on feature-rebased");
        check(h.log.find("couldn't be read") != std::string::npos &&
                  !std::filesystem::exists(h.repo + "/.git/tortilla-rebase-active"),
              "worktrees unreadable: says so, no rebase left marked");
    }
    // The recovery branch moved between its making and HEAD attached to it:
    // HEAD goes back to the result and a fresh name is tried, the moved one
    // left as the other program left it.
    {
        harness h(**ui, make_repo(root, "kept-moved"));
        std::string main_tip = git(h.repo, {"rev-parse", "main"});
        main_tip.pop_back();
        const std::string before = git(h.repo, {"rev-parse", "feature"});
        h.worktrees_fail = true;  // so the result is kept on a branch of its own
        h.on_kept = [&](const std::string& name)
        {
            if (name == "feature-rebased")
                git(h.repo, {"update-ref", "refs/heads/" + name, main_tip});
        };
        h.start();
        check(h.engine->stage() == rebase_stage::done &&
                  git(h.repo, {"rev-parse", "feature"}) == before &&
                  git(h.repo, {"rev-parse", "feature-rebased"}) == main_tip + "\n",
              "recovery branch moved: the branch and the moved one left as they are");
        check(git(h.repo, {"symbolic-ref", "HEAD"}) == "refs/heads/feature-rebased-2\n" &&
                  git(h.repo, {"log", "--format=%s", "main~1..HEAD"}) == "F3\nF2\nF1\nMain two\n" &&
                  git(h.repo, {"status", "--porcelain"}).empty(),
              "recovery branch moved: the result on feature-rebased-2, checked out, clean");
        check(h.log.find("WARN") != std::string::npos &&
                  h.log.find("feature-rebased-2") != std::string::npos &&
                  !std::filesystem::exists(h.repo + "/.git/tortilla-rebase-active"),
              "recovery branch moved: says so, no rebase left marked");
    }
    // Moved every time: after a few tries HEAD stays detached at the
    // result, and the message names it.
    {
        harness h(**ui, make_repo(root, "kept-moving"));
        std::string main_tip = git(h.repo, {"rev-parse", "main"});
        main_tip.pop_back();
        h.worktrees_fail = true;
        int tries = 0;
        h.on_kept = [&](const std::string& name)
        {
            ++tries;
            git(h.repo, {"update-ref", "refs/heads/" + name, main_tip});
        };
        h.start();
        std::string head = git(h.repo, {"rev-parse", "HEAD"});
        head.pop_back();
        check(h.engine->stage() == rebase_stage::done && tries == 3 &&
                  git(h.repo, {"symbolic-ref", "-q", "HEAD"}).empty() &&
                  git(h.repo, {"log", "--format=%s", "main~1..HEAD"}) == "F3\nF2\nF1\nMain two\n" &&
                  git(h.repo, {"status", "--porcelain"}).empty(),
              "recovery branch always moved: HEAD detached at the result, clean");
        check(git(h.repo, {"rev-parse", "feature-rebased-3"}) == main_tip + "\n" &&
                  h.log.find("WARN") != std::string::npos &&
                  h.log.find(head) != std::string::npos &&
                  !std::filesystem::exists(h.repo + "/.git/tortilla-rebase-active"),
              "recovery branch always moved: the moved ones left, the result named, unmarked");
    }
    // Aborting never resets the branch: one moved meanwhile keeps its commit.
    {
        harness h(**ui, make_repo(root, "abort-moved", true));
        h.start();
        check(h.engine->stage() == rebase_stage::conflict, "abort, moved: stops at F2");
        std::string tree = git(h.repo, {"rev-parse", "feature^{tree}"});
        tree.pop_back();
        std::string external =
            git(h.repo, {"commit-tree", tree, "-p", "feature", "-m", "External"});
        external.pop_back();
        git(h.repo, {"update-ref", "refs/heads/feature", external});
        bool aborted = false;
        h.engine->abort([&] { aborted = true; });
        for (int i = 0; i < 2000 && !aborted; ++i)
        {
            (*ui)->run();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        check(aborted && git(h.repo, {"rev-parse", "feature"}) == external + "\n" &&
                  git(h.repo, {"symbolic-ref", "HEAD"}) == "refs/heads/feature\n",
              "abort, moved: back on the branch, its new commit kept");
    }
    // Fast-forward from a tip that moved since the dialog read it: kept.
    {
        harness h(**ui, make_repo(root, "ff-moved"));
        const std::string g = "git -C '" + h.repo + "' ";
        sh(g + "checkout -q -B feature main~1");
        auto opened = repo::open(h.repo);
        const oid read_then = *opened->resolve("feature");
        sh(g + "commit -q --allow-empty -m Later");
        const std::string later = git(h.repo, {"rev-parse", "feature"});
        h.engine->start({.branch = "feature",
                         .branch_head = read_then,
                         .onto = *opened->resolve("main"),
                         .onto_name = "main",
                         .checked_out = "feature",
                         .fast_forward = true});
        h.settle();
        check(h.engine->stage() == rebase_stage::done &&
                  git(h.repo, {"rev-parse", "feature"}) == later &&
                  git(h.repo, {"rev-parse", "feature-rebased"}) ==
                      git(h.repo, {"rev-parse", "main"}),
              "fast-forward, moved: the later commit kept, main on feature-rebased");
    }

    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
