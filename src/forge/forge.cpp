#include "forge.h"

#include "i18n.h"
#include "settings.h"
#include "util/json.h"

#include <algorithm>
#include <cctype>
#include <thread>

namespace tortilla
{
namespace
{

// Tortilla's applications, registered on github.com and gitlab.com for the
// device flow. Public by design (a device-flow client has no secret); empty
// until registered, and then only token pasting is offered.
constexpr std::string_view k_github_client_id = "Ov23liL6L4nZYOzFMVRj";
constexpr std::string_view k_gitlab_client_id = "";

constexpr const char* k_accounts_setting = "forge.accounts";

std::string_view kind_id(forge_kind k)
{
    switch (k)
    {
    case forge_kind::github: return "github";
    case forge_kind::gitlab: return "gitlab";
    case forge_kind::gitea: return "gitea";
    }
    return "github";
}

std::optional<forge_kind> kind_of(std::string_view id)
{
    for (forge_kind k : {forge_kind::github, forge_kind::gitlab, forge_kind::gitea})
        if (kind_id(k) == id) return k;
    return std::nullopt;
}

std::string secret_name(const forge_account& a)
{
    return "tortilla:" + a.host + ":" + a.login;
}

// The next field of `s` up to `sep`, consumed.
std::string_view take(std::string_view& s, char sep)
{
    const size_t at = s.find(sep);
    const std::string_view field = s.substr(0, at);
    s.remove_prefix(at == std::string_view::npos ? s.size() : at + 1);
    return field;
}

// application/x-www-form-urlencoded.
std::string form_escape(std::string_view s)
{
    constexpr char digits[] = "0123456789ABCDEF";
    std::string out;
    for (const char c : s)
    {
        const auto u = static_cast<unsigned char>(c);
        if ((u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~')
            out += c;
        else
        {
            out += '%';
            out += digits[u >> 4];
            out += digits[u & 15];
        }
    }
    return out;
}

std::string form(std::initializer_list<std::pair<std::string_view, std::string_view>> fields)
{
    std::string out;
    for (const auto& [name, value] : fields)
        out += (out.empty() ? "" : "&") + form_escape(name) + "=" + form_escape(value);
    return out;
}

std::int64_t now_seconds()
{
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string_view client_id(forge_kind k)
{
    return k == forge_kind::github ? k_github_client_id : k_gitlab_client_id;
}

// The web host's root: https://host.
std::string web_root(std::string_view host)
{
    return "https://" + std::string(host);
}

// A POST of a form to the forge's OAuth endpoints, which answer in JSON.
// (The reply's JSON is parsed where the reply is kept: a value views it.)
rap::sys::http_response oauth_post(const std::string& url, const std::string& body)
{
    return rap::sys::http_request(
        "POST", url, body,
        {.headers = {{"Accept", "application/json"},
                     {"Content-Type", "application/x-www-form-urlencoded"},
                     {"User-Agent", "Tortilla"}}});
}

// Why a request failed, in words: no answer, or the forge's message.
std::string failure(const rap::sys::http_response& r, const std::optional<json::value>& j)
{
    if (r.status == 0) return rap::i18n::format(_("No answer from the server: {0}."), r.error);
    std::string message;
    if (j)
    {
        message = (*j)["message"].str();
        if (message.empty()) message = (*j)["error_description"].str();
        if (message.empty()) message = (*j)["error"].str();
    }
    if (message.empty())
        return rap::i18n::format(_("The server answered {0}."), std::to_string(r.status));
    return rap::i18n::format(_("The server answered {0}: {1}"), std::to_string(r.status), message);
}

std::expected<forge_token, std::string> token_from(const json::value& j)
{
    forge_token t{.access = j["access_token"].str(), .refresh = j["refresh_token"].str()};
    if (t.access.empty()) return std::unexpected(std::string(_("The server gave no token.")));
    if (const auto in = j["expires_in"].integer(); in && *in > 0)
        t.expires_at = now_seconds() + *in;
    return t;
}

}  // namespace

std::string forge_name(forge_kind k)
{
    switch (k)
    {
    case forge_kind::github: return "GitHub";
    case forge_kind::gitlab: return "GitLab";
    case forge_kind::gitea: return "Gitea / Forgejo";
    }
    return {};
}

std::string_view default_host(forge_kind k)
{
    switch (k)
    {
    case forge_kind::github: return "github.com";
    case forge_kind::gitlab: return "gitlab.com";
    case forge_kind::gitea: return "codeberg.org";
    }
    return {};
}

// -- accounts -----------------------------------------------------------------

// One account a tab apart: "<kind> <host> <login> <avatar or -> <name>",
// the name last, as the one field that may hold spaces.
std::vector<forge_account> saved_accounts()
{
    std::vector<forge_account> out;
    const std::string saved = settings::get().value(k_accounts_setting);
    std::string_view rest = saved;
    while (!rest.empty())
    {
        std::string_view record = take(rest, '\t');
        const auto kind = kind_of(take(record, ' '));
        forge_account a{.host = std::string(take(record, ' ')),
                        .login = std::string(take(record, ' '))};
        const std::string_view avatar = take(record, ' ');
        if (!kind || a.host.empty() || a.login.empty()) continue;
        a.kind = *kind;
        if (avatar != "-") a.avatar_url = avatar;
        a.name = record;
        out.push_back(std::move(a));
    }
    return out;
}

void save_accounts(const std::vector<forge_account>& accounts)
{
    std::string saved;
    for (const forge_account& a : accounts)
    {
        std::string name = a.name;
        for (char& c : name)
            if (c == '\t' || c == '\n' || c == '\r') c = ' ';
        if (!saved.empty()) saved += '\t';
        saved += std::string(kind_id(a.kind)) + ' ' + a.host + ' ' + a.login + ' ' +
                 (a.avatar_url.empty() ? "-" : a.avatar_url) + ' ' + name;
    }
    if (saved.empty())
        settings::get().remove(k_accounts_setting);
    else
        settings::get().set(k_accounts_setting, saved);
}

std::optional<forge_account> account_for_url(std::string_view url,
                                             const std::vector<forge_account>& accounts)
{
    // scheme://[user@]host[:port]/..., or scp-style [user@]host:path.
    std::string_view host = url;
    if (const size_t scheme = host.find("://"); scheme != std::string_view::npos)
    {
        host.remove_prefix(scheme + 3);
        host = host.substr(0, host.find('/'));
    }
    else
        host = host.substr(0, host.find(':'));
    if (const size_t at = host.rfind('@'); at != std::string_view::npos) host.remove_prefix(at + 1);
    host = host.substr(0, host.find(':'));
    for (const forge_account& a : accounts)
    {
        const std::string_view own = std::string_view(a.host).substr(0, a.host.find(':'));
        if (host.size() == own.size() &&
            std::equal(host.begin(), host.end(), own.begin(),
                       [](char x, char y) { return std::tolower(x) == std::tolower(y); }))
            return a;
    }
    return std::nullopt;
}

// The keychain holds the token as JSON: {"access":..,"refresh":..,"expires_at":..}.
bool store_token(const forge_account& a, const forge_token& t)
{
    const std::string blob = "{\"access\":" + json::quoted(t.access) +
                             ",\"refresh\":" + json::quoted(t.refresh) +
                             ",\"expires_at\":" + std::to_string(t.expires_at) + "}";
    return rap::sys::store_secret(secret_name(a), blob,
                                  rap::i18n::format(_("Tortilla: {0} on {1}"), a.login, a.host));
}

std::optional<forge_token> load_token(const forge_account& a, std::string* why)
{
    const auto blob = rap::sys::load_secret(secret_name(a));
    if (why) *why = rap::sys::secret_error();
    if (!blob) return std::nullopt;
    const auto j = json::parse(*blob);
    if (!j) return std::nullopt;
    forge_token t{.access = (*j)["access"].str(), .refresh = (*j)["refresh"].str()};
    t.expires_at = (*j)["expires_at"].integer().value_or(0);
    if (t.access.empty()) return std::nullopt;
    return t;
}

bool erase_token(const forge_account& a)
{
    return rap::sys::erase_secret(secret_name(a));
}

// -- talking to it ------------------------------------------------------------

std::string api_root(forge_kind k, std::string_view host)
{
    switch (k)
    {
    case forge_kind::github:
        return host == "github.com" ? "https://api.github.com" : web_root(host) + "/api/v3";
    case forge_kind::gitlab: return web_root(host) + "/api/v4";
    case forge_kind::gitea: return web_root(host) + "/api/v1";
    }
    return {};
}

rap::sys::http_headers api_headers(forge_kind k, std::string_view token)
{
    rap::sys::http_headers h{{"User-Agent", "Tortilla"}};
    switch (k)
    {
    case forge_kind::github:
        h.emplace_back("Accept", "application/vnd.github+json");
        h.emplace_back("X-GitHub-Api-Version", "2022-11-28");
        h.emplace_back("Authorization", "Bearer " + std::string(token));
        break;
    case forge_kind::gitlab:
        h.emplace_back("Accept", "application/json");
        h.emplace_back("Authorization", "Bearer " + std::string(token));
        break;
    case forge_kind::gitea:
        h.emplace_back("Accept", "application/json");
        h.emplace_back("Authorization", "token " + std::string(token));
        break;
    }
    return h;
}

std::expected<forge_account, std::string> who_am_i(forge_kind k, std::string_view host,
                                                   std::string_view token)
{
    const auto r =
        rap::sys::http_get(api_root(k, host) + "/user", {.headers = api_headers(k, token)});
    const auto j = json::parse(r.body);
    if (r.status == 401)
        return std::unexpected(std::string(
            _("The server refused the token: it may be mistyped, expired or revoked.")));
    if (r.status != 200 || !j) return std::unexpected(failure(r, j));
    // GitLab calls the login its username, Gitea the display name its full name.
    forge_account a{.kind = k,
                    .host = std::string(host),
                    .login = (*j)[k == forge_kind::gitlab ? "username" : "login"].str(),
                    .name = (*j)[k == forge_kind::gitea ? "full_name" : "name"].str(),
                    .avatar_url = (*j)["avatar_url"].str()};
    if (a.login.empty())
        return std::unexpected(std::string(_("The server didn't say who the token is for.")));
    return a;
}

// -- signing in through the browser -------------------------------------------

bool browser_sign_in(forge_kind k, std::string_view host)
{
    if (k == forge_kind::github) return host == "github.com" && !k_github_client_id.empty();
    if (k == forge_kind::gitlab) return host == "gitlab.com" && !k_gitlab_client_id.empty();
    return false;
}

std::string token_page(forge_kind k, std::string_view host)
{
    switch (k)
    {
    case forge_kind::github:
        return web_root(host) + "/settings/tokens/new?scopes=repo&description=Tortilla";
    case forge_kind::gitlab:
        return web_root(host) + "/-/user_settings/personal_access_tokens?name=Tortilla&scopes=api";
    case forge_kind::gitea: return web_root(host) + "/user/settings/applications";
    }
    return {};
}

std::string token_scopes(forge_kind k)
{
    switch (k)
    {
    case forge_kind::github: return _("A classic token with the “repo” scope.");
    case forge_kind::gitlab: return _("A personal access token with the “api” scope.");
    case forge_kind::gitea:
        return _("A token with read and write access to repositories and issues, and read access "
                 "to the user.");
    }
    return {};
}

std::expected<device_code, std::string> start_sign_in(forge_kind k, std::string_view host)
{
    if (!browser_sign_in(k, host))
        return std::unexpected(
            std::string(_("Signing in through the browser isn't available here.")));
    const std::string url = k == forge_kind::github ? web_root(host) + "/login/device/code"
                                                    : web_root(host) + "/oauth/authorize_device";
    const auto r = oauth_post(url, form({{"client_id", client_id(k)},
                                         {"scope", k == forge_kind::github ? "repo" : "api"}}));
    const auto j = json::parse(r.body);
    if (r.status != 200 || !j) return std::unexpected(failure(r, j));
    const json::value& reply = *j;
    device_code c{.device = reply["device_code"].str(),
                  .user = reply["user_code"].str(),
                  .verification = reply["verification_uri"].str(),
                  .verification_full = reply["verification_uri_complete"].str()};
    if (c.device.empty() || c.user.empty() || c.verification.empty())
        return std::unexpected(failure(r, j));
    c.interval =
        std::chrono::seconds(std::max<std::int64_t>(reply["interval"].integer().value_or(5), 1));
    c.expires = std::chrono::steady_clock::now() +
                std::chrono::seconds(reply["expires_in"].integer().value_or(900));
    return c;
}

std::expected<forge_token, std::string> finish_sign_in(forge_kind k, std::string_view host,
                                                       const device_code& code,
                                                       const std::atomic<bool>& cancelled)
{
    const std::string url = k == forge_kind::github ? web_root(host) + "/login/oauth/access_token"
                                                    : web_root(host) + "/oauth/token";
    const std::string body = form({{"client_id", client_id(k)},
                                   {"device_code", code.device},
                                   {"grant_type", "urn:ietf:params:oauth:grant-type:device_code"}});
    std::chrono::seconds interval = code.interval;
    for (;;)
    {
        // A wait in small steps, so a cancel isn't kept waiting.
        const auto until = std::chrono::steady_clock::now() + interval;
        while (std::chrono::steady_clock::now() < until)
        {
            if (cancelled) return std::unexpected(std::string(_("Cancelled.")));
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (std::chrono::steady_clock::now() >= code.expires)
            return std::unexpected(std::string(_("The code expired before it was entered.")));
        const auto r = oauth_post(url, body);
        const auto j = json::parse(r.body);
        if (!j)
        {
            if (r.status == 0) continue;  // a dropped connection: ask again
            return std::unexpected(failure(r, j));
        }
        // GitHub answers 200 with an error; GitLab 400.
        const json::value error = (*j)["error"];
        if (!error) return token_from(*j);
        if (error.str_is("authorization_pending")) continue;
        if (error.str_is("slow_down"))
        {
            interval =
                std::chrono::seconds((*j)["interval"].integer().value_or(interval.count() + 5));
            continue;
        }
        if (error.str_is("expired_token"))
            return std::unexpected(std::string(_("The code expired before it was entered.")));
        if (error.str_is("access_denied"))
            return std::unexpected(std::string(_("Signing in was refused in the browser.")));
        return std::unexpected(failure(r, j));
    }
}

std::expected<forge_token, std::string> renew(forge_kind k, std::string_view host,
                                              const forge_token& t)
{
    if (t.refresh.empty() || k != forge_kind::gitlab)
        return std::unexpected(std::string(_("This sign-in can't be renewed: sign in again.")));
    const auto r =
        oauth_post(web_root(host) + "/oauth/token", form({{"client_id", client_id(k)},
                                                          {"grant_type", "refresh_token"},
                                                          {"refresh_token", t.refresh}}));
    const auto j = json::parse(r.body);
    if (r.status != 200 || !j) return std::unexpected(failure(r, j));
    return token_from(*j);
}

}  // namespace tortilla
