// Headless checks of the log model on a generated repository:
//   tortilla-tests <scratch dir>
// Builds a repo with branches and merges via the git CLI, then loads it
// through LogModel on the null UI backend.
#include "git/command.h"
#include "git/diff_tool.h"
#include "git/status.h"
#include "log/log_model.h"
#include "shell.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>

namespace
{

int failures = 0;

void check(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

using tortilla::test::sh;

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: %s <empty scratch dir>\n", argv[0]);
        return 2;
    }
    std::string repo = std::string(argv[1]) + "/repo";
    std::filesystem::create_directories(repo);
    std::string git = "git -C '" + repo + "' -c user.name=Ada -c user.email=ada@example.com ";
    auto commit = [&](const std::string& name)
    {
        sh("echo " + name + " > '" + repo + "/" + name + ".txt'");
        sh(git + "add . && " + git + "commit -qm '" + name + " subject' -m 'body of " + name + "'");
    };
    sh("git init -q -b main '" + repo + "'");
    commit("A");
    commit("B");
    sh(git + "checkout -qb feat");
    commit("F1");
    sh(git + "checkout -q main");
    commit("C");
    sh(git + "merge -q --no-ff feat -m 'Merge feat'");
    sh(git + "tag v1");

    int uiArgc = 1;
    rap::LoadOptions opts{.backend = "null"};
    auto ui = rap::Ui::load(uiArgc, argv, opts);
    if (!ui)
    {
        std::fprintf(stderr, "%s", ui.error().c_str());
        return 2;
    }

    tortilla::LogModel log(**ui);
    std::string error = "pending";
    // No quit(): the null backend stops draining its queue for good once
    // quit is requested, and the git runner checks below need it.
    log.load(repo, {}, [&](const std::string& e) { error = e; });
    // The null backend's loop returns whenever its queue is empty, before
    // the worker has posted anything; keep pumping until loading is done.
    while (error == "pending")
    {
        (*ui)->run();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    check(error.empty(), "loads without error");
    check(log.commitRows() == 5, "five commits (A, B, F1, C, merge)");
    check(log.isWorkingTree(0), "row 0 is the working tree");
    if (failures) return 1;  // the rest index rows
    check(log.commit(1)->summary == "Merge feat", "newest commit first");
    check(log.isHead(1), "HEAD is the merge");

    // Hashes are searched too by default, and hex is full of letters like
    // "c" and "f1", so these stick to subject and message.
    constexpr unsigned text = tortilla::FilterSubject | tortilla::FilterMessages;
    log.setFilter({.text = "F1", .fields = text});
    check(log.commitRows() == 1 && log.commit(1)->summary == "F1 subject", "plain term");
    log.setFilter({.text = "subject -F1", .fields = text});
    check(log.commitRows() == 3, "excluded term");
    log.setFilter({.text = "!subject"});
    check(log.commitRows() == 1 && log.commit(1)->summary == "Merge feat", "inverted");
    log.setFilter({.text = "\"body of b\""});
    check(log.commitRows() == 1, "quoted phrase in the message body");
    log.setFilter({.text = "\"body of b\"", .caseSensitive = true});
    check(log.commitRows() == 0, "case-sensitive");
    log.setFilter({.text = "v1", .fields = tortilla::FilterRefname});
    check(log.commitRows() == 1, "refname field");
    log.setFilter({.text = "^[AB] ", .regex = true});
    check(log.commitRows() == 2, "regex");
    check(!log.setFilter({.text = "(", .regex = true}).empty(), "invalid regex reported");
    log.setFilter({});
    check(log.commitRows() == 5, "cleared");

    // Commit order is git's --topo-order, for one branch and for --all.
    {
        sh(git + "checkout -qb side HEAD~2");
        commit("S1");
        sh(git + "checkout -q main");
        for (bool all : {false, true})
        {
            tortilla::LogModel m(**ui);
            std::string err = "pending";
            m.load(repo, {.allBranches = all}, [&](const std::string& e) { err = e; });
            while (err == "pending")
            {
                (*ui)->run();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            std::string ours;
            for (size_t row = 1; row < m.rowCount(); ++row) ours += m.commit(row)->id.hex() + "\n";
            std::vector<std::string> args{"log", "--topo-order", "--format=%H"};
            if (all) args.push_back("--all");
            check(ours == tortilla::runGit(repo, args).output,
                  all ? "order matches git --all --topo-order" : "order matches git --topo-order");
        }
    }

    // Refreshing labels after branch/tag changes, without a reload unless
    // the commits shown could have changed.
    {
        auto pump = [&](auto& done)
        {
            while (!done)
            {
                (*ui)->run();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        };
        tortilla::LogModel m(**ui);
        std::optional<std::string> err;
        m.load(repo, {.allBranches = true}, [&](const std::string& e) { err = e; });
        pump(err);
        auto refresh = [&]
        {
            std::optional<bool> reload;
            m.refreshRefs([&](bool r) { reload = r; });
            pump(reload);
            return *reload;
        };
        auto labelled = [&](const std::string& name)
        {
            for (size_t row = 0; row < m.rowCount(); ++row)
                if (auto* refs = m.refsOf(row))
                    for (auto& r : *refs)
                        if (r.name == name) return true;
            return false;
        };
        sh(git + "tag t1 HEAD");
        check(!refresh() && labelled("t1"), "refresh: new tag on a loaded commit, labels only");
        sh(git + "tag -d t1");
        check(!refresh() && !labelled("t1"), "refresh: tag deleted from a reachable commit");
        sh(git + "branch lonely $(" + git + "commit-tree -m lonely HEAD^{tree})");
        check(refresh(), "refresh: a branch at a commit not loaded reloads");
        sh(git + "branch -D lonely");
        check(!refresh(), "refresh: nothing changed in the loaded commits");
        sh(git + "branch -D side");
        check(refresh(), "refresh: a tip's last label deleted reloads");
    }

    // Diff tool commands: TortoiseGit's placeholders become git difftool's
    // shell words; a custom tool runs through git for real.
    {
        using tortilla::expandPlaceholders;
        check(expandPlaceholders("meld %base %mine", "a", "b") == "meld \"$LOCAL\" \"$REMOTE\"",
              "placeholders: %base %mine");
        check(expandPlaceholders("tool \"%base\" %mine", "a", "b") == "tool \"$LOCAL\" \"$REMOTE\"",
              "placeholders: already quoted");
        check(expandPlaceholders("x /t1=%bname", "abc12345", "b") ==
                  "x /t1=\"abc12345\" \"$LOCAL\" \"$REMOTE\"",
              "placeholders: names only, sides appended");
        check(expandPlaceholders("x %base %mine -L %yname", "a", "it's $x") ==
                  "x \"$LOCAL\" \"$REMOTE\" -L \"it's \\$x\"",
              "placeholders: names quoted for sh");
        check(expandPlaceholders("bcompare", "a", "b") == "bcompare \"$LOCAL\" \"$REMOTE\"",
              "placeholders: none, sides appended");
        auto args =
            tortilla::customDifftoolArgs("ls %base %mine", {"HEAD~1", "HEAD"}, "HEAD~1", "HEAD");
        tortilla::GitResult r = tortilla::runGit(repo, args, tortilla::GitOutput::Stdout);
        check(r.ok() && r.output.find("left") != std::string::npos &&
                  r.output.find("right") != std::string::npos,
              "custom diff tool runs through git difftool --dir-diff");
    }

    // The git runner: synchronous, then streamed on a worker.
    tortilla::GitResult r = tortilla::runGit(repo, {"rev-parse", "--abbrev-ref", "HEAD"});
    check(r.ok() && r.output == "main\n", "runGit output and exit code");
    check(!tortilla::runGit(repo, {"rev-parse", "no-such-ref"}).ok(), "runGit failure exit code");

    std::string streamed;
    int code = -2;
    auto proc = tortilla::GitProcess::start(
        **ui, repo, {"log", "--format=%s"}, [&](const std::string& chunk) { streamed += chunk; },
        [&](int c) { code = c; });
    while (code == -2)
    {
        (*ui)->run();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    check(code == 0 && streamed.starts_with("Merge feat\n") && streamed.ends_with("A subject\n"),
          "GitProcess streams output");

    code = -2;
    proc = tortilla::GitProcess::start(**ui, repo, {"branch", "-D", "no-such-branch"}, {},
                                       [&](int c) { code = c; });
    while (code == -2)
    {
        (*ui)->run();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    check(code != 0, "GitProcess reports failure");
    check(tortilla::commandLine({"commit", "-m", "two words"}) == "git commit -m \"two words\"",
          "commandLine quoting");

    // Status parsing: porcelain v1 -z plus numstat -z.
    {
        using namespace std::string_literals;
        std::string porcelain = " M a.txt\0?? build/\0R  new.txt\0old.txt\0UU c.txt\0A  img.png\0"s;
        std::string numstat = "3\t1\ta.txt\0"
                              "0\t0\t\0old.txt\0new.txt\0"
                              "-\t-\timg.png\0"s;
        auto files = tortilla::parseStatus(porcelain, numstat);
        check(files.size() == 5, "status: five entries");
        if (files.size() == 5)
        {
            check(files[0].status == 'M' && files[0].added == 3 && files[0].removed == 1,
                  "status: modified with line counts");
            check(files[1].status == '?' && files[1].path == "build/",
                  "status: untracked directory");
            check(files[2].status == 'R' && files[2].path == "new.txt" &&
                      files[2].oldPath == "old.txt",
                  "status: rename");
            check(files[3].status == 'U', "status: conflicted");
            check(files[4].status == 'A' && files[4].binary, "status: binary added");
        }
        check(tortilla::parseStatus(" M x\0"s, "x\ty\tz\0garbage"s).size() == 1,
              "status: odd numstat output doesn't throw");
    }

    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
