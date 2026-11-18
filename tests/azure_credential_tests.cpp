#include "forge/azure.h"
#include "forge/azure_credentials.h"
#include "forge/forge_detail.h"
#include "test_support.h"

#include <algorithm>

namespace tortilla::test
{
void run_azure_credential_tests()
{
    // Request/response fixtures use Git's documented credential protocol.
    // https://git-scm.com/docs/git-credential
    // https://git-scm.com/docs/gitcredentials
    // https://github.com/git-ecosystem/git-credential-manager/blob/main/docs/azrepos-users-and-tokens.md
    const std::string modern =
        "https://alice%40example.com@dev.azure.com/fabrikam/My%20Project/_git/repo";
    const auto account = azure::automatic_account(modern, "fixture-workdir");
    const auto legacy =
        azure::automatic_account("https://alice%40example.com@fabrikam.visualstudio.com/"
                                 "DefaultCollection/My%20Project/_git/repo",
                                 "fixture-workdir");
    check(
        account && legacy && account->host == "dev.azure.com/fabrikam" &&
            legacy->host == account->host && account->credential_username == "alice@example.com" &&
            legacy->credential_username == account->credential_username &&
            account->credential_directory == "fixture-workdir" && account->via_git_credential,
        "azure helper: HTTPS modern/legacy aliases retain decoded username and repository context");
    if (!account) return;
    const auto request = azure::credential_request(*account);
    check(request && request->input == "capability[]=authtype\nprotocol=https\nhost=dev.azure."
                                       "com\npath=fabrikam\nusername=alice@example.com\n\n",
          "azure helper: exact capability, HTTPS host, organization path, remote username and "
          "final blank line");
    const auto anonymous = azure::automatic_account(
        "https://dev.azure.com/fabrikam/project/_git/repo", "fixture-workdir");
    const auto without_user = anonymous ? azure::credential_request(*anonymous)
                                        : std::unexpected(std::string("missing account"));
    check(without_user &&
              without_user->input ==
                  "capability[]=authtype\nprotocol=https\nhost=dev.azure.com\npath=fabrikam\n\n",
          "azure helper: absent remote username is omitted, not replaced with organization name");
    check(!azure::automatic_account("git@ssh.dev.azure.com:v3/fabrikam/project/repo",
                                    "fixture-workdir") &&
              !azure::automatic_account("http://dev.azure.com/fabrikam/project/_git/repo",
                                        "fixture-workdir") &&
              !azure::automatic_account("https://a%0Ab@dev.azure.com/fabrikam/project/_git/repo",
                                        "fixture-workdir"),
          "azure helper: no new SSH work, cleartext lookup or injected username fields");
    const forge_account pasted{.kind = forge_kind::azure,
                               .host = "dev.azure.com/fabrikam",
                               .login = "configured PAT user"};
    const auto chosen = azure::account_for_remote(modern, "fixture-workdir", {pasted});
    const auto automatic = azure::account_for_remote(
        modern, "fixture-workdir",
        {{.kind = forge_kind::azure, .host = "dev.azure.com/other", .login = "different"}});
    check(chosen && !chosen->via_git_credential && chosen->login == pasted.login && automatic &&
              automatic->via_git_credential,
          "azure helper: explicit same-organization PAT overrides automatic source; other org does "
          "not");
    auto bad_account = *account;
    bad_account.credential_username = "alice\npassword=not-a-real-secret";
    check(!azure::credential_request(bad_account),
          "azure helper: request field injection is refused");

    // Synthetic username/password, CRLF and capability-gated protocol responses.
    // https://git-scm.com/docs/git-credential#_input_output_format
    const auto empty_user =
        azure::parse_credential("username=\npassword=fixture-password\n\n", "fabrikam");
    const auto crlf =
        azure::parse_credential("protocol=https\r\nhost=dev.azure.com\r\npath=fabrikam\r\nusername="
                                "alice\r\npassword=fixture-password\r\n\r\n",
                                "fabrikam");
    check(empty_user && empty_user->username.empty() && crlf &&
              crlf->password == "fixture-password",
          "azure helper: empty username and standard CRLF output are parsed without trimming "
          "passwords");
    for (const char* output :
         {"username=alice\npassword=\n", "password=fixture\n", "username=alice\n",
          "username=alice\npassword=one\npassword=two\n",
          "username=alice\npassword=one\nbad-line\n",
          "protocol=http\nusername=alice\npassword=fixture\n",
          "host=other.example\nusername=alice\npassword=fixture\n",
          "path=other\nusername=alice\npassword=fixture\n",
          "username=alice\npassword=fixture\npassword_expiry_utc=1\n",
          "authtype=Bearer\ncredential=fixture\n",
          "capability[]=authtype\nauthtype=digest\ncredential=fixture\n",
          "capability[]=authtype\nauthtype=Bearer\ncredential=fixture\rmalformed\n"})
        check(!azure::parse_credential(output, "fabrikam"),
              "azure helper: malformed, missing, expired, mismatched or unsupported credential "
              "fields fail safely");
    const auto explicit_bearer = azure::parse_credential(
        "capability[]=authtype\nauthtype=bearer\ncredential=opaque-fixture-token\n\n", "fabrikam");
    check(explicit_bearer && explicit_bearer->explicit_scheme &&
              explicit_bearer->authtype == "Bearer",
          "azure helper: declared Bearer credential is opaque, no JWT or PAT format heuristic");

    int fills = 0;
    bool safe_request = true;
    std::string output = "protocol=https\nhost=dev.azure.com\npath=fabrikam\nusername=actual-"
                         "helper-user\npassword=opaque-fixture-one\n\n";
    credential_helper::set_runner(
        [&](const credential_helper::request& r)
            -> std::expected<credential_helper::reply, std::string>
        {
            ++fills;
            auto env = [&](std::string_view key, std::string_view value)
            {
                return std::ranges::any_of(r.environment, [&](const auto& e)
                                           { return e.first == key && e.second == value; });
            };
            safe_request &= r.directory == "fixture-workdir" && r.input == request->input &&
                            r.timeout == std::chrono::seconds(10) && r.output_limit == 65536 &&
                            env("GCM_INTERACTIVE", "never") && env("GIT_TERMINAL_PROMPT", "0") &&
                            env("GIT_ASKPASS", "") && env("SSH_ASKPASS", "") &&
                            std::ranges::find(r.arguments, "core.askPass=") != r.arguments.end() &&
                            std::ranges::find(r.arguments, "credential.useHttpPath=true") !=
                                r.arguments.end() &&
                            r.arguments[r.arguments.size() - 2] == "credential" &&
                            r.arguments.back() == "fill";
            return credential_helper::reply{.exit_code = 0, .output = output};
        });
    // Synthetic REST replies use the PR-list collection schema and root-message errors.
    // https://learn.microsoft.com/en-us/rest/api/azure/devops/git/pull-requests/get-pull-requests?view=azure-devops-rest-7.1
    // https://learn.microsoft.com/en-us/azure/devops/integrate/how-to/call-rest-api?view=azure-devops
    int calls = 0, basic = 0, bearer = 0;
    bool stripped_context = true, exact_basic = true;
    forge_detail::set_transport(
        [&](std::string_view method, const std::string& url, std::string_view,
            const rap::sys::http_options& options)
        {
            ++calls;
            std::string auth;
            for (const auto& [name, value] : options.headers)
            {
                stripped_context &= name != azure::credential_context_header;
                if (name == "Authorization") auth = value;
            }
            if (auth.starts_with("Basic "))
            {
                ++basic;
                exact_basic &= auth == "Basic " + azure::basic_pair("actual-helper-user",
                                                                    "opaque-fixture-one");
                return rap::sys::http_response{
                    .status = 401, .body = R"({"message":"Basic compatibility denied"})"};
            }
            ++bearer;
            return rap::sys::http_response{.status = 200, .body = R"({"value":[]})"};
        });
    std::string expired_handle;
    {
        auto token = load_token(*account);
        check(token && token->lifetime && azure::is_helper_token(token->access),
              "azure helper: load_token owns ephemeral context, not keychain data");
        if (token)
        {
            check(!store_token(*account, *token) && !store_token(pasted, *token) &&
                      erase_token(*account),
                  "azure helper: neither automatic nor manual-account storage persists an "
                  "ephemeral credential; erase never calls helper");
            expired_handle = token->access;
            const auto url = "https://dev.azure.com/fabrikam/project/_apis/git/repositories/repo/"
                             "pullrequests?api-version=7.1";
            auto reply = azure::helper_get(
                url, {.headers = api_headers(forge_kind::azure, token->access)}, token->access);
            check(reply.status == 200 && basic == 1 && bearer == 1 && exact_basic &&
                      stripped_context,
                  "azure helper: exact Basic pair, then one opaque Bearer retry; internal context "
                  "never reaches transport");
            const auto headers = api_headers(forge_kind::azure, token->access);
            const auto before = calls;
            const auto refused =
                forge_detail::http_get("https://dev.azure.com/other/project/_apis/git/repositories/"
                                       "repo/pullrequests?api-version=7.1",
                                       {.headers = headers});
            check(!refused.status && calls == before,
                  "azure helper: accepted credential still cannot reach another organization");
            auto again = load_token(*account);
            const auto reused =
                again ? api_headers(forge_kind::azure, again->access) : rap::sys::http_headers{};
            check(again && fills == 2 &&
                      std::ranges::any_of(reused,
                                          [](const auto& h) {
                                              return h.first == "Authorization" &&
                                                     h.second == "Bearer opaque-fixture-one";
                                          }),
                  "azure helper: nonsecret scoped fingerprint cache avoids repeated Basic401 on "
                  "the same OAuth credential");
        }
    }
    check(!azure::is_helper_token(expired_handle) &&
              !forge_detail::token_allowed(expired_handle,
                                           "https://dev.azure.com/fabrikam/project/_apis/git/"
                                           "repositories/repo/pullrequests?api-version=7.1"),
          "azure helper: worker lifetime ends secret ownership and removes opaque scope binding");
    check(safe_request, "azure helper: only bounded noninteractive fill with path preservation and "
                        "all askpass routes disabled");

    // Changed credentials reset accepted mode. A 403/429/500 must not negotiate.
    output = "username=actual-helper-user\npassword=opaque-fixture-two\n\n";
    auto changed = load_token(*account);
    check(changed &&
              std::ranges::any_of(api_headers(forge_kind::azure, changed->access),
                                  [](const auto& h) { return h.second.starts_with("Basic "); }),
          "azure helper: changed credential fingerprint resets the remembered scheme");
    for (const int status : {403, 429, 500})
    {
        calls = 0;
        forge_detail::set_transport(
            [&](std::string_view, const std::string&, std::string_view,
                const rap::sys::http_options&)
            {
                ++calls;
                return rap::sys::http_response{.status = status,
                                               .body = R"({"message":"fixture denied"})"};
            });
        const auto r = azure::helper_get(
            "https://dev.azure.com/fabrikam/project/_apis/git/repositories/repo/"
            "pullrequests?api-version=7.1",
            {.headers = api_headers(forge_kind::azure, changed->access)}, changed->access);
        check(r.status == status && calls == 1,
              "azure helper: permission/rate/service failures are never scheme-retried");
    }
    output = "capability[]=authtype\nauthtype=bearer\ncredential=declared-fixture-token\n\n";
    auto declared = load_token(*account);
    calls = 0;
    forge_detail::set_transport(
        [&](std::string_view, const std::string&, std::string_view, const rap::sys::http_options&)
        {
            ++calls;
            return rap::sys::http_response{.status = 401,
                                           .body = R"({"message":"declared scheme rejected"})"};
        });
    const auto denied = azure::helper_get(
        "https://dev.azure.com/fabrikam/project/_apis/git/repositories/repo/"
        "pullrequests?api-version=7.1",
        {.headers = api_headers(forge_kind::azure, declared->access)}, declared->access);
    check(denied.status == 401 && calls == 1 && fills == 4,
          "azure helper: explicit scheme401 never retries, rejects, approves or re-fills");

    // Additional fault-injection REST responses retain both rejected scheme contexts.
    // https://learn.microsoft.com/en-us/azure/devops/integrate/how-to/call-rest-api?view=azure-devops
    output = "username=helper-user\npassword=both-denied-fixture\n\n";
    auto both = load_token(*account);
    calls = 0;
    forge_detail::set_transport(
        [&](std::string_view, const std::string&, std::string_view, const rap::sys::http_options&)
        {
            ++calls;
            return rap::sys::http_response{
                .status = 401,
                .body = std::string("{\"message\":") +
                        json::quoted((calls == 1 ? "first rejection " : "second rejection ") +
                                     std::string("both-denied-fixture")) +
                        "}"};
        });
    const auto api_url = "https://dev.azure.com/fabrikam/project/_apis/git/repositories/repo/"
                         "pullrequests?api-version=7.1";
    const auto double_denied = azure::helper_get(
        api_url, {.headers = api_headers(forge_kind::azure, both->access)}, both->access);
    const auto diagnostic = azure::response_error(double_denied, api_url, both->access);
    check(calls == 2 && diagnostic.find("Git credential invalid") != std::string::npos &&
              diagnostic.find("first rejection") != std::string::npos &&
              diagnostic.find("second rejection") != std::string::npos &&
              diagnostic.find("both-denied-fixture") == std::string::npos,
          "azure helper: failed negotiation retains both sanitized contexts and never exposes the "
          "opaque password");
    calls = 0;
    forge_detail::set_transport(
        [&](std::string_view, const std::string&, std::string_view, const rap::sys::http_options&)
        {
            ++calls;
            return rap::sys::http_response{.status = 203,
                                           .body = R"({"message":"ordinary JSON203"})"};
        });
    azure::helper_get(api_url, {.headers = api_headers(forge_kind::azure, both->access)},
                      both->access);
    check(calls == 1, "azure helper: JSON203 never negotiates");
    calls = 0;
    forge_detail::set_transport(
        [&](std::string_view, const std::string&, std::string_view, const rap::sys::http_options&)
        {
            ++calls;
            return calls == 1
                       ? rap::sys::http_response{.status = 203, .body = "<html>Sign in</html>"}
                       : rap::sys::http_response{.status = 200, .body = R"({"value":[]})"};
        });
    const auto html_retry = azure::helper_get(
        api_url, {.headers = api_headers(forge_kind::azure, both->access)}, both->access);
    check(calls == 2 && html_retry.status == 200,
          "azure helper: HTML203 permits exactly one documented Bearer compatibility retry");

    // Identical credential bytes in two orgs must not alias a gate binding.
    output = "username=helper-user\npassword=shared-fixture-only\n\n";
    auto first_org = load_token(*account);
    auto other_account = *account;
    other_account.host = "dev.azure.com/other";
    auto other_org = load_token(other_account);
    calls = 0;
    forge_detail::set_transport(
        [&](std::string_view, const std::string&, std::string_view,
            const rap::sys::http_options& options)
        {
            ++calls;
            check(std::ranges::none_of(options.headers, [](const auto& h)
                                       { return h.first == azure::credential_context_header; }),
                  "azure helper: per-context scope marker is removed from every outgoing request");
            return rap::sys::http_response{.status = 200, .body = R"({"value":[]})"};
        });
    const auto good_first = forge_detail::http_get(
        api_url, {.headers = api_headers(forge_kind::azure, first_org->access)});
    const auto other_url = "https://dev.azure.com/other/project/_apis/git/repositories/repo/"
                           "pullrequests?api-version=7.1";
    const auto good_other = forge_detail::http_get(
        other_url, {.headers = api_headers(forge_kind::azure, other_org->access)});
    const auto wrong_org = forge_detail::http_get(
        other_url, {.headers = api_headers(forge_kind::azure, first_org->access)});
    check(good_first.status == 200 && good_other.status == 200 && !wrong_org.status && calls == 2,
          "azure helper: same credential bytes remain isolated by requested organization/context");

    credential_helper::set_runner([](const credential_helper::request&)
                                      -> std::expected<credential_helper::reply, std::string>
                                  { return credential_helper::reply{.timed_out = true}; });
    std::string why;
    check(!load_token(*account, &why) && why.find("timed out") != std::string::npos,
          "azure helper: timeout has bounded nonsecret PAT fallback diagnostic");
    credential_helper::set_runner([](const credential_helper::request&)
                                      -> std::expected<credential_helper::reply, std::string>
                                  { return credential_helper::reply{.exceeded_limit = true}; });
    check(!load_token(*account, &why) && why.find("limit") != std::string::npos,
          "azure helper: excessive output is discarded without leaking its content");
    credential_helper::set_runner({});
    forge_detail::set_transport({});
}
}  // namespace tortilla::test
