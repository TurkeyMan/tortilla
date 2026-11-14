// forge.cpp's own helpers, for the other files that talk to the forges
// (ci.cpp, pr_details.cpp).
#pragma once

#include "forge/forge.h"
#include "util/json.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace tortilla::forge_detail
{

// Labels from REST arrays or GitHub GraphQL nodes; malformed colours stay absent.
std::vector<pr_label> decode_labels(const json::value&);

// Why a request failed, in words: no answer, or the forge's message.
std::string failure(const rap::sys::http_response&, const std::optional<json::value>&);
// GitHub's GraphQL endpoint on that host.
std::string graphql_url(std::string_view host);
// The repository's REST root: /repos/owner/name, GitLab's /projects/<path>.
std::string repo_api(forge_kind, const forge_repo&);
// A time member (a string without escapes); 0 for null, or none.
std::int64_t time_of(const json::value&);
// GitHub's rollup and status words; Gitea's and GitLab's status words.
check_state github_state(std::string_view);
check_state status_word(std::string_view);
// GitHub REST check/workflow runs. The PR list leaves unknown conclusions blank;
// the CI panel and fast poll historically show them as failed.
check_state github_rest_state(const json::value& run, check_state unknown_conclusion);
// A check run or status context from GitHub's GraphQL, as in the PR list.
check_run github_run(const json::value&);
// What an anonymous GitHub reply says is left of the hour's allowance
// (anonymous_budget).
void note_budget(const rap::sys::http_response&);
// `s` escaped for a URL's query or path segment.
std::string url_escape(std::string_view s);

// -- every request to a forge goes through these ------------------------------

// A token is tied to the web origin it was given for (load_token: its
// account's host; who_am_i: the host it's tried on). A request carrying an
// Authorization header is refused, unsent, unless its URL is that origin
// (or github.com's API host, api.github.com): a token for forge.example
// (port 443) never reaches forge.example:8443.
void bind_token(std::string_view token, std::string_view host);
// Whether `token` may go to `url`.
bool token_allowed(std::string_view token, std::string_view url);

// rap::sys's requests, behind that check.
rap::sys::http_response http_request(std::string_view method, const std::string& url,
                                     std::string_view body = {},
                                     const rap::sys::http_options& = {});
rap::sys::http_response http_get(const std::string& url, const rap::sys::http_options& = {});
rap::sys::http_response http_download(const std::string& url, const std::filesystem::path& to,
                                      const rap::sys::http_options& = {});

// For the tests: what answers in place of the network (a download's body is
// what's written), or none to put the network back.
using transport =
    std::function<rap::sys::http_response(std::string_view method, const std::string& url,
                                          std::string_view body, const rap::sys::http_options&)>;
void set_transport(transport);

}  // namespace tortilla::forge_detail
