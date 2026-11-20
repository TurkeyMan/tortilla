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

std::string repository_id(const json::value& repository)
{
    auto id = repository["id"].str();
    std::ranges::transform(id, id.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return id;
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
    return "https://dev.azure.com/" + url_escape(org(r)) +
           (slash == std::string::npos ? "" : "/" + url_escape(r.owner.substr(slash + 1))) +
           "/_git/" + url_escape(r.name);
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

std::expected<forge_repo, std::string> with_project(const forge_repo& r, const json::value& repo)
{
    if (r.owner.find('/') != std::string::npos) return r;
    // GitRepository.project is documented by Repositories/Get Repository.
    auto project = repo["project"]["id"].str();
    if (project.empty()) project = repo["project"]["name"].str();
    if (project.empty())
        return std::unexpected(std::string(_("Azure DevOps HTTP 200: repository.project is missing.")));
    auto scoped = r;
    scoped.owner = org(r) + "/" + project;
    return scoped;
}

std::expected<forge_repo, std::string> resolve_project(const forge_repo& r, std::string_view token)
{
    if (r.owner.find('/') != std::string::npos) return r;
    // https://learn.microsoft.com/en-us/rest/api/azure/devops/git/repositories/get-repository?view=azure-devops-rest-7.1
    const auto body = get(repository_api(r) + "?" + std::string(version), token);
    if (!body) return std::unexpected(body.error());
    return with_project(r, *json::parse(*body));
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
    if (const auto id = repository_id(v["repository"]); !id.empty()) p.head.azure_id = id;
    if (const auto source = v["forkSource"]; source && !source.is_null())
    {
        // A declared fork is never the target merely because source metadata
        // is omitted. Only explicit source URL/identity may match a remote.
        p.head = {};
        const auto fork = v["forkSource"]["repository"];
        if (const auto other = repo_of_url(fork["remoteUrl"].str())) p.head = *other;
        p.head.azure_id = repository_id(fork);
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

namespace
{
bool valid_branch(std::string_view name)
{
    if (name.empty() || name.starts_with('/') || name.ends_with('/') || name.ends_with('.') ||
        name.find("..") != name.npos || name.find("@{") != name.npos || name == "@")
        return false;
    while (!name.empty())
    {
        const auto slash = name.find('/');
        const auto part = name.substr(0, slash);
        if (part.empty() || part.starts_with('.') || part.ends_with(".lock")) return false;
        for (const unsigned char c : part)
            if (c <= 32 || c == 127 || std::string_view("~^:?*[\\").find(char(c)) != name.npos)
                return false;
        if (slash == name.npos) break;
        name.remove_prefix(slash + 1);
    }
    return true;
}

bool object_id(std::string_view id)
{
    return id.size() == 40 &&
           std::ranges::all_of(id,
                               [](unsigned char c) {
                                   return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                                          (c >= 'A' && c <= 'F');
                               }) &&
           id != std::string(40, '0');
}

std::string pr_url(const forge_repo& r, int number)
{
    return repository_api(r) + "/pullrequests/" + std::to_string(number);
}

// Only GETs negotiate helper authentication. A mutation is sent exactly once,
// including when its response is lost: a retry could duplicate a PR or comment.
std::expected<std::string, std::string> write(std::string_view method, const std::string& url,
                                              const std::string& body, std::string_view token,
                                              int expected_status)
{
    if (token.empty()) return std::unexpected(std::string(_("Sign in to Azure DevOps first.")));
    if (auto why = helper_preflight(token); !why.empty()) return std::unexpected(std::move(why));
    auto headers = api_headers(forge_kind::azure, token);
    headers.emplace_back("Content-Type", "application/json");
    const auto reply = forge_detail::http_request(method, url, body, {.headers = headers});
    const auto parsed = json::parse(reply.body);
    if (reply.status != expected_status || !reply.error.empty() || html_response(reply) || !parsed ||
        parsed->kind() != json::value::type::object)
        return std::unexpected(
            response_error(reply, url, token) +
            ". Required scope: Code (Read & Write); check repository permissions. "
            "The write was not retried; check Azure before trying again.");
    return reply.body;
}
}  // namespace

std::expected<std::string, std::string> default_branch(const forge_repo& r, std::string_view token)
{
    // https://learn.microsoft.com/en-us/rest/api/azure/devops/git/repositories/get-repository?view=azure-devops-rest-7.1
    const auto reply = get(repository_api(r) + "?api-version=7.1", token);
    if (!reply) return std::unexpected(reply.error());
    const auto ref = (*json::parse(*reply))["defaultBranch"].str();
    if (!ref.starts_with("refs/heads/") || !valid_branch(std::string_view(ref).substr(11)))
        return std::unexpected(std::string(_("Azure DevOps HTTP 200: no valid default branch.")));
    return ref.substr(11);
}

std::expected<pull_request, std::string> create(const forge_repo& r, const new_pull_request& n,
                                                std::string_view token)
{
    if (!(r == n.head))
        return std::unexpected(
            std::string(_("Azure DevOps pull requests between forks are not supported; choose "
                          "branches in the same repository.")));
    if (!valid_branch(n.head_branch) || !valid_branch(n.base_branch) ||
        n.head_branch == n.base_branch || n.title.empty())
        return std::unexpected(
            std::string(_("Choose distinct valid source and target branches and a title.")));
    const auto body = "{\"sourceRefName\":" + json::quoted("refs/heads/" + n.head_branch) +
                      ",\"targetRefName\":" + json::quoted("refs/heads/" + n.base_branch) +
                      ",\"title\":" + json::quoted(n.title) +
                      ",\"description\":" + json::quoted(n.body) +
                      ",\"isDraft\":" + (n.draft ? "true" : "false") + "}";
    // https://learn.microsoft.com/en-us/rest/api/azure/devops/git/pull-requests/create?view=azure-devops-rest-7.1
    const auto reply =
        write("POST", repository_api(r) + "/pullrequests?api-version=7.1", body, token, 201);
    if (!reply) return std::unexpected(reply.error());
    auto pr = parse_pr(*json::parse(*reply), r, true);
    if (pr.number <= 0)
        return std::unexpected(std::string(
            _("Azure DevOps HTTP 201: missing pullRequestId; check Azure before creating again.")));
    return pr;
}

std::expected<merge_state, std::string> merge_status(const forge_repo& r, int number,
                                                     std::string_view token)
{
    if (number <= 0) return std::unexpected(std::string(_("Invalid Azure pull request number.")));
    // https://learn.microsoft.com/en-us/rest/api/azure/devops/git/pull-requests/get-pull-request?view=azure-devops-rest-7.1
    const auto reply = get(pr_url(r, number) + "?api-version=7.1", token);
    if (!reply) return std::unexpected(reply.error());
    const auto j = *json::parse(*reply);
    const auto full = details(r, parse_pr(j, r, true), token);
    if (!full) return std::unexpected(full.error());
    merge_state out;
    out.title = full->title;
    out.head_sha = full->head_sha;
    out.methods = {merge_method::merge, merge_method::squash, merge_method::rebase};
    out.can_merge_when_ready = true;
    out.merge_when_ready_set = !j["autoCompleteSetBy"]["id"].str().empty();
    const auto strategy = j["completionOptions"]["mergeStrategy"].str();
    if (strategy == "squash") out.preferred = merge_method::squash;
    if (strategy == "rebase") out.preferred = merge_method::rebase;
    // rebaseMerge is distinct from the UI's rebase/fast-forward option; never silently select it.
    using v = merge_state::verdict;
    if (!j["status"].str_is("active") || full->draft || full->conflicts)
        out.state = v::blocked,
        out.why = _("The pull request is inactive, draft, or has conflicts.");
    else if (!object_id(out.head_sha) || !full->merge_known || !full->checks_known)
        out.state = v::checking;
    else if (full->blocked && full->checks != check_state::pending)
        out.state = v::blocked, out.why = _("Azure DevOps policies prevent completion.");
    else if (full->checks == check_state::pending)
        out.state = v::waiting;
    else if (full->checks == check_state::failure)
        out.state = v::blocked, out.why = _("Azure DevOps policies failed.");
    else
        out.state = v::ready;
    return out;
}

std::string merge(const forge_repo& r, int number, const merge_request& m, std::string_view token)
{
    if (number <= 0 || !object_id(m.head_sha))
        return _("A valid expected head commit is required.");
    std::string strategy;
    switch (m.method)
    {
    case merge_method::merge: strategy = "noFastForward"; break;
    case merge_method::squash: strategy = "squash"; break;
    case merge_method::rebase: strategy = "rebase"; break;
    }
    if (strategy.empty()) return _("Unsupported Azure DevOps merge strategy.");
    std::string options = "{\"mergeStrategy\":" + json::quoted(strategy);
    if (!m.commit_title.empty() || !m.commit_message.empty())
        options += ",\"mergeCommitMessage\":" +
                   json::quoted(m.commit_title +
                                (m.commit_title.empty() || m.commit_message.empty() ? "" : "\n\n") +
                                m.commit_message);
    options += "}";  // No bypassPolicy or optional-policy exclusions: Azure enforces its policies.
    std::string body = "{\"lastMergeSourceCommit\":{\"commitId\":" + json::quoted(m.head_sha) +
                       "},\"completionOptions\":" + options;
    if (m.when_ready)
    {
        const auto user = identity(account_host(r), token);
        if (!user) return user.error();
        if (user->login.empty()) return _("Azure DevOps did not identify the signed-in user.");
        body += ",\"autoCompleteSetBy\":{\"id\":" + json::quoted(user->login) + "}";
    }
    else
        body += ",\"status\":\"completed\"";
    body += "}";
    // https://learn.microsoft.com/en-us/rest/api/azure/devops/git/pull-requests/update?view=azure-devops-rest-7.1
    const auto reply = write("PATCH", pr_url(r, number) + "?api-version=7.1", body, token, 200);
    if (!reply) return reply.error();
    const auto j = *json::parse(*reply);
    if (m.when_ready ? j["autoCompleteSetBy"]["id"].str().empty()
                     : !j["status"].str_is("completed"))
        return _("Azure DevOps did not confirm completion or auto-complete; check the PR before "
                 "retrying.");
    return {};
}

std::string close(const forge_repo& r, int number, std::string_view reason, std::string_view token)
{
    if (number <= 0) return _("Invalid Azure pull request number.");
    if (!reason.empty())
    {
        const auto body =
            "{\"comments\":[{\"parentCommentId\":0,\"content\":" + json::quoted(reason) +
            ",\"commentType\":1}],\"status\":1}";
        // https://learn.microsoft.com/en-us/rest/api/azure/devops/git/pull-request-threads/create?view=azure-devops-rest-7.1
        const auto reply =
            write("POST", pr_url(r, number) + "/threads?api-version=7.1", body, token, 200);
        if (!reply)
            return "The reason could not be confirmed; the PR was not abandoned. " + reply.error();
        if ((*json::parse(*reply))["id"].integer().value_or(0) <= 0)
            return _("Azure DevOps HTTP 200: no thread ID; the PR was not abandoned. Check Azure "
                     "before posting again.");
    }
    // https://learn.microsoft.com/en-us/rest/api/azure/devops/git/pull-requests/update?view=azure-devops-rest-7.1
    const auto reply = write("PATCH", pr_url(r, number) + "?api-version=7.1",
                             "{\"status\":\"abandoned\"}", token, 200);
    if (!reply)
        return (reason.empty() ? ""
                               : "The reason was posted, but abandonment was not confirmed. ") +
               reply.error();
    if (!(*json::parse(*reply))["status"].str_is("abandoned"))
        return reason.empty() ? _("Azure DevOps did not confirm abandonment; check the PR.")
                              : _("The reason was posted, but Azure DevOps did not confirm "
                                  "abandonment; check the PR.");
    return {};
}

std::string delete_branch(const forge_repo& r, std::string_view name, std::string_view token)
{
    if (!valid_branch(name)) return _("Invalid Azure DevOps branch name.");
    const auto ref = "refs/heads/" + std::string(name);
    // https://learn.microsoft.com/en-us/rest/api/azure/devops/git/refs/list?view=azure-devops-rest-7.1
    const auto current =
        get(repository_api(r) + "/refs?filter=" + url_escape("heads/" + std::string(name)) +
                "&api-version=7.1",
            token);
    if (!current) return current.error();
    std::string sha;
    for (const json::value& v : (*json::parse(*current))["value"].elements())
        if (v["name"].str() == ref) sha = v["objectId"].str();
    if (!object_id(sha))
        return _("Azure DevOps did not return the exact branch and current commit; nothing was "
                 "deleted.");
    const auto body = "[{\"name\":" + json::quoted(ref) + ",\"oldObjectId\":" + json::quoted(sha) +
                      ",\"newObjectId\":\"0000000000000000000000000000000000000000\"}]";
    // https://learn.microsoft.com/en-us/rest/api/azure/devops/git/refs/update-refs?view=azure-devops-rest-7.1
    const auto reply = write("POST", repository_api(r) + "/refs?api-version=7.1", body, token, 200);
    if (!reply) return reply.error();
    for (const json::value& v : (*json::parse(*reply))["value"].elements())
        if (v["name"].str() == ref)
        {
            const auto state = v["updateStatus"].str();
            if (v["success"].boolean() &&
                (state == "succeeded" || state == "succeededNonExistentRef" ||
                 state == "succeededCorruptRef"))
                return {};
            // Reuse bounded/redacted message handling even for an HTTP 200 per-ref failure.
            rap::sys::http_response failed;
            failed.status = 200;
            failed.body =
                "{\"message\":" + json::quoted(state + ": " + v["customMessage"].str()) + "}";
            return response_error(failed, repository_api(r), token) +
                   ". Required scope: Code (Read & Write), plus branch deletion/Force push "
                   "permission.";
        }
    return _("Azure DevOps HTTP 200: missing ref update result; check Azure before retrying.");
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
    if (h == "dev.azure.com" && p.size() == 3 && p[1] == "_git")
        organization = p[0], name = p[2];
    else if (h == "dev.azure.com" && p.size() == 4 && p[2] == "_git")
        organization = p[0], project = p[1], name = p[3];
    else if ((h == "ssh.dev.azure.com" || h == "vs-ssh.visualstudio.com") && p.size() == 4 &&
             p[0] == "v3")
        organization = p[1], project = p[2], name = p[3];
    else if (h.ends_with(".visualstudio.com"))
    {
        if (!p.empty() && p.front() == "DefaultCollection") p.erase(p.begin());
        organization = h.substr(0, h.size() - std::string_view(".visualstudio.com").size());
        if (p.size() == 2 && p[0] == "_git") name = p[1];
        else if (p.size() == 3 && p[1] == "_git") project = p[0], name = p[2];
        else return {};
    }
    if (organization.empty() || name.empty()) return {};
    if (!std::ranges::all_of(organization,
                             [](unsigned char c) {
                                 return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                        (c >= '0' && c <= '9') || c == '-';
                             }))
        return {};
    std::ranges::transform(organization, organization.begin(),
                           [](unsigned char c) { return char(std::tolower(c)); });
    return forge_repo{.host = "dev.azure.com",
                      .owner = organization + (project.empty() ? "" : "/" + project), .name = name};
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

rap::sys::http_response redirected_write(int status, std::string_view url)
{
    rap::sys::http_response r;
    r.url = std::string(url);
    r.error = rap::i18n::format(_("Azure DevOps answered the change with a redirect (HTTP {0}); "
                                  "it wasn't followed or sent again"),
                                std::to_string(status));
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
    const auto slash = r.owner.find('/');
    return "https://" + account_host(r) +
           (slash == std::string::npos ? "" : "/" + url_escape(r.owner.substr(slash + 1))) + "/_apis";
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
    // Resolve projectless and qualified clone aliases using server identity,
    // not repository names (which are not unique across projects).
    // https://learn.microsoft.com/en-us/rest/api/azure/devops/git/repositories/get-repository?view=azure-devops-rest-7.1
    const auto repository = get(repository_api(r) + "?" + std::string(version), token);
    auto resolved = r;
    if (repository)
        if (const auto id = repository_id(*json::parse(*repository)); !id.empty())
            resolved.azure_id = id;
    // https://learn.microsoft.com/en-us/rest/api/azure/devops/git/pull-requests/get-pull-requests?view=azure-devops-rest-7.1
    const auto reply =
        helper_get(repository_api(r) + "/pullrequests?searchCriteria.status=active&$top=100&" +
                       std::string(version),
                   {.headers = api_headers(forge_kind::azure, token)}, token);
    pull_requests_reply out;
    out.repository = resolved;
    const auto j = json::parse(reply.body);
    if (reply.status != 200 || !j || (*j)["value"].kind() != json::value::type::array ||
        !reply.error.empty())
        return std::unexpected(response_error(reply, repository_api(r), token));
    out.access = token.empty()
                     ? repo_access::read
                     : repo_access::unknown;  // Azure enforces write permissions at the action.
    for (const json::value& v : (*j)["value"].elements())
    {
        auto p = parse_pr(v, resolved, false);
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
            get(project_api({.host = r.host, .owner = org(r) + "/" + project, .name = r.name}) +
                    "/policy/evaluations?artifactId=" + url_escape(artifact) +
                    "&api-version=7.1-preview.1",
                token);
        if (!policy) return std::unexpected(policy.error());
        policies(*json::parse(*policy), p);
    }
    return p;
}

std::expected<commit_ci, std::string> checks(const forge_repo& remote, std::string_view sha,
                                             std::string_view token)
{
    // https://learn.microsoft.com/en-us/rest/api/azure/devops/git/repositories/get-repository?view=azure-devops-rest-7.1
    const auto repo = get(repository_api(remote) + "?" + std::string(version), token);
    if (!repo) return std::unexpected(repo.error());
    const auto scoped = with_project(remote, *json::parse(*repo));
    if (!scoped) return std::unexpected(scoped.error());
    const auto& r = *scoped;
    const auto repo_id = (*json::parse(*repo))["id"].str();
    if (repo_id.empty())
        return std::unexpected(std::string(_("Azure DevOps HTTP 200: repository.id is missing.")));
    std::map<std::int64_t, std::string> builds;
    std::set<std::pair<std::string, std::string>> pr_commits;
    bool correlation_asked = false;
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
        {
            bool matches = v["sourceVersion"].str_is(sha);
            // Build.SourceBranch for PR validation is refs/pull/N/merge;
            // sourceVersion is its generated merge, not the PR source tip.
            // https://learn.microsoft.com/en-us/azure/devops/pipelines/build/variables?view=azure-devops
            const auto branch = v["sourceBranch"].str();
            if (!matches && branch.starts_with("refs/pull/") && branch.ends_with("/merge"))
            {
                if (!correlation_asked)
                {
                    correlation_asked = true;
                    // This PR context list is bounded like pulls(), including closed PRs: only
                    // source tips matching this selection supply merge context.
                    // Optional correlation never discards direct commit builds.
                    // https://learn.microsoft.com/en-us/rest/api/azure/devops/git/pull-requests/get-pull-requests?view=azure-devops-rest-7.1
                    auto canonical = r;
                    canonical.name = repo_id;
                    const auto pulls = get(repository_api(canonical) +
                        "/pullrequests?searchCriteria.status=all&$top=100&" + std::string(version), token);
                    if (pulls)
                        for (const json::value& p : (*json::parse(*pulls))["value"].elements())
                        {
                            const auto number = p["pullRequestId"].integer().value_or(0);
                            const auto merge = p["lastMergeCommit"]["commitId"].str();
                            if (number > 0 && p["lastMergeSourceCommit"]["commitId"].str_is(sha) &&
                                !merge.empty())
                                pr_commits.emplace("refs/pull/" + std::to_string(number) + "/merge", merge);
                        }
                }
                matches = pr_commits.contains({branch, v["sourceVersion"].str()});
            }
            if (matches)
                builds.try_emplace(v["definition"]["id"].integer().value_or(0),
                                   std::string(v.text()));
        }
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
artifacts(const forge_repo& remote, const commit_ci& ci, std::string_view token)
{
    if (std::ranges::none_of(ci.workflows, [](const auto& w) { return w.run_id != 0; }))
        return std::vector<ci_artifact>{};
    const auto scoped = resolve_project(remote, token);
    if (!scoped) return std::unexpected(scoped.error());
    const auto& r = *scoped;
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

std::expected<job_failure, std::string> failure_details(const forge_repo& remote, const ci_job& job,
                                                        std::string_view token)
{
    job_failure out{.log_url = job.url};
    if (job.azure_record.empty()) return out;
    const auto scoped = resolve_project(remote, token);
    if (!scoped) return std::unexpected(scoped.error());
    const auto& r = *scoped;
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
