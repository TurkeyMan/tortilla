// forge.cpp's own helpers, for the other files that talk to the forges
// (ci.cpp, pr_details.cpp).
#pragma once

#include "forge/forge.h"
#include "util/json.h"

#include <optional>
#include <string>
#include <string_view>

namespace tortilla::forge_detail
{

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
// A check run or status context from GitHub's GraphQL, as in the PR list.
check_run github_run(const json::value&);
// What an anonymous GitHub reply says is left of the hour's allowance
// (anonymous_budget).
void note_budget(const rap::sys::http_response&);
// `s` escaped for a URL's query or path segment.
std::string url_escape(std::string_view s);

}  // namespace tortilla::forge_detail
