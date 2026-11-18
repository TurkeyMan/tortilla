#pragma once

#include "forge/ci.h"
#include "util/json.h"

namespace tortilla::azure
{
// Azure accounts are scoped to an organization, not the shared API hostname.
std::string repository_web(const forge_repo&);
std::string account_host(const forge_repo&);
std::string project_api(const forge_repo&);
std::string repository_api(const forge_repo&);
bool scope_allows(std::string_view scope, std::string_view url);
std::string basic_token(std::string_view);
std::optional<forge_repo> remote(std::string_view host, std::string_view path);
std::expected<forge_account, std::string> identity(std::string_view host, std::string_view token);
std::expected<pull_requests_reply, std::string> pulls(const forge_repo&, std::string_view token,
                                                      bool tips);
std::expected<pull_request, std::string> details(const forge_repo&, pull_request,
                                                 std::string_view token);
std::expected<commit_ci, std::string> checks(const forge_repo&, std::string_view sha,
                                             std::string_view token);
std::expected<std::vector<ci_artifact>, std::string> artifacts(const forge_repo&, const commit_ci&,
                                                               std::string_view token);
std::expected<job_failure, std::string> failure_details(const forge_repo&, const ci_job&,
                                                        std::string_view token);
std::string unsupported();
}  // namespace tortilla::azure
