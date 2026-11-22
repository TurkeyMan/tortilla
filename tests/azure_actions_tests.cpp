#include "forge/azure.h"
#include "forge/azure_credentials.h"
#include "forge/forge_detail.h"
#include "forge/pr_watch.h"
#include "test_support.h"

#include <algorithm>

namespace tortilla::test
{
void run_azure_actions_tests()
{
    const forge_repo repo{
        .host = "dev.azure.com", .owner = "fabrikam/My Project", .name = "My Repo"};
    std::string root =
        "https://dev.azure.com/fabrikam/My%20Project/_apis/git/repositories/My%20Repo";
    const std::string sha(40, 'a'), token = "actions-fixture-pat";
    forge_detail::bind_token(token, "dev.azure.com/fabrikam");
    struct request
    {
        std::string method, url, body, authorization, content_type;
    };
    std::vector<request> requests;
    int status = 200;
    std::string error, ref_status = "succeeded", pr_status = "active", merge_status = "succeeded";
    bool ref_success = true, auto_complete = false, missing_thread = false;
    // W1 GitPullRequest request/response fields: source/target refs, title, description, isDraft,
    // pullRequestId, status, mergeStatus, lastMergeSourceCommit, autoCompleteSetBy, completionOptions.
    // https://learn.microsoft.com/en-us/rest/api/azure/devops/git/pull-requests/create?view=azure-devops-rest-7.1
    // https://learn.microsoft.com/en-us/rest/api/azure/devops/git/pull-requests/update?view=azure-devops-rest-7.1
    auto transport = [&](std::string_view method, const std::string& url, std::string_view body,
                         const rap::sys::http_options& options)
    {
        request sent{std::string(method), url, std::string(body)};
        for (const auto& [name, value] : options.headers)
        {
            if (name == "Authorization") sent.authorization = value;
            if (name == "Content-Type") sent.content_type = value;
        }
        requests.push_back(std::move(sent));
        rap::sys::http_response response;
        response.status = status;
        response.error = error;
        response.body = R"({"message":"Permission denied"})";
        if (status != 200 || !error.empty()) return response;
        // W2 documented GitRepository.defaultBranch.
        // https://learn.microsoft.com/en-us/rest/api/azure/devops/git/repositories/get-repository?view=azure-devops-rest-7.1
        if (url == root + "?api-version=7.1")
            response.body = R"({"defaultBranch":"refs/heads/main"})";
        // W3 Identity from ConnectionData (same read-only preflight as PR1).
        // https://github.com/microsoft/azure-devops-node-api/blob/master/api/WebApi.ts
        else if (url.ends_with("/_apis/connectionData"))
            response.body =
                R"({"authenticatedUser":{"id":"d6245f20-2af8-44f4-9451-8107cb2767db","providerDisplayName":"Alice"}})";
        // W4 CommentThread.id and comments (the request uses documented numeric enum values).
        // https://learn.microsoft.com/en-us/rest/api/azure/devops/git/pull-request-threads/create?view=azure-devops-rest-7.1
        else if (url.find("/threads?") != url.npos)
            response.body =
                missing_thread
                    ? "{}"
                    : R"({"id":17,"comments":[{"id":1,"content":"Reason","commentType":"text"}],"status":"active"})";
        // W5 GitRef.name/objectId and GitRefUpdateResult success/updateStatus/customMessage.
        // https://learn.microsoft.com/en-us/rest/api/azure/devops/git/refs/list?view=azure-devops-rest-7.1
        // https://learn.microsoft.com/en-us/rest/api/azure/devops/git/refs/update-refs?view=azure-devops-rest-7.1
        else if (url.find("/refs?") != url.npos)
            response.body = method == "GET"
                                ? "{\"value\":[{\"name\":\"refs/heads/topic/one\",\"objectId\":\"" +
                                      sha + "\"}]}"
                                : "{\"value\":[{\"name\":\"refs/heads/topic/one\",\"success\":" +
                                      std::string(ref_success ? "true" : "false") +
                                      ",\"updateStatus\":" + json::quoted(ref_status) +
                                      ",\"customMessage\":\"Branch policy\"}]}";
        // W6 enabled required policy schema, also exercised by read-only F4.
        // https://learn.microsoft.com/en-us/rest/api/azure/devops/policy/evaluations/list?view=azure-devops-rest-7.1
        else if (url.find("/policy/evaluations?") != url.npos)
            response.body = R"({"value":[]})";
        else
        {
            response.status = method == "POST" ? 201 : 200;
            const auto posted = json::parse(body);
            response.body =
                "{\"pullRequestId\":42,\"title\":\"Fix\",\"sourceRefName\":\"refs/heads/topic/"
                "one\","
                "\"targetRefName\":\"refs/heads/main\",\"lastMergeSourceCommit\":{\"commitId\":\"" +
                sha + "\"},\"status\":" +
                json::quoted(posted && (*posted)["status"] ? (*posted)["status"].str()
                                                           : pr_status) +
                ",\"mergeStatus\":" + json::quoted(merge_status) +
                ",\"repository\":{\"project\":{\"id\":\"a7573007-bbb3-4341-b726-0c4148a07853\"}},"
                "\"autoCompleteSetBy\":{" +
                (auto_complete || (posted && (*posted)["autoCompleteSetBy"])
                     ? std::string("\"id\":\"d6245f20-2af8-44f4-9451-8107cb2767db\"")
                     : std::string()) +
                "}}";
        }
        return response;
    };
    forge_detail::set_transport(transport);
    const auto branch = default_branch(forge_kind::azure, repo, token);
    check(branch && *branch == "main" && requests.back().url == root + "?api-version=7.1",
          "azure writes: documented defaultBranch read and decoded default branch");
    const auto targets = pull_request_targets(forge_kind::azure, repo, token);
    check(targets && *targets == std::vector<forge_repo>{repo},
          "azure writes: same-repository targets only");
    new_pull_request n{.head = repo,
                       .head_branch = "topic/one",
                       .base_branch = "main",
                       .title = "A \"fix\"",
                       .body = "line1\nline2",
                       .draft = true};
    requests.clear();
    const auto created = create_pull_request(forge_kind::azure, repo, n, token);
    const auto created_body = json::parse(requests.back().body);
    check(created && created->number == 42 && created->url.ends_with("/pullrequest/42") &&
              requests.size() == 1 && requests[0].method == "POST" &&
              requests[0].url == root + "/pullrequests?api-version=7.1" &&
              requests[0].authorization == "Basic " + azure::basic_token(token) && created_body &&
              requests[0].content_type == "application/json" &&
              (*created_body)["sourceRefName"].str_is("refs/heads/topic/one") &&
              (*created_body)["targetRefName"].str_is("refs/heads/main") &&
              (*created_body)["title"].str() == n.title &&
              (*created_body)["description"].str() == n.body &&
              (*created_body)["isDraft"].boolean(),
          "azure writes: exact create URL, auth and documented JSON fields");
    auto fork = n;
    fork.head.name = "fork";
    const auto before = requests.size();
    check(!create_pull_request(forge_kind::azure, repo, fork, token) && before == requests.size(),
          "azure writes: fork creation refused before transport");
    for (const auto invalid : {"", "../main", "a//b", "a.lock", "a b", "a\\b", "a@{b", ".hidden"})
    {
        auto bad = n;
        bad.head_branch = invalid;
        check(!create_pull_request(forge_kind::azure, repo, bad, token) &&
                  before == requests.size(),
              "azure writes: invalid branch rejected before transport");
    }
    for (const auto method : {merge_method::merge, merge_method::squash, merge_method::rebase})
    {
        requests.clear();
        merge_request m{
            .method = method, .commit_title = "Title", .commit_message = "Body", .head_sha = sha};
        const auto why = merge_pull_request(forge_kind::azure, repo, 42, m, token);
        const auto j = json::parse(requests.back().body);
        check(why.empty() && requests.size() == 1 && requests[0].method == "PATCH" &&
                  requests[0].url == root + "/pullrequests/42?api-version=7.1" && j &&
                  (*j)["status"].str_is("completed") &&
                  (*j)["lastMergeSourceCommit"]["commitId"].str() == sha &&
                  (*j)["completionOptions"]["mergeStrategy"].str() ==
                      (method == merge_method::merge    ? "noFastForward"
                       : method == merge_method::squash ? "squash"
                                                        : "rebase") &&
                  (*j)["completionOptions"]["mergeCommitMessage"].str_is("Title\n\nBody") &&
                  !(*j)["completionOptions"]["bypassPolicy"],
              "azure writes: each UI merge strategy and expected-head guard, no policy bypass");
    }
    requests.clear();
    check(!merge_pull_request(forge_kind::azure, repo, 42, {}, token).empty() && requests.empty(),
          "azure writes: missing expected head refuses mutation");
    merge_request when{.head_sha = sha, .when_ready = true};
    check(merge_pull_request(forge_kind::azure, repo, 42, when, token).empty() &&
              requests.size() == 2 && requests[0].method == "GET" &&
              requests[0].url.ends_with("/_apis/connectionData") &&
              (*json::parse(requests[1].body))["autoCompleteSetBy"]["id"].str_is(
                  "d6245f20-2af8-44f4-9451-8107cb2767db") &&
              !(*json::parse(requests[1].body))["status"],
          "azure writes: auto-complete identity read precedes single PATCH; does not complete "
          "immediately");
    for (const auto status_word : {"active", "completed", "abandoned", "future"})
    {
        pr_status = status_word;
        const auto state = pull_request_merge_state(forge_kind::azure, repo, 42, token);
        check(state && state->state == (pr_status == "active" ? merge_state::verdict::ready
                                                              : merge_state::verdict::blocked),
              "azure writes: inactive or unknown PR status never enables immediate completion");
    }
    pr_status = "active";
    merge_status = "future";
    check(pull_request_merge_state(forge_kind::azure, repo, 42, token)->state ==
              merge_state::verdict::checking,
          "azure writes: unknown merge status stays checking");
    merge_status = "succeeded";
    requests.clear();
    check(close_pull_request(forge_kind::azure, repo, 42, "Reason", token).empty() &&
              requests.size() == 2 && requests[0].method == "POST" &&
              requests[0].url == root + "/pullrequests/42/threads?api-version=7.1" &&
              (*json::parse(requests[0].body))["comments"][size_t(0)]["content"].str_is("Reason") &&
              requests[1].method == "PATCH" &&
              (*json::parse(requests[1].body))["status"].str_is("abandoned"),
          "azure writes: close posts documented reason thread before abandonment");
    missing_thread = true;
    requests.clear();
    check(!close_pull_request(forge_kind::azure, repo, 42, "Reason", token).empty() &&
              requests.size() == 1,
          "azure writes: unconfirmed reason prevents abandonment");
    missing_thread = false;
    forge_detail::set_transport(
        [&](auto method, const auto& url, auto body, const auto& options)
        {
            auto response = transport(method, url, body, options);
            if (method == "PATCH")
            {
                response.status = 403;
                response.body = R"({"message":"Abandon denied"})";
            }
            return response;
        });
    const auto partial = close_pull_request(forge_kind::azure, repo, 42, "Reason", token);
    check(partial.find("reason was posted") != partial.npos &&
              partial.find("HTTP 403") != partial.npos &&
              partial.find("Abandon denied") != partial.npos,
          "azure writes: partial close failure describes posted reason and HTTP error");
    forge_detail::set_transport(transport);
    requests.clear();
    check(delete_branch(forge_kind::azure, repo, "topic/one", token).empty() &&
              requests.size() == 2 && requests[0].method == "GET" &&
              requests[0].url == root + "/refs?filter=heads%2Ftopic%2Fone&api-version=7.1" &&
              requests[1].method == "POST" && requests[1].url == root + "/refs?api-version=7.1" &&
              (*json::parse(requests[1].body))[size_t(0)]["oldObjectId"].str() == sha &&
              (*json::parse(requests[1].body))[size_t(0)]["newObjectId"].str() ==
                  std::string(40, '0'),
          "azure writes: exact branch lookup and guarded refs deletion");
    for (const auto failure : {"staleOldObjectId", "forcePushRequired", "writePermissionRequired",
                               "rejectedByPolicy", "locked", "future"})
    {
        ref_status = failure;
        ref_success = false;
        const auto why = delete_branch(forge_kind::azure, repo, "topic/one", token);
        check(why.find(failure) != why.npos && why.find("HTTP 200") != why.npos,
              "azure writes: HTTP200 per-ref failure and unknown status are not success");
    }
    ref_status = "succeeded";
    ref_success = true;
    {
        // W1/W2/W4/W5 accept an omitted project; Git routes stay organization-scoped.
        // The documented repository.project hydration is needed for Build, not these routes.
        const auto with_project = root;
        root = "https://dev.azure.com/fabrikam/_apis/git/repositories/My%20Repo";
        const forge_repo projectless{
            .host = "dev.azure.com", .owner = "fabrikam", .name = "My Repo"};
        auto projectless_new = n;
        projectless_new.head = projectless;
        requests.clear();
        const auto default_ref = default_branch(forge_kind::azure, projectless, token);
        const auto new_pr = create_pull_request(forge_kind::azure, projectless, projectless_new, token);
        const auto merge_error = merge_pull_request(
            forge_kind::azure, projectless, 42, {.head_sha = sha}, token);
        const auto close_error = close_pull_request(
            forge_kind::azure, projectless, 42, "Reason", token);
        const auto delete_error = delete_branch(forge_kind::azure, projectless, "topic/one", token);
        check(default_ref && *default_ref == "main" && new_pr && merge_error.empty() &&
                  close_error.empty() && delete_error.empty() && requests.size() == 7 &&
                  std::ranges::all_of(requests, [&](const request& r)
                                     { return r.url.starts_with(root); }),
              "azure writes: projectless default/create/merge/close/delete use optional-project Git routes");
        root = with_project;
        requests.clear();
    }
    {
        // A write follows no redirect, not even one within the organization:
        // a 307/308 would send the change again, a 301-303 turn it into a GET.
        // This transport asks on_redirect as rapport's does, before the hop.
        int redirect_status = 307;
        bool hooked = true;
        const std::string elsewhere =
            "https://dev.azure.com/fabrikam/My%20Project/_apis/git/elsewhere?api-version=7.1";
        forge_detail::set_transport(
            [&](std::string_view method, const std::string& url, std::string_view body,
                const rap::sys::http_options& options) -> rap::sys::http_response
            {
                if (method == "GET") return transport(method, url, body, options);
                requests.push_back({std::string(method), url, std::string(body)});
                if (!options.on_redirect)
                {
                    hooked = false;
                    return transport(method, url, body, options);
                }
                if (options.on_redirect(url, elsewhere, redirect_status) ==
                    rap::sys::redirect_action::stop)
                    return {.status = redirect_status,
                            .headers = {{"Location", elsewhere}},
                            .url = url};
                return transport(method, elsewhere, body, options);  // sent again
            });
        const auto writes = [&]
        {
            return std::ranges::count_if(requests,
                                         [](const request& r) { return r.method != "GET"; });
        };
        for (const int s : {301, 302, 303, 307, 308})
        {
            redirect_status = s;
            requests.clear();
            const auto created_again = create_pull_request(forge_kind::azure, repo, n, token);
            check(!created_again && writes() == 1 && hooked &&
                      created_again.error().find("redirect (HTTP " + std::to_string(s) + ")") !=
                          std::string::npos &&
                      created_again.error().find("wasn't followed or sent again") !=
                          std::string::npos,
                  "azure writes: a redirected create stops, sent once and not followed");
        }
        redirect_status = 307;
        requests.clear();
        const auto merged = merge_pull_request(
            forge_kind::azure, repo, 42,
            {.method = merge_method::squash, .head_sha = sha}, token);
        check(merged.find("wasn't followed") != std::string::npos && writes() == 1 && hooked,
              "azure writes: a redirected merge PATCH stops, sent once");
        requests.clear();
        const auto closed = close_pull_request(forge_kind::azure, repo, 42, "Reason", token);
        check(closed.find("wasn't followed") != std::string::npos && writes() == 1 && hooked,
              "azure writes: a redirected close stops at its first write");
        requests.clear();
        const auto deleted = delete_branch(forge_kind::azure, repo, "topic/one", token);
        check(deleted.find("wasn't followed") != std::string::npos && writes() == 1 && hooked &&
                  requests.size() == 2,
              "azure writes: a redirected ref update stops, sent once");
        forge_detail::set_transport(transport);
    }
    for (const int rejected : {401, 403, 429, 500})
    {
        status = rejected;
        requests.clear();
        const auto why = create_pull_request(forge_kind::azure, repo, n, token);
        check(!why && requests.size() == 1 &&
                  why.error().find("Code (Read & Write)") != why.error().npos,
              "azure writes: PAT error names write scope and never retries mutation");
    }
    status = 200;
    error = "Connection lost";
    requests.clear();
    check(!create_pull_request(forge_kind::azure, repo, n, token) && requests.size() == 1,
          "azure writes: network failure does not duplicate PR creation");
    error.clear();
    forge_detail::set_transport([&](auto method, const auto& url, auto body, const auto& options)
    {
        auto response = transport(method, url, body, options);
        response.headers.emplace_back("Content-Type", "text/html");
        return response;
    });
    requests.clear();
    check(!create_pull_request(forge_kind::azure, repo, n, token) && requests.size() == 1,
          "azure writes: HTML content type never confirms a write even with JSON-shaped body");
    forge_detail::set_transport(transport);
    for (const bool lost : {false, true})
    {
        requests.clear();
        forge_detail::set_transport(
            [&](auto method, const auto& url, auto body, const auto& options)
            {
                auto response = transport(method, url, body, options);
                if (method != "GET")
                {
                    response.status = lost ? 0 : 401;
                    response.error = lost ? "Connection lost" : "";
                    response.body = R"({"message":"Write rejected"})";
                }
                return response;
            });
        merge_request now{.head_sha = sha};
        check(!merge_pull_request(forge_kind::azure, repo, 42, now, token).empty() &&
                  requests.size() == 1,
              "azure writes: merge auth/network failure sends exactly one PATCH");
        requests.clear();
        check(!close_pull_request(forge_kind::azure, repo, 42, "Reason", token).empty() &&
                  requests.size() == 1,
              "azure writes: reason auth/network failure sends one comment and never abandons");
        requests.clear();
        check(!delete_branch(forge_kind::azure, repo, "topic/one", token).empty() &&
                  requests.size() == 2 && requests[0].method == "GET" &&
                  requests[1].method == "POST",
              "azure writes: branch auth/network failure never repeats guarded mutation");
    }
    forge_detail::set_transport(transport);

    // W7 synthetic legacy credential protocol: no native helper or credential store is consulted.
    // https://git-scm.com/docs/git-credential
    credential_helper::set_runner(
        [](const credential_helper::request&)
            -> std::expected<credential_helper::reply, std::string>
        {
            return credential_helper::reply{
                .exit_code = 0, .output = "username=fixture\npassword=actions-helper-fixture\n\n"};
        });
    const auto automatic = azure::automatic_account(
        "https://dev.azure.com/fabrikam/My%20Project/_git/My%20Repo", "actions-fixture-workdir");
    const auto helper = load_token(*automatic);
    check(helper.has_value(), "azure writes: fake helper supplies worker-owned credential");
    if (!helper)
    {
        credential_helper::set_runner({});
        forge_detail::set_transport({});
        return;
    }
    requests.clear();
    forge_detail::set_transport(
        [&](auto method, const auto& url, auto body, const auto& options)
        {
            auto response = transport(method, url, body, options);
            if (url.ends_with("/_apis/connectionData") &&
                requests.back().authorization.starts_with("Basic "))
            {
                response.status = 401;
                response.body = R"({"message":"Use OAuth"})";
            }
            return response;
        });
    const auto helper_pr = create_pull_request(forge_kind::azure, repo, n, helper->access);
    check(helper_pr && requests.size() == 3 && requests[0].method == "GET" &&
              requests[1].method == "GET" && requests[0].url.ends_with("/_apis/connectionData") &&
              requests[0].url == requests[1].url && requests[2].method == "POST" &&
              requests[2].authorization == "Bearer actions-helper-fixture",
          "azure writes: ambiguous helper negotiates only read-only identity, then sends one write "
          "with known scheme");
    requests.clear();
    status = 401;
    check(!create_pull_request(forge_kind::azure, repo, n, helper->access) &&
              requests.size() == 1 && requests[0].method == "POST",
          "azure writes: resolved helper receiving write401 does not negotiate or repeat mutation");
    status = 200;
    // A different fixture secret bypasses the successful scheme cache above.
    credential_helper::set_runner(
        [](const credential_helper::request&)
            -> std::expected<credential_helper::reply, std::string>
        {
            return credential_helper::reply{
                .exit_code = 0,
                .output = "username=fixture\npassword=unresolved-actions-fixture\n\n"};
        });
    const auto unresolved = load_token(*automatic);
    requests.clear();
    status = 401;
    check(unresolved && !create_pull_request(forge_kind::azure, repo, n, unresolved->access) &&
              requests.size() == 2 &&
              std::ranges::all_of(requests, [](const request& r) { return r.method == "GET"; }),
          "azure writes: failed read-only auth preflight prevents every write");
    status = 200;
    credential_helper::set_runner({});
    forge_detail::set_transport({});
}
}  // namespace tortilla::test
