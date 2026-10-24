// Headless checks of the log model on a generated repository:
//   tortilla-tests <scratch dir>
// Builds a repo with branches and merges via the git CLI, then loads it
// through log_model on the null UI backend.
#include "changes/working_tree.h"
#include "git/command.h"
#include "git/diff_tool.h"
#include "git/repo_worker.h"
#include "git/status.h"
#include "log/log_model.h"
#include "shell.h"
#include "util/sha256.h"

#include <algorithm>
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
    // FIPS 180-4's examples, and a message over one block's padding room.
    check(tortilla::sha256_hex("") ==
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
          "sha256 of nothing");
    check(tortilla::sha256_hex("abc") ==
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "sha256 of abc");
    check(tortilla::sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
          "sha256 of 448 bits");
    check(tortilla::sha256_hex(std::string(1000, 'a')) ==
              "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3",
          "sha256 of 1000 bytes");

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

    int ui_argc = 1;
    rap::load_options opts{.backend = "null"};
    auto ui = rap::ui::load(ui_argc, argv, opts);
    if (!ui)
    {
        std::fprintf(stderr, "%s", ui.error().c_str());
        return 2;
    }

    tortilla::log_model log(**ui);
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
    check(log.commit_rows() == 5, "five commits (A, B, F1, C, merge)");
    check(log.is_working_tree(0), "row 0 is the working tree");
    if (failures) return 1;  // the rest index rows
    check(log.commit(1)->summary == "Merge feat", "newest commit first");
    check(log.is_head(1), "HEAD is the merge");

    // Hashes are searched too by default, and hex is full of letters like
    // "c" and "f1", so these stick to subject and message.
    constexpr unsigned text = tortilla::filter_subject | tortilla::filter_messages;
    log.set_filter({.text = "F1", .fields = text});
    check(log.commit_rows() == 1 && log.commit(1)->summary == "F1 subject", "plain term");
    log.set_filter({.text = "subject -F1", .fields = text});
    check(log.commit_rows() == 3, "excluded term");
    log.set_filter({.text = "!subject"});
    check(log.commit_rows() == 1 && log.commit(1)->summary == "Merge feat", "inverted");
    log.set_filter({.text = "\"body of b\""});
    check(log.commit_rows() == 1, "quoted phrase in the message body");
    log.set_filter({.text = "\"body of b\"", .case_sensitive = true});
    check(log.commit_rows() == 0, "case-sensitive");
    log.set_filter({.text = "v1", .fields = tortilla::filter_refname});
    check(log.commit_rows() == 1, "refname field");
    log.set_filter({.text = "^[AB] ", .regex = true});
    check(log.commit_rows() == 2, "regex");
    check(!log.set_filter({.text = "(", .regex = true}).empty(), "invalid regex reported");
    log.set_filter({});
    check(log.commit_rows() == 5, "cleared");

    // Commit order is git's --topo-order, for one branch and for --all.
    {
        sh(git + "checkout -qb side HEAD~2");
        commit("S1");
        sh(git + "checkout -q main");
        for (bool all : {false, true})
        {
            tortilla::log_model m(**ui);
            std::string err = "pending";
            m.load(repo, {.all_branches = all}, [&](const std::string& e) { err = e; });
            while (err == "pending")
            {
                (*ui)->run();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            std::string ours;
            for (size_t row = 1; row < m.row_count(); ++row) ours += m.commit(row)->id.hex() + "\n";
            std::vector<std::string> args{"log", "--topo-order", "--format=%H"};
            if (all) args.push_back("--all");
            check(ours == tortilla::run_git(repo, args).output,
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
        tortilla::log_model m(**ui);
        std::optional<std::string> err;
        m.load(repo, {.all_branches = true}, [&](const std::string& e) { err = e; });
        pump(err);
        auto refresh = [&]
        {
            std::optional<bool> reload;
            m.refresh_refs([&](bool r) { reload = r; });
            pump(reload);
            return *reload;
        };
        auto labelled = [&](const std::string& name)
        {
            for (size_t row = 0; row < m.row_count(); ++row)
                if (auto* refs = m.refs_of(row))
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
        using tortilla::expand_placeholders;
        check(expand_placeholders("meld %base %mine", "a", "b") == "meld \"$LOCAL\" \"$REMOTE\"",
              "placeholders: %base %mine");
        check(expand_placeholders("tool \"%base\" %mine", "a", "b") == "tool \"$LOCAL\" \"$REMOTE\"",
              "placeholders: already quoted");
        check(expand_placeholders("x /t1=%bname", "abc12345", "b") ==
                  "x /t1=\"abc12345\" \"$LOCAL\" \"$REMOTE\"",
              "placeholders: names only, sides appended");
        check(expand_placeholders("x %base %mine -L %yname", "a", "it's $x") ==
                  "x \"$LOCAL\" \"$REMOTE\" -L \"it's \\$x\"",
              "placeholders: names quoted for sh");
        check(expand_placeholders("bcompare", "a", "b") == "bcompare \"$LOCAL\" \"$REMOTE\"",
              "placeholders: none, sides appended");
        auto args =
            tortilla::custom_difftool_args("ls %base %mine", {"HEAD~1", "HEAD"}, "HEAD~1", "HEAD");
        tortilla::git_result r = tortilla::run_git(repo, args, tortilla::git_output::all);
        const bool ran = r.ok() && r.output.find("left") != std::string::npos &&
                         r.output.find("right") != std::string::npos;
        check(ran, "custom diff tool runs through git difftool --dir-diff");
        if (!ran) std::printf("     git exited %d:\n%s\n", r.exit_code, r.output.c_str());
    }

    // The git runner: synchronous, then streamed on a worker.
    tortilla::git_result r = tortilla::run_git(repo, {"rev-parse", "--abbrev-ref", "HEAD"});
    check(r.ok() && r.output == "main\n", "runGit output and exit code");
    check(!tortilla::run_git(repo, {"rev-parse", "no-such-ref"}).ok(), "runGit failure exit code");

    std::string streamed;
    int code = -2;
    auto proc = tortilla::git_process::start(
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
    proc = tortilla::git_process::start(**ui, repo, {"branch", "-D", "no-such-branch"}, {},
                                       [&](int c) { code = c; });
    while (code == -2)
    {
        (*ui)->run();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    check(code != 0, "GitProcess reports failure");
    check(tortilla::command_line({"commit", "-m", "two words"}) == "git commit -m \"two words\"",
          "commandLine quoting");

    // Status parsing: porcelain v1 -z plus numstat -z.
    {
        using namespace std::string_literals;
        std::string porcelain = " M a.txt\0?? build/\0R  new.txt\0old.txt\0UU c.txt\0A  img.png\0"s;
        std::string numstat = "3\t1\ta.txt\0"
                              "0\t0\t\0old.txt\0new.txt\0"
                              "-\t-\timg.png\0"s;
        auto files = tortilla::parse_status(porcelain, numstat);
        check(files.size() == 5, "status: five entries");
        if (files.size() == 5)
        {
            check(files[0].status == 'M' && files[0].added == 3 && files[0].removed == 1,
                  "status: modified with line counts");
            check(files[1].status == '?' && files[1].path == "build/",
                  "status: untracked directory");
            check(files[2].status == 'R' && files[2].path == "new.txt" &&
                      files[2].old_path == "old.txt",
                  "status: rename");
            check(files[3].status == 'U', "status: conflicted");
            check(files[4].status == 'A' && files[4].binary, "status: binary added");
        }
        check(tortilla::parse_status(" M x\0"s, "x\ty\tz\0garbage"s).size() == 1,
              "status: odd numstat output doesn't throw");
    }

    // Check for modifications' reloads: each cancels the git still running
    // and starts again. Whatever the timing, results arrive newest last and
    // never older after newer, and the last is the last options'.
    {
        sh("echo changed >> '" + repo + "/A.txt'");
        sh("echo new > '" + repo + "/untracked.txt'");
        tortilla::git_canceller stopped;
        stopped.cancel();
        check(tortilla::load_working_tree(repo, {}, &stopped).error == "cancelled",
              "reloads: a cancelled load says so");

        tortilla::repo_worker worker(**ui, repo);
        std::shared_ptr<tortilla::git_canceller> current;
        std::vector<int> delivered;
        std::vector<tortilla::file_change> last;
        constexpr int loads = 25;
        for (int i = 0; i < loads; ++i)
        {
            if (current) current->cancel();
            current = std::make_shared<tortilla::git_canceller>();
            const tortilla::working_tree_options o{.unversioned = i % 2 == 0};
            worker.submit(
                [&, i, o, c = current](tortilla::repo&, const tortilla::repo_worker::progress&)
                    -> std::function<void()>
                {
                    auto r = tortilla::load_working_tree(repo, o, c.get());
                    return [&, i, files = r.files]
                    {
                        delivered.push_back(i);
                        last = files;
                    };
                });
            // Toggles at uneven speeds: some land mid-load, some after.
            if (i % 3 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(i));
            (*ui)->run();
        }
        for (int spin = 0; spin < 1000 && (delivered.empty() || delivered.back() != loads - 1);
             ++spin)
        {
            (*ui)->run();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        check(!delivered.empty() && delivered.back() == loads - 1,
              "reloads: the newest load arrives last");
        check(std::ranges::is_sorted(delivered) &&
                  std::ranges::adjacent_find(delivered) == delivered.end(),
              "reloads: never an older result after a newer one");
        auto has = [&](const char* path)
        {
            return std::ranges::any_of(last, [&](const tortilla::file_change& f)
                                       { return f.path == path; });
        };
        check(has("A.txt") && has("untracked.txt"), "reloads: the last options' files");
        std::printf("     (%zu of %d loads delivered)\n", delivered.size(), loads);
    }

    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
