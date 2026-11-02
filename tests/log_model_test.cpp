// Headless checks of the log model on a generated repository:
//   tortilla-tests <scratch dir>
// Builds a repo with branches and merges via the git CLI, then loads it
// through log_model on the null UI backend.
#include "cache/badge_cache.h"
#include "cache/cache_main.h"
#include "cache/channel.h"
#include "changes/file_model.h"
#include "changes/working_tree.h"
#include "forge/ci.h"
#include "forge/ci_poll.h"
#include "forge/forge.h"
#include "forge/pr_details.h"
#include "forge/pr_watch.h"
#include "forge/remote_sign_in.h"
#include "git/archive.h"
#include "git/badges.h"
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
#include "git/submodule.h"
#include "git/sync.h"
#include "git/tree_list.h"
#include "git/worktrees.h"
#include "log/log_model.h"
#include "raw_client.h"
#include "shell.h"
#include "util/json.h"
#include "util/sha256.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

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

    // JSON, as the forges answer.
    {
        using tortilla::json::value;
        const std::string reply = R"( {"login": "ada", "id": 42, "admin": false, "name": null,
            "bio": "tab\there \"q\" \u00e9 \ud83c\udf2e", "list": [1, [2, {"x": "]"}], "three"],
            "esc\u0061ped": 7, "neg": -3.5e2, "empty": {}, "none": []} )";
        const auto j = tortilla::json::parse(reply);
        check(j && j->kind() == value::type::object, "json: an object parses");
        check(j && (*j)["login"].str() == "ada" && (*j)["login"].str_is("ada"),
              "json: a string member");
        check(j && (*j)["id"].integer() == 42 && !(*j)["neg"].integer(),
              "json: integers, and a non-integer");
        check(j && !(*j)["admin"].boolean(true) && (*j)["name"].is_null() && (*j)["name"],
              "json: false and null");
        check(j && !(*j)["nothing"] && (*j)["nothing"].str("x") == "x", "json: a missing member");
        check(j && (*j)["bio"].str() == "tab\there \"q\" \xc3\xa9 \xf0\x9f\x8c\xae",
              "json: escapes and a surrogate pair to UTF-8");
        check(j && (*j)["list"][1][1]["x"].str() == "]" && (*j)["list"][2].str() == "three" &&
                  !(*j)["list"][3],
              "json: nested arrays, a bracket in a string");
        check(j && (*j)["escaped"].integer() == 7, "json: an escaped key");
        int n = 0;
        if (j)
            for (value e : (*j)["list"].elements()) n += e ? 1 : 0;
        int members = 0;
        if (j)
            for (auto [key, v] : j->members()) members += !key.empty() && v ? 1 : 0;
        check(n == 3 && members == 10, "json: elements and members in order");
        check(j && (*j)["empty"].members().begin() == (*j)["empty"].members().end() &&
                  (*j)["none"].elements().begin() == (*j)["none"].elements().end(),
              "json: empty object and array");
        bool bad = false;
        for (const char* text : {"", "{", "{\"a\":}", "[1,]", "{\"a\" 1}", "\"\\x\"", "01", "1.",
                                 "tru", "[1] 2", "\"a\nb\"", "{\"a\":1,}"})
            bad |= tortilla::json::parse(text).has_value();
        check(!bad, "json: malformed text refused");
        check(tortilla::json::parse(std::string(300, '[') + std::string(300, ']')) == std::nullopt,
              "json: nesting too deep refused");
        check(tortilla::json::quoted("a\"b\\c\nd\x01") == R"("a\"b\\c\nd\u0001")", "json: quoting");
        // parse() views the text: it must outlive the value.
        const std::string text = tortilla::json::quoted("x\ty\"\x1f");
        const auto round = tortilla::json::parse(text);
        check(round && round->str() == "x\ty\"\x1f", "json: quoted reads back");
    }

    // A remote's URL to its account, by host.
    {
        using tortilla::forge_account;
        using tortilla::forge_kind;
        const std::vector<forge_account> accounts{
            {.kind = forge_kind::github, .host = "github.com", .login = "ada"},
            {.kind = forge_kind::gitea, .host = "git.example.org:3000", .login = "bob"}};
        auto login = [&](std::string_view url)
        {
            const auto a = tortilla::account_for_url(url, accounts);
            return a ? a->login : std::string("-");
        };
        check(login("https://github.com/x/y.git") == "ada" &&
                  login("git@github.com:x/y.git") == "ada" &&
                  login("ssh://git@GitHub.com/x/y") == "ada" &&
                  login("https://ada:tok@github.com:443/x") == "ada",
              "forge: https, scp-style and ssh URLs find the account");
        check(login("https://git.example.org:3000/x/y") == "bob" &&
                  login("ssh://git@git.example.org:2222/x/y") == "bob",
              "forge: a host with a port, by its name");
        check(login("https://gitlab.com/x/y") == "-" && login("/srv/repo.git") == "-",
              "forge: no account for another host or a path");
        check(tortilla::api_root(forge_kind::github, "github.com") == "https://api.github.com" &&
                  tortilla::api_root(forge_kind::github, "ghe.corp") == "https://ghe.corp/api/v3" &&
                  tortilla::api_root(forge_kind::gitlab, "gitlab.com") ==
                      "https://gitlab.com/api/v4" &&
                  tortilla::api_root(forge_kind::gitea, "codeberg.org") ==
                      "https://codeberg.org/api/v1",
              "forge: API roots");
        // A remote's repository on its forge.
        {
            using tortilla::forge_repo;
            const auto https = tortilla::repo_of_url("https://github.com/ada/engine.git");
            const auto scp = tortilla::repo_of_url("git@gitlab.com:group/sub/engine.git");
            const auto ssh = tortilla::repo_of_url("ssh://git@git.corp:2222/ada/engine/");
            check(https && *https == forge_repo{"github.com", "ada", "engine"} && scp &&
                      *scp == forge_repo{"gitlab.com", "group/sub", "engine"} && ssh &&
                      *ssh == forge_repo{"git.corp", "ada", "engine"} &&
                      !tortilla::repo_of_url("/srv/git/engine") &&
                      !tortilla::repo_of_url("https://github.com/engine"),
                  "forge: a remote's repository, owner and name");
        }
        // What the Remotes page says of a remote, and whether it offers Sign
        // in... (a button for a private repository, a link for a public one).
        {
            using namespace tortilla;
            using offer = remote_sign_in::offer;
            check(known_forge("github.com") == forge_kind::github &&
                      known_forge("GitLab.com") == forge_kind::gitlab &&
                      known_forge("codeberg.org") == forge_kind::gitea &&
                      !known_forge("git.example.org"),
                  "remote sign-in: the forges known by their host's name");
            const forge_account ada{
                .kind = forge_kind::github, .host = "github.com", .login = "ada"};
            const auto off = describe_remote(false, nullptr, repo_seen::unknown, false);
            check(off.summary.empty() && off.text.empty() && off.sign_in == offer::none &&
                      off.mark == check_state::none,
                  "remote sign-in: nothing said of a remote that isn't on a forge");
            const auto in = describe_remote(true, &ada, repo_seen::shown, false);
            const auto in_unknown = describe_remote(true, &ada, repo_seen::unknown, true);
            check(in.summary == "ada" && in.mark == check_state::success &&
                      in.sign_in == offer::none && in.text.find("ada") != std::string::npos &&
                      in_unknown.mark == check_state::success && in_unknown.sign_in == offer::none,
                  "remote sign-in: signed in is a tick and the account, nothing offered");
            const auto blind = describe_remote(true, &ada, repo_seen::hidden, false);
            check(blind.mark == check_state::failure && blind.sign_in == offer::link &&
                      blind.text.find("can't see") != std::string::npos,
                  "remote sign-in: an account that gets a 404 can't see it");
            const auto hidden = describe_remote(true, nullptr, repo_seen::hidden, false);
            const auto shown = describe_remote(true, nullptr, repo_seen::shown, false);
            const auto asking = describe_remote(true, nullptr, repo_seen::unknown, true);
            const auto unknown = describe_remote(true, nullptr, repo_seen::unknown, false);
            check(hidden.summary == "Private" && hidden.sign_in == offer::button &&
                      hidden.text.starts_with("Private:") && hidden.mark == check_state::none,
                  "remote sign-in: private (a 404 as anyone) is the button");
            check(shown.summary == "Public" && shown.sign_in == offer::link &&
                      shown.text.starts_with("Public:"),
                  "remote sign-in: public is a link");
            check(asking.sign_in == offer::button && unknown.sign_in == offer::button &&
                      asking.summary == "Not signed in" && asking.text != unknown.text,
                  "remote sign-in: not known yet, or no answer, is the button");
        }
        // Which branches are pull requests' heads: the head repository's
        // remote branch (a fork's through the fork's remote); never a local one.
        {
            using namespace tortilla;
            const forge_repo base{"github.com", "org", "engine"},
                fork{"github.com", "ada", "engine"};
            const std::vector<forge_remote> remotes{{.name = "upstream", .repo = base},
                                                    {.name = "origin", .repo = fork}};
            std::vector<open_pr> prs;
            prs.push_back(
                {.pr = {.number = 7, .head = fork, .head_branch = "fix"}, .into = remotes[0]});
            prs.push_back(
                {.pr = {.number = 8, .head = base, .head_branch = "feat/x"}, .into = remotes[0]});
            const auto heads = match_pull_requests(remotes, prs);
            check(heads.size() == 2 && heads.at("refs/remotes/origin/fix").pr.number == 7 &&
                      heads.at("refs/remotes/upstream/feat/x").pr.number == 8,
                  "forge: pull requests' head branches, remote ones only");
        }
        // GitHub's noreply addresses name the account: its picture without the API.
        {
            using tortilla::noreply_avatar_url;
            check(noreply_avatar_url("12345+ada@users.noreply.github.com", 96) ==
                          "https://avatars.githubusercontent.com/u/12345?s=96" &&
                      noreply_avatar_url(" 49699333+dependabot[bot]@Users.NoReply.GitHub.com ",
                                         192) ==
                          "https://avatars.githubusercontent.com/u/49699333?s=192",
                  "avatars: a noreply address with an id, by the id");
            check(noreply_avatar_url("ada-l@users.noreply.github.com", 96) ==
                      "https://github.com/ada-l.png?size=96",
                  "avatars: an older noreply address, by the login");
            check(noreply_avatar_url("ada@example.com", 96).empty() &&
                      noreply_avatar_url("@users.noreply.github.com", 96).empty() &&
                      noreply_avatar_url("x1+@users.noreply.github.com", 96).empty() &&
                      noreply_avatar_url("a/b@users.noreply.github.com", 96).empty() &&
                      noreply_avatar_url("ada@users.noreply.github.com.evil", 96).empty() &&
                      noreply_avatar_url("ada@xusers.noreply.github.com", 96).empty(),
                  "avatars: anything else isn't one");
        }
        // Which remote leads, for asking a forge about an author: the newest
        // default branch, each host once.
        {
            using namespace tortilla;
            const std::string dir = std::string(argv[1]) + "/leading";
            const std::string g = "git -C '" + dir + "' -c user.name=Ada -c user.email=a@b ";
            sh("git init -q -b main '" + dir + "'");
            auto commit_at = [&](const std::string& when)
            { sh("GIT_COMMITTER_DATE='" + when + "' " + g + "commit -q --allow-empty -m x"); };
            commit_at("2020-01-01T00:00:00Z");
            sh(g + "update-ref refs/remotes/old/master HEAD");
            commit_at("2021-01-01T00:00:00Z");
            sh(g + "update-ref refs/remotes/upstream/trunk HEAD && " + g +
               "symbolic-ref refs/remotes/upstream/HEAD refs/remotes/upstream/trunk");
            sh(g + "update-ref refs/remotes/origin/master HEAD");  // main, below, comes first
            commit_at("2022-01-01T00:00:00Z");
            sh(g + "update-ref refs/remotes/lab/main HEAD");
            sh(g + "update-ref refs/remotes/fork/main HEAD");
            commit_at("2019-01-01T00:00:00Z");
            sh(g + "update-ref refs/remotes/origin/main HEAD");
            auto on = [](std::string name, std::string host)
            {
                return forge_remote{
                    .name = std::move(name), .repo = {.host = host}, .account = {.host = host}};
            };
            const auto led =
                leading_remotes(dir, {on("never", "github.com"), on("origin", "github.com"),
                                      on("old", "codeberg.org"), on("upstream", "github.com"),
                                      on("lab", "gitlab.com"), on("fork", "gitlab.com")});
            std::string names;
            for (const forge_remote& r : led) names += r.name + " ";
            check(names == "lab upstream old ",
                  "avatars: leading remotes, newest first, a host once");
        }
        // The forges' times, to seconds since 1970.
        check(tortilla::parse_iso_time("1970-01-01T00:00:00Z") == 0 &&
                  tortilla::parse_iso_time("2026-10-21T10:44:02Z") == 1792579442 &&
                  tortilla::parse_iso_time("2016-08-11T11:28:34.085Z") == 1470914914 &&
                  tortilla::parse_iso_time("2026-11-02T03:19:43+01:00") == 1793585983 &&
                  tortilla::parse_iso_time("2024-02-29T23:59:59-05:30") == 1709270999,
              "forge: ISO 8601 times, with fractions and offsets");
        check(!tortilla::parse_iso_time("") && !tortilla::parse_iso_time("null") &&
                  !tortilla::parse_iso_time("2026-10-21") &&
                  !tortilla::parse_iso_time("2026-13-01T00:00:00Z") &&
                  !tortilla::parse_iso_time("2026-10-21T10:44:02") &&
                  !tortilla::parse_iso_time("2026-10-21T10:44:02Zx"),
              "forge: not a time, 0");
        // What a PR's mark means, as its tooltip says.
        {
            using namespace tortilla;
            const std::int64_t t0 = 1'800'000'000;
            auto run = [&](std::string name, check_state s, std::int64_t start, std::int64_t end)
            {
                return check_run{.name = std::move(name),
                                 .state = s,
                                 .started_at = start ? t0 + start : 0,
                                 .completed_at = end ? t0 + end : 0};
            };
            pull_request passed{.number = 870, .base_branch = "master"};
            passed.checks = check_state::success;
            passed.runs = {run("build", check_state::success, 10, 300),
                           run("lint", check_state::success, 12, 382),
                           run("skipped", check_state::success, 50, 40)};
            check(pr_tooltip(passed, t0 + 999) ==
                      "#870 · Checks passed in 6 min 12 s · Can be merged",
                  "pr tooltip: passed, how long, can be merged");
            pull_request review = passed;
            review.number = 869;
            review.runs = {run("build", check_state::success, 0 + 1, 301)};
            review.blocked = true;
            review.blocks = {merge_block::review_required};
            check(pr_tooltip(review, t0 + 999) ==
                      "#869 · Checks passed in 5 min · Needs an approving review",
                  "pr tooltip: passed, but needs a review");
            pull_request failed{.number = 866, .base_branch = "master"};
            failed.checks = check_state::failure;
            for (int i = 0; i < 7; ++i)
                failed.runs.push_back(run("ok " + std::to_string(i), check_state::success, 1, 2));
            failed.runs.push_back(run("build (windows)", check_state::failure, 1, 2));
            failed.runs.push_back(run("test (fedora arm64)", check_state::failure, 1, 2));
            check(pr_tooltip(failed, t0) ==
                      "#866 · 2 of 9 checks failed: build (windows), test (fedora arm64)",
                  "pr tooltip: which checks failed");
            pull_request running{.number = 867};
            running.checks = check_state::pending;
            running.runs = {
                run("a", check_state::pending, 60, 0), run("b", check_state::pending, 100, 0),
                run("c", check_state::pending, 0, 0), run("d", check_state::success, 30, 50)};
            check(pr_tooltip(running, t0 + 60 + 4 * 60 + 25) ==
                      "#867 · 3 checks running, for 4 min: a, b, c",
                  "pr tooltip: which are running, and for how long");
            pull_request none{.number = 805, .base_branch = "master"};
            none.conflicts = true;
            none.blocks = {merge_block::conflicts, merge_block::draft};
            check(pr_tooltip(none, t0) == "#805 · No checks · Conflicts with master "
                                          "· Draft",
                  "pr tooltip: no checks, conflicts, a draft");
            pull_request pipe{.number = 12, .pipeline = true};
            pipe.checks = check_state::failure;
            pipe.runs = {run("build", check_state::success, 1, 100),
                         run("test", check_state::failure, 100, 200),
                         run("deploy", check_state::none, 0, 0)};
            check(pr_tooltip(pipe, t0) == "#12 · Pipeline failed: 1 of 2 jobs: test",
                  "pr tooltip: a GitLab pipeline's failed jobs, skipped ones not counted");
            pipe.checks = check_state::success;
            pipe.runs = {run("Pipeline", check_state::success, 1, 2 * 3600 + 3 * 60 + 1)};
            check(pr_tooltip(pipe, t0) == "#12 · Pipeline passed in 2 h 3 min · Can be "
                                          "merged",
                  "pr tooltip: a pipeline's hours");
            pull_request many{.number = 1};
            many.checks = check_state::failure;
            for (int i = 0; i < 6; ++i)
                many.runs.push_back(run(std::string(1, char('a' + i)), check_state::failure, 0, 0));
            check(pr_tooltip(many, t0) == "#1 · 6 of 6 checks failed: a, b, c, d and 2 more",
                  "pr tooltip: a long list cut short");
            check(checks_text(passed, t0 + 999) == "Checks passed in 6 min 12 s" &&
                      merge_text(review) == "Needs an approving review" &&
                      merge_text(failed).empty(),
                  "pr tooltip: its parts, for the forge panel");
            // Asked as no one: what GitHub's list doesn't say isn't claimed.
            pull_request unknown{.number = 3, .merge_known = false, .checks_known = false};
            check(pr_tooltip(unknown, t0) == "#3", "pr tooltip: unknown checks and merge, unsaid");
            unknown.checks_known = true;
            unknown.checks = check_state::success;
            check(pr_tooltip(unknown, t0) == "#3 · Checks passed",
                  "pr tooltip: checks known, merge not: no \"Can be merged\"");
        }
        // GitHub's REST answers, as no one (cli/cli's, trimmed): its open
        // PRs, then the checks from its latest workflow runs.
        {
            using namespace tortilla;
            const std::string pulls = R"([
 {"url": "https://api.github.com/repos/cli/cli/pulls/14845", "id": 5038405753,
  "html_url": "https://github.com/cli/cli/pull/14845", "number": 14845, "state": "open",
  "title": "Skip token refresh when gh's state directory is not writable",
  "user": {"login": "babakks", "type": "User"}, "draft": false,
  "head": {"label": "cli:babakks/skip-refresh-ro-lock", "ref": "babakks/skip-refresh-ro-lock",
           "sha": "04c4ebf5a1bc61e4a2b97a8a1f4c22e5d0c31f6e",
           "repo": {"name": "cli", "full_name": "cli/cli", "owner": {"login": "cli"}}},
  "base": {"ref": "trunk", "sha": "93d24ce9ffefb6cc86bde89b50271457bb0ef9cb"}},
 {"url": "https://api.github.com/repos/cli/cli/pulls/14783", "id": 4987654321,
  "html_url": "https://github.com/cli/cli/pull/14783", "number": 14783, "state": "open",
  "title": "Verify release downloads", "user": {"login": "mrecachinas", "type": "User"},
  "draft": true,
  "head": {"label": "mrecachinas:release-download-verify", "ref": "release-download-verify",
           "sha": "654d851d00000000000000000000000000000000",
           "repo": {"name": "cli", "full_name": "mrecachinas/cli",
                    "owner": {"login": "mrecachinas"}}},
  "base": {"ref": "trunk", "sha": "93d24ce9ffefb6cc86bde89b50271457bb0ef9cb"}},
 {"number": 9, "html_url": "https://github.com/cli/cli/pull/9", "title": "Gone fork",
  "user": {"login": "x"}, "draft": false,
  "head": {"ref": "old", "sha": "1111111111111111111111111111111111111111", "repo": null},
  "base": {"ref": "trunk"}}
])";
            const forge_repo cli{"github.com", "cli", "cli"};
            auto prs = parse_github_rest_pulls(pulls, cli);
            check(prs.size() == 3 && prs[0].number == 14845 &&
                      prs[0].url == "https://github.com/cli/cli/pull/14845" && prs[0].head == cli &&
                      prs[0].head_branch == "babakks/skip-refresh-ro-lock" &&
                      prs[0].base_branch == "trunk" && prs[0].author == "babakks" &&
                      prs[0].head_sha.starts_with("04c4ebf5") && !prs[0].draft &&
                      !prs[0].merge_known && !prs[0].checks_known && prs[0].blocks.empty(),
                  "github rest: a PR from the repository itself");
            check(prs.size() == 3 && prs[1].draft &&
                      prs[1].head == forge_repo{"github.com", "mrecachinas", "cli"} &&
                      prs[1].blocks == std::vector{merge_block::draft} &&
                      prs[2].head.owner.empty() && prs[2].head.name.empty(),
                  "github rest: a fork's draft, and a fork that's gone");
            const std::string runs = R"({"total_count": 5, "workflow_runs": [
 {"id": 5, "name": "Unit and Integration Tests", "workflow_id": 25016, "event": "pull_request",
  "head_sha": "04c4ebf5a1bc61e4a2b97a8a1f4c22e5d0c31f6e", "status": "completed",
  "conclusion": "success", "run_started_at": "2026-10-27T17:45:56Z",
  "updated_at": "2026-10-27T17:53:45Z"},
 {"id": 4, "name": "Lint", "workflow_id": 925645, "event": "pull_request",
  "head_sha": "04c4ebf5a1bc61e4a2b97a8a1f4c22e5d0c31f6e", "status": "completed",
  "conclusion": "success", "run_started_at": "2026-10-27T17:45:56Z",
  "updated_at": "2026-10-27T17:48:21Z"},
 {"id": 3, "name": "Lint", "workflow_id": 925645, "event": "pull_request",
  "head_sha": "04c4ebf5a1bc61e4a2b97a8a1f4c22e5d0c31f6e", "status": "completed",
  "conclusion": "failure", "run_started_at": "2026-10-27T16:00:00Z",
  "updated_at": "2026-10-27T16:01:00Z"},
 {"id": 2, "name": "Unit and Integration Tests", "workflow_id": 25016, "event": "pull_request",
  "head_sha": "654d851d00000000000000000000000000000000", "status": "in_progress",
  "conclusion": null, "run_started_at": "2026-10-28T02:11:58Z",
  "updated_at": "2026-10-28T02:12:00Z"},
 {"id": 1, "name": "Lint", "workflow_id": 925645, "event": "pull_request",
  "head_sha": "ffffffffffffffffffffffffffffffffffffffff", "status": "completed",
  "conclusion": "failure", "run_started_at": null, "updated_at": "2026-10-27T10:00:00Z"}
]})";
            apply_github_workflow_runs(runs, prs);
            check(prs[0].checks_known && prs[0].checks == check_state::success &&
                      prs[0].runs.size() == 2 && prs[0].runs[1].name == "Lint" &&
                      prs[0].runs[1].completed_at - prs[0].runs[1].started_at == 145,
                  "github rest: each workflow's latest run on a head, an older one passed over");
            check(prs[1].checks_known && prs[1].checks == check_state::pending &&
                      prs[1].runs.size() == 1 && prs[1].runs[0].completed_at == 0 &&
                      !prs[2].checks_known && prs[2].runs.empty(),
                  "github rest: a running one; a head without runs not known");
            const std::string check_runs = R"({"total_count": 2, "check_runs": [
 {"name": "build", "status": "completed", "conclusion": "failure",
  "started_at": "2026-10-27T10:00:00Z", "completed_at": "2026-10-27T10:05:00Z"},
 {"name": "docs", "status": "completed", "conclusion": "skipped",
  "started_at": "2026-10-27T10:00:00Z", "completed_at": "2026-10-27T10:00:00Z"}]})";
            check(apply_github_check_runs(check_runs, prs[2]) && prs[2].checks_known &&
                      prs[2].checks == check_state::failure && prs[2].runs.size() == 2 &&
                      prs[2].runs[1].state == check_state::success && !prs[2].runs[1].started_at,
                  "github rest: a head's own check runs");
            check(!apply_github_check_runs(R"({"message": "Not Found"})", prs[2]) &&
                      prs[2].runs.size() == 2,
                  "github rest: not check runs, left as it was");
        }
        // GitHub's anonymous allowance: what's asked, and when the next poll is.
        {
            using namespace tortilla;
            const std::int64_t now = 1'800'000'000;
            const rate_budget unheard, plenty{.remaining = 50, .reset_at = now + 1800},
                low{.remaining = 3, .reset_at = now + 1800},
                spent{.remaining = 0, .reset_at = now + 1800},
                reset{.remaining = 0, .reset_at = now - 1};
            check(budget_allows(unheard, 2, 2, now) && budget_allows(plenty, 1, 30, now) &&
                      !budget_allows(low, 2, 2, now) && budget_allows(low, 1, 2, now) &&
                      !budget_allows(spent, 1, 0, now) && budget_allows(reset, 2, 2, now),
                  "budget: what's left, less a reserve; full again once reset");
            check(anonymous_due(0, now, false, unheard) &&
                      !anonymous_due(now - 300, now, false, plenty) &&
                      anonymous_due(now - 600, now, false, plenty) &&
                      anonymous_due(now - 61, now, true, plenty) &&
                      !anonymous_due(now - 30, now, true, plenty) &&
                      !anonymous_due(0, now, true, spent),
                  "budget: anonymous GitHub every ten minutes, a minute after a refresh, "
                  "never while spent");
            check(poll_seconds(false, false, false, plenty, now) == 120 &&
                      poll_seconds(false, true, false, plenty, now) == 20 &&
                      poll_seconds(true, false, false, plenty, now) == 300 &&
                      poll_seconds(false, true, true, plenty, now) == 600 &&
                      poll_seconds(false, false, true, spent, now) == 1805 &&
                      poll_seconds(false, false, false, spent, now) == 120 &&
                      poll_seconds(false, false, true, reset, now) == 600,
                  "budget: the poll's pace, and waiting for the allowance to reset");
        }
        // The PR menu's Merge... and Close..., greyed with why for each tier.
        {
            using namespace tortilla;
            const forge_repo repo{"github.com", "org", "engine"};
            const forge_remote anonymous{
                .name = "origin",
                .repo = repo,
                .account = {.kind = forge_kind::github, .host = "github.com"}},
                signed_in{
                    .name = "origin",
                    .repo = repo,
                    .account = {.kind = forge_kind::github, .host = "github.com", .login = "Ada"}},
                codeberg{.name = "cb",
                         .repo = {"codeberg.org", "org", "engine"},
                         .account = {.kind = forge_kind::gitea, .host = "codeberg.org"}};
            const open_pr theirs{.pr = {.number = 1, .author = "bob"}, .into = anonymous},
                as_reader{.pr = {.number = 1, .author = "bob"}, .into = signed_in},
                own{.pr = {.number = 1, .author = "ada"}, .into = signed_in};
            auto a = pr_menu_for(theirs, repo_access::read);
            check(!a.can_merge && !a.can_close && a.merge == "_Merge... (sign in to GitHub)" &&
                      a.close == "_Close... (sign in to GitHub)",
                  "pr menu: not signed in, sign in to GitHub");
            auto r = pr_menu_for(as_reader, repo_access::read);
            check(!r.can_merge && !r.can_close && r.merge == "_Merge... (needs write access)" &&
                      r.close == "_Close... (needs write access)",
                  "pr menu: read access, needs write access");
            auto o = pr_menu_for(own, repo_access::read);
            check(!o.can_merge && o.can_close && o.close == "_Close...",
                  "pr menu: its author may close their own");
            auto t = pr_menu_for(as_reader, repo_access::triage);
            check(!t.can_merge && t.can_close, "pr menu: a triager may close, not merge");
            auto w = pr_menu_for(as_reader, repo_access::write);
            check(w.can_merge && w.can_close && w.merge == "_Merge..." && w.close == "_Close...",
                  "pr menu: write access, everything");
            check(pr_menu_for(as_reader, repo_access::unknown).can_merge,
                  "pr menu: access not heard yet, the dialog says");
            open_pr conflicted = as_reader;
            conflicted.pr.conflicts = true;
            auto c = pr_menu_for(conflicted, repo_access::write);
            check(!c.can_merge && c.can_close && c.merge == "_Merge... (conflicts)",
                  "pr menu: conflicts, as before");
            check(create_pr_blocker({anonymous}) == "sign in to GitHub" &&
                      create_pr_blocker({codeberg}) == "sign in to Codeberg" &&
                      create_pr_blocker({anonymous, signed_in}).empty() &&
                      create_pr_blocker({}).empty(),
                  "pr menu: Create pull request... until signed in");
            check(hint_text(anonymous) == "github.com/org/engine: sign in to see its pull requests",
                  "pr watch: a hidden repository's hint");
            check(known_forge("GitHub.com") == forge_kind::github &&
                      known_forge("codeberg.org") == forge_kind::gitea &&
                      known_forge("gitlab.com") == forge_kind::gitlab &&
                      !known_forge("git.example.org"),
                  "forge: the public forges asked as no one");
            const auto anon_headers = api_headers(forge_kind::gitea, "");
            check(std::ranges::none_of(anon_headers,
                                       [](const auto& h) { return h.first == "Authorization"; }) &&
                      std::ranges::any_of(api_headers(forge_kind::gitea, "t"),
                                          [](const auto& h) { return h.second == "token t"; }),
                  "forge: no Authorization as no one");
        }
        // The forge panel's build: failures first, a matrix's names, a log's tail.
        {
            using namespace tortilla;
            commit_ci ci;
            ci.workflows = {
                {.name = "Lint", .jobs = {{.name = "lint", .state = check_state::success}}},
                {.name = "Tests",
                 .jobs = {{.name = "build (ubuntu)", .state = check_state::success},
                          {.name = "build (windows)", .state = check_state::failure},
                          {.name = "build (macos)", .state = check_state::pending},
                          {.name = "docs", .state = check_state::failure}}},
                {.name = "Deploy", .jobs = {{.name = "deploy", .state = check_state::pending}}}};
            order_failures_first(ci);
            std::string order;
            for (const ci_workflow& w : ci.workflows)
            {
                order += w.name + ":";
                for (const ci_job& j : w.jobs) order += " " + j.name;
                order += "; ";
            }
            check(order == "Tests: build (windows) docs build (macos) build (ubuntu); Deploy: "
                           "deploy; Lint: lint; ",
                  "ci: failures first, then what runs, as listed otherwise");
            check(ci.state() == check_state::failure && !ci.empty() && commit_ci{}.empty() &&
                      commit_ci{}.state() == check_state::none,
                  "ci: the commit's state, combined");
            check(matrix_parts("build (fedora arm64)") ==
                          std::pair<std::string_view, std::string_view>{"build", "fedora arm64"} &&
                      matrix_parts("lint").second.empty() &&
                      matrix_parts("odd (x) y").first == "odd (x) y",
                  "ci: a matrix job's name and its variant");
            const std::string log = "2026-11-01T01:04:45.0326415Z ##[group]Run make\n"
                                    "2026-11-01T01:04:45.0326415Z make all\n"
                                    "2026-11-01T01:04:45.0326415Z ##[endgroup]\n"
                                    "2026-11-01T01:06:06.1477295Z builtin.c: undefined `drem'\n"
                                    "2026-11-01T01:06:06.1691857Z ##[error]Process completed with "
                                    "exit code 2.\n"
                                    "2026-11-01T01:06:06.1787025Z ##[group]Run upload\n"
                                    "2026-11-01T01:06:06.1787025Z uploading\n";
            check(log_tail(log, 2) == "builtin.c: undefined `drem'\nProcess completed with exit "
                                      "code 2.",
                  "ci: a GitHub log's tail ends at the error, without timestamps");
            check(log_tail("\x1b[0Ksection_start:1:step_script\r\x1b[0K$ make\n"
                           "\x1b[31;1mERROR: Job failed\x1b[0;m\n\n",
                           5) == "$ make\nERROR: Job failed",
                  "ci: a GitLab trace's tail, without colours or sections");
        }
        // Quick asks after running checks: how often, and which.
        {
            using namespace tortilla;
            using std::chrono::seconds;
            check(fast_interval(0, 600, true) == seconds(10) &&
                      fast_interval(240, 600, true) == seconds(9) &&
                      fast_interval(480, 600, true) == seconds(5) &&
                      fast_interval(9000, 600, true) == seconds(5),
                  "fast polls: the watched commit, gently, then faster near its expected end");
            check(fast_interval(0, 600, false) == seconds(30) &&
                      fast_interval(480, 600, false) == seconds(15) &&
                      fast_interval(100, 0, false) == seconds(20) &&
                      fast_interval(100, 0, true) == seconds(8),
                  "fast polls: the others more slowly; unknown times in between");
            const std::int64_t now = 10'000;
            std::vector<pending_head> pending;
            for (int i = 0; i < 25; ++i)
                pending.push_back(
                    {.sha = "pr" + std::to_string(i), .started_at = now - 60, .expected = 600});
            const pending_head shown{.sha = "shown", .started_at = now - 500, .expected = 600};
            auto plan = plan_fast_polls(pending, shown, now, true, 0);
            check(plan.size() == k_fast_poll_cap && plan[0].sha == "shown" && plan[0].watched &&
                      plan[0].interval == seconds(5) && plan[1].sha == "pr0" &&
                      plan[1].interval == seconds(30) && plan.back().sha == "pr18",
                  "fast polls: the watched first, then the newest PRs, twenty in all");
            plan = plan_fast_polls(pending, pending_head{.sha = "pr3"}, now, true, 0);
            check(plan.size() == k_fast_poll_cap && plan[0].sha == "pr3" &&
                      std::ranges::count(plan, std::string("pr3"), &fast_poll::sha) == 1,
                  "fast polls: a watched PR head once");
            check(plan_fast_polls(pending, std::nullopt, now, true, 0).size() == k_fast_poll_cap &&
                      plan_fast_polls({}, std::nullopt, now, true, 0).empty(),
                  "fast polls: none without running checks");
            check(plan_fast_polls(pending, shown, now, false, 31).size() == 1 &&
                      plan_fast_polls(pending, shown, now, false, 30).empty() &&
                      plan_fast_polls(pending, std::nullopt, now, false, 60).empty(),
                  "fast polls: without a token, only the watched one, while the budget lasts");
        }
        // Reviews summed up, each reviewer once by their latest say.
        {
            using namespace tortilla;
            using v = pr_review::verdict;
            const review_summary s = summarise_reviews({{"ada", v::approved},
                                                        {"bob", v::commented},
                                                        {"cy", v::changes_requested},
                                                        {"ada", v::commented},
                                                        {"dee", v::requested},
                                                        {"bob", v::approved},
                                                        {"cy", v::requested},
                                                        {"", v::approved}});
            using names = std::vector<std::string>;
            check(s.approved == names{"ada", "bob"} && s.changes_requested.empty() &&
                      s.commented.empty() && s.waiting == names{"cy", "dee"},
                  "reviews: latest say each; a comment keeps an approval, asking again waits");
        }
        // Which commits are a pull request's: its head's, not its base's.
        {
            const std::string dir = std::string(argv[1]) + "/pr-members";
            const std::string id = " -c user.name=Ada -c user.email=ada@example.com";
            tortilla::test::sh(
                "git init -q -b main '" + dir + "' && cd '" + dir + "' && git" + id +
                " commit -q --allow-empty -m base1 && git" + id +
                " commit -q --allow-empty -m base2 && git update-ref "
                "refs/remotes/origin/main HEAD && git checkout -q -b feature && git" +
                id + " commit -q --allow-empty -m one && git" + id +
                " commit -q --allow-empty -m two && git update-ref "
                "refs/remotes/fork/feature HEAD && git" +
                id + " commit -q --allow-empty -m local");
            auto rev = [&](const std::string& r)
            {
                std::string out =
                    tortilla::run_git(dir, {"rev-parse", r}, tortilla::git_output::stdout_only)
                        .output;
                while (!out.empty() && out.back() == '\n') out.pop_back();
                return out;
            };
            const auto commits = tortilla::pull_request_commits(dir, "refs/remotes/fork/feature",
                                                                "refs/remotes/origin/main");
            check(commits.size() == 2 && commits[0] == rev("HEAD~1") && commits[1] == rev("HEAD~2"),
                  "pr commits: its head first, then the middle");
            check(std::ranges::find(commits, rev("main")) == commits.end() &&
                      std::ranges::find(commits, rev("HEAD")) == commits.end(),
                  "pr commits: not the base's, nor a commit past the pushed head");
            check(tortilla::pull_request_commits(dir, "refs/remotes/fork/gone",
                                                 "refs/remotes/origin/main")
                      .empty(),
                  "pr commits: none for a missing ref");
            const std::vector<tortilla::forge_remote> remotes{{.name = "origin"}, {.name = "fork"}};
            const auto* base = tortilla::remote_with_commit(dir, rev("main~1"), remotes);
            const auto* pushed = tortilla::remote_with_commit(dir, rev("HEAD~1"), remotes);
            check(base && base->name == "origin" && pushed && pushed->name == "fork" &&
                      !tortilla::remote_with_commit(dir, rev("HEAD"), remotes),
                  "pr commits: where a commit was pushed, the first remote that has it");
        }
#ifndef _WIN32
        // The GitHub CLI's token, from a stand-in gh first on PATH.
        const std::string bin = std::string(argv[1]) + "/fake-gh";
        std::filesystem::create_directories(bin);
        tortilla::test::sh("printf '#!/bin/sh\\n[ \"$4\" = github.com ] && "
                           "echo tok-123 && exit 0\\necho no >&2; exit 1\\n' > '" +
                           bin + "/gh' && chmod +x '" + bin + "/gh'");
        const std::string path = std::getenv("PATH") ? std::getenv("PATH") : "";
        setenv("PATH", (bin + ":" + path).c_str(), 1);
        const auto token = tortilla::gh_token("github.com");
        const auto other = tortilla::gh_token("ghe.corp");
        check(tortilla::gh_installed() && token && *token == "tok-123",
              "forge: gh's token for its host, trimmed");
        check(!other && other.error().find("gh auth login") != std::string::npos,
              "forge: gh not signed in to a host says so");
        const auto via =
            tortilla::load_token({.host = "github.com", .login = "ada", .via_gh = true});
        check(via && via->access == "tok-123", "forge: an account through gh asks gh");
        // Who gh is signed in as, from its hosts.yml: the active user on each.
        tortilla::test::sh("printf 'github.com:\\n    git_protocol: ssh\\n    users:\\n        "
                           "ada:\\n        bob:\\n    user: bob\\nghe.corp:\\n    "
                           "oauth_token: x\\n    user: \"cy\"\\n' > '" +
                           bin + "/hosts.yml'");
        setenv("GH_CONFIG_DIR", bin.c_str(), 1);
        const auto gh = tortilla::gh_accounts();
        check(gh.size() == 2 && gh[0].host == "github.com" && gh[0].login == "bob" &&
                  gh[0].via_gh && gh[1].host == "ghe.corp" && gh[1].login == "cy",
              "forge: gh's sign-ins, from its hosts.yml");
        unsetenv("GH_CONFIG_DIR");
        setenv("PATH", path.c_str(), 1);
#endif
    }

    // The log's file list: unrelated paths (outside the log's path) and the
    // path filter box [spec 1.9.5, 1.11.2].
    {
        using tortilla::path_is_related;
        using tortilla::path_matches_filter;
        check(path_is_related("src/a.cpp", "src") && path_is_related("src", "src") &&
                  path_is_related("src/a.cpp", "src/") && path_is_related("x/y", ""),
              "related: the path itself and inside it");
        check(!path_is_related("srcs/a.cpp", "src") && !path_is_related("sr", "src") &&
                  !path_is_related("doc/src/a", "src"),
              "unrelated: a longer name or elsewhere");
        check(path_matches_filter("src/Log/Window.cpp", "log/w") && path_matches_filter("a", "") &&
                  path_matches_filter("README", "readme"),
              "path filter: a substring, any case");
        check(!path_matches_filter("src/a.cpp", "b.cpp") && !path_matches_filter("a", "ab"),
              "path filter: no match");

        tortilla::file_model files;
        // A status with no colour of its own, so any colour is the grey.
        auto change = [](std::string path, std::string old_path = {})
        {
            return tortilla::file_change{
                .path = path, .old_path = old_path, .status = 'S', .counted = false};
        };
        files.set({change("src/a.cpp"), change("doc/b.md"), change("src/c.h", "old/c.h"),
                   change("old/d.txt", "src/d.txt"), change("README")});
        check(files.row_count() == 5, "file view: everything until one is set");
        files.set_view({.related = "src", .unrelated = tortilla::file_view::gray_unrelated});
        auto grey = [&](size_t row) { return files.style(row, 0).fg.has_value(); };
        check(files.row_count() == 5 && !grey(0) && grey(1) && !grey(2) && !grey(3) && grey(4),
              "file view: unrelated rows greyed, renames by either path");
        files.set_view({.related = "src", .unrelated = tortilla::file_view::hide_unrelated});
        check(files.row_count() == 3 && files.files()[0].path == "src/a.cpp" &&
                  files.files()[2].path == "old/d.txt" && !grey(1),
              "file view: unrelated rows hidden");
        // Counts for every file set land on the rows still shown.
        auto counted = [&](std::string path, size_t added)
        {
            auto f = change(path);
            f.counted = true;
            f.added = added;
            return f;
        };
        files.set_counts(1, {counted("doc/b.md", 7), counted("src/c.h", 3)});
        check(files.files()[1].path == "src/c.h" && files.files()[1].added == 3 &&
                  files.text(1, tortilla::file_model::added) == "3",
              "file view: counts reach the shown rows");
        files.set_view(
            {.related = "src", .unrelated = tortilla::file_view::hide_unrelated, .filter = "C.H"});
        check(files.row_count() == 1 && files.files()[0].path == "src/c.h",
              "file view: the path filter with unrelated hidden");
        files.set_view(
            {.related = "src", .unrelated = tortilla::file_view::show_unrelated, .filter = ".md"});
        check(files.row_count() == 1 && files.files()[0].added == 7 && !grey(0),
              "file view: counts kept for hidden rows");
        files.set_view({.unrelated = tortilla::file_view::gray_unrelated});
        check(files.row_count() == 5 && !grey(1) && !grey(4),
              "file view: nothing unrelated without a path");
    }

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
        // The subjects of the patches git writes, in order.
        auto patches = [&](const tortilla::patch_range& range)
        {
            std::vector<std::string> subjects;
            auto r = tortilla::run_git(repo, tortilla::format_patch_args(range),
                                       tortilla::git_output::stdout_only);
            if (!r.ok()) return subjects;
            for (size_t at = r.output.find("\nSubject: ["); at != std::string::npos;
                 at = r.output.find("\nSubject: [", at + 1))
            {
                const size_t from = r.output.find("] ", at) + 2;
                subjects.push_back(r.output.substr(from, r.output.find('\n', from) - from));
            }
            return subjects;
        };
        check(tortilla::format_patch_args({.oldest = c, .newest = c}) ==
                  std::vector<std::string>{"format-patch", "--stdout", "-1", c},
              "format_patch_args: one commit takes -1");
        check(tortilla::format_patch_args({.oldest = b, .newest = merge}).back() ==
                  b + "^.." + merge,
              "format_patch_args: a range starts after the oldest's parent");
        {
            const std::string out = std::string(argv[1]) + "/patches";
            std::vector<std::string> files;
            if (tortilla::run_git(repo,
                                  tortilla::format_patch_args({.oldest = b, .newest = c}, out))
                    .ok())
                for (auto& e : std::filesystem::directory_iterator(out))
                    files.push_back(e.path().filename().string());
            std::ranges::sort(files);
            check(files == std::vector<std::string>{"0001-B-subject.patch", "0002-C-subject.patch"},
                  "format-patch: a series into a folder, a numbered file per commit");
        }
        check(patches({.oldest = c, .newest = c}) == std::vector<std::string>{"C subject"},
              "format-patch: one commit, one patch");
        check(patches({.oldest = a, .newest = c, .oldest_is_root = true}).size() == 3,
              "format-patch: a range from the root commit takes it in (A, B, C)");
        // F1 and C come in either order: they're siblings.
        const auto range = patches({.oldest = b, .newest = merge});
        check(range.size() == 3 && range[0] == "B subject",
              "format-patch: B to the merge is B, F1 and C, merges left out");
    }

    // Commits dragged out of a log: full hashes only come back.
    {
        const std::string a(40, 'a'), b = std::string(39, 'b') + "c";
        check(tortilla::parse_commits_payload(tortilla::commits_payload({a, b})) ==
                  std::vector<std::string>{a, b},
              "commits payload: round trip, in order");
        check(tortilla::parse_commits_payload(a + "\r\nnot a hash\n" + b.substr(0, 8) + "\n") ==
                  std::vector<std::string>{a},
              "commits payload: CRLF taken, anything else dropped");
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
    }

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

    // Export: git archive's arguments, and its tar unpacked into a folder,
    // with what plain ustar can't hold (a long path, through pax).
    {
        namespace fs = std::filesystem;
        check(tortilla::archive_args(tortilla::archive_format::zip, "v1", "/out/x.zip") ==
                  std::vector<std::string>{"archive", "--format=zip", "-o", "/out/x.zip", "v1"},
              "archive_args: zip");
        check(tortilla::export_name("/src/tortilla/", "HEAD") == "tortilla",
              "export_name: HEAD is the folder's name");
        check(tortilla::export_name("/srv/x.git/", std::string(40, 'a')) == "x-aaaaaaaa",
              "export_name: a bare repository's .git dropped, a hash shortened");
        check(tortilla::export_name("/src/t", "refs/heads/feat/one") == "t-feat-one",
              "export_name: a branch's folders made dashes");
        check(!tortilla::archive_entry_path("../x") && !tortilla::archive_entry_path("a/../../x") &&
                  !tortilla::archive_entry_path("/etc/x") && !tortilla::archive_entry_path(""),
              "archive_entry_path: nothing outside the folder");
        check(tortilla::archive_entry_path("a/b/") == fs::path("a") / "b",
              "archive_entry_path: a folder's entry");

        const std::string src = std::string(argv[1]) + "/export-src";
        const std::string long_dir = std::string(60, 'd') + "/" + std::string(60, 'e');
        fs::create_directories(src + "/" + long_dir);
        const std::string g =
            "git -C '" + src + "' -c user.name=Ada -c user.email=ada@example.com ";
        sh("git init -q -b main '" + src + "'");
        // Git for Windows sets core.autocrlf, and archive writes files as a
        // checkout would: CRLF there. The test wants the bytes as committed.
        sh(g + "config core.autocrlf false");
        sh("printf 'one\\n' > '" + src + "/top.txt' && printf 'deep\\n' > '" + src + "/" +
           long_dir + "/f.txt' && printf '#!/bin/sh\\n' > '" + src + "/run.sh' && chmod +x '" +
           src + "/run.sh'");
#ifndef _WIN32
        sh("ln -s top.txt '" + src + "/link'");
#endif
        sh(g + "add . && " + g + "update-index --chmod=+x run.sh && " + g + "commit -qm export");
        const std::string tar = std::string(argv[1]) + "/export.tar";
        check(tortilla::run_git(src,
                                tortilla::archive_args(tortilla::archive_format::tar, "HEAD", tar))
                  .ok(),
              "git archive writes the tar");
        const fs::path into = fs::path(argv[1]) / "export-out";
        const tortilla::extract_result r = tortilla::extract_tar(tar, into);
        check(r.ok(), "extract_tar: no errors");
        auto read = [](const fs::path& p)
        {
            std::ifstream in(p, std::ios::binary);
            return std::string((std::istreambuf_iterator<char>(in)), {});
        };
        check(read(into / "top.txt") == "one\n", "extract_tar: a file");
        check(read(into / long_dir / "f.txt") == "deep\n",
              "extract_tar: a path past ustar's 100 bytes");
#ifndef _WIN32
        check((fs::status(into / "run.sh").permissions() & fs::perms::owner_exec) !=
                  fs::perms::none,
              "extract_tar: executable kept");
        check(fs::is_symlink(into / "link") && fs::read_symlink(into / "link") == "top.txt",
              "extract_tar: a symbolic link");
#endif
        check(!tortilla::extract_tar(src + "/top.txt", into).ok(),
              "extract_tar: not a tar is an error");
        const std::string zip = std::string(argv[1]) + "/export.zip";
        check(tortilla::run_git(src,
                                tortilla::archive_args(tortilla::archive_format::zip, "HEAD", zip))
                      .ok() &&
                  read(zip).starts_with("PK"),
              "git archive writes the zip");
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

    // Worktrees: the main one first, then each linked one with its branch,
    // detached HEAD, lock and whether it's prunable, from any of them.
    {
        const std::string root = std::string(argv[1]);
        sh(git + "worktree add -q -b wt-topic '" + root + "/wt-topic' feat");
        sh(git + "worktree add -q --detach '" + root + "/wt-detached' v1");
        sh(git + "worktree lock --reason 'on a stick' '" + root + "/wt-detached'");
        sh(git + "worktree add -q --detach '" + root + "/wt-gone' main && rm -rf '" + root +
           "/wt-gone'");
        auto check_list = [&](const std::string& from, const char* where)
        {
            auto r = tortilla::repo::open(from);
            const auto list =
                r ? tortilla::list_worktrees(*r) : std::unexpected(std::string("no repo"));
            auto find = [&](const std::string& path) -> const tortilla::worktree_info*
            {
                if (!list) return nullptr;
                for (const auto& w : *list)
                    if (std::error_code ec; std::filesystem::equivalent(w.path, path, ec))
                        return &w;
                return nullptr;
            };
            const auto* main = list && !list->empty() ? &list->front() : nullptr;
            const auto* topic = find(root + "/wt-topic");
            const auto* detached = find(root + "/wt-detached");
            check(list && list->size() == 4 && main && main->main && !main->bare &&
                      std::filesystem::equivalent(main->path, repo) && main->branch == "main" &&
                      main->head == tortilla::resolve(repo, "HEAD"),
                  (std::string("worktrees: the main one first, ") + where).c_str());
            check(topic && !topic->main && topic->branch == "wt-topic" &&
                      topic->head == tortilla::resolve(repo, "feat") && !topic->locked &&
                      !topic->prunable,
                  (std::string("worktrees: a new branch's, ") + where).c_str());
            check(detached && detached->branch.empty() &&
                      detached->head == tortilla::resolve(repo, "v1") && detached->locked &&
                      detached->lock_reason.starts_with("on a stick") && !detached->prunable,
                  (std::string("worktrees: detached and locked, ") + where).c_str());
            check(list && std::ranges::count_if(*list, [](const auto& w)
                                                { return w.prunable && !w.main; }) == 1,
                  (std::string("worktrees: a missing folder is prunable, ") + where).c_str());
            check(list && std::ranges::any_of(*list,
                                              [&](const auto& w)
                                              {
                                                  return w.name == "wt-gone" && w.prunable &&
                                                         w.branch.empty() &&
                                                         w.head == tortilla::resolve(repo, "main");
                                              }),
                  (std::string("worktrees: a missing one's HEAD, from its admin dir, ") + where)
                      .c_str());
            return list ? std::ranges::count_if(*list, [](const auto& w) { return w.current; }) : 0;
        };
        check(check_list(repo, "from the main one") == 1, "worktrees: the main one is current");
        check(check_list(root + "/wt-topic", "from a linked one") == 1,
              "worktrees: a linked one is current");
        sh(git + "worktree prune && " + git + "worktree unlock '" + root + "/wt-detached' && " +
           git + "worktree remove '" + root + "/wt-detached' && " + git + "worktree remove '" +
           root + "/wt-topic' && " + git + "branch -qD wt-topic");
        auto r = tortilla::repo::open(repo);
        const auto left =
            r ? tortilla::list_worktrees(*r) : std::unexpected(std::string("no repo"));
        check(left && left->size() == 1 && left->front().main && left->front().current,
              "worktrees: pruned and removed, the main one is left");
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

    // Submodules: Add, Update and Sync's arguments, and the list Update
    // shows, before and after a clone's are initialized.
    {
        using tortilla::submodule_update_options;
        check(tortilla::submodule_add_args({.url = "u", .path = "p"}) ==
                  std::vector<std::string>{"submodule", "add", "--", "u", "p"},
              "submodule add: the URL and the path");
        check(
            tortilla::submodule_add_args({.url = "u", .path = "p", .branch = "b", .force = true}) ==
                std::vector<std::string>{"submodule", "add", "-b", "b", "-f", "--", "u", "p"},
            "submodule add: a branch, forced");
        check(tortilla::submodule_update_args({}, {"a", "b"}) ==
                  std::vector<std::string>{"submodule", "update", "--init", "--recursive", "--",
                                           "a", "b"},
              "submodule update: init and recursive by default, the paths chosen");
        check(tortilla::submodule_update_args({.init = false,
                                               .recursive = false,
                                               .remote = true,
                                               .no_fetch = true,
                                               .force = true,
                                               .mode = submodule_update_options::how::rebase},
                                              {}) ==
                  std::vector<std::string>{"submodule", "update", "--remote", "--no-fetch",
                                           "--force", "--rebase", "--"},
              "submodule update: every option");
        check(tortilla::submodule_sync_args(true) ==
                  std::vector<std::string>{"submodule", "sync", "--recursive"},
              "submodule sync: recursive");

        const std::string base = std::string(argv[1]) + "/submodules";
        const std::string lib = base + "/lib", super = base + "/super", copy = base + "/copy";
        std::filesystem::create_directories(base);
        auto in = [](const std::string& dir)
        { return "cd '" + dir + "' && git -c user.name=Ada -c user.email=ada@example.com "; };
        sh("git init -q -b main '" + lib + "'");
        sh("echo lib > '" + lib + "/lib.txt' && " + in(lib) + "add . && " + in(lib) +
           "commit -qm lib");
        sh("git init -q -b main '" + super + "'");
        sh("echo top > '" + super + "/top.txt' && " + in(super) + "add . && " + in(super) +
           "commit -qm top");
        check(!tortilla::has_submodules(super), "submodules: none yet");
        // A local URL: git refuses the file transport for submodules unless allowed.
        std::vector<std::string> add{"-c", "protocol.file.allow=always"};
        std::ranges::copy(tortilla::submodule_add_args({.url = lib, .path = "libs/lib"}),
                          std::back_inserter(add));
        check(tortilla::run_git(super, add).ok(), "submodule add: runs");
        sh(in(super) + "commit -qm 'add lib'");
        auto listed = [](const std::string& dir)
        {
            auto r = tortilla::repo::open(dir);
            return r ? tortilla::list_submodules(*r)
                     : std::expected<std::vector<tortilla::submodule_info>, std::string>{};
        };
        const auto added = listed(super);
        check(tortilla::has_submodules(super) && added && added->size() == 1 &&
                  (*added)[0].path == "libs/lib" && (*added)[0].url == lib &&
                  (*added)[0].initialized && (*added)[0].checked_out,
              "submodules: the one added, initialized and checked out");

        sh("git clone -q '" + super + "' '" + copy + "'");
        const auto cloned = listed(copy);
        check(cloned && cloned->size() == 1 && !(*cloned)[0].initialized &&
                  !(*cloned)[0].checked_out,
              "submodules: a clone's, not yet initialized");
        std::vector<std::string> update{"-c", "protocol.file.allow=always"};
        std::ranges::copy(tortilla::submodule_update_args({}, {"libs/lib"}),
                          std::back_inserter(update));
        check(tortilla::run_git(copy, update).ok() &&
                  std::filesystem::exists(copy + "/libs/lib/lib.txt"),
              "submodule update: initializes and checks out");
        const auto updated = listed(copy);
        check(updated && updated->size() == 1 && (*updated)[0].initialized &&
                  (*updated)[0].checked_out,
              "submodules: initialized and checked out after the update");
        check(tortilla::run_git(copy, tortilla::submodule_sync_args(true)).ok(),
              "submodule sync: runs");
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

    // Badges on file manager icons: from git's answers, then a real repository.
    {
        using tortilla::badge;
        using namespace std::string_literals;
        const auto b = tortilla::repo_badges::build("a.txt\0src/x.cpp\0src/y.cpp\0src/deep/z.h\0"s,
                                                    " M src/x.cpp\0R  src/new.cpp\0src/y.cpp\0"s);
        check(b.of("a.txt") == badge::clean && b.of("src/deep/z.h") == badge::clean,
              "badges: tracked and unchanged files are clean");
        check(b.of("src/x.cpp") == badge::modified && b.of("src/new.cpp") == badge::added,
              "badges: modified, and a rename's new name added");
        check(b.of("src/deep") == badge::clean && b.of("src") == badge::modified &&
                  b.of("src/") == badge::modified && b.of("") == badge::modified,
              "badges: a folder sums up what's inside it");
        check(b.of("nope.txt") == badge::none && b.of("srcs") == badge::none,
              "badges: untracked paths have none");
        const auto u = tortilla::repo_badges::build("a.txt\0src/x.cpp\0docs/d.md\0"s,
                                                    "?? src/new.cpp\0?? build/\0"s);
        check(u.untracked("src/new.cpp") && u.untracked("build") &&
                  u.untracked("build/out/o.obj") && u.untracked("src") && u.untracked(""),
              "badges: untracked files, a folder git collapsed, and what holds them");
        check(!u.untracked("docs") && !u.untracked("a.txt") && !u.untracked("src/x.cpp") &&
                  u.of("src") == badge::clean && u.of("src/new.cpp") == badge::none,
              "badges: untracked files aren't changes, nor badged");

        const std::string r = std::string(argv[1]) + "/badges";
        auto in = [&]
        { return "cd '" + r + "' && git -c user.name=Ada -c user.email=ada@example.com "; };
        sh("git init -q -b main '" + r + "' && mkdir -p '" + r + "/lib/sub' '" + r + "/docs'");
        sh("cd '" + r +
           "' && echo a > a.txt && echo m > lib/m.txt && echo c > lib/sub/c.txt && "
           "echo d > docs/d.txt && echo k > keep.txt && " +
           in() + "add . && " + in() + "commit -qm one");
        sh(in() + "checkout -qb other && echo theirs > '" + r + "/keep.txt' && " + in() +
           "commit -qam theirs && " + in() + "checkout -q main && echo ours > '" + r +
           "/keep.txt' && " + in() + "commit -qam ours");
        sh(in() + "merge -q other >/dev/null 2>&1 || true");
        sh("echo changed >> '" + r + "/lib/m.txt' && echo new > '" + r + "/lib/sub/n.txt' && " +
           in() + "add lib/sub/n.txt && echo stray > '" + r + "/stray.txt'");
        const auto live = tortilla::load_badges(r);
        check(live.has_value(), "badges: git answers in a real repository");
        if (live)
        {
            check(live->of("a.txt") == badge::clean && live->of("docs") == badge::clean &&
                      live->of("docs/d.txt") == badge::clean,
                  "badges: clean files and folders");
            check(live->of("lib/m.txt") == badge::modified &&
                      live->of("lib/sub/n.txt") == badge::added &&
                      live->of("keep.txt") == badge::conflicted,
                  "badges: modified, added and conflicted files");
            check(live->of("lib/sub") == badge::modified && live->of("lib") == badge::modified &&
                      live->of("") == badge::conflicted,
                  "badges: folders take the strongest inside, conflicted over modified");
            check(live->of("stray.txt") == badge::none && live->untracked("stray.txt") &&
                      live->untracked("") && !live->untracked("docs"),
                  "badges: an untracked file has none, but is known");
        }
    }

    // The badges' cache: the channel it answers on, and a repository it
    // loads in the background and watches for changes.
    {
        using tortilla::badge;
        const std::string name =
            "test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        std::thread(
            [name]
            {
                // "big" is answered with more than any pipe or socket holds
                // unread, so an asker that doesn't read leaves it stuck.
                (void)tortilla::channel::serve(
                    name, [](std::string_view q)
                    { return q == "big" ? std::string(8 << 20, 'x') : "got " + std::string(q); });
            })
            .detach();
        std::expected<std::string, tortilla::channel::ask_error> answer;
        for (int i = 0; i < 50; ++i)
        {
            answer = tortilla::channel::ask(name, "hello", std::chrono::milliseconds(500));
            if (answer || answer.error() != tortilla::channel::ask_error::no_server) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        check(answer && *answer == "got hello", "cache channel: a question answered");
        check(!tortilla::channel::ask(name + "-none", "x", std::chrono::milliseconds(200)) &&
                  tortilla::channel::ask(name + "-none", "x", std::chrono::milliseconds(200))
                          .error() == tortilla::channel::ask_error::no_server,
              "cache channel: no server is said so");

        // Askers that stall: one connected that sends nothing, one that asks
        // and never reads. Neither holds up the next question, and each is
        // let go after channel::k_serve_wait, on every platform.
        {
            using tortilla::test::raw_client;
            using namespace std::chrono_literals;
            const std::string at = tortilla::channel::address(name);
            {
                raw_client idle(at);
                std::this_thread::sleep_for(50ms);
                auto after = tortilla::channel::ask(name, "after idle", 200ms);
                check(idle.connected() && after && *after == "got after idle",
                      "cache channel: an idle asker doesn't hold up the next");
                check(idle.let_go(tortilla::channel::k_serve_wait * 3),
                      "cache channel: an idle asker is let go");
            }
            {
                raw_client deaf(at);
                const bool asked = deaf.send("big\n");
                std::this_thread::sleep_for(50ms);
                auto after = tortilla::channel::ask(name, "after deaf", 200ms);
                check(asked && after && *after == "got after deaf",
                      "cache channel: an asker that never reads doesn't hold up the next");
            }
            {
                // More stalled than are served at once, idle and deaf: they're
                // let go, and the next question is answered after all.
                std::vector<std::unique_ptr<raw_client>> stalled;
                for (int i = 0; i < tortilla::channel::k_serve_clients + 2; ++i)
                {
                    stalled.push_back(std::make_unique<raw_client>(at));
                    if (i % 2) stalled.back()->send("big\n");
                }
                auto after =
                    tortilla::channel::ask(name, "after many", tortilla::channel::k_serve_wait * 6);
                check(after && *after == "got after many",
                      "cache channel: every slot stalled, the stalled are let go in time");
            }
        }

        const std::string r = std::string(argv[1]) + "/badge-cache";
        sh("git init -q -b main '" + r + "' && mkdir '" + r + "/src' && echo a > '" + r +
           "/src/a.txt' && cd '" + r +
           "' && git -c user.name=Ada -c user.email=ada@example.com add . && git -c "
           "user.name=Ada -c user.email=ada@example.com commit -qm one");
        std::mutex m;
        std::vector<std::filesystem::path> redrawn;
        const auto summaries = std::filesystem::path(argv[1]) / "badge-summaries";
        tortilla::badge_cache cache(
            [&](const std::vector<std::filesystem::path>& paths)
            {
                std::lock_guard lock(m);
                redrawn.insert(redrawn.end(), paths.begin(), paths.end());
            },
            summaries);
        const auto file = std::filesystem::path(r) / "src" / "a.txt";
        check(cache.ask(file) == badge::none,
              "badge cache: nothing until the repository is loaded");
        auto wait_for = [&](badge want)
        {
            for (int i = 0; i < 100 && cache.ask(file) != want; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            return cache.ask(file) == want;
        };
        check(wait_for(badge::clean), "badge cache: loaded in the background, clean");
        {
            const auto st = cache.look(file);
            check(st.b == badge::clean && st.current && !st.untracked,
                  "badge cache: a loaded repository's answer is current");
        }
        check(cache.ask(std::filesystem::path(r) / "src") == badge::clean &&
                  cache.ask(std::filesystem::path(r)) == badge::clean,
              "badge cache: its folders and the top");
        check(cache.ask(std::filesystem::path(argv[1])) == badge::none,
              "badge cache: outside a repository, none");
        {
            std::lock_guard lock(m);
            redrawn.clear();
        }
        sh("echo more >> '" + rap::sys::path_to_utf8(file) + "'");
        check(wait_for(badge::modified), "badge cache: an edit seen by its watch");
        bool told = false;
        for (int i = 0; i < 40 && !told; ++i)
        {
            {
                std::lock_guard lock(m);
                told = std::ranges::find(redrawn, file) != redrawn.end();
            }
            if (!told) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        check(told, "badge cache: the changed file is reported for redrawing");
        check(tortilla::answer_cache_request(cache, "badge " + rap::sys::path_to_utf8(file)) ==
                  std::to_string(int(badge::modified)),
              "badge cache: the request's answer");
        check(tortilla::answer_cache_request(cache, "states\t" + rap::sys::path_to_utf8(file) +
                                                        "\t" + std::string(argv[1])) ==
                  std::to_string(int(badge::modified)) + " 1 0;0 0 0",
              "badge cache: a selection's states in one request");

        // A repository seen only from outside: its folder's summary, from a
        // plain status, without loading the repository.
        const std::string other = std::string(argv[1]) + "/badge-cache-other";
        sh("git init -q -b main '" + other + "' && echo o > '" + other + "/o.txt' && cd '" + other +
           "' && git -c user.name=Ada -c user.email=ada@example.com add . && git -c "
           "user.name=Ada -c user.email=ada@example.com commit -qm one && echo x >> o.txt");
        const std::filesystem::path other_root(other), other_file = other_root / "o.txt";
        check(cache.ask(other_root) == badge::none, "badge cache: an unknown repository, none yet");
        for (int i = 0; i < 100 && cache.ask(other_root) != badge::modified; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        check(cache.ask(other_root) == badge::modified,
              "badge cache: a repository's folder from its summary");
        check(cache.ask(other_file) == badge::none,
              "badge cache: the summary didn't load the repository");
        tortilla::badge_cache later([](const std::vector<std::filesystem::path>&) {}, summaries);
        check(later.ask(other_root) == badge::modified,
              "badge cache: summaries remembered across runs");

        // A linked worktree keeps its index and HEAD in the repository's
        // .git, outside it: a commit there changes no file in the tree, and
        // is seen by watching those too.
        const std::string linked = std::string(argv[1]) + "/badge-cache-linked";
        sh("cd '" + r + "' && git worktree add -q -b side '" + linked + "' && echo b > '" + linked +
           "/src/a.txt'");
        const auto linked_file = std::filesystem::path(linked) / "src" / "a.txt";
        auto settles_on = [&](badge want)
        {
            for (int i = 0; i < 100; ++i)
            {
                const auto st = cache.look(linked_file);
                if (st.b == want && st.current) return true;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            return false;
        };
        check(settles_on(badge::modified), "badge cache: a linked worktree's edit");
        sh("cd '" + linked +
           "' && git -c user.name=Ada -c user.email=ada@example.com commit -qam two");
        check(settles_on(badge::clean), "badge cache: a commit in a linked worktree is seen");
    }

    // The Show menu's branch filter: hide patterns and pins.
    {
        using tortilla::glob_match;
        check(glob_match("dependabot/*", "dependabot/npm/x") && glob_match("*", "") &&
                  glob_match("re?ovate/*-x", "renovate/a-x") &&
                  !glob_match("dependabot/*", "dependabot") && !glob_match("a*b", "acbd") &&
                  glob_match("*a*b*", "xxaxxbxx"),
              "branch filter: glob patterns, * across slashes");
        const tortilla::ref remote{.name = "origin/dependabot/npm/x",
                                   .full_name = "refs/remotes/origin/dependabot/npm/x",
                                   .kind = tortilla::ref_kind::remote_branch};
        const tortilla::ref tag{.name = "dependabot/v1",
                                .full_name = "refs/tags/dependabot/v1",
                                .kind = tortilla::ref_kind::tag};
        check(tortilla::hidden_by(remote, {"dependabot/*"}) &&
                  tortilla::hidden_by(remote, {"origin/dependabot/*"}) &&
                  !tortilla::hidden_by(remote, {"renovate/*"}) && !tortilla::hidden_by(tag, {"*"}),
              "branch filter: a remote branch hidden with or without its remote; tags never");
        tortilla::ref current{.name = "topic",
                              .full_name = "refs/heads/topic",
                              .kind = tortilla::ref_kind::local_branch,
                              .current = true};
        check(tortilla::is_pinned(current, {"HEAD"}) &&
                  tortilla::is_pinned(remote, {remote.full_name}) &&
                  !tortilla::is_pinned(remote, {"HEAD"}),
              "branch filter: pinned by name, or as HEAD");
        check(tortilla::preset_from_key(tortilla::preset_key(tortilla::show_preset::mine)) ==
                      tortilla::show_preset::mine &&
                  tortilla::preset_from_key("nonsense") == tortilla::show_preset::current,
              "branch filter: presets saved by name");
    }

    // Presets, pins, My branches, worktree branches, and the tips-only and
    // not-merged walks, on a repository made for them:
    //   main: m1 - m2 ------------- M (merge of `merged`)   = origin/main
    //   theirs: m2 - t1 - t2 (someone else's)
    //   buried: m2 - b1 (mine) - b2 (someone else's, the tip)
    //   merged: m2 - x1 (mine), merged into main
    //   aliased: m2 - a1 (mine under an old address the .mailmap maps)
    //   fresh: made by me at M, no commits; wt: checked out in a worktree
    {
        const std::string dir = std::string(argv[1]) + "/branch-filter";
        // Each identity in the environment too: CI exports GIT_AUTHOR_EMAIL
        // and GIT_COMMITTER_EMAIL, which outrank user.email.
        auto as = [&](const char* name, const char* email)
        {
            return std::string("GIT_AUTHOR_NAME=") + name + " GIT_AUTHOR_EMAIL=" + email +
                   " GIT_COMMITTER_NAME=" + name + " GIT_COMMITTER_EMAIL=" + email + " git -C '" +
                   dir + "' -c user.name=" + name + " -c user.email=" + email + " ";
        };
        const std::string me = as("Me", "me@example.com");
        const std::string other = as("Other", "other@example.com");
        const std::string old_me = as("Me", "me@old.example");
        sh("git init -q -b main '" + dir + "'");
        sh("git -C '" + dir + "' config user.email me@example.com");
        sh("printf 'Me <me@example.com> <me@old.example>\\n' > '" + dir + "/.mailmap'");
        sh(me + "add .mailmap && " + me + "commit -qm m1 && " + me +
           "commit -q --allow-empty -m m2");
        sh(me + "branch theirs && " + me + "branch buried && " + me + "branch merged && " + me +
           "branch aliased");
        sh(other + "checkout -q theirs && " + other + "commit -q --allow-empty -m t1 && " + other +
           "commit -q --allow-empty -m t2");
        sh(me + "checkout -q buried && " + me + "commit -q --allow-empty -m b1 && " + other +
           "commit -q --allow-empty -m b2");
        sh(old_me + "checkout -q aliased && " + old_me + "commit -q --allow-empty -m a1");
        sh(me + "checkout -q merged && " + me + "commit -q --allow-empty -m x1");
        sh(me + "checkout -q main && " + me + "merge -q --no-ff merged -m M");
        sh(me + "branch fresh");
        sh(me + "remote add up https://example.com/u.git && " + me +
           "remote add origin https://example.com/o.git");
        sh(me + "update-ref refs/remotes/origin/main main && " + me +
           "symbolic-ref refs/remotes/origin/HEAD refs/remotes/origin/main");
        sh(me + "update-ref refs/remotes/up/master main~1");
        sh(me + "worktree add -q -b wt '" + dir + "-wt' main");

        auto r = tortilla::repo::open(dir);
        check(r.has_value(), "branch filter: the repository opens");
        if (r)
        {
            const auto rev = [&](const char* name) { return *r->resolve(name); };
            check(
                tortilla::remote_defaults(*r) ==
                    std::vector<std::string>{"refs/remotes/origin/main", "refs/remotes/up/master"},
                "pins: each remote's default, origin's from its HEAD, up's master");
            check(tortilla::default_pins(*r) == std::vector<std::string>{"HEAD",
                                                                         "refs/remotes/origin/main",
                                                                         "refs/remotes/up/master"},
                  "pins: HEAD and the remotes' defaults by default");
            check(tortilla::merge_base_branch(*r, {}, {"up"}) == "refs/remotes/up/master" &&
                      tortilla::merge_base_branch(*r, {}, {}) == "refs/remotes/origin/main",
                  "not merged: the leading remote's main, else origin's");

            const tortilla::ref_map refs = r->refs();
            auto find = [&](std::string_view full) -> const tortilla::ref*
            {
                for (auto& [id, labels] : refs)
                    for (const tortilla::ref& x : labels)
                        if (x.full_name == full) return &x;
                return nullptr;
            };
            const tortilla::ref* wt = find("refs/heads/wt");
            const tortilla::ref* origin_main = find("refs/remotes/origin/main");
            const tortilla::ref* fresh = find("refs/heads/fresh");
            check(wt && wt->worktree.ends_with("branch-filter-wt") && fresh &&
                      fresh->worktree.empty() && find("refs/heads/main") &&
                      find("refs/heads/main")->worktree.empty(),
                  "worktrees: a branch checked out in another worktree says where");
            check(origin_main && origin_main->remote_default && find("refs/remotes/up/master") &&
                      find("refs/remotes/up/master")->remote_default,
                  "labels: each remote's default branch marked");

            // My branches.
            const tortilla::identity id(
                *r,
                {{.kind = tortilla::forge_kind::github, .host = "github.com", .login = "Me-gh"}});
            check(id.matches("ME@example.com") && id.matches("me@old.example") &&
                      id.matches("me-gh@users.noreply.github.com") &&
                      id.matches("123+me-gh@users.noreply.github.com") &&
                      !id.matches("x+me-gh@users.noreply.github.com") &&
                      !id.matches("other@example.com"),
                  "my branches: me by config, mailmap and GitHub noreply addresses");
            const std::optional<tortilla::oid> base = r->resolve("refs/remotes/origin/main");
            auto mine = [&](const char* name)
            {
                const tortilla::ref* b = find(std::string("refs/heads/") + name);
                return b && tortilla::branch_is_mine(*r, *b, rev(name), base, id);
            };
            check(mine("buried"), "my branches: my commit under someone else's tip");
            check(!mine("theirs"), "my branches: not someone else's");
            check(mine("fresh"), "my branches: made here, no commits yet");
            check(!mine("merged"), "my branches: not once merged");
            check(mine("aliased"), "my branches: authored under an address the mailmap maps");
            check(!mine("main"), "my branches: not main itself");

            auto choose =
                [&](tortilla::branch_filter f, tortilla::pr_branches prs = {.known = true})
            {
                return tortilla::choose_branches(
                    f, refs, r->head_id(), prs,
                    [&](const tortilla::ref& b, const tortilla::oid& tip)
                    { return tortilla::branch_is_mine(*r, b, tip, base, id); });
            };
            const auto mine_choice =
                choose({.preset = tortilla::show_preset::mine, .pins = {"HEAD"}});
            check(mine_choice.shown_names ==
                      std::vector<std::string>{"refs/heads/aliased", "refs/heads/buried",
                                               "refs/heads/fresh", "refs/heads/wt"},
                  "presets: My branches");
            const auto pulls =
                choose({.preset = tortilla::show_preset::pull_requests},
                       {.known = true, .all = {"refs/remotes/origin/main"}, .mine = {}});
            check(pulls.shown_names == std::vector<std::string>{"refs/remotes/origin/main"} &&
                      !pulls.waiting,
                  "presets: pull requests' branches");
            const auto local = choose({.preset = tortilla::show_preset::local,
                                       .pins = {"refs/remotes/up/master", "refs/heads/theirs"},
                                       .hidden = {"the*", "bur*"}});
            check(std::ranges::find(local.shown_names, "refs/heads/buried") ==
                          local.shown_names.end() &&
                      std::ranges::find(local.shown_names, "refs/heads/theirs") !=
                          local.shown_names.end() &&
                      std::ranges::find(local.shown_names, "refs/heads/main") !=
                          local.shown_names.end() &&
                      local.hidden == 1 && local.pins_added == 1 &&
                      local.pinned == std::vector{rev("main~1")},
                  "presets: local, less what's hidden (a pin wins), plus a pin");
            const auto waiting = choose({.preset = tortilla::show_preset::pull_requests,
                                         .pins = {"refs/remotes/origin/main"}},
                                        {.known = false});
            check(waiting.waiting && waiting.shown == std::vector{rev("main")} &&
                      waiting.pinned.empty(),
                  "presets: pull requests before the forge answers, the pins in full");
            const auto all = choose({.preset = tortilla::show_preset::all, .pins = {"HEAD"}});
            check(all.pins_added == 0 && all.shown.size() == 6, "presets: all, HEAD in it already");

            auto walk = [&](tortilla::walk_options o)
            {
                std::vector<tortilla::commit> out;
                (void)r->walk(o,
                              [&](tortilla::commit&& c)
                              {
                                  out.push_back(std::move(c));
                                  return true;
                              });
                return out;
            };
            auto has = [](const std::vector<tortilla::commit>& cs, const tortilla::oid& id)
            {
                return std::ranges::any_of(cs,
                                           [&](const tortilla::commit& c) { return c.id == id; });
            };
            auto get = [](const std::vector<tortilla::commit>& cs, const tortilla::oid& id)
            { return std::ranges::find(cs, id, &tortilla::commit::id); };

            // Branch tips only: the labelled commits, and where they part
            // and join. b1 (mine, under b2) and t1 go; m2, where they all
            // part, stays; b2's parent is m2 now.
            std::vector<tortilla::oid> labelled;
            for (auto& [c, labels] : refs) labelled.push_back(c);
            const auto full = walk({.refs = tortilla::walk_refs::all});
            const auto tips =
                walk({.refs = tortilla::walk_refs::all, .tips_only = true, .decorated = labelled});
            const auto b2 = get(tips, rev("buried"));
            check(tips.size() < full.size() && !has(tips, rev("buried~1")) &&
                      !has(tips, rev("theirs~1")) && has(tips, rev("main~1")) && b2 != tips.end() &&
                      b2->parents == std::vector{rev("main~1")} && has(tips, rev("main")),
                  "tips only: the labels, the fork between them, parents rewritten");

            // Only what's not merged into main: theirs and buried down to m2,
            // shown without parents; nothing of main's but that.
            const auto unmerged = walk({.refs = tortilla::walk_refs::tips,
                                        .tips = {rev("theirs"), rev("buried")},
                                        .unmerged_into = rev("main")});
            const auto m2 = get(unmerged, rev("main~1"));
            check(unmerged.size() == 5 && m2 != unmerged.end() && m2->parents.empty() &&
                      !has(unmerged, rev("main~2")),
                  "not merged: each branch down to its merge base, no further");
            const auto merged_tip = walk({.refs = tortilla::walk_refs::tips,
                                          .tips = {rev("merged")},
                                          .unmerged_into = rev("main")});
            check(merged_tip.size() == 1 && merged_tip.front().id == rev("merged"),
                  "not merged: a merged branch is its tip alone");

            // A pin: main's tip on theirs' history, joined where they part.
            const auto pinned = walk({.rev = "theirs", .sparse_tips = {rev("main")}});
            const auto main_tip = get(pinned, rev("main"));
            check(main_tip != pinned.end() && main_tip->parents == std::vector{rev("main~1")} &&
                      !has(pinned, rev("merged")) && has(pinned, rev("theirs~1")) &&
                      has(pinned, rev("main~2")),
                  "pins: a pinned branch's tip, joined to the rest without its own history");
            const auto pins_only = walk({.refs = tortilla::walk_refs::tips});
            check(pins_only.empty(), "pins: a tips walk with nothing to walk is empty");

            // Show log of a..b and a...b: theirs and buried each have two
            // commits on m2.
            const auto range = walk({.rev = "main..theirs"});
            check(range.size() == 2 && has(range, rev("theirs")) && has(range, rev("theirs~1")),
                  "range: a..b is b's history less a's");
            const auto symmetric = walk({.rev = "theirs...buried"});
            check(symmetric.size() == 4 && has(symmetric, rev("buried~1")) &&
                      has(symmetric, rev("theirs")) && !has(symmetric, rev("main~1")),
                  "range: a...b is both less their merge base");
            check(walk({.rev = "theirs..theirs"}).empty(), "range: a..a is empty");
        }
    }

    // The log's sync: a bare "remote", a clone of it, then commits on the
    // remote the clone hasn't fetched.
    {
        using namespace tortilla;
        const std::string root = std::string(argv[1]) + "/sync";
        const std::string bare = root + "/remote.git", seed = root + "/seed",
                          clone = root + "/clone";
        const std::string id = "-c user.name=Ada -c user.email=ada@example.com ";
        const std::string s = "git -C '" + seed + "' " + id, c = "git -C '" + clone + "' " + id;
        sh("git init -q --bare -b main '" + bare + "' && git init -q -b main '" + seed + "'");
        sh(s + "commit -q --allow-empty -m one && " + s + "remote add origin '" + bare + "'");
        for (const char* b : {"feature", "other", "feature2", "gone-branch"}) sh(s + "branch " + b);
        sh(s + "push -q origin main feature other feature2 gone-branch");
        sh("git clone -q '" + bare + "' '" + clone + "'");
        // Local branches: two to move on, one with work of its own, one in
        // another worktree.
        sh(c + "branch -q --track feature origin/feature && " + c +
           "branch -q --track stale origin/other && " + c +
           "branch -q --track mine origin/main && " + c +
           "branch -q --track wt origin/feature2 && " + c + "worktree add -q '" + root + "/wt' wt");
        sh(c + "checkout -q mine && " + c + "commit -q --allow-empty -m mine && " + c +
           "checkout -q main");
        // The remote moves on.
        sh(s + "commit -q --allow-empty -m two");
        for (const char* b : {"feature", "other", "feature2"}) sh(s + "branch -f " + b);
        sh(s + "branch newbranch && " + s +
           "push -q -f origin main feature other feature2 newbranch :gone-branch");
        const std::string remote_main = resolve(seed, "main");
        const std::string before_main = resolve(clone, "origin/main");

        const git_result listed = run_git(clone, {"ls-remote", "--heads", "origin"});
        const remote_tips theirs =
            parse_ls_remote("warning: --heads is deprecated\n" + listed.output);
        check(listed.ok() && theirs.branches.size() == 5 && theirs.complete &&
                  std::ranges::any_of(theirs.branches, [&](const branch_tip& b)
                                      { return b.name == "main" && b.sha == remote_main; }),
              "sync: ls-remote's branches, its warnings skipped");
        check(parse_ls_remote("abc\trefs/heads/x\n" + std::string(40, 'a') + "\trefs/tags/v1\n")
                  .branches.empty(),
              "sync: ls-remote's tags and junk aren't branches");

        const auto ours = local_tips(clone, {"origin"});
        const remote_changes changes = compare_tips("origin", theirs, ours.at("origin"));
        check(changes.moved == std::vector<std::string>{"feature", "feature2", "main", "other"} &&
                  changes.added == std::vector<std::string>{"newbranch"} &&
                  changes.deleted == std::vector<std::string>{"gone-branch"},
              "sync: moved, new and deleted branches against refs/remotes");
        check(changes_text(changes) ==
                  "origin: feature moved, feature2 moved, main moved, other moved, newbranch new, "
                  "gone-branch deleted",
              "sync: the changes in words");
        remote_tips partial = theirs;
        partial.complete = false;
        check(compare_tips("origin", partial, ours.at("origin")).deleted.empty(),
              "sync: a partial list deletes nothing");
        check(compare_tips("origin", theirs,
                           local_tips(clone, {"origin"}, "refs/prefetch/").at("origin"))
                      .added.size() == 5,
              "sync: nothing prefetched yet");

        // The download ahead of a click: refs/prefetch only.
        const git_result pre =
            run_git(clone, prefetch_args("origin"), git_output::all, git_prompts::never);
        check(pre.ok() && resolve(clone, "origin/main") == before_main &&
                  resolve(clone, "refs/prefetch/origin/main") == remote_main &&
                  resolve(clone, "refs/remotes/origin/gone-branch").size() == 40 &&
                  !resolve(clone, "refs/tags/v1").size(),
              "sync: a prefetch leaves refs/remotes as they were");
        check(compare_tips("origin", theirs,
                           local_tips(clone, {"origin"}, "refs/prefetch/").at("origin"))
                  .empty(),
              "sync: prefetched, nothing left to download");
        {
            auto r = repo::open(clone);
            bool none = r.has_value();
            if (r)
                for (const auto& [oid, refs] : r->refs())
                    for (const ref& x : refs)
                        none = none && !x.full_name.starts_with("refs/prefetch/");
            check(none && !walks_ref(walk_refs::all, "refs/prefetch/origin/main") &&
                      walks_ref(walk_refs::all, "refs/remotes/origin/main"),
                  "sync: prefetched refs are no labels, and not walked");
        }

        // After the real fetch: what can be fast-forwarded.
        const auto fetched = fetch_remotes(clone, {"origin"});
        check(fetched.at("origin").failure == fetch_failure::none &&
                  resolve(clone, "origin/main") == remote_main,
              "sync: the fetch");
        const auto targets = fast_forward_targets(clone);
        std::vector<std::string> names;
        for (const fast_forward& f : targets) names.push_back(f.branch);
        std::ranges::sort(names);
        check(names == std::vector<std::string>{"feature", "stale"},
              "sync: behind branches move; diverged, current and other worktrees' stay");
        check(fast_forward_targets(clone, "origin").size() == 2 &&
                  fast_forward_targets(clone, "elsewhere").empty(),
              "sync: a remote's fetch moves only branches tracking it");
        {
            const auto args = fetch_args("origin");
            check(std::ranges::find(args, "--no-auto-maintenance") != args.end() ||
                      std::ranges::find(args, "gc.auto=0") != args.end(),
                  "sync: fetches side by side leave maintenance alone");
        }
        for (const auto& cmd : fast_forward_commands(targets)) run_git(clone, cmd);
        check(resolve(clone, "feature") == resolve(clone, "origin/feature") &&
                  resolve(clone, "stale") == resolve(clone, "origin/other") &&
                  resolve(clone, "wt") != resolve(clone, "origin/feature2") &&
                  resolve(clone, "mine") != remote_main,
              "sync: fast-forwarded");
        const current_state cur = current_against_upstream(clone);
        check(cur.branch == "main" && cur.upstream == "origin/main" && cur.behind == 1 &&
                  cur.ahead == 0 && cur.clean,
              "sync: the current branch is behind its upstream");

        // Remotes that can't be fetched fail, not hang.
        sh(c + "remote add nowhere '" + root + "/missing.git' && " + c +
           "remote add closed http://127.0.0.1:9/x.git && " + c + "config remote.skipped.url '" +
           bare + "' && " + c + "config remote.skipped.skipFetchAll yes");
        check(fetch_all_remotes(clone) == std::vector<std::string>{"closed", "nowhere", "origin"} ||
                  fetch_all_remotes(clone) ==
                      std::vector<std::string>{"origin", "nowhere", "closed"},
              "sync: --all's remotes, without the skipped");
        const auto failed = fetch_remotes(clone, {"nowhere", "closed"});
        check(failed.at("nowhere").failure == fetch_failure::other &&
                  failed.at("closed").failure == fetch_failure::unreachable,
              "sync: a missing repository and a closed port fail");
        check(classify_failure("fatal: could not read Username for 'https://github.com': terminal "
                               "prompts disabled")
                          .failure == fetch_failure::credentials &&
                  classify_failure("ssh: Could not resolve hostname x: Name or service not known")
                          .failure == fetch_failure::unreachable &&
                  classify_failure("git@github.com: Permission denied (publickey).").failure ==
                      fetch_failure::credentials,
              "sync: failures sorted by kind");
    }

    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
