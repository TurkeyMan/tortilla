#include "forge/azure.h"
#include "forge/forge_detail.h"
#include "forge/pr_details.h"
#include "forge/pr_watch.h"
#include "test_support.h"

#include <algorithm>

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

    // Fixtures follow Microsoft REST API 7.1 GitPullRequest, PolicyEvaluationRecord,
    // Build, TimelineRecord, BuildArtifact and GitStatus documented shapes.
    const std::string pr =
        R"({"pullRequestId":42,"title":"Fix","description":"Full description","sourceRefName":"refs/heads/topic","targetRefName":"refs/heads/main","lastMergeSourceCommit":{"commitId":"sha"},"isDraft":true,"mergeStatus":"conflicts","repository":{"project":{"id":"project-guid"}},"createdBy":{"uniqueName":"alice@example.com","imageUrl":"https://identity.invalid/avatar"},"labels":[{"name":"reviewed"}],"reviewers":[{"uniqueName":"yes","vote":10},{"uniqueName":"yes-with-suggestions","vote":5},{"uniqueName":"waiting","vote":-5},{"uniqueName":"no","vote":-10},{"uniqueName":"requested","vote":0}]})";
    const std::string timeline =
        R"({"records":[{"id":"stage","type":"Stage","name":"Test"},{"id":"phase","type":"Phase","parentId":"stage"},{"id":"job-one","parentId":"phase","type":"Job","name":"Linux","state":"completed","result":"failed","attempt":1,"startTime":"2026-01-01T10:00:00Z","finishTime":"2026-01-01T10:01:00Z"},{"id":"job-two","parentId":"phase","type":"Job","name":"Windows","state":"completed","result":"succeeded"},{"id":"task","parentId":"job-one","type":"Task","name":"Compile","state":"completed","result":"failed","issues":[{"type":"error","message":"compiler error"}],"log":{"id":7}}]})";
    int requests = 0, build_pages = 0, artifact_requests = 0;
    bool auth_ok = true, urls_ok = true;
    forge_detail::set_transport(
        [&](std::string_view method, const std::string& url, std::string_view,
            const rap::sys::http_options& options)
        {
            ++requests;
            auth_ok &= std::ranges::find(options.headers,
                                         std::pair<std::string, std::string>{"Authorization",
                                                                             "Basic " + encoded}) !=
                       options.headers.end();
            urls_ok &= method == "GET" && url.starts_with("https://dev.azure.com/fabrikam/") &&
                       url.find("api-version=7.1") != std::string::npos &&
                       url.find(' ') == std::string::npos;
            rap::sys::http_response reply;
            reply.status = 200;
            reply.body = R"({"value":[]})";
            if (url.find("connectionData") != std::string::npos)
                reply.body =
                    R"({"authenticatedUser":{"id":"identity-guid","providerDisplayName":"Alice","properties":{"Account":{"$value":"alice@example.com"}}}})";
            else if (url.find("/pullrequests?") != std::string::npos)
                reply.body = "{\"value\":[" + pr + "]}";
            else if (url.find("/pullrequests/42?") != std::string::npos)
                reply.body = pr;
            else if (url.find("/policy/evaluations?") != std::string::npos)
                reply.body =
                    R"({"value":[{"status":"rejected","configuration":{"isEnabled":true,"isBlocking":true,"type":{"displayName":"Build validation"}}}]})";
            else if (url.find("/git/repositories/My%20Repo?") != std::string::npos)
                reply.body = R"({"id":"repo-guid"})";
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
            else if (url.find("/statuses?") != std::string::npos)
                reply.body =
                    R"({"value":[{"context":{"genre":"external","name":"lint"},"state":"succeeded","targetUrl":"https://external.example/result"}]})";
            else if (url.find("/artifacts?") != std::string::npos)
            {
                ++artifact_requests;
                reply.body =
                    R"({"value":[{"name":"package","resource":{"downloadUrl":"https://dev.azure.com/fabrikam/My%20Project/_apis/build/builds/23/artifacts?artifactName=package&api-version=7.1"}}]})";
            }
            else if (url.find("/logs?") != std::string::npos)
                reply.body = R"({"value":[{"id":7,"lineCount":1000}]})";
            else if (url.find("/logs/7?") != std::string::npos)
                reply.body = "compiling\ncompiler error\n";
            return reply;
        });
    const auto account = who_am_i(forge_kind::azure, "dev.azure.com/fabrikam", "azure-fixture-pat");
    check(account && account->host == "dev.azure.com/fabrikam" &&
              account->login == "alice@example.com" && account->name == "Alice",
          "azure: organization connectionData retains friendly identity");
    const auto list = open_pull_requests(forge_kind::azure, repo, "azure-fixture-pat", true, false);
    check(list && list->access == repo_access::read && list->prs.size() == 1 &&
              list->prs[0].head_sha == "sha" && list->prs[0].head_branch == "topic" &&
              list->prs[0].base_branch == "main" && list->prs[0].labels.size() == 1 &&
              list->prs[0].author_avatar.empty() && !list->prs[0].body_known,
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
          "azure: every API request uses scoped Basic auth, escaped path segments and API 7.1");
    forge_detail::set_transport({});
}
}  // namespace tortilla::test
