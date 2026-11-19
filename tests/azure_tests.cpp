#include "forge/azure.h"
#include "forge/forge_detail.h"
#include "forge/pr_details.h"
#include "forge/pr_watch.h"
#include "test_support.h"

#include <algorithm>
#include <chrono>
#include <fstream>

namespace tortilla::test
{
void run_azure_tests()
{
    const forge_repo repo{
        .host = "dev.azure.com", .owner = "fabrikam/My Project", .name = "My Repo"};
    for (const char* url :
         {"https://dev.azure.com/fabrikam/My%20Project/_git/My%20Repo",
          "https://user@dev.azure.com/fabrikam/My%20Project/_git/My%20Repo",
          "git@ssh.dev.azure.com:v3/fabrikam/My%20Project/My%20Repo",
          "ssh://git@ssh.dev.azure.com/v3/fabrikam/My%20Project/My%20Repo",
          "https://fabrikam.visualstudio.com/My%20Project/_git/My%20Repo",
          "https://fabrikam.visualstudio.com/DefaultCollection/My%20Project/_git/My%20Repo",
          "fabrikam@vs-ssh.visualstudio.com:v3/fabrikam/My%20Project/My%20Repo"})
        check(repo_of_url(url) == repo,
              "azure: modern, userinfo, SSH and legacy URLs decode project/repository names");
    // Azure clone URLs name the repository literally, including a .git suffix.
    // https://learn.microsoft.com/en-us/azure/devops/repos/git/clone?view=azure-devops
    for (const char* url :
         {"https://dev.azure.com/fabrikam/project/_git/sdk.git",
          "https://user@dev.azure.com/fabrikam/project/_git/sdk.git",
          "https://fabrikam.visualstudio.com/project/_git/sdk.git",
          "https://fabrikam.visualstudio.com/DefaultCollection/project/_git/sdk.git",
          "git@ssh.dev.azure.com:v3/fabrikam/project/sdk.git",
          "ssh://git@ssh.dev.azure.com/v3/fabrikam/project/sdk.git"})
    {
        const auto literal = repo_of_url(url);
        check(literal && literal->name == "sdk.git" &&
                  repo_of_url(azure::repository_web(*literal)) == literal,
              "azure: literal .git repository name survives parsing and web URL roundtrip");
    }
    check(repo_of_url("https://github.com/owner/sdk.git")->name == "sdk",
          "azure: other forges retain conventional clone suffix stripping");
    check(token_scopes(forge_kind::azure).find("Code (Read) and Build (Read)") !=
                  std::string::npos &&
              token_scopes(forge_kind::azure).find("Write") == std::string::npos,
          "azure: readonly PAT instructions request Code Read and Build Read only");
    check(!repo_of_url("https://dev.azure.com/org%2Fother/project/_git/repo"),
          "azure: encoded scope separator refused");
    check(known_forge("DEV.AZURE.COM") == forge_kind::azure, "azure: shared hostname recognized");
    const std::vector<forge_account> accounts{
        {.kind = forge_kind::azure, .host = "dev.azure.com/other", .login = "other"},
        {.kind = forge_kind::azure, .host = "dev.azure.com/fabrikam", .login = "correct"},
        {.kind = forge_kind::azure, .host = "dev.azure.com", .login = "host-wide"}};
    check(account_for_url("https://fabrikam.visualstudio.com/My%20Project/_git/My%20Repo", accounts)
                      ->login == "correct" &&
              account_for_url("git@ssh.dev.azure.com:v3/fabrikam/My%20Project/My%20Repo", accounts)
                      ->login == "correct" &&
              !account_for_url("https://dev.azure.com/missing/project/_git/repo", accounts),
          "azure: account selection requires the correct organization, with no hostname fallback");
    forge_detail::bind_token("azure-fixture-pat", "dev.azure.com/fabrikam");
    const auto encoded = azure::basic_token("azure-fixture-pat");
    check(azure::basic_token("pat") == "OnBhdA==",
          "azure: Basic auth uses an empty username and PAT password");
    check(
        forge_detail::token_allowed(encoded, "https://dev.azure.com/fabrikam/project/_apis/git") &&
            !forge_detail::token_allowed(
                encoded, "https://dev.azure.com/fabrikam-other/project/_apis/git") &&
            !forge_detail::token_allowed(encoded,
                                         "https://dev.azure.com/other/project/_apis/git") &&
            !forge_detail::token_allowed(encoded,
                                         "https://example.com/fabrikam/project/_apis/git") &&
            !forge_detail::token_allowed(encoded,
                                         "https://dev.azure.com/fabrikam/../other/_apis") &&
            !forge_detail::token_allowed(encoded,
                                         "https://dev.azure.com/fabrikam/%2E%2E/other/_apis"),
        "azure: Basic credentials are bound to the organization path boundary");
    check(!browser_sign_in(forge_kind::azure, "dev.azure.com/fabrikam") &&
              token_page(forge_kind::azure, "dev.azure.com/fabrikam") ==
                  "https://dev.azure.com/fabrikam/_usersSettings/tokens",
          "azure: PAT token page, with no browser OAuth");

    // F1 synthetic GitPullRequest using only documented fields (list + full PR).
    // https://learn.microsoft.com/en-us/rest/api/azure/devops/git/pull-requests/get-pull-requests?view=azure-devops-rest-7.1
    // https://learn.microsoft.com/en-us/rest/api/azure/devops/git/pull-requests/get-pull-request?view=azure-devops-rest-7.1
    // Reviewer fields/votes: https://learn.microsoft.com/en-us/rest/api/azure/devops/git/pull-request-reviewers/list?view=azure-devops-rest-7.1
    const std::string pr =
        R"({"pullRequestId":42,"title":"Fix","description":"Full description","sourceRefName":"refs/heads/topic","targetRefName":"refs/heads/main","lastMergeSourceCommit":{"commitId":"sha"},"isDraft":true,"mergeStatus":"conflicts","repository":{"project":{"id":"a7573007-bbb3-4341-b726-0c4148a07853"}},"createdBy":{"uniqueName":"alice@example.com","imageUrl":"https://identity.invalid/avatar"},"labels":[{"name":"reviewed"}],"reviewers":[{"uniqueName":"yes","vote":10},{"uniqueName":"yes-with-suggestions","vote":5},{"uniqueName":"waiting","vote":-5},{"uniqueName":"no","vote":-10},{"uniqueName":"requested","vote":0}]})";
    // F2 Timeline.records: id/parentId/type/name/state/result/attempt/times/issues/log.id.
    // https://learn.microsoft.com/en-us/rest/api/azure/devops/build/timeline/get?view=azure-devops-rest-7.1
    const std::string timeline =
        R"({"records":[{"id":"00000000-0000-0000-0000-000000000001","type":"Stage","name":"Test"},{"id":"00000000-0000-0000-0000-000000000002","type":"Phase","parentId":"00000000-0000-0000-0000-000000000001"},{"id":"00000000-0000-0000-0000-000000000003","parentId":"00000000-0000-0000-0000-000000000002","type":"Job","name":"Linux","state":"completed","result":"failed","attempt":1,"startTime":"2026-01-01T10:00:00Z","finishTime":"2026-01-01T10:01:00Z"},{"id":"00000000-0000-0000-0000-000000000004","parentId":"00000000-0000-0000-0000-000000000002","type":"Job","name":"Windows","state":"completed","result":"succeeded"},{"id":"00000000-0000-0000-0000-000000000005","parentId":"00000000-0000-0000-0000-000000000003","type":"Task","name":"Compile","state":"completed","result":"failed","issues":[{"type":"error","message":"compiler error"}],"log":{"id":7}}]})";
    int requests = 0, build_pages = 0, artifact_requests = 0;
    bool auth_ok = true, urls_ok = true;
    auto documented = [&](std::string_view method, const std::string& url, std::string_view,
                          const rap::sys::http_options& options)
    {
        ++requests;
        auth_ok &= std::ranges::find(
                       options.headers,
                       std::pair<std::string, std::string>{"Authorization", "Basic " + encoded}) !=
                   options.headers.end();
        urls_ok &= method == "GET" && url.starts_with("https://dev.azure.com/fabrikam/") &&
                   (url.find("api-version=7.1") != std::string::npos ||
                    url.ends_with("/_apis/connectionData")) &&
                   url.find(' ') == std::string::npos;
        rap::sys::http_response reply;
        reply.status = 200;
        reply.body = R"({"value":[]})";
        // F3 ConnectionData.authenticatedUser Identity has documented id/providerDisplayName.
        // https://learn.microsoft.com/en-us/javascript/api/azure-devops-extension-api/connectiondata
        // https://learn.microsoft.com/en-us/javascript/api/azure-devops-extension-api/identity
        if (url.find("connectionData") != std::string::npos)
            reply.body =
                R"({"authenticatedUser":{"id":"d6245f20-2af8-44f4-9451-8107cb2767db","providerDisplayName":"Alice"}})";
        else if (url.find("/pullrequests?") != std::string::npos)
            reply.body = "{\"value\":[" + pr + "]}";
        else if (url.find("/pullrequests/42?") != std::string::npos)
            reply.body = pr;
        // F4 PolicyEvaluationRecord.status/configuration.isEnabled/isBlocking/type.displayName.
        // https://learn.microsoft.com/en-us/rest/api/azure/devops/policy/evaluations/list?view=azure-devops-rest-7.1
        else if (url.find("/policy/evaluations?") != std::string::npos)
            reply.body =
                R"({"value":[{"status":"rejected","configuration":{"isEnabled":true,"isBlocking":true,"type":{"displayName":"Build validation"}}}]})";
        // F5 GitRepository.id (repository names are accepted in the route).
        // https://learn.microsoft.com/en-us/rest/api/azure/devops/git/repositories/get-repository?view=azure-devops-rest-7.1
        else if (url.find("/git/repositories/My%20Repo?") != std::string::npos)
            reply.body = R"({"id":"3411ebc1-d5aa-464f-9615-0b527bc66719"})";
        // F6 Build.id/sourceVersion/definition.id/name/_links.web.href and continuation header.
        // https://learn.microsoft.com/en-us/rest/api/azure/devops/build/builds/list?view=azure-devops-rest-7.1
        else if (url.find("/build/builds?") != std::string::npos)
        {
            ++build_pages;
            if (url.find("continuationToken=next") == std::string::npos)
            {
                reply.body =
                    R"({"value":[{"id":99,"sourceVersion":"different","definition":{"id":1,"name":"Pipeline"}}]})";
                reply.headers.emplace_back("x-ms-continuationtoken", "next");
            }
            else
                reply.body =
                    R"({"value":[{"id":23,"sourceVersion":"sha","definition":{"id":1,"name":"Pipeline"},"_links":{"web":{"href":"https://dev.azure.com/fabrikam/My%20Project/_build/results?buildId=23"}}},{"id":22,"sourceVersion":"sha","definition":{"id":1,"name":"Old run"}}]})";
        }
        else if (url.find("/timeline?") != std::string::npos)
            reply.body = timeline;
        // F7 GitStatus.context/state/targetUrl (documented sample response fields).
        // https://learn.microsoft.com/en-us/rest/api/azure/devops/git/statuses/list?view=azure-devops-rest-7.1
        else if (url.find("/statuses?") != std::string::npos)
            reply.body =
                R"({"value":[{"context":{"genre":"external","name":"lint"},"state":"succeeded","targetUrl":"https://external.example/result"}]})";
        // F8 BuildArtifact.name/resource.downloadUrl.
        // https://learn.microsoft.com/en-us/rest/api/azure/devops/build/artifacts/list?view=azure-devops-rest-7.1
        else if (url.find("/artifacts?") != std::string::npos)
        {
            ++artifact_requests;
            reply.body =
                R"({"value":[{"name":"package","resource":{"downloadUrl":"https://dev.azure.com/fabrikam/My%20Project/_apis/build/builds/23/artifacts?artifactName=package&api-version=7.1"}}]})";
        }
        // F9 BuildLog.id/lineCount.
        // https://learn.microsoft.com/en-us/rest/api/azure/devops/build/builds/get-build-logs?view=azure-devops-rest-7.1
        else if (url.find("/logs?") != std::string::npos)
            reply.body = R"({"value":[{"id":7,"lineCount":1000}]})";
        // F10 text/plain log body; startLine retrieves only the tail.
        // https://learn.microsoft.com/en-us/rest/api/azure/devops/build/builds/get-build-log?view=azure-devops-rest-7.1
        else if (url.find("/logs/7?") != std::string::npos)
            reply.body = "compiling\ncompiler error\n";
        // F11 GitRef.name/objectId.
        // https://learn.microsoft.com/en-us/rest/api/azure/devops/git/refs/list?view=azure-devops-rest-7.1
        else if (url.find("/refs?") != std::string::npos)
            reply.body =
                R"({"value":[{"name":"refs/heads/main","objectId":"67cae2b029dff7eb3dc062b49403aaedca5bad8d"}]})";
        else
        {
            reply.status = 500;
            reply.body = R"({"message":"Unexpected fixture endpoint"})";
        }
        return reply;
    };
    forge_detail::set_transport(documented);
    const auto account = who_am_i(forge_kind::azure, "dev.azure.com/fabrikam", "azure-fixture-pat");
    check(account && account->host == "dev.azure.com/fabrikam" &&
              account->login == "d6245f20-2af8-44f4-9451-8107cb2767db" && account->name == "Alice",
          "azure: organization connectionData retains friendly identity");
    const auto list = open_pull_requests(forge_kind::azure, repo, "azure-fixture-pat", true, true);
    check(list && list->access == repo_access::read && list->prs.size() == 1 &&
              list->prs[0].head_sha == "sha" && list->prs[0].head_branch == "topic" &&
              list->prs[0].base_branch == "main" && list->prs[0].labels.size() == 1 &&
              list->prs[0].author_avatar.empty() && !list->prs[0].body_known && list->tips &&
              list->tips->branches[0].name == "main",
          "azure: read-only PR list, decoded branches/labels and deferred full description");
    const auto details =
        pull_request_details(forge_kind::azure, repo, {.number = 42}, "azure-fixture-pat");
    check(details && details->body_known && details->blocked &&
              details->checks == check_state::failure && details->draft && details->conflicts &&
              details->reviews.size() == 5 &&
              details->reviews[0].say == pr_review::verdict::approved &&
              details->reviews[1].say == pr_review::verdict::approved &&
              details->reviews[2].say == pr_review::verdict::requested &&
              details->reviews[3].say == pr_review::verdict::changes_requested &&
              details->reviews[4].say == pr_review::verdict::requested,
          "azure: full PR, all reviewer votes and rejected required policies");
    const auto ci = commit_checks(forge_kind::azure, repo, "sha", "azure-fixture-pat");
    check(ci && build_pages == 2 && ci->workflows.size() == 2 &&
              ci->workflows[0].name == "Pipeline / Test" && ci->workflows[0].run_id == 23 &&
              ci->workflows[0].jobs.size() == 2 && ci->state() == check_state::failure &&
              ci->workflows[0].jobs[0].id != ci->workflows[0].jobs[1].id &&
              ci->workflows[1].jobs[0].state == check_state::success,
          "azure: paginated commit builds, newest per definition, phase/stage hierarchy, distinct "
          "jobs and external statuses");
    if (ci && !ci->workflows.empty())
    {
        auto repeated = commit_checks(forge_kind::azure, repo, "sha", "azure-fixture-pat");
        check(repeated && repeated->workflows[0].jobs[0].id == ci->workflows[0].jobs[0].id,
              "azure: polling preserves job cache identity");
        const auto f = job_failure_details(forge_kind::azure, repo, ci->workflows[0].jobs[0],
                                           "azure-fixture-pat");
        check(f && f->step == "Compile" && f->annotations.size() == 1 &&
                  f->log_tail.find("compiler error") != std::string::npos,
              "azure: failed task issues and log tail");
        const auto a = commit_artifacts(forge_kind::azure, repo, "sha", *ci, "azure-fixture-pat");
        check(a && a->size() == 1 && artifact_requests == 1 && (*a)[0].size == 0,
              "azure: artifact names/download URLs with unknown size and one request per run");
    }
    const int before = requests;
    check(!create_pull_request(forge_kind::azure, repo, {}, "azure-fixture-pat") &&
              !merge_pull_request(forge_kind::azure, repo, 42, {}, "azure-fixture-pat").empty() &&
              !close_pull_request(forge_kind::azure, repo, 42, "reason", "azure-fixture-pat")
                   .empty() &&
              !delete_branch(forge_kind::azure, repo, "topic", "azure-fixture-pat").empty() &&
              before == requests,
          "azure: unsupported mutations never reach the transport");
    check(auth_ok && urls_ok,
          "azure: scoped Basic auth and API 7.1; connectionData follows unversioned SDK route");

    // E1 synthetic enum fixtures use the exact documented state/result/vote values.
    // https://learn.microsoft.com/en-us/rest/api/azure/devops/build/timeline/get?view=azure-devops-rest-7.1
    for (auto [word, expected] : std::vector<std::pair<std::string, check_state>>{
             {"succeeded", check_state::success},
             {"succeededWithIssues", check_state::success},
             {"failed", check_state::failure},
             {"canceled", check_state::failure},
             {"skipped", check_state::none},
             {"abandoned", check_state::failure},
             {"future-result", check_state::none}})
    {
        const auto text = "{\"state\":\"completed\",\"result\":" + json::quoted(word) + "}";
        check(azure::timeline_state(*json::parse(text)) == expected,
              "azure: every timeline result and future result");
    }
    for (auto [word, expected] :
         std::vector<std::pair<std::string, check_state>>{{"pending", check_state::pending},
                                                          {"inProgress", check_state::pending},
                                                          {"completed", check_state::none},
                                                          {"future-state", check_state::none}})
    {
        const auto text = "{\"state\":" + json::quoted(word) + ",\"result\":null}";
        check(azure::timeline_state(*json::parse(text)) == expected,
              "azure: every timeline state and unknown state");
    }
    // E2 BuildResult and BuildStatus, deliberately distinct from TaskResult.
    // https://learn.microsoft.com/en-us/rest/api/azure/devops/build/builds/list?view=azure-devops-rest-7.1
    for (auto [word, expected] : std::vector<std::pair<std::string, check_state>>{
             {"none", check_state::none},
             {"succeeded", check_state::success},
             {"partiallySucceeded", check_state::failure},
             {"failed", check_state::failure},
             {"canceled", check_state::failure},
             {"future-result", check_state::none}})
    {
        const auto text = "{\"status\":\"completed\",\"result\":" + json::quoted(word) + "}";
        check(azure::build_state(*json::parse(text)) == expected,
              "azure: every build result and unknown result");
    }
    for (auto [word, expected] :
         std::vector<std::pair<std::string, check_state>>{{"none", check_state::none},
                                                          {"inProgress", check_state::pending},
                                                          {"completed", check_state::none},
                                                          {"cancelling", check_state::pending},
                                                          {"postponed", check_state::pending},
                                                          {"notStarted", check_state::pending},
                                                          {"all", check_state::none},
                                                          {"future-status", check_state::none}})
    {
        const auto text = "{\"status\":" + json::quoted(word) + ",\"result\":null}";
        check(azure::build_state(*json::parse(text)) == expected,
              "azure: every build status and unknown status");
    }
    // E3 GitStatusState.
    // https://learn.microsoft.com/en-us/rest/api/azure/devops/git/statuses/list?view=azure-devops-rest-7.1
    for (auto [word, expected] :
         std::vector<std::pair<std::string, check_state>>{{"notSet", check_state::none},
                                                          {"pending", check_state::pending},
                                                          {"succeeded", check_state::success},
                                                          {"failed", check_state::failure},
                                                          {"error", check_state::failure},
                                                          {"notApplicable", check_state::none},
                                                          {"future-status", check_state::none}})
        check(azure::status_state(word) == expected,
              "azure: every external status and unknown status");
    // E4 PolicyEvaluationStatus.
    // https://learn.microsoft.com/en-us/rest/api/azure/devops/policy/evaluations/list?view=azure-devops-rest-7.1
    for (auto [word, expected] :
         std::vector<std::pair<std::string, check_state>>{{"queued", check_state::pending},
                                                          {"running", check_state::pending},
                                                          {"approved", check_state::success},
                                                          {"rejected", check_state::failure},
                                                          {"broken", check_state::failure},
                                                          {"notApplicable", check_state::none},
                                                          {"future-status", check_state::none}})
        check(azure::policy_state(word) == expected,
              "azure: every policy status and unknown status");
    // E5 IdentityRefWithVote.vote; future votes are not approvals or rejections.
    // https://learn.microsoft.com/en-us/rest/api/azure/devops/git/pull-request-reviewers/list?view=azure-devops-rest-7.1
    check(!azure::review_vote(1) && !azure::review_vote(-1) && !azure::review_vote(100),
          "azure: unknown reviewer vote stays unknown");

    // E6 PullRequestAsyncStatus; unknown/missing states never assert mergeability.
    // https://learn.microsoft.com/en-us/rest/api/azure/devops/git/pull-requests/get-pull-request?view=azure-devops-rest-7.1
    for (const auto& word : {"notSet", "queued", "conflicts", "succeeded", "rejectedByPolicy",
                             "failure", "future-state"})
    {
        std::string payload = pr;
        payload.replace(payload.find("conflicts"), std::string("conflicts").size(), word);
        forge_detail::set_transport(
            [&](std::string_view, const std::string&, std::string_view,
                const rap::sys::http_options&) {
                return rap::sys::http_response{.status = 200,
                                               .body = "{\"value\":[" + payload + "]}"};
            });
        const auto p =
            open_pull_requests(forge_kind::azure, repo, "azure-fixture-pat", false, false);
        const bool known =
            std::string_view(word) == "conflicts" || std::string_view(word) == "succeeded" ||
            std::string_view(word) == "rejectedByPolicy" || std::string_view(word) == "failure";
        const bool blocked =
            std::string_view(word) == "rejectedByPolicy" || std::string_view(word) == "failure";
        check(p && p->prs.size() == 1 && p->prs[0].merge_known == known &&
                  p->prs[0].blocked == blocked,
              "azure: every documented PR merge state and unknown state");
    }

    // D1/D2/D3 are fault-injection responses, not claimed successful Azure payloads.
    // HTTP statuses: https://learn.microsoft.com/en-us/azure/devops/integrate/how-to/call-rest-api?view=azure-devops
    // PAT errors/scopes: https://learn.microsoft.com/en-us/azure/devops/organizations/accounts/use-personal-access-tokens-to-authenticate?view=azure-devops
    rap::sys::http_response fault;
    auto fail_at = [&](std::string marker)
    {
        forge_detail::set_transport(
            [&, marker](std::string_view method, const std::string& url, std::string_view body,
                        const rap::sys::http_options& options)
            {
                if (marker.empty() || url.find(marker) != std::string::npos) return fault;
                return documented(method, url, body, options);
            });
    };
    fault.status = 401;
    fault.body =
        "{\"message\":" + json::quoted("fixture denied azure-fixture-pat " + encoded) + "}";
    fail_at("");
    const auto denied = open_pull_requests(forge_kind::azure, repo, "azure-fixture-pat");
    check(!denied && denied.error().find("HTTP 401") != std::string::npos &&
              denied.error().find("fixture denied") != std::string::npos &&
              denied.error().find("PAT invalid") != std::string::npos &&
              denied.error().find("Code (Read)") != std::string::npos &&
              denied.error().find("azure-fixture-pat") == std::string::npos &&
              denied.error().find(encoded) == std::string::npos,
          "azure: HTTP401 keeps safe Azure message plus PAT and Code scope hint");
    const auto invalid_identity =
        who_am_i(forge_kind::azure, "dev.azure.com/fabrikam", "azure-fixture-pat");
    check(!invalid_identity && invalid_identity.error().find("HTTP 401") != std::string::npos,
          "azure: sign-in propagates HTTP diagnostics");
    fault.status = 203;
    fault.headers = {{"CONTENT-TYPE", "Text/HTML; charset=utf-8"}};
    fault.body =
        "  <!DOCTYPE HTML><html><title>Sign in</title><body>azure-fixture-pat</body></html>";
    const auto html = commit_checks(forge_kind::azure, repo, "sha", "azure-fixture-pat");
    check(!html && html.error().find("HTTP 203") != std::string::npos &&
              html.error().find("PAT invalid") != std::string::npos &&
              html.error().find("<html") == std::string::npos &&
              html.error().find("azure-fixture-pat") == std::string::npos,
          "azure: 203 HTML sign-in is diagnosed without dumping HTML or token");
    fault.headers.clear();
    fault.body = R"({"message":"Non-authoritative fixture response"})";
    const auto nonhtml = open_pull_requests(forge_kind::azure, repo, "azure-fixture-pat");
    check(!nonhtml && nonhtml.error().find("HTTP 203") != std::string::npos &&
              nonhtml.error().find("PAT invalid") == std::string::npos,
          "azure: JSON203 is not assumed to be an authentication failure");
    fault.status = 403;
    fault.body = R"({"message":"Build permission denied"})";
    fail_at("/artifacts?");
    if (ci)
    {
        const auto denied_artifacts =
            commit_artifacts(forge_kind::azure, repo, "sha", *ci, "azure-fixture-pat");
        check(!denied_artifacts &&
                  denied_artifacts.error().find("HTTP 403: Build permission denied") !=
                      std::string::npos &&
                  denied_artifacts.error().find("Build (Read)") != std::string::npos,
              "azure: artifact errors include HTTP status, Azure message and Build scope");
    }
    fail_at("/policy/evaluations?");
    const auto denied_policy =
        pull_request_details(forge_kind::azure, repo, {.number = 42}, "azure-fixture-pat");
    check(!denied_policy && denied_policy.error().find("HTTP 403") != std::string::npos,
          "azure: policy permission failure is not swallowed");
    fail_at("/refs?");
    const auto denied_refs =
        open_pull_requests(forge_kind::azure, repo, "azure-fixture-pat", true, true);
    check(!denied_refs && denied_refs.error().find("HTTP 403") != std::string::npos,
          "azure: refs permission failure is not mistaken for no branches");
    fail_at("/logs?");
    if (ci)
    {
        const auto denied_log = job_failure_details(forge_kind::azure, repo,
                                                    ci->workflows[0].jobs[0], "azure-fixture-pat");
        check(!denied_log && denied_log.error().find("Build (Read)") != std::string::npos,
              "azure: log-list failure reaches the existing failure error path");
    }
    fault.status = 500;
    fault.body = R"({"message":"fixture service unavailable"})";
    fail_at("");
    const auto unavailable = commit_checks(forge_kind::azure, repo, "sha", "azure-fixture-pat");
    check(!unavailable &&
              unavailable.error().find("HTTP 500: fixture service unavailable") !=
                  std::string::npos &&
              unavailable.error().find("PAT invalid") == std::string::npos,
          "azure: ordinary service error keeps status/message without invented auth diagnosis");
    // D4 download failure: downloaded HTTP error body is bounded, redacted and removed.
    // resource.downloadUrl contract: https://learn.microsoft.com/en-us/rest/api/azure/devops/build/artifacts/list?view=azure-devops-rest-7.1
    fault.status = 401;
    fault.body = R"({"message":"artifact access denied"})";
    const auto destination =
        std::filesystem::temp_directory_path() /
        ("tortilla-azure-fault-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".zip");
    const auto download_error = download_artifact(
        forge_kind::azure,
        {.name = "fixture",
         .download_url = "https://dev.azure.com/fabrikam/My%20Project/_apis/build/builds/23/"
                         "artifacts?artifactName=fixture&api-version=7.1"},
        destination, "azure-fixture-pat");
    check(download_error.find("HTTP 401: artifact access denied") != std::string::npos &&
              download_error.find("Build (Read)") != std::string::npos &&
              !std::filesystem::exists(destination),
          "azure: artifact download errors preserve JSON message without saving an error as a zip");
    {
        std::ofstream(destination) << "existing artifact";
    }
    const auto preserve_error = download_artifact(
        forge_kind::azure,
        {.name = "fixture",
         .download_url = "https://dev.azure.com/fabrikam/My%20Project/_apis/build/builds/23/"
                         "artifacts?artifactName=fixture&api-version=7.1"},
        destination, "azure-fixture-pat");
    std::string preserved;
    {
        std::ifstream in(destination);
        std::getline(in, preserved);
    }
    check(!preserve_error.empty() && preserved == "existing artifact",
          "azure: failed artifact HTTP request preserves the existing destination");
    std::filesystem::remove(destination);
    fault.status = 203;
    fault.body = "<html>Sign in</html>";
    fault.headers = {{"Content-Type", "text/html"}};
    const auto html_download = download_artifact(
        forge_kind::azure,
        {.name = "fixture",
         .download_url = "https://dev.azure.com/fabrikam/My%20Project/_apis/build/builds/23/"
                         "artifacts?artifactName=fixture&api-version=7.1"},
        destination, "azure-fixture-pat");
    check(html_download.find("PAT invalid") != std::string::npos &&
              !std::filesystem::exists(destination),
          "azure: artifact HTML203 is diagnosed and discarded");
    {
        std::ofstream(destination) << "existing artifact";
    }
    fault.status = 200;
    const auto preserve_html = download_artifact(
        forge_kind::azure,
        {.name = "fixture",
         .download_url = "https://dev.azure.com/fabrikam/My%20Project/_apis/build/builds/23/"
                         "artifacts?artifactName=fixture&api-version=7.1"},
        destination, "azure-fixture-pat");
    {
        std::ifstream in(destination);
        std::getline(in, preserved);
    }
    check(!preserve_html.empty() && preserved == "existing artifact",
          "azure: HTTP200 HTML artifact rejection preserves the existing destination content");
    fault.headers.clear();
    fault.body = R"({"message":"artifact error returned with success status"})";
    const auto preserve_json = download_artifact(
        forge_kind::azure,
        {.name = "fixture",
         .download_url = "https://dev.azure.com/fabrikam/My%20Project/_apis/build/"
                         "builds/23/artifacts?artifactName=fixture&api-version=7.1"},
        destination, "azure-fixture-pat");
    {
        std::ifstream in(destination);
        std::getline(in, preserved);
    }
    check(preserve_json.find("HTTP 200: artifact error") != std::string::npos &&
              preserved == "existing artifact",
          "azure: HTTP200 JSON error never replaces existing artifact content");
    std::filesystem::remove(destination);
    forge_detail::set_transport({});
}
}  // namespace tortilla::test
