#include "forge.h"

#include "i18n.h"
#include "settings.h"
#include "util/json.h"
#include "util/programs.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
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
// The GitHub CLI's sign-ins not to use: "<host> <login>", a tab apart.
constexpr const char* k_gh_off_setting = "forge.gh_off";

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

// One account a tab apart: "<kind>[+gh] <host> <login> <avatar or -> <name>",
// the name last, as the one field that may hold spaces.
std::vector<forge_account> saved_accounts()
{
    std::vector<forge_account> out;
    const std::string saved = settings::get().value(k_accounts_setting);
    std::string_view rest = saved;
    while (!rest.empty())
    {
        std::string_view record = take(rest, '\t');
        std::string_view kind_field = take(record, ' ');
        const auto kind = kind_of(take(kind_field, '+'));
        forge_account a{.host = std::string(take(record, ' ')),
                        .login = std::string(take(record, ' '))};
        const std::string_view avatar = take(record, ' ');
        if (!kind || a.host.empty() || a.login.empty()) continue;
        a.kind = *kind;
        a.via_gh = kind_field == "gh";
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
        saved += std::string(kind_id(a.kind)) + (a.via_gh ? "+gh " : " ") + a.host + ' ' + a.login +
                 ' ' + (a.avatar_url.empty() ? "-" : a.avatar_url) + ' ' + name;
    }
    if (saved.empty())
        settings::get().remove(k_accounts_setting);
    else
        settings::get().set(k_accounts_setting, saved);
}

std::vector<forge_account> accounts()
{
    std::vector<forge_account> out = saved_accounts();
    const std::string off = settings::get().value(k_gh_off_setting);
    for (forge_account& g : gh_accounts())
    {
        const std::string id = g.host + ' ' + g.login;
        bool skip = false;
        for (std::string_view rest = off; !rest.empty() && !skip;) skip = take(rest, '\t') == id;
        skip = skip || std::ranges::any_of(out, [&](const forge_account& a)
                                           { return a.host == g.host && a.login == g.login; });
        if (!skip) out.push_back(std::move(g));
    }
    return out;
}

void set_gh_account_off(const forge_account& a, bool off)
{
    const std::string id = a.host + ' ' + a.login;
    std::string kept;
    const std::string saved = settings::get().value(k_gh_off_setting);
    for (std::string_view rest = saved; !rest.empty();)
        if (const std::string_view entry = take(rest, '\t'); entry != id && !entry.empty())
            kept += (kept.empty() ? "" : "\t") + std::string(entry);
    if (off) kept += (kept.empty() ? "" : "\t") + id;
    if (kept.empty())
        settings::get().remove(k_gh_off_setting);
    else
        settings::get().set(k_gh_off_setting, kept);
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
    if (a.via_gh) return true;  // gh keeps it
    const std::string blob = "{\"access\":" + json::quoted(t.access) +
                             ",\"refresh\":" + json::quoted(t.refresh) +
                             ",\"expires_at\":" + std::to_string(t.expires_at) + "}";
    return rap::sys::store_secret(secret_name(a), blob,
                                  rap::i18n::format(_("Tortilla: {0} on {1}"), a.login, a.host));
}

std::optional<forge_token> load_token(const forge_account& a, std::string* why)
{
    if (a.via_gh)
    {
        auto token = gh_token(a.host);
        if (why) *why = token ? std::string() : token.error();
        if (!token) return std::nullopt;
        return forge_token{.access = std::move(*token)};
    }
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
    return a.via_gh || rap::sys::erase_secret(secret_name(a));  // gh's sign-in stays gh's
}

bool gh_installed()
{
    return !find_program("gh").empty();
}

// hosts.yml: each host a key at the left, its active user under it:
//   github.com:
//       users:
//           ada:
//       user: ada
std::vector<forge_account> gh_accounts()
{
    std::vector<forge_account> out;
    if (!gh_installed()) return out;
    std::filesystem::path dir;
    if (const char* own = std::getenv("GH_CONFIG_DIR"); own && *own)
        dir = rap::sys::path_from_utf8(own);
    else
#ifdef _WIN32
        dir = rap::sys::config_dir("") / "GitHub CLI";
#else
        dir = rap::sys::config_dir("") / "gh";
#endif
    std::ifstream in(dir / "hosts.yml", std::ios::binary);
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    std::string_view host;
    for (std::string_view rest = text; !rest.empty();)
    {
        std::string_view line = take(rest, '\n');
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
        if (line.empty() || line.front() == '#') continue;
        if (line.front() != ' ')
        {
            host = line.back() == ':' ? line.substr(0, line.size() - 1) : std::string_view();
            continue;
        }
        line.remove_prefix(std::min(line.find_first_not_of(' '), line.size()));
        if (host.empty() || !line.starts_with("user:")) continue;
        std::string_view login = line.substr(5);
        login.remove_prefix(std::min(login.find_first_not_of(' '), login.size()));
        if (login.size() >= 2 && (login.front() == '"' || login.front() == '\''))
            login = login.substr(1, login.size() - 2);
        if (login.empty()) continue;
        out.push_back({.kind = forge_kind::github,
                       .host = std::string(host),
                       .login = std::string(login),
                       .via_gh = true});
        host = {};  // one active user a host
    }
    return out;
}

std::expected<std::string, std::string> gh_token(std::string_view host)
{
    const std::vector<std::string> args{"auth", "token", "--hostname", std::string(host)};
    auto run = rap::sys::process::run("gh", args, {.merge_stderr = false});
    if (!run)
        return std::unexpected(
            rap::i18n::format(_("The GitHub CLI (gh) couldn't be run: {0}"), run.error()));
    std::string token = std::move(run->output);
    while (!token.empty() && std::isspace(static_cast<unsigned char>(token.back())))
        token.pop_back();
    if (run->exit_code != 0 || token.empty())
        return std::unexpected(rap::i18n::format(
            _("The GitHub CLI isn't signed in to {0}: run “gh auth login” first."), host));
    return token;
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

// -- repositories and their pull requests -------------------------------------

std::optional<forge_repo> repo_of_url(std::string_view url)
{
    // scheme://[user@]host[:port]/path, or scp-style [user@]host:path.
    std::string_view host, path;
    if (const size_t scheme = url.find("://"); scheme != std::string_view::npos)
    {
        std::string_view rest = url.substr(scheme + 3);
        const size_t slash = rest.find('/');
        if (slash == std::string_view::npos) return std::nullopt;
        host = rest.substr(0, slash);
        path = rest.substr(slash + 1);
        if (const size_t at = host.rfind('@'); at != std::string_view::npos)
            host.remove_prefix(at + 1);
        // An ssh port isn't the web's: ssh://git@host:2222/... is https://host/...
        if (url.starts_with("ssh://")) host = host.substr(0, host.find(':'));
    }
    else
    {
        const size_t colon = url.find(':');
        if (colon == std::string_view::npos) return std::nullopt;
        host = url.substr(0, colon);
        path = url.substr(colon + 1);
        if (const size_t at = host.rfind('@'); at != std::string_view::npos)
            host.remove_prefix(at + 1);
    }
    while (!path.empty() && path.back() == '/') path.remove_suffix(1);
    if (path.ends_with(".git")) path.remove_suffix(4);
    while (!path.empty() && path.front() == '/') path.remove_prefix(1);
    const size_t last = path.rfind('/');
    if (host.empty() || last == std::string_view::npos || last == 0 || last + 1 == path.size())
        return std::nullopt;
    return forge_repo{.host = std::string(host),
                      .owner = std::string(path.substr(0, last)),
                      .name = std::string(path.substr(last + 1))};
}

namespace
{

// GitLab names a project by its path, escaped: group%2Fsub%2Fname.
std::string gitlab_project(const forge_repo& r)
{
    return api_root(forge_kind::gitlab, r.host) + "/projects/" + form_escape(r.path());
}

std::string repo_api(forge_kind k, const forge_repo& r)
{
    return k == forge_kind::gitlab ? gitlab_project(r) : api_root(k, r.host) + "/repos/" + r.path();
}

std::string graphql_url(std::string_view host)
{
    return host == "github.com" ? "https://api.github.com/graphql"
                                : web_root(host) + "/api/graphql";
}

check_state github_rollup(std::string_view state)
{
    if (state == "SUCCESS") return check_state::success;
    if (state == "FAILURE" || state == "ERROR") return check_state::failure;
    if (state == "PENDING" || state == "EXPECTED") return check_state::pending;
    return check_state::none;
}

// A combined commit status (Gitea), or a pipeline's (GitLab).
check_state status_word(std::string_view s)
{
    if (s == "success") return check_state::success;
    if (s == "failure" || s == "error" || s == "failed" || s == "canceled")
        return check_state::failure;
    if (s == "pending" || s == "running" || s == "created" || s == "preparing" ||
        s == "waiting_for_resource" || s == "scheduled")
        return check_state::pending;
    return check_state::none;
}

std::expected<std::vector<pull_request>, std::string> github_pulls(const forge_repo& r,
                                                                   std::string_view token)
{
    // One query for them all, checks included: what polling costs is one
    // request a repository.
    constexpr std::string_view query =
        "query($owner:String!,$name:String!){repository(owner:$owner,name:$name){"
        "pullRequests(states:OPEN,first:100,orderBy:{field:UPDATED_AT,direction:DESC}){nodes{"
        "number title url isDraft headRefName headRefOid baseRefName reviewDecision mergeable "
        "mergeStateStatus "
        "headRepository{name owner{login}} "
        "commits(last:1){nodes{commit{statusCheckRollup{state}}}}}}}}";
    const std::string body = "{\"query\":" + json::quoted(query) +
                             ",\"variables\":{\"owner\":" + json::quoted(r.owner) +
                             ",\"name\":" + json::quoted(r.name) + "}}";
    auto headers = api_headers(forge_kind::github, token);
    headers.push_back({"Content-Type", "application/json"});
    const auto reply =
        rap::sys::http_request("POST", graphql_url(r.host), body, {.headers = headers});
    const auto j = json::parse(reply.body);
    if (reply.status != 200 || !j) return std::unexpected(failure(reply, j));
    if (const json::value errors = (*j)["errors"]; errors.kind() == json::value::type::array)
        for (const json::value& e : errors.elements())
            return std::unexpected(e["message"].str(_("GitHub refused the query.")));
    std::vector<pull_request> out;
    for (const json::value& n : (*j)["data"]["repository"]["pullRequests"]["nodes"].elements())
    {
        pull_request pr{.number = int(n["number"].integer().value_or(0)),
                        .title = n["title"].str(),
                        .url = n["url"].str(),
                        .head_branch = n["headRefName"].str(),
                        .base_branch = n["baseRefName"].str(),
                        .head_sha = n["headRefOid"].str(),
                        .draft = n["isDraft"].boolean()};
        const json::value head = n["headRepository"];  // null: the fork's gone
        pr.head = {
            .host = r.host, .owner = head["owner"]["login"].str(), .name = head["name"].str()};
        for (const json::value& c : n["commits"]["nodes"].elements())
            pr.checks = github_rollup(c["commit"]["statusCheckRollup"]["state"].str());
        // A list's merge state can be UNKNOWN until GitHub works it out; the
        // review decision is always there.
        pr.conflicts =
            n["mergeable"].str_is("CONFLICTING") || n["mergeStateStatus"].str_is("DIRTY");
        pr.blocked = n["reviewDecision"].str_is("REVIEW_REQUIRED") ||
                     n["reviewDecision"].str_is("CHANGES_REQUESTED") ||
                     n["mergeStateStatus"].str_is("BEHIND") ||
                     (n["mergeStateStatus"].str_is("BLOCKED") &&
                      pr.checks != check_state::pending && pr.checks != check_state::failure);
        out.push_back(std::move(pr));
    }
    return out;
}

std::expected<std::vector<pull_request>, std::string> gitlab_pulls(const forge_repo& r,
                                                                   std::string_view token)
{
    const auto headers = api_headers(forge_kind::gitlab, token);
    const auto reply = rap::sys::http_get(
        gitlab_project(r) + "/merge_requests?state=opened&per_page=100", {.headers = headers});
    const auto j = json::parse(reply.body);
    if (reply.status != 200 || !j) return std::unexpected(failure(reply, j));
    // The list leaves pipelines out: the project's latest, matched by commit.
    const auto pipes_reply =
        rap::sys::http_get(gitlab_project(r) + "/pipelines?per_page=100", {.headers = headers});
    const auto pipes = json::parse(pipes_reply.body);
    std::vector<pull_request> out;
    for (const json::value& m : j->elements())
    {
        pull_request pr{.number = int(m["iid"].integer().value_or(0)),
                        .title = m["title"].str(),
                        .url = m["web_url"].str(),
                        .head = r,
                        .head_branch = m["source_branch"].str(),
                        .base_branch = m["target_branch"].str(),
                        .head_sha = m["sha"].str(),
                        .draft = m["draft"].boolean()};
        // A fork's merge request: its project isn't this one, and its path
        // would take another request; it's left unmatched.
        if (m["source_project_id"].integer() != m["target_project_id"].integer()) pr.head = {};
        const std::string status = m["detailed_merge_status"].str();
        pr.conflicts = status == "conflict";
        pr.blocked = status == "not_approved" || status == "need_rebase" ||
                     status == "discussions_not_resolved" || status == "blocked_status" ||
                     status == "requested_changes";
        if (pipes && pipes_reply.status == 200)
            for (const json::value& p : pipes->elements())  // newest first
                if (p["sha"].str_is(pr.head_sha))
                {
                    pr.checks = status_word(p["status"].str());
                    break;
                }
        out.push_back(std::move(pr));
    }
    return out;
}

std::expected<std::vector<pull_request>, std::string> gitea_pulls(const forge_repo& r,
                                                                  std::string_view token)
{
    const auto headers = api_headers(forge_kind::gitea, token);
    const std::string root = repo_api(forge_kind::gitea, r);
    const auto reply =
        rap::sys::http_get(root + "/pulls?state=open&limit=50", {.headers = headers});
    const auto j = json::parse(reply.body);
    if (reply.status != 200 || !j) return std::unexpected(failure(reply, j));
    std::vector<pull_request> out;
    for (const json::value& p : j->elements())
    {
        const json::value head = p["head"];
        pull_request pr{.number = int(p["number"].integer().value_or(0)),
                        .title = p["title"].str(),
                        .url = p["html_url"].str(),
                        .head_branch = head["ref"].str(),
                        .base_branch = p["base"]["ref"].str(),
                        .head_sha = head["sha"].str(),
                        .draft = p["draft"].boolean()};
        pr.conflicts = !p["mergeable"].boolean(true);  // Gitea's "not mergeable" is conflicts
        pr.head = {.host = r.host,
                   .owner = head["repo"]["owner"]["login"].str(),
                   .name = head["repo"]["name"].str()};
        // Each one's checks: the head commit's combined status.
        const auto status =
            rap::sys::http_get(root + "/commits/" + pr.head_sha + "/status", {.headers = headers});
        if (const auto s = json::parse(status.body);
            s && status.status == 200 && (*s)["total_count"].integer().value_or(0) > 0)
            pr.checks = status_word((*s)["state"].str());
        out.push_back(std::move(pr));
    }
    return out;
}

}  // namespace

std::expected<std::vector<pull_request>, std::string>
open_pull_requests(forge_kind k, const forge_repo& r, std::string_view token)
{
    switch (k)
    {
    case forge_kind::github: return github_pulls(r, token);
    case forge_kind::gitlab: return gitlab_pulls(r, token);
    case forge_kind::gitea: return gitea_pulls(r, token);
    }
    return {};
}

std::expected<std::string, std::string> default_branch(forge_kind k, const forge_repo& r,
                                                       std::string_view token)
{
    const auto reply = rap::sys::http_get(repo_api(k, r), {.headers = api_headers(k, token)});
    const auto j = json::parse(reply.body);
    if (reply.status != 200 || !j) return std::unexpected(failure(reply, j));
    std::string branch = (*j)["default_branch"].str();
    if (branch.empty()) return std::unexpected(std::string(_("The server didn't say.")));
    return branch;
}

std::expected<std::vector<forge_repo>, std::string>
pull_request_targets(forge_kind k, const forge_repo& head, std::string_view token)
{
    const auto reply = rap::sys::http_get(repo_api(k, head), {.headers = api_headers(k, token)});
    const auto j = json::parse(reply.body);
    if (reply.status != 200 || !j) return std::unexpected(failure(reply, j));
    std::vector<forge_repo> out{head};
    auto add = [&](std::string_view path)
    {
        const size_t slash = path.rfind('/');
        if (slash == std::string_view::npos) return;
        forge_repo r{.host = head.host,
                     .owner = std::string(path.substr(0, slash)),
                     .name = std::string(path.substr(slash + 1))};
        if (std::ranges::find(out, r) == out.end()) out.push_back(std::move(r));
    };
    // GitHub names the parent and the network's root; Gitea the parent;
    // GitLab the project it was forked from.
    if (k == forge_kind::gitlab)
        add((*j)["forked_from_project"]["path_with_namespace"].str());
    else
    {
        add((*j)["parent"]["full_name"].str());
        add((*j)["source"]["full_name"].str());
    }
    return out;
}

std::expected<pull_request, std::string> create_pull_request(forge_kind k, const forge_repo& into,
                                                             const new_pull_request& n,
                                                             std::string_view token)
{
    auto headers = api_headers(k, token);
    headers.push_back({"Content-Type", "application/json"});
    std::string body, url;
    if (k == forge_kind::gitlab)
    {
        if (!(n.head == into))
            return std::unexpected(std::string(
                _("Tortilla opens GitLab merge requests only from a branch in the same project.")));
        // A draft is one titled so.
        url = gitlab_project(into) + "/merge_requests";
        body = "{\"source_branch\":" + json::quoted(n.head_branch) +
               ",\"target_branch\":" + json::quoted(n.base_branch) +
               ",\"title\":" + json::quoted((n.draft ? "Draft: " : "") + n.title) +
               ",\"description\":" + json::quoted(n.body) + "}";
    }
    else
    {
        // A fork's branch is named owner:branch.
        const std::string head =
            n.head == into ? n.head_branch : n.head.owner + ":" + n.head_branch;
        url = repo_api(k, into) + "/pulls";
        body = "{\"head\":" + json::quoted(head) + ",\"base\":" + json::quoted(n.base_branch) +
               ",\"title\":" + json::quoted(n.title) + ",\"body\":" + json::quoted(n.body) +
               (k == forge_kind::github ? std::string(",\"draft\":") + (n.draft ? "true" : "false")
                                        : std::string()) +
               "}";
    }
    const auto reply = rap::sys::http_request("POST", url, body, {.headers = headers});
    const auto j = json::parse(reply.body);
    if (reply.status != 201 || !j)
    {
        // GitHub says what's wrong in errors[]: "A pull request already exists...".
        if (j)
            for (const json::value& e : (*j)["errors"].elements())
                if (const std::string m = e["message"].str(); !m.empty()) return std::unexpected(m);
        return std::unexpected(failure(reply, j));
    }
    return pull_request{
        .number = int((*j)[k == forge_kind::gitlab ? "iid" : "number"].integer().value_or(0)),
        .title = n.title,
        .url = (*j)[k == forge_kind::gitlab ? "web_url" : "html_url"].str(),
        .head = n.head,
        .head_branch = n.head_branch,
        .base_branch = n.base_branch,
        .draft = n.draft};
}

// -- merging and closing ------------------------------------------------------

namespace
{

std::string_view method_word(merge_method m)
{
    switch (m)
    {
    case merge_method::squash: return "squash";
    case merge_method::rebase: return "rebase";
    default: return "merge";
    }
}

// A request with a JSON body; "" when the forge said 2xx, else why not.
std::string send_json(forge_kind k, std::string_view method, const std::string& url,
                      const std::string& body, std::string_view token)
{
    auto headers = api_headers(k, token);
    if (!body.empty()) headers.push_back({"Content-Type", "application/json"});
    const auto reply = rap::sys::http_request(method, url, body, {.headers = headers});
    if (reply.status >= 200 && reply.status < 300) return {};
    const auto j = json::parse(reply.body);
    return failure(reply, j);
}

std::expected<merge_state, std::string> github_merge_state(const forge_repo& r, int number,
                                                           std::string_view token)
{
    constexpr std::string_view query =
        "query($owner:String!,$name:String!,$number:Int!){repository(owner:$owner,name:$name){"
        "mergeCommitAllowed squashMergeAllowed rebaseMergeAllowed autoMergeAllowed "
        "deleteBranchOnMerge viewerDefaultMergeMethod viewerPermission "
        "pullRequest(number:$number){id title headRefOid isDraft mergeable mergeStateStatus "
        "reviewDecision "
        "viewerCanMergeAsAdmin viewerCanEnableAutoMerge autoMergeRequest{enabledAt} "
        "commits(last:1){nodes{commit{statusCheckRollup{state}}}}}}}";
    const std::string body =
        "{\"query\":" + json::quoted(query) + ",\"variables\":{\"owner\":" + json::quoted(r.owner) +
        ",\"name\":" + json::quoted(r.name) + ",\"number\":" + std::to_string(number) + "}}";
    auto headers = api_headers(forge_kind::github, token);
    headers.push_back({"Content-Type", "application/json"});
    const auto reply =
        rap::sys::http_request("POST", graphql_url(r.host), body, {.headers = headers});
    const auto j = json::parse(reply.body);
    if (reply.status != 200 || !j) return std::unexpected(failure(reply, j));
    for (const json::value& e : (*j)["errors"].elements())
        return std::unexpected(e["message"].str(_("GitHub refused the query.")));
    const json::value repo = (*j)["data"]["repository"];
    const json::value pr = repo["pullRequest"];
    merge_state m;
    if (repo["mergeCommitAllowed"].boolean()) m.methods.push_back(merge_method::merge);
    if (repo["squashMergeAllowed"].boolean()) m.methods.push_back(merge_method::squash);
    if (repo["rebaseMergeAllowed"].boolean()) m.methods.push_back(merge_method::rebase);
    const std::string preferred = repo["viewerDefaultMergeMethod"].str();
    m.preferred = preferred == "SQUASH"   ? merge_method::squash
                  : preferred == "REBASE" ? merge_method::rebase
                                          : merge_method::merge;
    if (std::ranges::find(m.methods, m.preferred) == m.methods.end() && !m.methods.empty())
        m.preferred = m.methods.front();
    m.forge_deletes_branch = repo["deleteBranchOnMerge"].boolean();
    m.title = pr["title"].str();
    m.head_sha = pr["headRefOid"].str();
    m.node_id = pr["id"].str();
    m.can_bypass = pr["viewerCanMergeAsAdmin"].boolean();
    m.merge_when_ready_set = !pr["autoMergeRequest"].is_null() && bool(pr["autoMergeRequest"]);
    m.can_merge_when_ready =
        repo["autoMergeAllowed"].boolean() && pr["viewerCanEnableAutoMerge"].boolean();
    check_state checks = check_state::none;
    for (const json::value& c : pr["commits"]["nodes"].elements())
        checks = github_rollup(c["commit"]["statusCheckRollup"]["state"].str());

    const std::string permission = repo["viewerPermission"].str();
    const std::string status = pr["mergeStateStatus"].str();
    using v = merge_state::verdict;
    if (permission != "ADMIN" && permission != "MAINTAIN" && permission != "WRITE")
        m = {.state = v::blocked,
             .why = _("You can't merge into this repository: it takes write access.")};
    else if (pr["isDraft"].boolean() || status == "DRAFT")
        m.state = v::blocked, m.why = _("It's a draft: mark it ready for review first.");
    else if (status == "DIRTY" || pr["mergeable"].str_is("CONFLICTING"))
        m.state = v::blocked,
        m.why = _("It conflicts with its base branch: resolve the conflicts first.");
    else if (status == "CLEAN" || status == "UNSTABLE" || status == "HAS_HOOKS")
        m.state = v::ready;
    else if (status == "BEHIND")
        m.state = v::blocked, m.why = _("Its branch is behind the base, which this repository "
                                        "doesn't allow: update the branch first.");
    else if (status == "BLOCKED")
    {
        // The review first: no waiting on checks gets past one.
        const std::string review = pr["reviewDecision"].str();
        const bool needs_review = review == "REVIEW_REQUIRED" || review == "CHANGES_REQUESTED";
        m.state = checks == check_state::pending && !needs_review ? v::waiting : v::blocked;
        m.why = review == "CHANGES_REQUESTED"    ? _("A reviewer asked for changes.")
                : review == "REVIEW_REQUIRED"    ? _("It needs an approving review.")
                : checks == check_state::pending ? _("Required checks are still running.")
                : checks == check_state::failure ? _("Required checks failed.")
                                                 : _("The branch's rules block it with something "
                                                     "GitHub doesn't name: a code owner's review, "
                                                     "signed commits, a deployment...");
    }
    else
        m.state = v::checking, m.why = _("GitHub is still working out whether it can be merged.");
    if (m.methods.empty() && m.state != v::blocked)
        m.state = v::blocked, m.why = _("The repository allows no way of merging.");
    return m;
}

std::expected<merge_state, std::string> gitlab_merge_state(const forge_repo& r, int number,
                                                           std::string_view token)
{
    const auto headers = api_headers(forge_kind::gitlab, token);
    const auto project_reply = rap::sys::http_get(gitlab_project(r), {.headers = headers});
    const auto project = json::parse(project_reply.body);
    if (project_reply.status != 200 || !project)
        return std::unexpected(failure(project_reply, project));
    const auto reply = rap::sys::http_get(
        gitlab_project(r) + "/merge_requests/" + std::to_string(number), {.headers = headers});
    const auto mr = json::parse(reply.body);
    if (reply.status != 200 || !mr) return std::unexpected(failure(reply, mr));
    merge_state m;
    // The project's one method; squashing is an option on top of it.
    const std::string method = (*project)["merge_method"].str();
    m.methods.push_back(method == "merge" ? merge_method::merge : merge_method::rebase);
    const std::string squash = (*project)["squash_option"].str();
    if (squash != "never") m.methods.push_back(merge_method::squash);
    if (squash == "always") m.methods = {merge_method::squash};
    m.preferred =
        squash == "default_on" || squash == "always" ? merge_method::squash : m.methods.front();
    m.forge_deletes_branch = (*mr)["force_remove_source_branch"].boolean();
    m.title = (*mr)["title"].str();
    m.head_sha = (*mr)["sha"].str();
    m.merge_when_ready_set = (*mr)["merge_when_pipeline_succeeds"].boolean();
    using v = merge_state::verdict;
    if (!(*mr)["user"]["can_merge"].boolean())
        return merge_state{.state = v::blocked, .why = _("You can't merge into this project.")};
    const std::string status = (*mr)["detailed_merge_status"].str();
    if (status == "mergeable")
        m.state = v::ready;
    else if (status == "ci_still_running")
        m.state = v::waiting, m.why = _("The pipeline is still running."),
        m.can_merge_when_ready = true;
    else if (status == "checking" || status == "unchecked" || status == "preparing" ||
             status == "approvals_syncing")
        m.state = v::checking, m.why = _("GitLab is still working out whether it can be merged.");
    else
    {
        m.state = v::blocked;
        m.why = status == "conflict"
                    ? _("It conflicts with its target branch: resolve the conflicts first.")
                : status == "need_rebase"  ? _("It needs rebasing onto its target branch first.")
                : status == "draft_status" ? _("It's a draft: mark it ready first.")
                : status == "not_approved" ? _("It needs approval first.")
                : status == "discussions_not_resolved"
                    ? _("Its discussions must be resolved first.")
                : status == "ci_must_pass"
                    ? _("The pipeline must pass first.")
                    : rap::i18n::format(_("GitLab says it can't be merged ({0})."), status);
    }
    return m;
}

std::expected<merge_state, std::string> gitea_merge_state(const forge_repo& r, int number,
                                                          std::string_view token)
{
    const auto headers = api_headers(forge_kind::gitea, token);
    const auto repo_reply =
        rap::sys::http_get(repo_api(forge_kind::gitea, r), {.headers = headers});
    const auto repo = json::parse(repo_reply.body);
    if (repo_reply.status != 200 || !repo) return std::unexpected(failure(repo_reply, repo));
    const auto reply = rap::sys::http_get(
        repo_api(forge_kind::gitea, r) + "/pulls/" + std::to_string(number), {.headers = headers});
    const auto pr = json::parse(reply.body);
    if (reply.status != 200 || !pr) return std::unexpected(failure(reply, pr));
    merge_state m;
    if ((*repo)["allow_merge_commits"].boolean(true)) m.methods.push_back(merge_method::merge);
    if ((*repo)["allow_squash_merge"].boolean(true)) m.methods.push_back(merge_method::squash);
    if ((*repo)["allow_rebase"].boolean(true)) m.methods.push_back(merge_method::rebase);
    const std::string style = (*repo)["default_merge_style"].str();
    m.preferred = style == "squash"   ? merge_method::squash
                  : style == "rebase" ? merge_method::rebase
                                      : merge_method::merge;
    if (std::ranges::find(m.methods, m.preferred) == m.methods.end() && !m.methods.empty())
        m.preferred = m.methods.front();
    m.forge_deletes_branch = (*repo)["default_delete_branch_after_merge"].boolean();
    m.title = (*pr)["title"].str();
    m.head_sha = (*pr)["head"]["sha"].str();
    using v = merge_state::verdict;
    if (!(*repo)["permissions"]["push"].boolean())
        return merge_state{.state = v::blocked,
                           .why =
                               _("You can't merge into this repository: it takes write access.")};
    if ((*pr)["mergeable"].boolean())
        m.state = v::ready;
    else
        m.state = v::blocked,
        m.why = _("The server says it can't be merged: conflicts, or the branch's rules.");
    return m;
}

}  // namespace

std::expected<merge_state, std::string> pull_request_merge_state(forge_kind k, const forge_repo& r,
                                                                 int number, std::string_view token)
{
    switch (k)
    {
    case forge_kind::github: return github_merge_state(r, number, token);
    case forge_kind::gitlab: return gitlab_merge_state(r, number, token);
    case forge_kind::gitea: return gitea_merge_state(r, number, token);
    }
    return {};
}

std::string merge_pull_request(forge_kind k, const forge_repo& r, int number,
                               const merge_request& m, std::string_view token)
{
    const std::string n = std::to_string(number);
    switch (k)
    {
    case forge_kind::github:
    {
        if (m.when_ready)
        {
            // Auto-merge is GraphQL's alone.
            std::string method(method_word(m.method));
            for (char& c : method) c = char(std::toupper(static_cast<unsigned char>(c)));
            const std::string body =
                "{\"query\":" +
                json::quoted(
                    "mutation($id:ID!,$method:PullRequestMergeMethod!,$sha:GitObjectID){"
                    "enablePullRequestAutoMerge(input:{pullRequestId:$id,mergeMethod:$method,"
                    "expectedHeadOid:$sha}){clientMutationId}}") +
                ",\"variables\":{\"id\":" + json::quoted(m.node_id) +
                ",\"method\":" + json::quoted(method) + ",\"sha\":" + json::quoted(m.head_sha) +
                "}}";
            auto headers = api_headers(k, token);
            headers.push_back({"Content-Type", "application/json"});
            const auto reply =
                rap::sys::http_request("POST", graphql_url(r.host), body, {.headers = headers});
            const auto j = json::parse(reply.body);
            if (reply.status != 200 || !j) return failure(reply, j);
            for (const json::value& e : (*j)["errors"].elements()) return e["message"].str();
            return {};
        }
        std::string body = "{\"merge_method\":" + json::quoted(method_word(m.method)) +
                           ",\"sha\":" + json::quoted(m.head_sha);
        if (!m.commit_title.empty()) body += ",\"commit_title\":" + json::quoted(m.commit_title);
        if (!m.commit_message.empty())
            body += ",\"commit_message\":" + json::quoted(m.commit_message);
        body += "}";
        return send_json(k, "PUT", repo_api(k, r) + "/pulls/" + n + "/merge", body, token);
    }
    case forge_kind::gitlab:
    {
        std::string body = "{\"sha\":" + json::quoted(m.head_sha) +
                           ",\"squash\":" + (m.method == merge_method::squash ? "true" : "false");
        if (m.when_ready) body += ",\"merge_when_pipeline_succeeds\":true";
        if (!m.commit_message.empty())
            body += std::string(m.method == merge_method::squash ? ",\"squash_commit_message\":"
                                                                 : ",\"merge_commit_message\":") +
                    json::quoted(m.commit_title + (m.commit_title.empty() ? "" : "\n\n") +
                                 m.commit_message);
        body += "}";
        return send_json(k, "PUT", gitlab_project(r) + "/merge_requests/" + n + "/merge", body,
                         token);
    }
    case forge_kind::gitea:
    {
        std::string body = "{\"Do\":" + json::quoted(method_word(m.method)) +
                           ",\"head_commit_id\":" + json::quoted(m.head_sha);
        if (!m.commit_title.empty()) body += ",\"MergeTitleField\":" + json::quoted(m.commit_title);
        if (!m.commit_message.empty())
            body += ",\"MergeMessageField\":" + json::quoted(m.commit_message);
        if (m.when_ready) body += ",\"merge_when_checks_succeed\":true";
        body += "}";
        return send_json(k, "POST", repo_api(k, r) + "/pulls/" + n + "/merge", body, token);
    }
    }
    return {};
}

std::string close_pull_request(forge_kind k, const forge_repo& r, int number,
                               std::string_view comment, std::string_view token)
{
    const std::string n = std::to_string(number);
    if (!comment.empty())
    {
        const std::string body = "{\"body\":" + json::quoted(comment) + "}";
        const std::string url = k == forge_kind::gitlab
                                    ? gitlab_project(r) + "/merge_requests/" + n + "/notes"
                                    : repo_api(k, r) + "/issues/" + n + "/comments";
        if (std::string why = send_json(k, "POST", url, body, token); !why.empty())
            return rap::i18n::format(_("The reason couldn't be posted: {0}"), why);
    }
    if (k == forge_kind::gitlab)
        return send_json(k, "PUT", gitlab_project(r) + "/merge_requests/" + n,
                         "{\"state_event\":\"close\"}", token);
    return send_json(k, "PATCH", repo_api(k, r) + "/pulls/" + n, "{\"state\":\"closed\"}", token);
}

std::string delete_branch(forge_kind k, const forge_repo& r, std::string_view branch,
                          std::string_view token)
{
    const std::string escaped = form_escape(branch);
    switch (k)
    {
    case forge_kind::github:
        return send_json(k, "DELETE", repo_api(k, r) + "/git/refs/heads/" + std::string(branch), {},
                         token);
    case forge_kind::gitlab:
        return send_json(k, "DELETE", gitlab_project(r) + "/repository/branches/" + escaped, {},
                         token);
    case forge_kind::gitea:
        return send_json(k, "DELETE", repo_api(k, r) + "/branches/" + std::string(branch), {},
                         token);
    }
    return {};
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
