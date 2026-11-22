#include "azure.h"
#include "azure_credentials.h"

#include "forge/forge_detail.h"
#include "i18n.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <map>
#include <mutex>
#include <set>

namespace tortilla::azure
{
using namespace forge_detail;
namespace
{
constexpr std::string_view version = "api-version=7.1";

std::string decoded(std::string_view s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] == '%' && i + 2 < s.size())
        {
            unsigned v = 0;
            const auto [end, error] = std::from_chars(s.data() + i + 1, s.data() + i + 3, v, 16);
            if (error == std::errc{} && end == s.data() + i + 3 && v && v != '/' && v != '\\')
            {
                out += char(v);
                i += 2;
                continue;
            }
            return {};  // Ambiguous encoded separators must not change account scope.
        }
        if (s[i] == '\\' || static_cast<unsigned char>(s[i]) < 32 || s[i] == '%') return {};
        out += s[i];
    }
    return out;
}

std::vector<std::string> segments(std::string_view path)
{
    std::vector<std::string> out;
    while (!path.empty())
    {
        const size_t slash = path.find('/');
        auto part = decoded(path.substr(0, slash));
        if (part.empty() || part == "." || part == "..") return {};
        out.push_back(std::move(part));
        if (slash == std::string_view::npos) break;
        path.remove_prefix(slash + 1);
    }
    return out;
}

std::string org(const forge_repo& r)
{
    return r.owner.substr(0, r.owner.find('/'));
}

std::string web_repo(const forge_repo& r)
{
    const size_t slash = r.owner.find('/');
    return "https://dev.azure.com/" + url_escape(org(r)) + "/" +
           url_escape(r.owner.substr(slash + 1)) + "/_git/" + url_escape(r.name);
}

std::expected<std::string, std::string> get(const std::string& url, std::string_view token)
{
    const auto reply = helper_get(url, {.headers = api_headers(forge_kind::azure, token)}, token);
    const auto parsed = json::parse(reply.body);
    if (reply.status != 200 || !parsed || !reply.error.empty())
        return std::unexpected(response_error(reply, url, token));
    const bool collection = url.find("/refs?") != std::string::npos ||
                            url.find("/policy/evaluations?") != std::string::npos ||
                            url.find("/statuses?") != std::string::npos ||
                            url.find("/artifacts?") != std::string::npos ||
                            url.find("/logs?") != std::string::npos;
    if ((collection && (*parsed)["value"].kind() != json::value::type::array) ||
        (url.find("/timeline?") != std::string::npos &&
         (*parsed)["records"].kind() != json::value::type::array))
        return std::unexpected(response_error(reply, url, token) +
                               ": missing documented response array");
    return reply.body;
}

std::string branch(const json::value& v)
{
    std::string s = v.str();
    if (s.starts_with("refs/heads/")) s.erase(0, 11);
    return s;
}

pull_request parse_pr(const json::value& v, const forge_repo& r, bool complete)
{
    pull_request p;
    p.number = int(v["pullRequestId"].integer().value_or(0));
    p.title = v["title"].str();
    p.url = web_repo(r) + "/pullrequest/" + std::to_string(p.number);
    p.head = r;
    if (const auto fork = v["forkSource"]["repository"]; fork)
    {
        if (const auto other = repo_of_url(fork["remoteUrl"].str())) p.head = *other;
    }
    p.head_branch = branch(v["sourceRefName"]);
    p.base_branch = branch(v["targetRefName"]);
    p.head_sha = v["lastMergeSourceCommit"]["commitId"].str();
    p.draft = v["isDraft"].boolean();
    p.conflicts = v["mergeStatus"].str_is("conflicts");
    const auto merge = v["mergeStatus"].str();
    p.merge_known = merge == "conflicts" || merge == "succeeded" || merge == "rejectedByPolicy" ||
                    merge == "failure";
    p.blocked = merge == "rejectedByPolicy" || merge == "failure";
    if (p.blocked) p.blocks.push_back(merge_block::blocked_other);
    p.author = v["createdBy"]["uniqueName"].str(v["createdBy"]["displayName"].str());
    // Identity image URLs often need authentication on a different origin.
    // Leave avatars absent rather than forwarding the PAT to an image host.
    p.body = v["description"].str();
    p.body_known = complete;  // Azure's list truncates descriptions to 400 characters.
    p.labels = decode_labels(v["labels"]);
    p.reviews_known = true;
    // https://learn.microsoft.com/en-us/rest/api/azure/devops/git/pull-request-reviewers/list?view=azure-devops-rest-7.1
    for (const json::value& reviewer : v["reviewers"].elements())
    {
        const auto vote = reviewer["vote"].integer();
        const auto verdict = vote ? review_vote(*vote) : std::nullopt;
        if (!verdict)
        {
            p.reviews_known = false;
            continue;
        }
        p.reviews.push_back(
            {.who = reviewer["uniqueName"].str(reviewer["displayName"].str()), .say = *verdict});
    }
    p.checks_known = false;
    if (p.draft) p.blocks.push_back(merge_block::draft);
    if (p.conflicts) p.blocks.push_back(merge_block::conflicts);
    return p;
}

void policies(const json::value& v, pull_request& p)
{
    bool any = false, pending = false, failed = false, unknown = false;
    for (const json::value& policy : v["value"].elements())
    {
        if (!policy["configuration"]["isEnabled"].boolean(true)) continue;
        const auto status = policy["status"].str();
        if (status == "notApplicable") continue;
        const auto mapped = policy_state(status);
        unknown |= mapped == check_state::none;
        const bool rejected = mapped == check_state::failure;
        const bool waiting = mapped == check_state::pending;
        any |= mapped != check_state::none;
        failed |= rejected;
        pending |= waiting;
        if ((rejected || waiting) && policy["configuration"]["isBlocking"].boolean())
        {
            p.blocked = true;
            p.blocks.push_back(merge_block::blocked_other);
        }
        p.runs.push_back({.name = policy["configuration"]["type"]["displayName"].str("Policy"),
                          .state = mapped});
    }
    p.checks = failed    ? check_state::failure
               : pending ? check_state::pending
               : any     ? check_state::success
                         : check_state::none;
    p.checks_known = !unknown;
}

// Build IDs are numeric, but timeline record IDs are GUIDs. Keep both,
// rather than hashing GUIDs into the existing numeric job identity.
std::int64_t cache_id(const forge_repo& r, std::int64_t build, std::string_view record,
                      std::int64_t attempt)
{
    static std::mutex mutex;
    static std::map<std::string, std::int64_t> ids;
    std::lock_guard lock(mutex);
    const auto key = r.host + "/" + r.path() + "/" + std::to_string(build) + "/" +
                     std::string(record) + "/" + std::to_string(attempt);
    const auto it = ids.try_emplace(key, -std::int64_t(ids.size()) - 1).first;
    return it->second;
}

std::string build_root(const forge_repo& r, std::int64_t id)
{
    return project_api(r) + "/build/builds/" + std::to_string(id);
}
}  // namespace

// https://learn.microsoft.com/en-us/rest/api/azure/devops/build/timeline/get?view=azure-devops-rest-7.1
check_state timeline_state(const json::value& record)
{
    const auto result = record["result"].str();
    if (result == "succeeded" || result == "succeededWithIssues") return check_state::success;
    if (result == "failed" || result == "canceled" || result == "abandoned")
        return check_state::failure;
    if (result == "skipped") return check_state::none;
    if (!result.empty()) return check_state::none;  // Future result values are unknown.
    const auto state = record["state"].str();
    return state == "pending" || state == "inProgress" ? check_state::pending : check_state::none;
}

// https://learn.microsoft.com/en-us/rest/api/azure/devops/build/builds/list?view=azure-devops-rest-7.1
check_state build_state(const json::value& record)
{
    const auto result = record["result"].str();
    if (result == "succeeded") return check_state::success;
    if (result == "failed" || result == "partiallySucceeded" || result == "canceled")
        return check_state::failure;
    if (!result.empty() && result != "none") return check_state::none;
    const auto state = record["status"].str();
    return state == "inProgress" || state == "cancelling" || state == "postponed" ||
                   state == "notStarted"
               ? check_state::pending
               : check_state::none;
}

// https://learn.microsoft.com/en-us/rest/api/azure/devops/git/statuses/list?view=azure-devops-rest-7.1
check_state status_state(std::string_view state)
{
    if (state == "succeeded") return check_state::success;
    if (state == "failed" || state == "error") return check_state::failure;
    if (state == "pending") return check_state::pending;
    return check_state::none;  // notSet, notApplicable and future values.
}

// https://learn.microsoft.com/en-us/rest/api/azure/devops/policy/evaluations/list?view=azure-devops-rest-7.1
check_state policy_state(std::string_view state)
{
    if (state == "approved") return check_state::success;
    if (state == "rejected" || state == "broken") return check_state::failure;
    if (state == "queued" || state == "running") return check_state::pending;
    return check_state::none;  // notApplicable and future values.
}

// https://learn.microsoft.com/en-us/rest/api/azure/devops/git/pull-request-reviewers/list?view=azure-devops-rest-7.1
std::optional<pr_review::verdict> review_vote(std::int64_t vote)
{
    switch (vote)
    {
    case 10:
    case 5: return pr_review::verdict::approved;
    case -10: return pr_review::verdict::changes_requested;
    case 0:
    case -5: return pr_review::verdict::requested;
    default: return std::nullopt;
    }
}

bool html_response(const rap::sys::http_response& reply)
{
    std::string prefix = reply.body.substr(0, 8192);
    std::ranges::transform(prefix, prefix.begin(),
                           [](unsigned char c) { return char(std::tolower(c)); });
    bool html = prefix.find("<html") != std::string::npos ||
                prefix.find("<!doctype html") != std::string::npos;
    for (const auto& [name, value] : reply.headers)
    {
        std::string n = name, v = value;
        std::ranges::transform(n, n.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        std::ranges::transform(v, v.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        html |= n == "content-type" && v.find("text/html") != std::string::npos;
    }
    return html;
}

std::string response_error(const rap::sys::http_response& reply, std::string_view url,
                           std::string_view token)
{
    auto safe = [&](std::string text)
    {
        auto secrets = helper_secrets(token);
        secrets.insert(secrets.end(), {std::string(token), basic_token(token), url_escape(token)});
        for (const auto& secret : secrets)
        {
            if (secret.empty()) continue;
            size_t pos = 0;
            while ((pos = text.find(secret, pos)) != std::string::npos)
            {
                text.replace(pos, secret.size(), "[redacted]");
                pos += 10;
            }
        }
        for (char& c : text)
            if (static_cast<unsigned char>(c) < 32) c = ' ';
        if (text.size() > 512) text = text.substr(0, 512) + "...";
        return text;
    };
    const bool html = html_response(reply);
    const auto parsed = json::parse(reply.body);
    std::string error = "Azure DevOps HTTP " + std::to_string(reply.status);
    if (parsed)
    {
        const auto message = (*parsed)["message"].str();
        if (!message.empty()) error += ": " + safe(message);
    }
    if (!reply.error.empty())
        error += ": " + safe(reply.error);
    else if (reply.status == 200)
        error += ": invalid or incomplete response";
    const bool signin = reply.status == 401 || (reply.status == 203 && html && !parsed);
    if (signin)
        error += is_helper_token(token) ? ". Git credential invalid, expired, or missing required "
                                          "scope; sign in through Git or paste an organization PAT"
                                        : ". PAT invalid, expired, or missing required scope; "
                                          "check the organization and token";
    if (signin || reply.status == 403)
        error += url.find("/build/") != std::string_view::npos ? ". Required scope: Build (Read)."
                 : url.find("/git/") != std::string_view::npos ||
                         url.find("/policy/") != std::string_view::npos
                     ? ". Required scope: Code (Read)."
                     : ". Check PAT organization access and Code/Build read scopes.";
    return error;
}

std::string unsupported()
{
    return _("Azure DevOps mutations are not supported yet; use the Azure DevOps web page.");
}

std::string basic_pair(std::string_view username, std::string_view password)
{
    constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const std::string s = std::string(username) + ":" + std::string(password);
    std::string out;
    unsigned bits = 0;
    int count = 0;
    for (unsigned char c : s)
    {
        bits = (bits << 8) | c;
        count += 8;
        while (count >= 6)
        {
            count -= 6;
            out += alphabet[(bits >> count) & 63];
        }
    }
    if (count) out += alphabet[(bits << (6 - count)) & 63];
    while (out.size() % 4) out += '=';
    return out;
}

std::string basic_token(std::string_view pat)
{
    return basic_pair({}, pat);
}

std::optional<forge_repo> remote(std::string_view host, std::string_view path)
{
    std::string h(host);
    std::ranges::transform(h, h.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    auto p = segments(path);
    std::string organization, project, name;
    if (h == "dev.azure.com" && p.size() == 4 && p[2] == "_git")
        organization = p[0], project = p[1], name = p[3];
    else if ((h == "ssh.dev.azure.com" || h == "vs-ssh.visualstudio.com") && p.size() == 4 &&
             p[0] == "v3")
        organization = p[1], project = p[2], name = p[3];
    else if (h.ends_with(".visualstudio.com"))
    {
        if (!p.empty() && p.front() == "DefaultCollection") p.erase(p.begin());
        if (p.size() != 3 || p[1] != "_git") return {};
        organization = h.substr(0, h.size() - std::string_view(".visualstudio.com").size());
        project = p[0], name = p[2];
    }
    if (organization.empty() || project.empty() || name.empty()) return {};
    if (!std::ranges::all_of(organization,
                             [](unsigned char c) {
                                 return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                        (c >= '0' && c <= '9') || c == '-';
                             }))
        return {};
    std::ranges::transform(organization, organization.begin(),
                           [](unsigned char c) { return char(std::tolower(c)); });
    return forge_repo{.host = "dev.azure.com", .owner = organization + "/" + project, .name = name};
}

bool scope_allows(std::string_view scope, std::string_view url)
{
    if (!url.starts_with(scope)) return false;
    // Refuse dot segments and escaped separators before the HTTP stack can
    // normalize them into a different organization. Queries are not paths.
    const auto path = url.substr(std::string_view("https://dev.azure.com/").size());
    return !segments(path.substr(0, path.find_first_of("?#"))).empty();
}

namespace
{
std::string lowered(std::string s)
{
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

// The organization an https URL is in, read the way a server would: the
// legacy host's label, or dev.azure.com's first path segment after
// percent-decoding and resolving "." and ".." (RFC 3986 5.2.4). None when
// that can't be read one way only: an encoded slash or backslash, a raw
// backslash or control character, bad escapes, a climb above the root.
std::optional<std::string> url_organization(std::string_view url)
{
    const auto o = rap::sys::url_origin(url);
    if (!o || o->scheme != "https" || o->port != 443) return {};
    constexpr std::string_view legacy = ".visualstudio.com";
    if (o->host.ends_with(legacy))
    {
        std::string label = o->host.substr(0, o->host.size() - legacy.size());
        if (label.empty() || label.find('.') != std::string::npos) return {};
        return label;  // url_origin's host is in lower case
    }
    if (o->host != "dev.azure.com") return {};
    const size_t authority = url.find("://");
    if (authority == std::string_view::npos) return {};
    const size_t at = url.find_first_of("/?#", authority + 3);
    if (at == std::string_view::npos || url[at] != '/') return {};
    std::string_view path = url.substr(at + 1);
    path = path.substr(0, path.find_first_of("?#"));
    std::vector<std::string> kept, as_sent;
    for (;;)
    {
        const size_t slash = path.find('/');
        const std::string_view piece = path.substr(0, slash);
        std::string segment;
        for (size_t i = 0; i < piece.size(); ++i)
        {
            unsigned char c = static_cast<unsigned char>(piece[i]);
            if (c == '%')
            {
                unsigned v = 0;
                if (i + 2 >= piece.size()) return {};
                const auto [end, error] =
                    std::from_chars(piece.data() + i + 1, piece.data() + i + 3, v, 16);
                if (error != std::errc{} || end != piece.data() + i + 3) return {};
                c = static_cast<unsigned char>(v);
                i += 2;
                if (c == '/') return {};
            }
            if (c == '\\' || c < 32 || c == 127) return {};
            segment += char(c);
        }
        // Rapport preserves escaped dot segments on the wire. A server may
        // decode them, so both interpretations must name the same tenant.
        if (piece == "..")
        {
            if (as_sent.empty()) return {};
            as_sent.pop_back();
        }
        else if (piece != ".")
            as_sent.push_back(segment);
        if (segment == "..")
        {
            if (kept.empty()) return {};
            kept.pop_back();
        }
        else if (segment != ".")
            kept.push_back(std::move(segment));
        if (slash == std::string_view::npos) break;
        path.remove_prefix(slash + 1);
    }
    if (kept.empty() || kept.front().empty() || as_sent.empty() ||
        lowered(kept.front()) != lowered(as_sent.front())) return {};
    return lowered(std::move(kept.front()));
}
}  // namespace

bool redirect_in_scope(std::string_view org, std::string_view from_url, std::string_view to_url)
{
    // Location: "" or "#x" is the same document (RFC 3986 5.2.2).
    const std::string_view target = to_url.empty() || to_url.front() == '#' ? from_url : to_url;
    const auto in = url_organization(target);
    return in && !org.empty() && *in == lowered(std::string(org));
}

rap::sys::redirect_action redirect_decision(std::string_view org, std::string_view from_url,
                                            std::string_view to_url)
{
    using rap::sys::redirect_action;
    if (redirect_in_scope(org, from_url, to_url)) return redirect_action::follow;
    const auto o = rap::sys::url_origin(to_url);
    // A downgrade to http, or what isn't a URL, goes nowhere.
    if (!o || o->scheme != "https") return redirect_action::stop;
    // The shared origin (any port), or another organization's own legacy
    // host: another organization, or a path that can't be read as this one.
    constexpr std::string_view legacy = ".visualstudio.com";
    if (o->host == "dev.azure.com" ||
        (o->host.ends_with(legacy) &&
         o->host.substr(0, o->host.size() - legacy.size()).find('.') == std::string::npos))
        return redirect_action::stop;
    // Elsewhere (a signed download on blob storage or a CDN, the URL its
    // own key): on, with no credential from here.
    return redirect_action::follow_without_credentials;
}

std::string scope_organization(std::string_view scope)
{
    constexpr std::string_view prefix = "https://dev.azure.com/";
    if (scope.starts_with(prefix)) scope.remove_prefix(prefix.size());
    else if (scope.starts_with("dev.azure.com/")) scope.remove_prefix(14);
    while (scope.ends_with('/')) scope.remove_suffix(1);
    return lowered(decoded(scope));
}

rap::sys::http_response redirected_outside(std::string_view org, std::string_view url)
{
    rap::sys::http_response r;
    r.url = std::string(url);
    r.error = rap::i18n::format(_("Azure DevOps redirected outside organization {0}; the "
                                  "sign-in wasn't sent there."),
                                std::string(org));
    return r;
}

std::string repository_web(const forge_repo& r)
{
    return web_repo(r);
}

std::string account_host(const forge_repo& r)
{
    return "dev.azure.com/" + url_escape(org(r));
}
std::string project_api(const forge_repo& r)
{
    return "https://" + account_host(r) + "/" + url_escape(r.owner.substr(r.owner.find('/') + 1)) +
           "/_apis";
}
std::string repository_api(const forge_repo& r)
{
    return project_api(r) + "/git/repositories/" + url_escape(r.name);
}

std::expected<forge_account, std::string> identity(std::string_view host, std::string_view token)
{
    // Reuse remote parsing to validate and normalize the organization field.
    const auto r = repo_of_url("https://" + std::string(host) + "/project/_git/repository");
    if (!r || r->host != "dev.azure.com")
        return std::unexpected(
            std::string(_("Enter the Azure organization as dev.azure.com/organization.")));
    const std::string scope = account_host(*r);
    bind_token(token, scope);
    // https://github.com/microsoft/azure-devops-node-api/blob/master/api/WebApi.ts (WebApi.connect; unversioned)
    // https://learn.microsoft.com/en-us/javascript/api/azure-devops-extension-api/connectiondata
    const auto body = get("https://" + scope + "/_apis/connectionData", token);
    if (!body) return std::unexpected(body.error());
    const auto j = *json::parse(*body);
    const auto user = j["authenticatedUser"];
    const std::string id = user["id"].str();
    if (id.empty() || id == "00000000-0000-0000-0000-000000000000")
        return std::unexpected(
            std::string(_("Azure DevOps HTTP 200: authenticatedUser.id is missing or anonymous.")));
    return forge_account{.kind = forge_kind::azure,
                         .host = scope,
                         .login = id,
                         .name = user["providerDisplayName"].str(user["customDisplayName"].str())};
}

std::expected<pull_requests_reply, std::string> pulls(const forge_repo& r, std::string_view token,
                                                      bool tips)
{
    // https://learn.microsoft.com/en-us/rest/api/azure/devops/git/pull-requests/get-pull-requests?view=azure-devops-rest-7.1
    const auto reply =
        helper_get(repository_api(r) + "/pullrequests?searchCriteria.status=active&$top=100&" +
                       std::string(version),
                   {.headers = api_headers(forge_kind::azure, token)}, token);
    pull_requests_reply out;
    const auto j = json::parse(reply.body);
    if (reply.status != 200 || !j || (*j)["value"].kind() != json::value::type::array ||
        !reply.error.empty())
        return std::unexpected(response_error(reply, repository_api(r), token));
    out.access = repo_access::read;  // PR1 deliberately exposes no mutation capabilities.
    for (const json::value& v : (*j)["value"].elements())
    {
        auto p = parse_pr(v, r, false);
        // Policy checks are fetched for the shown PR by details(), not N per list poll.
        out.prs.push_back(std::move(p));
    }
    if (tips)
    {
        // https://learn.microsoft.com/en-us/rest/api/azure/devops/git/refs/list?view=azure-devops-rest-7.1
        const auto body =
            get(repository_api(r) + "/refs?filter=heads/&$top=100&" + std::string(version), token);
        if (!body) return std::unexpected(body.error());
        {
            remote_tips branches;
            for (const json::value& v : (*json::parse(*body))["value"].elements())
                branches.branches.push_back({branch(v["name"]), v["objectId"].str()});
            branches.complete = branches.branches.size() < 100;
            out.tips = std::move(branches);
        }
    }
    return out;
}

std::expected<pull_request, std::string> details(const forge_repo& r, pull_request pr,
                                                 std::string_view token)
{
    // https://learn.microsoft.com/en-us/rest/api/azure/devops/git/pull-requests/get-pull-request?view=azure-devops-rest-7.1
    const auto body = get(repository_api(r) + "/pullrequests/" + std::to_string(pr.number) + "?" +
                              std::string(version),
                          token);
    if (!body) return std::unexpected(body.error());
    const auto j = *json::parse(*body);
    auto p = parse_pr(j, r, true);
    const auto project = j["repository"]["project"]["id"].str();
    if (!project.empty())
    {
        const std::string artifact =
            "vstfs:///CodeReview/CodeReviewId/" + project + "/" + std::to_string(p.number);
        // https://learn.microsoft.com/en-us/rest/api/azure/devops/policy/evaluations/list?view=azure-devops-rest-7.1
        const auto policy =
            get(project_api(r) + "/policy/evaluations?artifactId=" + url_escape(artifact) +
                    "&api-version=7.1-preview.1",
                token);
        if (!policy) return std::unexpected(policy.error());
        policies(*json::parse(*policy), p);
    }
    return p;
}

std::expected<commit_ci, std::string> checks(const forge_repo& r, std::string_view sha,
                                             std::string_view token)
{
    // https://learn.microsoft.com/en-us/rest/api/azure/devops/git/repositories/get-repository?view=azure-devops-rest-7.1
    const auto repo = get(repository_api(r) + "?" + std::string(version), token);
    if (!repo) return std::unexpected(repo.error());
    const auto repo_id = (*json::parse(*repo))["id"].str();
    if (repo_id.empty())
        return std::unexpected(std::string(_("Azure DevOps HTTP 200: repository.id is missing.")));
    std::map<std::int64_t, std::string> builds;
    std::string continuation;
    // Azure has no sourceVersion query parameter. Walk all returned pages,
    // taking the newest matching commit per definition, not the latest build
    // overall (which may be on a different commit).
    std::set<std::string> seen;
    do
    {
        // https://learn.microsoft.com/en-us/rest/api/azure/devops/build/builds/list?view=azure-devops-rest-7.1
        std::string url = project_api(r) + "/build/builds?repositoryId=" + url_escape(repo_id) +
                          "&repositoryType=TfsGit&queryOrder=queueTimeDescending&$top=100&" +
                          std::string(version);
        if (!continuation.empty()) url += "&continuationToken=" + url_escape(continuation);
        const auto reply =
            helper_get(url, {.headers = api_headers(forge_kind::azure, token)}, token);
        const auto j = json::parse(reply.body);
        if (reply.status != 200 || !j || (*j)["value"].kind() != json::value::type::array ||
            !reply.error.empty())
            return std::unexpected(response_error(reply, url, token));
        for (const json::value& v : (*j)["value"].elements())
            if (v["sourceVersion"].str_is(sha))
                builds.try_emplace(v["definition"]["id"].integer().value_or(0),
                                   std::string(v.text()));
        continuation.clear();
        for (const auto& [name, value] : reply.headers)
            if (std::ranges::equal(name, std::string_view("x-ms-continuationtoken"),
                                   [](unsigned char a, char b) { return std::tolower(a) == b; }))
                continuation = value;
        if (!continuation.empty() && !seen.insert(continuation).second)
            return std::unexpected(
                std::string(_("Azure DevOps HTTP 200: repeated build continuation token.")));
    } while (!continuation.empty());
    commit_ci out;
    for (const auto& [definition, text] : builds)
    {
        const auto b = *json::parse(text);
        const auto id = b["id"].integer().value_or(0);
        const std::string url = b["_links"]["web"]["href"].str();
        // https://learn.microsoft.com/en-us/rest/api/azure/devops/build/timeline/get?view=azure-devops-rest-7.1
        const auto timeline = get(build_root(r, id) + "/timeline?" + std::string(version), token);
        if (!timeline) return std::unexpected(timeline.error());
        const auto records = (*json::parse(*timeline))["records"];
        std::map<std::string, std::string> parents, stages;
        for (const json::value& record : records.elements())
        {
            const auto key = record["id"].str();
            parents[key] = record["parentId"].str();
            if (record["type"].str_is("Stage")) stages[key] = record["name"].str();
        }
        std::map<std::string, size_t> groups;
        for (const json::value& record : records.elements())
        {
            if (!record["type"].str_is("Job")) continue;
            std::string parent = record["parentId"].str(), stage;
            std::set<std::string> visited;
            while (!parent.empty() && visited.insert(parent).second)
            {
                if (stages.contains(parent))
                {
                    stage = stages[parent];
                    break;
                }
                if (!parents.contains(parent)) break;
                parent = parents[parent];
            }
            if (!groups.contains(stage))
            {
                groups[stage] = out.workflows.size();
                out.workflows.push_back(
                    {.name = b["definition"]["name"].str() + (stage.empty() ? "" : " / " + stage),
                     .url = url,
                     .run_id = id,
                     .run_number = id});
            }
            ci_job job{
                .name = record["name"].str(),
                .state = timeline_state(record),
                .started_at = time_of(record["startTime"]),
                .completed_at = time_of(record["finishTime"]),
                .url = url,
                .id = cache_id(r, id, record["id"].str(), record["attempt"].integer().value_or(1))};
            job.azure_build = id;
            job.azure_record = record["id"].str();
            for (const json::value& issue : record["issues"].elements())
                if (issue["type"].str_is("error"))
                    job.failure_reason += issue["message"].str() + "\n";
            out.workflows[groups[stage]].jobs.push_back(std::move(job));
        }
        if (groups.empty())
            out.workflows.push_back({.name = b["definition"]["name"].str(),
                                     .url = url,
                                     .run_id = id,
                                     .run_number = id,
                                     .jobs = {{.name = b["definition"]["name"].str(),
                                               .state = build_state(b),
                                               .started_at = time_of(b["startTime"]),
                                               .completed_at = time_of(b["finishTime"]),
                                               .url = url}}});
    }
    // https://learn.microsoft.com/en-us/rest/api/azure/devops/git/statuses/list?view=azure-devops-rest-7.1
    const auto statuses = get(repository_api(r) + "/commits/" + url_escape(sha) +
                                  "/statuses?latestOnly=true&" + std::string(version),
                              token);
    if (!statuses) return std::unexpected(statuses.error());
    ci_workflow external;
    std::set<std::string> contexts;
    for (const json::value& v : (*json::parse(*statuses))["value"].elements())
    {
        const auto name = v["context"]["genre"].str() + "/" + v["context"]["name"].str();
        if (!contexts.insert(name).second) continue;
        const auto word = v["state"].str();
        external.jobs.push_back({.name = name,
                                 .state = status_state(word),
                                 .completed_at = time_of(v["creationDate"]),
                                 .url = v["targetUrl"].str()});
    }
    if (!external.jobs.empty()) out.workflows.push_back(std::move(external));
    return out;
}

std::expected<std::vector<ci_artifact>, std::string>
artifacts(const forge_repo& r, const commit_ci& ci, std::string_view token)
{
    std::vector<ci_artifact> out;
    std::set<std::int64_t> seen;
    for (const auto& w : ci.workflows)
    {
        if (!w.run_id || !seen.insert(w.run_id).second) continue;
        // https://learn.microsoft.com/en-us/rest/api/azure/devops/build/artifacts/list?view=azure-devops-rest-7.1
        const auto body =
            get(build_root(r, w.run_id) + "/artifacts?" + std::string(version), token);
        if (!body) return std::unexpected(body.error());
        for (const json::value& v : (*json::parse(*body))["value"].elements())
        {
            const auto download = v["resource"]["downloadUrl"].str();
            if (download.empty()) continue;
            out.push_back(
                {.name = v["name"].str(), .workflow = w.name, .size = 0, .download_url = download});
        }
    }
    return out;
}

std::expected<job_failure, std::string> failure_details(const forge_repo& r, const ci_job& job,
                                                        std::string_view token)
{
    job_failure out{.log_url = job.url};
    if (job.azure_record.empty()) return out;
    // https://learn.microsoft.com/en-us/rest/api/azure/devops/build/timeline/get?view=azure-devops-rest-7.1
    const auto body =
        get(build_root(r, job.azure_build) + "/timeline?" + std::string(version), token);
    if (!body) return std::unexpected(body.error());
    const auto records = (*json::parse(*body))["records"];
    std::map<std::string, std::string> parents;
    for (const json::value& v : records.elements()) parents[v["id"].str()] = v["parentId"].str();
    for (const json::value& v : records.elements())
    {
        auto parent = v["id"].str();
        std::set<std::string> visited;
        while (parent != job.azure_record && parents.contains(parent) &&
               visited.insert(parent).second)
            parent = parents[parent];
        if (parent != job.azure_record || timeline_state(v) != check_state::failure) continue;
        if (v["type"].str_is("Task")) out.step = v["name"].str();
        for (const json::value& issue : v["issues"].elements())
            if (issue["type"].str_is("error"))
                out.annotations.push_back({.message = issue["message"].str()});
        const auto log_id = v["log"]["id"].integer().value_or(0);
        if (log_id)
        {
            std::int64_t lines = 0;
            // https://learn.microsoft.com/en-us/rest/api/azure/devops/build/builds/get-build-logs?view=azure-devops-rest-7.1
            const auto logs =
                get(build_root(r, job.azure_build) + "/logs?" + std::string(version), token);
            if (!logs) return std::unexpected(logs.error());
            if (logs)
                for (const json::value& log : (*json::parse(*logs))["value"].elements())
                    if (log["id"].integer().value_or(0) == log_id)
                        lines = log["lineCount"].integer().value_or(0);
            auto headers = api_headers(forge_kind::azure, token);
            for (auto& [name, value] : headers)
                if (name == "Accept") value = "text/plain";
            // https://learn.microsoft.com/en-us/rest/api/azure/devops/build/builds/get-build-log?view=azure-devops-rest-7.1
            const auto reply = helper_get(
                build_root(r, job.azure_build) + "/logs/" + std::to_string(log_id) +
                    "?startLine=" + std::to_string(std::max<std::int64_t>(0, lines - 100)) + "&" +
                    std::string(version),
                {.headers = std::move(headers), .max_bytes = size_t(1) << 20}, token);
            if (reply.status != 200 || !reply.error.empty())
                return std::unexpected(
                    response_error(reply, build_root(r, job.azure_build), token));
            out.log_tail = log_tail(reply.body, 80);
        }
    }
    return out;
}
}  // namespace tortilla::azure
