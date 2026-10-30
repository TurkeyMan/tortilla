// Headless checks of the log model on a generated repository:
//   tortilla-tests <scratch dir>
// Builds a repo with branches and merges via the git CLI, then loads it
// through log_model on the null UI backend.
#include "changes/working_tree.h"
#include "git/blame.h"
#include "git/clean.h"
#include "git/command.h"
#include "git/diff_tool.h"
#include "git/ignore_patterns.h"
#include "git/lfs.h"
#include "git/merge_tool.h"
#include "git/ref_list.h"
#include "git/reflog.h"
#include "git/remotes.h"
#include "git/repo.h"
#include "git/repo_worker.h"
#include "git/status.h"
#include "git/tree_list.h"
#include "log/log_model.h"
#include "shell.h"
#include "util/sha256.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
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

    // Ignore patterns, as the Ignore dialog writes them.
    {
        using tortilla::ignore_item;
        using tortilla::ignore_target;
        const std::vector<ignore_item> items{{"sub", "build.log", false}, {"sub", "out", true},
                                             {"", "#odd*.txt", false}, {"sub", "README", false}};
        auto lines = [](auto plan, std::string file)
        {
            for (auto& [f, l] : plan)
                if (f == file) return l;
            return std::vector<std::string>{};
        };
        auto here = tortilla::ignore_patterns(items, false, false, ignore_target::repository);
        check(here.size() == 1 &&
                  lines(here, ".gitignore") ==
                      std::vector<std::string>{"/sub/build.log", "/sub/out/", "/\\#odd\\*.txt", "/sub/README"},
              "ignore by name, only here, in the repository's .gitignore (anchored, escaped)");
        auto every = tortilla::ignore_patterns(items, false, true, ignore_target::repository);
        check(lines(every, ".gitignore") ==
                  std::vector<std::string>{"build.log", "out/", "\\#odd\\*.txt", "README"},
              "ignore by name in every folder");
        auto masks = tortilla::ignore_patterns(items, true, true, ignore_target::folder);
        check(lines(masks, "sub/.gitignore") == std::vector<std::string>{"*.log"} &&
                  lines(masks, ".gitignore") == std::vector<std::string>{"*.txt"},
              "ignore by extension in each folder's .gitignore (folders and extensionless skipped)");
        auto local = tortilla::ignore_patterns(items, true, false, ignore_target::folder);
        check(lines(local, "sub/.gitignore") == std::vector<std::string>{"/*.log"},
              "by extension only here, relative to the folder's own .gitignore");
        auto excl = tortilla::ignore_patterns(items, false, false, ignore_target::exclude);
        check(excl.size() == 1 && excl.front().first.empty() && excl.front().second.size() == 4,
              "the exclude file is one target");
        check(tortilla::literal_pattern("a b  ") == "a b\\ \\ ", "trailing spaces escaped, each one");

        const auto file = std::filesystem::path(argv[1]) / "ignore-test" / ".gitignore";
        std::filesystem::create_directories(file.parent_path());
        { std::ofstream(file, std::ios::binary) << "keep\n*.log"; }  // no final newline
        check(tortilla::append_ignore_lines(file, {"*.log", "new/"}).empty(), "append to an ignore file");
        std::ifstream in(file, std::ios::binary);
        const std::string got((std::istreambuf_iterator<char>(in)), {});
        check(got == "keep\n*.log\nnew/\n", "existing lines kept once, a newline added before new ones");
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

    // Format Patch's ranges, written for real: the files git names are
    // counted, so a range that took in too much or too little shows.
    {
        auto rev = [&](const std::string& r)
        {
            auto out =
                tortilla::run_git(repo, {"rev-parse", r}, tortilla::git_output::stdout_only).output;
            while (!out.empty() && std::isspace(static_cast<unsigned char>(out.back())))
                out.pop_back();
            return out;
        };
        const std::string a = rev("main~3"), b = rev("main~2"), c = rev("main~1"),
                          merge = rev("main");
        int run = 0;
        auto patches = [&](const tortilla::patch_range& range, bool signoff)
        {
            const std::string out = std::string(argv[1]) + "/patches" + std::to_string(++run);
            // Signing off needs a committer; CI's environment, if set, wins over this.
            std::vector<std::string> args{"-c", "user.name=Ada", "-c",
                                          "user.email=ada@example.com"};
            for (auto& arg : tortilla::format_patch_args(range, out, signoff)) args.push_back(arg);
            std::vector<std::string> files;
            if (tortilla::run_git(repo, args).exit_code != 0) return files;
            for (auto& e : std::filesystem::directory_iterator(out))
                files.push_back(e.path().filename().string());
            std::ranges::sort(files);
            return files;
        };
        check(tortilla::format_patch_args({.oldest = c, .newest = c}, "/out", true) ==
                  std::vector<std::string>{"format-patch", "-o", "/out", "--signoff", "-1", c},
              "format_patch_args: one commit takes -1");
        check(tortilla::format_patch_args({.oldest = b, .newest = merge}, "/out", false).back() ==
                  b + "^.." + merge,
              "format_patch_args: a range starts after the oldest's parent");
        const auto one = patches({.oldest = c, .newest = c}, true);
        check(one == std::vector<std::string>{"0001-C-subject.patch"},
              "format-patch: one commit, one file");
        if (one.size() == 1)
        {
            std::ifstream in(std::string(argv[1]) + "/patches1/" + one[0]);
            const std::string text((std::istreambuf_iterator<char>(in)), {});
            check(text.find("\nSigned-off-by: ") != std::string::npos, "format-patch: signed off");
        }
        check(patches({.oldest = a, .newest = c, .oldest_is_root = true}, false).size() == 3,
              "format-patch: a range from the root commit takes it in (A, B, C)");
        // F1 and C come in either order: they're siblings.
        const auto range = patches({.oldest = b, .newest = merge}, false);
        check(range.size() == 3 && range[0] == "0001-B-subject.patch",
              "format-patch: B to the merge is B, F1 and C, merges left out");
    }

    // Edit Notes: saved with git notes (a note commit needs an identity;
    // CI's environment, if set, wins over this), read back with libgit2.
    {
        const tortilla::commit* head = log.commit(1);
        const std::string hash = head->id.hex();
        check(tortilla::note_args(hash, " \n\t", "/n.txt") ==
                  std::vector<std::string>{"notes", "remove", "--ignore-missing", hash},
              "note_args: a blank note is removed");
        check(tortilla::note_args(hash, "x", "/n.txt") ==
                  std::vector<std::string>{"notes", "add", "-f", "-F", "/n.txt", hash},
              "note_args: a note is written from its file");
        auto r = tortilla::repo::open(repo);
        const std::string file = std::string(argv[1]) + "/note.txt";
        auto save = [&](const std::string& text)
        {
            std::ofstream(file, std::ios::binary) << text;
            std::vector<std::string> args{"-c", "user.name=Ada", "-c",
                                          "user.email=ada@example.com"};
            for (auto& arg : tortilla::note_args(hash, text, file)) args.push_back(arg);
            return tortilla::run_git(repo, args).exit_code == 0;
        };
        check(r && r->note(head->id).empty(), "note: none to begin with");
        check(save("first line\nsecond line\n") && r &&
                  r->note(head->id) == "first line\nsecond line\n",
              "note: added, lines kept");
        check(save("replaced\n") && r && r->note(head->id) == "replaced\n", "note: replaced");
        check(save("") && r && r->note(head->id).empty(), "note: emptied removes it");
        check(save(""), "note: removing a missing one is no error");
        // The notes commits aren't the history the checks below walk.
        (void)tortilla::run_git(repo, {"update-ref", "-d", "refs/notes/commits"});
    // Bisect's commands, run for real on a history of its own: seven
    // commits, the fifth bringing in the "bug", found by marking each
    // revision git checks out as the working tree says.
    {
        using tortilla::bisect_step;
        check(tortilla::parse_bisect_step("skip") == bisect_step::skip &&
                  !tortilla::parse_bisect_step("begin"),
              "parse_bisect_step: the command line's names");
        check(tortilla::bisect_args(bisect_step::start, {"bad", "good"}) ==
                  std::vector<std::string>{"bisect", "start", "bad", "good", "--"},
              "bisect_args: start takes bad, then good, then ends the revisions");
        check(tortilla::bisect_args(bisect_step::reset, {"x"}) ==
                  std::vector<std::string>{"bisect", "reset"},
              "bisect_args: reset takes no revision");
        const std::string b = std::string(argv[1]) + "/bisect";
        std::filesystem::create_directories(b);
        const std::string in = "git -C '" + b + "' -c user.name=Ada -c user.email=ada@example.com ";
        sh("git init -q -b main '" + b + "'");
        std::vector<std::string> ids;
        for (int i = 1; i <= 7; ++i)
        {
            if (i == 5) sh("echo bug > '" + b + "/bug.txt'");
            sh("echo " + std::to_string(i) + " > '" + b + "/n.txt'");
            sh(in + "add . && " + in + "commit -qm c" + std::to_string(i));
            std::string id =
                tortilla::run_git(b, {"rev-parse", "HEAD"}, tortilla::git_output::stdout_only)
                    .output;
            while (!id.empty() && std::isspace(static_cast<unsigned char>(id.back())))
                id.pop_back();
            ids.push_back(id);
        }
        auto state = [&]
        {
            auto r = tortilla::repo::open(b);
            return r ? r->state() : tortilla::repo::repo_state::none;
        };
        tortilla::git_result r =
            tortilla::run_git(b, tortilla::bisect_args(bisect_step::start, {ids[6], ids[0]}));
        check(r.ok() && r.output.find("Bisecting:") != std::string::npos,
              "bisect start: git says how many revisions are left");
        check(state() == tortilla::repo::repo_state::bisect,
              "bisect start: the repository is bisecting");
        // "is the first bad commit", or "first 'bad' commit" in newer gits.
        int steps = 0;
        while (r.ok() && r.output.find(" is the first ") == std::string::npos && ++steps < 10)
            r = tortilla::run_git(b, tortilla::bisect_args(std::filesystem::exists(b + "/bug.txt")
                                                               ? bisect_step::bad
                                                               : bisect_step::good));
        check(r.ok() && r.output.find(ids[4] + " is the first ") != std::string::npos,
              "bisect good/bad: finds the commit that brought the bug in");
        r = tortilla::run_git(b, tortilla::bisect_args(bisect_step::reset));
        std::string head =
            tortilla::run_git(b, {"rev-parse", "HEAD"}, tortilla::git_output::stdout_only).output;
        check(r.ok() && state() == tortilla::repo::repo_state::none && head.starts_with(ids[6]),
              "bisect reset: back on the branch, bisecting no more");
    }

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
            m.load(repo, {.refs = all ? tortilla::walk_refs::all : tortilla::walk_refs::head},
                   [&](const std::string& e) { err = e; });
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

    // The other starting points [spec 1.9.1], against the git log
    // equivalents: a ref, local branches, basic refs (a commit only another
    // kind of ref reaches is left out of both).
    {
        // Older than the rest: commits of one second tie-break differently.
        sh(git + "update-ref refs/other/x $(GIT_COMMITTER_DATE=@1000000000 GIT_AUTHOR_DATE=@1000000000 " +
           git + "commit-tree -m other HEAD^{tree})");
        auto order = [&](tortilla::walk_options opts)
        {
            tortilla::log_model m(**ui);
            std::string err = "pending";
            m.load(repo, opts, [&](const std::string& e) { err = e; });
            while (err == "pending")
            {
                (*ui)->run();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            std::string ours;
            for (size_t row = 1; row < m.row_count(); ++row) ours += m.commit(row)->id.hex() + "\n";
            return ours;
        };
        auto git_order = [&](std::vector<std::string> extra)
        {
            std::vector<std::string> args{"log", "--topo-order", "--format=%H"};
            args.insert(args.end(), extra.begin(), extra.end());
            return tortilla::run_git(repo, args).output;
        };
        check(order({.rev = "refs/heads/side"}) == git_order({"refs/heads/side"}), "walk from a ref");
        check(order({.refs = tortilla::walk_refs::local}) == git_order({"--branches", "HEAD"}),
              "walk local branches");
        check(order({.refs = tortilla::walk_refs::basic}) ==
                  git_order({"--branches", "--remotes", "--tags", "HEAD"}),
              "walk basic refs");
        check(order({.refs = tortilla::walk_refs::all}) == git_order({"--all"}), "walk all includes other refs");
        sh(git + "update-ref -d refs/other/x");
    }

    // Browse References' refs [spec 10.3] and filter [spec 10.6].
    {
        check(tortilla::strip_ref_name("refs/heads/a/b") == "a/b" &&
                  tortilla::strip_ref_name("refs/remotes/o/x") == "remotes/o/x" &&
                  tortilla::strip_ref_name("refs/tags/v1") == "tags/v1",
              "strip_ref_name");
        using tortilla::ref_filter_matches;
        const std::string row = "feature/x\nFix the Parser\nAda\nabc123";
        check(ref_filter_matches("parser ada", row), "filter: terms all present, any case");
        check(!ref_filter_matches("parser -ada", row), "filter: -term excludes");
        check(ref_filter_matches("\"the parser\"", row) && !ref_filter_matches("\"parser the\"", row),
              "filter: quoted phrase");
        check(ref_filter_matches("!zzz", row) && !ref_filter_matches("!ada", row), "filter: ! negates");

        // The tagger is the committer identity, which the environment
        // (makepkg sets one) takes over from the repository's.
        sh("GIT_COMMITTER_NAME=Ada GIT_COMMITTER_EMAIL=ada@example.com " + git + "tag -a -m 'release one' rel1");
        sh(git + "config branch.main.description 'the trunk'");
        sh(git + "remote add origin /nowhere");  // the upstream maps through its fetch refspec
        sh(git + "branch gone-up");
        sh(git + "config branch.gone-up.remote origin");
        sh(git + "config branch.gone-up.merge refs/heads/nowhere");
        sh(git + "checkout -qb feature/x");
        commit("FX");
        sh(git + "checkout -q main");
        const tortilla::ref_list_result all = tortilla::list_refs(repo);
        auto find = [&](const std::string& full) -> const tortilla::ref_entry*
        {
            for (auto& e : all.refs)
                if (e.full == full) return &e;
            return nullptr;
        };
        const auto* tag = find("refs/tags/rel1");
        check(all.error.empty() && tag && tag->type == "tag" && tag->author == "Ada" && tag->subject == "release one" &&
                  tag->author_date > 0,
              "list_refs: an annotated tag's tagger and message");
        const auto* main = find("refs/heads/main");
        check(main && main->description == "the trunk" && main->commit(), "list_refs: a branch's description");
        const auto* gone = find("refs/heads/gone-up");
        check(gone && gone->upstream == "(gone: origin/nowhere)", "list_refs: an upstream that isn't there is gone");
        const auto folders = tortilla::ref_folders(all.refs);
        check(std::ranges::contains(folders, std::string("refs/heads/feature")) &&
                  std::ranges::contains(folders, std::string("refs/tags")) &&
                  !std::ranges::contains(folders, std::string("refs/heads/main")),
              "ref_folders: folders, not refs");
        const auto merged = tortilla::list_refs(repo, tortilla::merged_filter::merged);
        check(std::ranges::none_of(merged.refs, [](auto& e) { return e.full == "refs/heads/feature/x"; }) &&
                  std::ranges::any_of(merged.refs, [](auto& e) { return e.full == "refs/heads/main"; }),
              "list_refs: merged into HEAD");
        sh(git + "branch -D -q feature/x gone-up");
        sh(git + "tag -d rel1 >/dev/null");
        sh(git + "config --unset branch.main.description");
        sh(git + "remote remove origin");
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
        m.load(repo, {.refs = tortilla::walk_refs::all}, [&](const std::string& e) { err = e; });
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
        // A file a commit added: no old side, which git calls /dev/null;
        // the tool gets an empty file instead.
        std::string first = tortilla::run_git(repo, {"rev-list", "--max-parents=0", "HEAD"},
                                              tortilla::git_output::stdout_only)
                                .output;
        while (!first.empty() && std::isspace(static_cast<unsigned char>(first.back())))
            first.pop_back();
        tortilla::git_result added = tortilla::run_git(
            repo,
            tortilla::custom_difftool_args("echo %base %mine",
                                           {"4b825dc642cb6eb9a060e54bf8d69288fbee4904", first}, "",
                                           "", {"A.txt"}, false),
            tortilla::git_output::stdout_only);
        check(added.ok() && added.output.find("/dev/null") == std::string::npos &&
                  added.output.find(tortilla::empty_file()) != std::string::npos,
              "custom diff tool: an added file's missing side is Tortilla's empty file, not "
              "/dev/null");
        if (!ran) std::printf("     git exited %d:\n%s\n", r.exit_code, r.output.c_str());
    }

    // core.editor as words, for View revision in alternative editor.
    {
        using words = std::vector<std::string>;
        check(tortilla::command_words("code --wait") == words{"code", "--wait"},
              "command words: split at spaces");
        check(tortilla::command_words("\"C:\\Program Files\\Notepad++\\notepad++.exe\" "
                                      "-multiInst  'a b'") ==
                  words{"C:\\Program Files\\Notepad++\\notepad++.exe", "-multiInst", "a b"},
              "command words: quotes keep spaces and go, backslashes stay");
        check(tortilla::command_words("  ").empty(), "command words: none in blanks");
        check(tortilla::command_words("x \"\"") == words{"x", ""},
              "command words: an empty quoted one");
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

    // Reflogs, for the RefLog dialog.
    {
        using tortilla::split_reflog_message;
        check(split_reflog_message("checkout: moving from main to feat\n", false) ==
                  std::pair<std::string, std::string>{"checkout", "moving from main to feat"},
              "reflog: action and message");
        check(split_reflog_message("commit (amend): fix: typo", false).first == "commit (amend)",
              "reflog: split at the first colon");
        check(split_reflog_message("WIP on main: 1a2b3c4 subject", true).first.empty(),
              "reflog: a stash's message has no action");
        check(tortilla::short_selector("refs/heads/main", 2) == "main@{2}" &&
                  tortilla::short_selector("refs/stash", 0) == "stash@{0}" &&
                  tortilla::short_selector("HEAD", 1) == "HEAD@{1}",
              "reflog: short selectors");

        auto r = tortilla::repo::open(repo);
        const auto refs = r ? tortilla::reflog_refs(*r) : std::vector<std::string>{};
        check(!refs.empty() && refs.front() == "HEAD" &&
                  std::ranges::contains(refs, "refs/heads/main"),
              "reflog: HEAD first, then the branches");
        const auto head =
            r ? tortilla::read_reflog(*r, "HEAD") : std::unexpected(std::string("no repo"));
        check(head && !head->empty() && head->front().selector == "HEAD@{0}" &&
                  head->front().id == tortilla::resolve(repo, "HEAD"),
              "reflog: HEAD's newest entry is where HEAD is");
        check(head &&
                  std::ranges::any_of(
                      *head, [](const tortilla::reflog_entry& e)
                      { return e.action == "checkout" && e.message.starts_with("moving from"); }),
              "reflog: a checkout's entry");

        sh("echo stashed >> '" + repo + "/A.txt' && " + git + "stash -q");
        r = tortilla::repo::open(repo);
        const auto stash =
            r ? tortilla::read_reflog(*r, "refs/stash") : std::unexpected(std::string("no repo"));
        check(stash && stash->size() == 1 && stash->front().selector == "stash@{0}" &&
                  stash->front().action.empty() && stash->front().message.starts_with("WIP on"),
              "reflog: the stash's entry");
        sh(git + "stash drop -q");
    }

    // Changed Files: any two revisions' files, or one's against the working tree.
    {
        auto r = tortilla::repo::open(repo);
        auto between = [&](const std::string& from, const std::string& to)
        {
            std::vector<std::string> paths;
            if (r)
                (void)r->changes_between(
                    from, to,
                    {.listed = [&](const std::vector<tortilla::file_change>& files)
                     {
                         for (const auto& f : files) paths.push_back(f.path);
                     }});
            std::ranges::sort(paths);
            return paths;
        };
        // From A (the root) to the merge: B, F1 and C were added since.
        const std::string root = tortilla::run_git(repo, {"rev-list", "--max-parents=0", "HEAD"},
                                                   tortilla::git_output::stdout_only)
                                     .output.substr(0, 40);
        check(between(root, "HEAD") == std::vector<std::string>{"B.txt", "C.txt", "F1.txt"},
              "changes between: two revisions' files");
        sh("echo changed >> '" + repo + "/B.txt'");
        check(between("HEAD", "") == std::vector<std::string>{"B.txt"},
              "changes between: a revision and the working tree");
        sh("cd '" + repo + "' && git checkout -q -- B.txt");
    }

    // A log of a path: only the commits that changed it, their parents
    // rewritten to join those up (A, B, F1 and C each added their file).
    {
        auto r = tortilla::repo::open(repo);
        auto walked = [&](std::vector<std::string> paths)
        {
            std::vector<tortilla::commit> out;
            if (r)
                (void)r->walk({.paths = std::move(paths)},
                              [&](tortilla::commit&& c)
                              {
                                  out.push_back(std::move(c));
                                  return true;
                              });
            return out;
        };
        const auto f1 = walked({"F1.txt"});
        check(f1.size() == 1 && f1[0].summary == "F1 subject" && f1[0].parents.empty(),
              "path log: one file's one commit, no parent before it");
        const auto ac = walked({"A.txt", "C.txt"});
        check(ac.size() == 2 && ac[0].summary == "C subject" && ac[1].summary == "A subject" &&
                  ac[0].parents == std::vector<tortilla::oid>{ac[1].id},
              "path log: two files' commits, the newer's parent rewritten to the older");
        check(walked({"missing.txt"}).empty(), "path log: a path no commit changed");

        // Follow renames: a file's history from before its move.
        const std::string moved = std::string(argv[1]) + "/follow";
        std::filesystem::create_directories(moved);
        const std::string g = "cd '" + moved + "' && git -c user.name=Ada -c user.email=ada@x ";
        sh("git init -q '" + moved + "'");
        sh("printf 'one\\ntwo\\nthree\\nfour\\n' > '" + moved + "/old.txt' && " + g + "add . && " +
           g + "commit -qm made");
        sh(g + "mv old.txt new.txt && " + g + "commit -qm moved");
        sh("printf 'five\\n' >> '" + moved + "/new.txt' && " + g + "commit -qam grown");
        auto m = tortilla::repo::open(moved);
        auto follow = [&](bool on)
        {
            std::vector<tortilla::commit> out;
            if (m)
                (void)m->walk({.paths = {"new.txt"}, .follow = on},
                              [&](tortilla::commit&& c)
                              {
                                  out.push_back(std::move(c));
                                  return true;
                              });
            return out;
        };
        const auto plain = follow(false), followed = follow(true);
        check(plain.size() == 2, "path log: without following, the history stops at the move");
        check(followed.size() == 3 && followed[2].summary == "made" &&
                  followed[1].parents.size() == 1 && followed[1].parents[0] == followed[2].id &&
                  followed[2].parents.empty(),
              "path log: following the rename reaches the commit that made it, as a line");
    }

    // Blame: which commit each line comes from, across a rename.
    {
        const std::string dir = std::string(argv[1]) + "/blame";
        // The author through the environment, which CI's own (GIT_AUTHOR_NAME)
        // would override over -c user.name.
        const std::string g = "cd '" + dir + "' && GIT_AUTHOR_NAME=Ada GIT_AUTHOR_EMAIL=ada@x git -c user.name=Ada -c user.email=ada@x ";
        sh("git init -q '" + dir + "'");
        sh("printf 'one\\ntwo\\nthree\\n' > '" + dir + "/f.txt' && " + g + "add . && " + g +
           "commit -qm first");
        sh("printf 'one\\nTWO\\nthree\\n' > '" + dir + "/f.txt' && " + g + "commit -qam second");
        sh(g + "mv f.txt g.txt && " + g + "commit -qm moved");
        auto run = [&](tortilla::blame_options o, std::string path = "g.txt")
        {
            const auto r = tortilla::run_git(dir, tortilla::blame_args(path, o),
                                             tortilla::git_output::stdout_only);
            return tortilla::parse_blame(r.output);
        };
        const auto lines = run({});
        check(lines.size() == 3 && lines[0].text == "one" && lines[1].text == "TWO" &&
                  lines[2].text == "three",
              "blame: every line, in order");
        check(lines.size() == 3 && lines[0].commit->summary == "first" &&
                  lines[1].commit->summary == "second" && lines[0].commit == lines[2].commit &&
                  lines[0].commit->author == "Ada" && lines[0].commit->author_email == "ada@x" &&
                  lines[0].commit->author_time > 0,
              "blame: each line's commit, shared by lines from the same one");
        check(
            lines.size() == 3 && lines[0].filename == "f.txt" && lines[1].original_line == 2 &&
                lines[1].commit->previous_filename == "f.txt" &&
                !lines[1].commit->previous_hash.empty(),
            "blame: the name and line in the commit, and the commit before it, across the rename");
        sh("printf 'one\\nTWO\\nthree\\nfour\\n' > '" + dir + "/g.txt'");
        const auto wt = run({});
        check(wt.size() == 4 && wt[3].commit->uncommitted() && !wt[0].commit->uncommitted(),
              "blame: the working tree's own lines are uncommitted");
        // The slider's revisions: the file's commits, its name in each.
        const auto revs = tortilla::parse_file_revisions(
            tortilla::run_git(dir, tortilla::file_revision_args("g.txt"),
                              tortilla::git_output::stdout_only)
                .output);
        check(revs.size() == 3 && revs[0].summary == "moved" && revs[0].path == "g.txt" &&
                  revs[1].path == "f.txt" && revs[2].summary == "first" &&
                  revs[2].author == "Ada" && revs[2].time > 0,
              "blame: the file's revisions, newest first, with its name in each");
        const auto older = revs.size() == 3 ? run({.revision = revs[1].hash}, revs[1].path)
                                            : std::vector<tortilla::blame_line>{};
        check(older.size() == 3 && older[1].commit->summary == "second",
              "blame: at a revision, by its name there");
    }

    // The Repository Browser's reading: a revision's folders and files, in process.
    {
        const std::string dir = std::string(argv[1]) + "/browse";
        const std::string g =
            "cd '" + dir +
            "' && GIT_AUTHOR_NAME=Ada GIT_AUTHOR_EMAIL=ada@x GIT_COMMITTER_NAME=Ada "
            "GIT_COMMITTER_EMAIL=ada@x git -c user.name=Ada -c user.email=ada@x ";
        sh("git init -q '" + dir + "' && mkdir -p '" + dir + "/src/core' '" + dir + "/docs'");
        // The executable bit and the symlink set in the index, as Windows
        // has neither on disk.
        sh("printf 'abc' > '" + dir + "/src/core/a.txt' && printf 'readme\\n' > '" + dir +
           "/README' && printf '#!/bin/sh\\n' > '" + dir + "/run.sh' && printf 'x' > '" + dir +
           "/docs/d.md' && " + g + "add . && " + g + "update-index --chmod=+x run.sh && " + g +
           "update-index --add --cacheinfo \"120000,$(printf README | git hash-object -w "
           "--stdin),link\" && " +
           g + "commit -qm first");
        sh("printf 'abcdefgh' > '" + dir + "/src/core/a.txt' && " + g + "add src/core/a.txt && " +
           g + "commit -qm second");
        auto r = tortilla::repo::open(dir);
        check(r.has_value(), "browse: the repository opens");
        if (r)
        {
            using kind = tortilla::tree_entry::kind;
            const auto head = tortilla::resolve_tree(*r, "HEAD");
            const auto older = tortilla::resolve_tree(*r, "HEAD~1");
            check(head && head->commit && head->subject == "second" && older &&
                      older->subject == "first" && older->tree != head->tree,
                  "browse: a revision resolves to its commit, subject and tree");
            check(!tortilla::resolve_tree(*r, "no-such-branch"),
                  "browse: an unknown revision is an error");
            const auto by_tree = head ? tortilla::resolve_tree(*r, head->tree.hex()) : head;
            check(by_tree && !by_tree->commit && by_tree->tree == head->tree,
                  "browse: a tree's own id browses it, with no commit");
            const auto folders =
                head ? tortilla::tree_folders(*r, head->tree) : std::vector<std::string>{};
            check(folders == std::vector<std::string>{"docs", "src", "src/core"},
                  "browse: every folder, each before those inside it");
            const auto top = head ? tortilla::tree_entries(*r, head->tree, "")
                                  : std::expected<std::vector<tortilla::tree_entry>, std::string>{};
            auto find = [](const auto& entries,
                           const std::string& name) -> const tortilla::tree_entry*
            {
                for (const auto& e : entries)
                    if (e.name == name) return &e;
                return nullptr;
            };
            const tortilla::tree_entry *readme = top ? find(*top, "README") : nullptr,
                                       *run = top ? find(*top, "run.sh") : nullptr,
                                       *link = top ? find(*top, "link") : nullptr,
                                       *src = top ? find(*top, "src") : nullptr;
            check(top && top->size() == 5 && readme && readme->type == kind::file &&
                      readme->size == 7 && run && run->type == kind::executable && link &&
                      link->type == kind::symlink && link->size == 6 && src &&
                      src->type == kind::folder,
                  "browse: the top's entries, with their kinds and sizes");
            const tortilla::oid none;
            const auto core_now = tortilla::tree_entries(*r, head ? head->tree : none, "src/core");
            const auto core_then =
                tortilla::tree_entries(*r, older ? older->tree : none, "src/core");
            check(core_now && core_now->size() == 1 && (*core_now)[0].size == 8 && core_then &&
                      core_then->size() == 1 && (*core_then)[0].size == 3 &&
                      (*core_then)[0].id != (*core_now)[0].id,
                  "browse: an older revision shows a changed file's old size");
            check(head && !tortilla::tree_entries(*r, head->tree, "nowhere") &&
                      !tortilla::tree_entries(*r, head->tree, "README"),
                  "browse: a folder that isn't there, or a file, is an error");
        }
    }

    // Settings' Remotes page: what Save runs, run on a repository.
    {
        const std::string dir = std::string(argv[1]) + "/remotes";
        sh("git init -q '" + dir + "'");
        auto apply = [&](const std::string& from, const tortilla::remote_settings& to)
        {
            for (const auto& args : tortilla::save_remote_commands(from, to))
                if (const auto r = tortilla::run_git(dir, args); !r.ok() && r.exit_code != 5)
                    return false;
            return true;
        };
        auto value = [&](const std::string& key) { return tortilla::config_value(dir, key); };
        check(apply({}, {.name = "up",
                         .url = "https://example.com/a.git",
                         .tag_opt = "--no-tags",
                         .prune = true}) &&
                  value("remote.up.url") == "https://example.com/a.git" &&
                  value("remote.up.tagOpt") == "--no-tags" && value("remote.up.prune") == "true",
              "remotes: added, with tags and prune");
        check(apply("up", {.name = "origin",
                           .url = "https://example.com/b.git",
                           .push_url = "ssh://example.com/b.git"}) &&
                  tortilla::remote_names(dir) == std::vector<std::string>{"origin"} &&
                  value("remote.origin.url") == "https://example.com/b.git" &&
                  value("remote.origin.pushurl") == "ssh://example.com/b.git" &&
                  value("remote.origin.tagOpt").empty() && value("remote.origin.prune").empty(),
              "remotes: renamed and changed, the options unset");
    }

    // Clone's URL from the clipboard [spec 7.1.2].
    {
        using tortilla::clone_url_in;
        check(clone_url_in("https://github.com/TurkeyMan/tortilla.git") ==
                  "https://github.com/TurkeyMan/tortilla.git",
              "clipboard URL: plain https");
        check(clone_url_in("  \"git@github.com:a/b.git\"\nsecond line") == "git@github.com:a/b.git",
              "clipboard URL: first line, quotes stripped");
        check(clone_url_in("git clone --depth 1 -b main ssh://host/r.git dir") ==
                  "ssh://host/r.git",
              "clipboard URL: a git clone command, options skipped");
        check(clone_url_in("C:\\repos\\thing") == "C:\\repos\\thing",
              "clipboard URL: a drive-letter path");
        check(clone_url_in("hello world").empty() && clone_url_in("").empty() &&
                  clone_url_in("ftp://example.com/x").empty(),
              "clipboard URL: not one");
    }

    // Clean up: git clean's arguments, and its dry run read back.
    {
        using tortilla::clean_options;
        const std::vector<std::string> plain = tortilla::clean_args({}, true, ".");
        check(plain == std::vector<std::string>{"-c", "core.quotepath=false", "clean", "-n", "-d"},
              "clean up: a dry run of untracked files and folders, at the top");
        check(tortilla::clean_args({.files = clean_options::all, .directories = false}, false,
                                   "src") == std::vector<std::string>{"-C", "src", "-c",
                                                                      "core.quotepath=false",
                                                                      "clean", "-f", "-x"},
              "clean up: everything untracked in a folder, files only, from inside it");
        check(tortilla::clean_args({.files = clean_options::ignored}, false, ".")[5] == "-X",
              "clean up: ignored only");
        check(tortilla::clean_args({}, false, ".", {"a.txt"}).back() == "a.txt",
              "clean up: files chosen one by one");
        check(tortilla::parse_clean_listing(
                  "Would remove a b.txt\nWould remove \"q\\\"uote\\303\\251.txt\"\n"
                  "Entering 'sub'\nWould remove junk/\n") ==
                  std::vector<std::string>{"a b.txt", "q\"uote\xc3\xa9.txt", "sub/junk/"},
              "clean up: the listing, quoted paths and submodules");

        const std::string dir = std::string(argv[1]) + "/cleanup";
        std::filesystem::create_directories(dir + "/build");
        sh("git init -q '" + dir + "' && printf 'build/\\n*.log\\n' > '" + dir + "/.gitignore'");
        sh("cd '" + dir +
           "' && git add .gitignore && git -c user.name=Ada -c user.email=ada@x commit -qm init");
        sh("cd '" + dir +
           "' && touch new.txt x.log build/out.o && mkdir -p tmp && touch tmp/t.txt");
        auto listed = [&](clean_options o, std::string folder = ".")
        {
            auto r = tortilla::run_git(dir, tortilla::clean_args(o, true, folder),
                                       tortilla::git_output::stdout_only);
            auto l = tortilla::parse_clean_listing(r.output);
            std::ranges::sort(l);
            return l;
        };
        check(listed({}) == std::vector<std::string>{"new.txt", "tmp/"},
              "clean up: untracked, not ignored");
        check(listed({.files = clean_options::ignored}) ==
                  std::vector<std::string>{"build/", "x.log"},
              "clean up: only ignored");
        check(listed({.files = clean_options::all, .directories = false}) ==
                  std::vector<std::string>{"new.txt", "x.log"},
              "clean up: all untracked files, no folders");
        // An untracked folder cleaned from inside goes as a whole: "./".
        check(listed({}, "tmp") == std::vector<std::string>{"./"},
              "clean up: a folder, from inside it");
    }

    // A repository uses Git LFS by its attributes, nested ones too, whether
    // or not git-lfs is installed (no filter is set up here).
    {
        const std::string dir = std::string(argv[1]) + "/lfs-attributes";
        std::filesystem::create_directories(dir + "/assets");
        sh("git init -q '" + dir + "'");
        sh("printf 'x' > '" + dir + "/a.txt' && cd '" + dir + "' && git add a.txt");
        check(!tortilla::uses_lfs(dir), "LFS: a repository without the lfs filter");
        sh("printf '*.bin filter=lfs diff=lfs merge=lfs -text\\n' > '" + dir +
           "/assets/.gitattributes'");
        sh("printf 'x' > '" + dir + "/assets/b.bin' && cd '" + dir + "' && git add .");
        check(tortilla::uses_lfs(dir), "LFS: a nested .gitattributes's filter=lfs");
        check(!tortilla::uses_lfs(dir + "/missing"), "LFS: no repository");
    }

    // Git LFS, where git-lfs is installed: Tortilla's git does the LFS part
    // itself. A clone and a push move the files, the working tree lists see
    // a file as git does, and a revision's file is the file, not its pointer.
    if (auto v = rap::sys::process::run("git", std::vector<std::string>{"lfs", "version"});
        !v || v->exit_code != 0)
        std::printf("skip Git LFS: git-lfs isn't installed\n");
    else
    {
        const std::string base = std::string(argv[1]) + "/lfs";
        const std::string origin = base + "/origin.git";
        const std::string seed = base + "/seed";
        const std::string work = base + "/work";
        std::filesystem::create_directories(base);
        // The filters as `git lfs install` puts them in the user's config,
        // which the test leaves alone.
        const std::string lfs = " -c filter.lfs.process='git-lfs filter-process'"
                                " -c filter.lfs.required=true"
                                " -c 'filter.lfs.clean=git-lfs clean -- %f'"
                                " -c 'filter.lfs.smudge=git-lfs smudge -- %f' ";
        auto in = [](const std::string& dir)
        { return "cd '" + dir + "' && git -c user.name=Ada -c user.email=ada@example.com "; };
        sh("git init -q --bare -b main '" + origin + "'");
        sh("git clone -q" + lfs + "'" + origin + "' '" + seed + "' 2>&1");
        sh(in(seed) + "lfs install --local >/dev/null");
        sh(in(seed) + "lfs track '*.bin' >/dev/null");
        sh("printf 'one\\000binary' > '" + seed + "/a.bin'");
        sh(in(seed) + "add . && " + in(seed) + "commit -qm one");
        sh("printf 'two\\000binary!' > '" + seed + "/a.bin'");
        sh(in(seed) + "commit -qam two && " + in(seed) + "push -q origin HEAD:main 2>&1");
        sh("git clone -q" + lfs + "'" + origin + "' '" + work + "' 2>&1");
        auto read = [](const std::string& path)
        {
            std::ifstream in(path, std::ios::binary);
            return std::string((std::istreambuf_iterator<char>(in)), {});
        };
        using namespace std::string_literals;
        check(read(work + "/a.bin") == "two\0binary!"s,
              "LFS: a clone checks out the file, not its pointer");
        check(tortilla::uses_lfs(work) && tortilla::lfs_installed() && !tortilla::lfs_missing(work),
              "LFS: nothing missing with git-lfs installed");
        check(tortilla::run_git(work, {"show", "HEAD:a.bin"}, tortilla::git_output::stdout_only)
                  .output.starts_with("version https://git-lfs"),
              "LFS: the commit holds a pointer");
        check(tortilla::revision_file(work, "HEAD~1", "a.bin").output == "one\0binary"s,
              "LFS: revision_file gives an older revision's file");
        check(tortilla::load_working_tree(work, {}).files.empty(),
              "LFS: a fresh clone has no changes");
        sh("touch -d 2001-01-01 '" + work + "/a.bin'");
        check(tortilla::load_working_tree(work, {}).files.empty(),
              "LFS: a touched file is unchanged");
        sh("printf 'three' > '" + work + "/a.bin'");
        const auto changed = tortilla::load_working_tree(work, {}).files;
        check(changed.size() == 1 && changed[0].path == "a.bin" && changed[0].status == 'M',
              "LFS: an edited file is modified");
        sh(in(work) + "commit -qam three && " + in(work) + "push -q origin HEAD:main 2>&1");
        size_t objects = 0;
        std::error_code ec;
        for (auto it = std::filesystem::recursive_directory_iterator(origin + "/lfs/objects", ec);
             !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
            objects += it->is_regular_file();
        check(objects == 3, "LFS: a push uploads the new file to the remote's LFS store");
    }

    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
