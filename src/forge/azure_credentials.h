#pragma once

#include "forge/credential_helper.h"
#include "forge/forge.h"

namespace tortilla::azure
{
struct helper_credential
{
    std::string username, password;
    std::string authtype, credential;
    bool explicit_scheme = false;
};
inline constexpr std::string_view credential_context_header = "X-Tortilla-Credential-Context";
std::optional<forge_account> account_for_remote(std::string_view remote, std::string_view directory,
                                                const std::vector<forge_account>&);
std::optional<forge_account> automatic_account(std::string_view remote, std::string_view directory);
std::expected<credential_helper::request, std::string> credential_request(const forge_account&);
std::expected<helper_credential, std::string> parse_credential(std::string_view,
                                                               std::string_view organization);
std::expected<forge_token, std::string> helper_token(const forge_account&);
// Internal bridge for the existing string-token APIs. Actual helper secrets
// have a typed, worker-owned RAII lifetime; this registry contains weak refs.
bool is_helper_token(std::string_view);
std::string helper_preflight(std::string_view);
std::optional<std::string> helper_authorization(std::string_view);
std::optional<std::string> helper_key_for_headers(const rap::sys::http_headers&);
std::vector<std::string> helper_secrets(std::string_view);
rap::sys::http_response helper_get(const std::string& url, const rap::sys::http_options&,
                                   std::string_view token);
}  // namespace tortilla::azure
