#include "azure_credentials.h"

#include "forge/azure.h"
#include "forge/forge_detail.h"
#include "util/sha256.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <ctime>
#include <map>
#include <mutex>
#include <set>

namespace tortilla::azure
{
namespace
{
bool field(std::string_view value)
{
    return value.size() <= 1024 &&
           std::ranges::none_of(value,
                                [](unsigned char c) { return c == '\r' || c == '\n' || c == 0; });
}

std::string decode_username(std::string_view input)
{
    std::string out;
    for (size_t i = 0; i < input.size(); ++i)
    {
        if (input[i] == '%')
        {
            if (i + 2 >= input.size()) return {};
            unsigned value = 0;
            const auto [end, error] =
                std::from_chars(input.data() + i + 1, input.data() + i + 3, value, 16);
            if (error != std::errc{} || end != input.data() + i + 3 || value == 0) return {};
            out += char(value);
            i += 2;
        }
        else
            out += input[i];
    }
    return field(out) ? out : std::string();
}

struct context
{
    std::mutex mutex;
    std::string key, scope, fingerprint;
    helper_credential value;
    bool bearer = false, resolved = false;
    ~context() { forge_detail::forget_token_binding(key); }
};
std::mutex contexts_mutex;
std::map<std::string, std::weak_ptr<context>, std::less<>> contexts;
std::atomic<std::uint64_t> serial{0};
struct accepted_scheme
{
    bool bearer;
    std::chrono::steady_clock::time_point at;
};
// Nonsecret fingerprint -> accepted mode; bounded and never serialized.
std::map<std::string, accepted_scheme> schemes;
void accepted(const std::shared_ptr<context>& c, bool bearer)
{
    {
        std::lock_guard lock(c->mutex);
        c->bearer = bearer;
        c->resolved = true;
    }
    std::lock_guard lock(contexts_mutex);
    if (schemes.size() >= 128) schemes.erase(schemes.begin());
    schemes[c->fingerprint] = {bearer, std::chrono::steady_clock::now()};
}

std::shared_ptr<context> find(std::string_view key)
{
    std::lock_guard lock(contexts_mutex);
    const auto it = contexts.find(key);
    return it == contexts.end() ? nullptr : it->second.lock();
}

std::string basic(const helper_credential& c)
{
    // Keep the exact username/password pair supplied by the helper.
    return "Basic " + basic_pair(c.username, c.password);
}

std::string header(const context& c)
{
    if (c.value.explicit_scheme) return c.value.authtype + " " + c.value.credential;
    return c.bearer ? "Bearer " + c.value.password : basic(c.value);
}

bool challenge(const rap::sys::http_response& reply)
{
    if (reply.status == 401) return true;
    if (reply.status != 203 || json::parse(reply.body)) return false;
    auto text = reply.body.substr(0, 8192);
    std::ranges::transform(text, text.begin(),
                           [](unsigned char c) { return char(std::tolower(c)); });
    if (text.find("<html") != std::string::npos || text.find("<!doctype html") != std::string::npos)
        return true;
    for (auto [name, value] : reply.headers)
    {
        std::ranges::transform(name, name.begin(),
                               [](unsigned char c) { return char(std::tolower(c)); });
        std::ranges::transform(value, value.begin(),
                               [](unsigned char c) { return char(std::tolower(c)); });
        if (name == "content-type" && value.find("text/html") != std::string::npos) return true;
    }
    return false;
}
}  // namespace

std::optional<forge_account> account_for_remote(std::string_view remote, std::string_view directory,
                                                const std::vector<forge_account>& accounts)
{
    if (const auto configured = account_for_url(remote, accounts)) return configured;
    return automatic_account(remote, directory);
}

std::optional<forge_account> automatic_account(std::string_view remote, std::string_view directory)
{
    // HTTPS is primary. Do not consult helpers for SSH transports.
    if (!remote.starts_with("https://")) return {};
    const auto repo = repo_of_url(remote);
    if (!repo || repo->host != "dev.azure.com") return {};
    std::string username;
    const auto authority = remote.substr(8, remote.find('/', 8) - 8);
    if (const auto at = authority.rfind('@'); at != std::string_view::npos)
    {
        const auto encoded = authority.substr(0, at);
        if (encoded.find(':') != std::string_view::npos) return {};  // Never copy a URL password.
        username = decode_username(encoded);
        if (username.empty() && !encoded.empty()) return {};
    }
    return forge_account{.kind = forge_kind::azure,
                         .host = account_host(*repo),
                         .login = username.empty() ? "Git credentials" : username,
                         .name = "Git credential helper",
                         .via_git_credential = true,
                         .credential_username = username,
                         .credential_directory = std::string(directory)};
}

std::expected<credential_helper::request, std::string>
credential_request(const forge_account& account)
{
    if (account.kind != forge_kind::azure || !account.via_git_credential ||
        !account.host.starts_with("dev.azure.com/") || !field(account.credential_username) ||
        account.credential_username.find(':') != std::string::npos)
        return std::unexpected("Invalid Azure Git credential account scope.");
    const std::string organization = account.host.substr(std::string("dev.azure.com/").size());
    if (organization.empty() || organization.find_first_not_of(
                                    "abcdefghijklmnopqrstuvwxyz0123456789-") != std::string::npos)
        return std::unexpected("Invalid Azure Git credential organization.");
    credential_helper::request r;
    r.directory = account.credential_directory;
    // https://git-scm.com/docs/gitcredentials (useHttpPath and askpass order)
    // https://github.com/git-ecosystem/git-credential-manager/blob/main/docs/configuration.md
    r.arguments = {"-C",         r.directory,
                   "-c",         "core.askPass=",
                   "-c",         "credential.useHttpPath=true",
                   "-c",         "credential.https://dev.azure.com.useHttpPath=true",
                   "-c",         "credential.interactive=false",
                   "-c",         "credential.guiPrompt=false",
                   "-c",         "credential.trace=false",
                   "-c",         "credential.traceSecrets=false",
                   "credential", "fill"};
    r.arguments.insert(r.arguments.end() - 2,
                       {"-c", "credential.https://" + account.host + ".useHttpPath=true"});
    r.environment = {
        {"GCM_INTERACTIVE", "never"}, {"GCM_GUI_PROMPT", "0"},  {"GIT_TERMINAL_PROMPT", "0"},
        {"GIT_ASKPASS", ""},          {"SSH_ASKPASS", ""},      {"GCM_TRACE", "0"},
        {"GCM_TRACE_SECRETS", "0"},   {"GIT_TRACE", "0"},       {"GIT_TRACE2", "0"},
        {"GIT_TRACE2_EVENT", "0"},    {"GIT_TRACE2_PERF", "0"}, {"GIT_TRACE_CURL", "0"},
        {"GIT_CURL_VERBOSE", "0"},    {"GCM_DEBUG", "0"}};
    // https://git-scm.com/docs/git-credential (capability must precede dependent fields)
    r.input =
        "capability[]=authtype\nprotocol=https\nhost=dev.azure.com\npath=" + organization + "\n";
    if (!account.credential_username.empty())
        r.input += "username=" + account.credential_username + "\n";
    r.input += '\n';
    return r;
}

std::expected<helper_credential, std::string> parse_credential(std::string_view output,
                                                               std::string_view organization)
{
    // https://git-scm.com/docs/git-credential#_input_output_format
    if (output.size() > 65536 || output.find('\0') != std::string_view::npos)
        return std::unexpected("Invalid Git credential response.");
    std::map<std::string, std::string> values;
    bool capability = false;
    for (auto rest = output; !rest.empty();)
    {
        const auto end = rest.find('\n');
        auto line = rest.substr(0, end);
        rest.remove_prefix(end == std::string_view::npos ? rest.size() : end + 1);
        if (line.ends_with('\r')) line.remove_suffix(1);
        if (line.empty()) break;
        const auto equal = line.find('=');
        if (equal == std::string_view::npos || line.find('\r') != std::string_view::npos)
            return std::unexpected("Invalid Git credential response fields.");
        const auto key = line.substr(0, equal), value = line.substr(equal + 1);
        if (key == "capability[]")
        {
            capability |= value == "authtype";
            continue;
        }
        if ((key == "authtype" || key == "credential") && !capability)
            return std::unexpected("Git credential scheme appeared before its capability.");
        if (!values.emplace(std::string(key), std::string(value)).second)
            return std::unexpected("Duplicate Git credential response field.");
    }
    for (const auto& [key, expected] : std::vector<std::pair<std::string, std::string>>{
             {"protocol", "https"}, {"host", "dev.azure.com"}, {"path", std::string(organization)}})
        if (const auto it = values.find(key); it != values.end() && it->second != expected)
            return std::unexpected(
                "Git credentials did not match the requested Azure organization.");
    if (values.contains("url") || values["quit"] == "1" || values["continue"] == "1")
        return std::unexpected(
            "Git credential lookup did not provide a complete scoped credential.");
    if (const auto it = values.find("password_expiry_utc"); it != values.end())
    {
        std::int64_t expiry = 0;
        const auto [end, error] =
            std::from_chars(it->second.data(), it->second.data() + it->second.size(), expiry);
        if (error != std::errc{} || end != it->second.data() + it->second.size() ||
            expiry <= std::time(nullptr))
            return std::unexpected("Git credentials have expired.");
    }
    helper_credential result;
    if (values.contains("authtype") || values.contains("credential"))
    {
        if (!capability || !values.contains("authtype") || values["credential"].empty())
            return std::unexpected("Incomplete Git credential authentication scheme.");
        auto scheme = values["authtype"];
        std::ranges::transform(scheme, scheme.begin(),
                               [](unsigned char c) { return char(std::tolower(c)); });
        if (scheme != "basic" && scheme != "bearer")
            return std::unexpected("Unsupported Git credential authentication scheme.");
        result.explicit_scheme = true;
        result.authtype = scheme == "basic" ? "Basic" : "Bearer";
        result.credential = values["credential"];
        if (std::ranges::any_of(result.credential,
                                [](unsigned char c) { return c <= 32 || c == 127; }))
            return std::unexpected("Invalid Git credential authorization value.");
    }
    else
    {
        if (!values.contains("username") || values["password"].empty() ||
            !field(values["username"]) || values["username"].find(':') != std::string::npos)
            return std::unexpected("Git credential helper returned no usable username/password; "
                                   "paste an organization PAT in Settings.");
        result.username = values["username"];
        result.password = values["password"];
    }
    return result;
}

std::expected<forge_token, std::string> helper_token(const forge_account& account)
{
    const auto request = credential_request(account);
    if (!request) return std::unexpected(request.error());
    const auto reply = credential_helper::run(*request);
    if (!reply)
        return std::unexpected(
            "Git credential lookup couldn't start; paste an organization PAT in Settings.");
    if (reply->timed_out)
        return std::unexpected(
            "Git credential lookup timed out; paste an organization PAT in Settings.");
    if (reply->exceeded_limit)
        return std::unexpected("Git credential helper output exceeded its safe limit.");
    if (reply->exit_code != 0)
        return std::unexpected("Git credentials are unavailable without interaction; paste an "
                               "organization PAT in Settings.");
    const auto credential =
        parse_credential(reply->output, account.host.substr(std::string("dev.azure.com/").size()));
    if (!credential) return std::unexpected(credential.error());
    auto owned = std::make_shared<context>();
    owned->key = "azure-helper-context-" + std::to_string(++serial);
    owned->scope = account.host;
    owned->value = *credential;
    owned->resolved = credential->explicit_scheme;
    owned->fingerprint =
        sha256_hex(json::quoted(account.host) + json::quoted(account.credential_directory) +
                   json::quoted(account.credential_username) + json::quoted(credential->username) +
                   json::quoted(credential->password) + json::quoted(credential->authtype) +
                   json::quoted(credential->credential));
    {
        std::lock_guard lock(contexts_mutex);
        const auto now = std::chrono::steady_clock::now();
        std::erase_if(schemes, [&](const auto& item)
                      { return now - item.second.at > std::chrono::minutes(10); });
        if (!credential->explicit_scheme)
            if (const auto it = schemes.find(owned->fingerprint); it != schemes.end())
                owned->bearer = it->second.bearer, owned->resolved = true;
        std::erase_if(contexts, [](const auto& item) { return item.second.expired(); });
        contexts[owned->key] = owned;
    }
    forge_detail::bind_token(owned->key, account.host);
    return forge_token{.access = owned->key, .lifetime = std::move(owned)};
}

bool is_helper_token(std::string_view key)
{
    return bool(find(key));
}
std::optional<std::string> helper_authorization(std::string_view key)
{
    const auto c = find(key);
    if (!c) return {};
    std::lock_guard lock(c->mutex);
    return header(*c);
}
std::optional<std::string> helper_key_for_headers(const rap::sys::http_headers& headers)
{
    std::string key, authorization;
    for (const auto& [name, value] : headers)
    {
        if (name == credential_context_header) key = value;
        if (name == "Authorization") authorization = value;
    }
    const auto c = find(key);
    if (!c) return {};
    std::lock_guard lock(c->mutex);
    if (authorization == header(*c) ||
        (!c->value.explicit_scheme &&
         (authorization == basic(c->value) || authorization == "Bearer " + c->value.password)))
        return key;
    return {};
}
std::vector<std::string> helper_secrets(std::string_view key)
{
    const auto c = find(key);
    if (!c) return {};
    std::lock_guard lock(c->mutex);
    std::vector<std::string> values{c->value.password, c->value.credential, basic(c->value),
                                    basic_pair(c->value.username, c->value.password),
                                    "Bearer " + c->value.password};
    const auto count = values.size();
    for (size_t i = 0; i < count; ++i)
        if (!values[i].empty()) values.push_back(forge_detail::url_escape(values[i]));
    return values;
}

std::string helper_preflight(std::string_view key)
{
    const auto c = find(key);
    if (!c) return {};
    {
        std::lock_guard lock(c->mutex);
        if (c->resolved) return {};
    }
    // https://github.com/microsoft/azure-devops-node-api/blob/master/api/WebApi.ts (WebApi.connect)
    const auto url = "https://" + c->scope + "/_apis/connectionData";
    const auto reply = helper_get(url, {.headers = api_headers(forge_kind::azure, key)}, key);
    return reply.status == 200 && reply.error.empty() && json::parse(reply.body)
               ? std::string()
               : response_error(reply, url, key);
}

rap::sys::http_response helper_get(const std::string& url, const rap::sys::http_options& options,
                                   std::string_view token)
{
    auto first = forge_detail::http_get(url, options);
    const auto c = find(token);
    if (c && first.status == 200 && first.error.empty() && json::parse(first.body))
    {
        bool bearer;
        {
            std::lock_guard lock(c->mutex);
            bearer = c->bearer;
        }
        accepted(c, bearer);
    }
    if (!c || !challenge(first)) return first;
    {
        std::lock_guard lock(c->mutex);
        if (c->value.explicit_scheme || c->bearer) return first;
    }
    // Compatibility policy for ambiguous legacy output, not a claim that
    // OAuth-over-Basic works with REST. Both attempts are read-only GETs.
    // PAT Basic: https://learn.microsoft.com/en-us/azure/devops/organizations/accounts/use-personal-access-tokens-to-authenticate?view=azure-devops
    // OAuth Bearer: https://learn.microsoft.com/en-us/azure/devops/integrate/get-started/rest/samples?view=azure-devops
    auto retry = options;
    {
        std::lock_guard lock(c->mutex);
        for (auto& [name, value] : retry.headers)
            if (name == "Authorization") value = "Bearer " + c->value.password;
    }
    auto second = forge_detail::http_get(url, retry);
    if (second.status == 200 && second.error.empty() && json::parse(second.body))
    {
        accepted(c, true);
    }
    else
    {
        // Keep both service failures safely; never return raw helper output.
        second.error = response_error(first, url, token) +
                       "; Bearer compatibility attempt: " + response_error(second, url, token);
    }
    return second;
}
}  // namespace tortilla::azure
