// Headless checks of the rebase engine against real repositories:
//   tortilla-rebase-tests <scratch dir>
// Each case builds a repo with the git CLI, runs RebaseEngine on the null
// UI backend, and checks the resulting history.
#include "git/command.h"
#include "rebase/rebase_engine.h"
#include "shell.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <thread>

namespace
{

using namespace tortilla;
using Stage = RebaseEngine::Stage;

int failures = 0;

void check(bool ok, const std::string& what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) ++failures;
}

using tortilla::test::sh;

std::string git(const std::string& repo, std::vector<std::string> args)
{
    GitResult r = runGit(repo, args);
    return r.output;
}

// base - m1 - m2 (main);  m1 - f1 - f2 - f3 (feature). Each commit adds its
// own file; `conflict` makes f2 and m2 edit the same line of shared.txt.
std::string makeRepo(const std::string& root, const std::string& name, bool conflict = false)
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
    commit("shared.txt", "original", "Main one");
    sh(g + "checkout -qb feature");
    commit("f1.txt", "f1", "F1");
    if (conflict)
        commit("shared.txt", "feature edit", "F2");
    else
        commit("f2.txt", "f2", "F2");
    commit("f3.txt", "f3", "F3");
    sh(g + "checkout -q main");
    if (conflict)
        commit("shared.txt", "main edit", "Main two");
    else
        commit("m2.txt", "m2", "Main two");
    sh(g + "checkout -q feature");
    return r;
}

struct Harness
{
    rap::Ui& ui;
    std::string repo;
    RebaseModel model;
    std::unique_ptr<RebaseEngine> engine;
    std::string log;
    bool skipAnswer = true;

    Harness(rap::Ui& u, const std::string& r)
        : ui(u)
        , repo(r)
    {
        auto opened = Repo::open(r);
        Repo& rp = *opened;
        auto rows = rp.range(*rp.resolve("main"), *rp.resolve("feature"), false);
        std::vector<RebaseRow> v;
        for (auto& c : *rows) v.push_back({.commit = c});
        model.set(std::move(v), {});
        RebaseEngine::Hooks hooks{
            .log = [this](const std::string& t, bool) { log += t; },
            .stageChanged = [] {},
            .progress = [](size_t, size_t) {},
            .message = [](const std::string&) {},
            .warn = [this](const std::string& m) { log += "WARN " + m; },
            .askSkip = [this](const std::string&, std::function<void(bool)> a) { a(skipAnswer); },
        };
        engine = std::make_unique<RebaseEngine>(ui, r, rp.gitDir(), model, hooks);
    }

    // Pumps the null backend's loop until the engine stops.
    void settle()
    {
        for (int i = 0; i < 2000; ++i)
        {
            ui.run();
            if (!engine->busy() && engine->stage() != Stage::Running) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        std::fprintf(stderr, "engine never settled; log:\n%s\n", log.c_str());
    }

    void start()
    {
        auto opened = Repo::open(repo);
        engine->start({.branch = "feature",
                       .branchHead = *opened->resolve("feature"),
                       .onto = *opened->resolve("main"),
                       .checkedOut = "feature"});
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
    int uiArgc = 1;
    auto ui = rap::Ui::load(uiArgc, argv, {.backend = "null"});
    if (!ui) return 2;

    {
        // Drag-reordering in the model: rows are F3, F2, F1 (top to bottom).
        Harness h(**ui, makeRepo(root, "reorder"));
        auto names = [&]
        {
            std::string s;
            for (auto& r : h.model.rows()) s += r.commit.summary;
            return s;
        };
        auto sel = h.model.moveTo({0}, 3);  // F3 to the bottom
        check(names() == "F2F1F3" && sel == std::vector<size_t>{2}, "moveTo: top row to the end");
        sel = h.model.moveTo({1, 2}, 0);  // F1, F3 to the top
        check(names() == "F1F3F2" && sel == std::vector<size_t>{0, 1}, "moveTo: block to the top");
        sel = h.model.moveTo({0, 2}, 2);  // F1, F2 gathered before F2's old place
        check(names() == "F3F1F2" && sel == std::vector<size_t>{1, 2},
              "moveTo: scattered rows gathered at the drop point");
        sel = h.model.moveTo({1, 2}, 0);  // back to F1F2F3 order for the next check
        check(names() == "F1F2F3", "moveTo: gathered block moves on");
        h.model.moveTo({1}, 0);        // F2F1F3
        h.model.moveTo({2}, 1);        // F2F3F1
        sel = h.model.moveTo({1}, 0);  // F3F2F1: the original order
        check(names() == "F3F2F1", "moveTo: original order restored");
        h.model.setAction(1, RebaseAction::Squash);  // F2 squashes into F1
        h.model.moveTo({1}, 3);                      // ...then lands on the bottom
        check(h.model.rows().back().action == RebaseAction::Pick,
              "moveTo: no squash on the bottom row");
    }
    {
        Harness h(**ui, makeRepo(root, "plain"));
        h.model.setAction(h.row("F2"), RebaseAction::Skip);
        h.start();
        check(h.engine->stage() == Stage::Done, "pick/skip: finishes");
        check(h.subjects() == "F3\nF1\nMain two\n", "pick/skip: F2 dropped, rest on top of main");
        check(git(h.repo, {"rev-parse", "--abbrev-ref", "HEAD"}) == "feature\n",
              "pick/skip: branch checked out");
    }
    {
        Harness h(**ui, makeRepo(root, "squash"));
        h.model.setAction(h.row("F2"), RebaseAction::Squash);
        h.start();
        check(h.engine->stage() == Stage::SquashEdit, "squash: stops for the message");
        h.engine->resume("F1 and F2 together\n");
        h.settle();
        check(h.engine->stage() == Stage::Done, "squash: finishes");
        check(h.subjects() == "F3\nF1 and F2 together\nMain two\n", "squash: one commit for F1+F2");
        check(git(h.repo, {"show", "--format=", "--name-only", "HEAD~1"}) == "f1.txt\nf2.txt\n",
              "squash: combined changes");
    }
    {
        Harness h(**ui, makeRepo(root, "edit"));
        h.model.setAction(h.row("F1"), RebaseAction::Edit);
        h.start();
        check(h.engine->stage() == Stage::Edit, "edit: stops after F1");
        h.engine->resume("F1 reworded\n");
        h.settle();
        check(h.engine->stage() == Stage::Done, "edit: finishes");
        check(h.subjects() == "F3\nF2\nF1 reworded\nMain two\n", "edit: amended message");
    }
    {
        Harness h(**ui, makeRepo(root, "conflict", true));
        h.start();
        check(h.engine->stage() == Stage::Conflict, "conflict: stops at F2");
        h.engine->resume("F2\n");
        h.settle();
        check(h.engine->stage() == Stage::Conflict && h.log.find("WARN") != std::string::npos,
              "conflict: Commit refused while unmerged");
        sh("printf 'resolved\\n' > '" + h.repo + "/shared.txt'");
        h.engine->resume("F2 (resolved)\n");
        h.settle();
        check(h.engine->stage() == Stage::Done, "conflict: finishes after resolving");
        if (h.engine->stage() != Stage::Done) std::printf("engine log:\n%s\n", h.log.c_str());
        check(h.subjects() == "F3\nF2 (resolved)\nF1\nMain two\n",
              "conflict: resolved commit kept");
        check(git(h.repo, {"show", "HEAD~1:shared.txt"}) == "resolved\n", "conflict: resolution");
    }
    {
        Harness h(**ui, makeRepo(root, "abort", true));
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

    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
