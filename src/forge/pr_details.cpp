#include "pr_details.h"

#include "forge/forge_detail.h"
#include "git/command.h"
#include "git/repo.h"
#include "i18n.h"
#include "util/json.h"

#include <algorithm>
#include <ctime>

namespace tortilla
{

using namespace forge_detail;

namespace
{

// As no one: REST, the PR (its description and who's asked to review) and
// its reviews, two requests of the hour's sixty, while plenty is left.
std::expected<pull_request, std::string> github_rest_details(const forge_repo& r, pull_request pr)
{
    if (!budget_allows(anonymous_budget(), 2, 20, std::int64_t(std::time(nullptr))))
        return std::unexpected(std::string(_("GitHub's allowance for asking as no one is short.")));
    const std::string root =
        repo_api(forge_kind::github, r) + "/pulls/" + std::to_string(pr.number);
    const auto headers = api_headers(forge_kind::github, {});
    const auto one = forge_detail::http_get(root, {.headers = headers});
    note_budget(one);
    const auto j = json::parse(one.body);
    if (one.status != 200 || !j) return std::unexpected(failure(one, j));
    pr.labels = decode_labels((*j)["labels"]);
    pr.body = (*j)["body"].str();
    pr.body_known = true;
    const auto reviews =
        forge_detail::http_get(root + "/reviews?per_page=50", {.headers = headers});
    note_budget(reviews);
    pr.reviews.clear();
    // Oldest first: the summary keeps each reviewer's latest say.
    if (const auto v = json::parse(reviews.body); v && reviews.status == 200)
        for (const json::value& r : v->elements())
        {
            const json::value state = r["state"];
            pr.reviews.push_back({.who = r["user"]["login"].str(),
                                  .say = state.str_is("APPROVED") ? pr_review::verdict::approved
                                         : state.str_is("CHANGES_REQUESTED")
                                             ? pr_review::verdict::changes_requested
                                             : pr_review::verdict::commented});
        }
    for (const json::value& who : (*j)["requested_reviewers"].elements())
        pr.reviews.push_back({.who = who["login"].str(), .say = pr_review::verdict::requested});
    for (const json::value& team : (*j)["requested_teams"].elements())
        pr.reviews.push_back({.who = team["name"].str(), .say = pr_review::verdict::requested});
    pr.reviews_known = true;
    return pr;
}

std::expected<pull_request, std::string> github_details(const forge_repo& r, pull_request pr,
                                                        std::string_view token)
{
    if (token.empty()) return github_rest_details(r, std::move(pr));
    constexpr std::string_view query =
        "query($owner:String!,$name:String!,$number:Int!){repository(owner:$owner,name:$name){"
        "pullRequest(number:$number){body labels(first:100){nodes{name color}} "
        "latestReviews(first:20){nodes{author{login} state}} "
        "reviewRequests(first:20){nodes{requestedReviewer{"
        "... on User{login} ... on Team{name} ... on Bot{login} ... on Mannequin{login}}}}}}}";
    const std::string body =
        "{\"query\":" + json::quoted(query) + ",\"variables\":{\"owner\":" + json::quoted(r.owner) +
        ",\"name\":" + json::quoted(r.name) + ",\"number\":" + std::to_string(pr.number) + "}}";
    auto headers = api_headers(forge_kind::github, token);
    headers.push_back({"Content-Type", "application/json"});
    const auto reply =
        forge_detail::http_request("POST", graphql_url(r.host), body, {.headers = headers});
    const auto j = json::parse(reply.body);
    if (reply.status != 200 || !j) return std::unexpected(failure(reply, j));
    if (const json::value errors = (*j)["errors"]; errors.kind() == json::value::type::array)
        for (const json::value& e : errors.elements())
            return std::unexpected(e["message"].str(_("GitHub refused the query.")));
    const json::value n = (*j)["data"]["repository"]["pullRequest"];
    pr.labels = decode_labels(n["labels"]["nodes"]);
    pr.body = n["body"].str();
    pr.body_known = true;
    // Each reviewer's latest say, then who's still asked to review.
    pr.reviews.clear();
    for (const json::value& v : n["latestReviews"]["nodes"].elements())
    {
        const json::value state = v["state"];
        pr.reviews.push_back({.who = v["author"]["login"].str(),
                              .say = state.str_is("APPROVED") ? pr_review::verdict::approved
                                     : state.str_is("CHANGES_REQUESTED")
                                         ? pr_review::verdict::changes_requested
                                         : pr_review::verdict::commented});
    }
    for (const json::value& q : n["reviewRequests"]["nodes"].elements())
    {
        const json::value who = q["requestedReviewer"];
        std::string name = who["login"].str();
        if (name.empty()) name = who["name"].str();
        if (!name.empty())
            pr.reviews.push_back({.who = std::move(name), .say = pr_review::verdict::requested});
    }
    pr.reviews_known = true;
    return pr;
}

std::expected<pull_request, std::string> gitlab_details(const forge_repo& r, pull_request pr,
                                                        std::string_view token)
{
    const auto reply = forge_detail::http_get(repo_api(forge_kind::gitlab, r) + "/merge_requests/" +
                                                  std::to_string(pr.number) + "/approvals",
                                              {.headers = api_headers(forge_kind::gitlab, token)});
    const auto j = json::parse(reply.body);
    if (reply.status != 200 || !j) return std::unexpected(failure(reply, j));
    // Approvals come after the reviewers asked: the summary lets them win.
    for (const json::value& a : (*j)["approved_by"].elements())
        pr.reviews.push_back(
            {.who = a["user"]["username"].str(), .say = pr_review::verdict::approved});
    pr.reviews_known = true;
    return pr;
}

std::expected<pull_request, std::string> gitea_details(const forge_repo& r, pull_request pr,
                                                       std::string_view token)
{
    const auto reply = forge_detail::http_get(repo_api(forge_kind::gitea, r) + "/pulls/" +
                                                  std::to_string(pr.number) + "/reviews?limit=50",
                                              {.headers = api_headers(forge_kind::gitea, token)});
    const auto j = json::parse(reply.body);
    if (reply.status != 200 || !j) return std::unexpected(failure(reply, j));
    // Oldest first: the summary keeps each reviewer's latest say. The ones
    // asked come from the list, before these.
    for (const json::value& v : j->elements())
    {
        const json::value state = v["state"];
        if (v["dismissed"].boolean() || v["stale"].boolean()) continue;
        pr.reviews.push_back(
            {.who = v["user"]["login"].str(),
             .say = state.str_is("APPROVED")          ? pr_review::verdict::approved
                    : state.str_is("REQUEST_CHANGES") ? pr_review::verdict::changes_requested
                    : state.str_is("REQUEST_REVIEW")  ? pr_review::verdict::requested
                                                      : pr_review::verdict::commented});
    }
    pr.reviews_known = true;
    return pr;
}

}  // namespace

std::expected<pull_request, std::string>
pull_request_details(forge_kind k, const forge_repo& into, pull_request pr, std::string_view token)
{
    switch (k)
    {
    case forge_kind::github:
        if (pr.body_known && pr.reviews_known) return pr;
        return github_details(into, std::move(pr), token);
    case forge_kind::gitlab:
        if (pr.reviews_known) return pr;
        return gitlab_details(into, std::move(pr), token);
    case forge_kind::gitea:
        if (pr.reviews_known) return pr;
        return gitea_details(into, std::move(pr), token);
    }
    return pr;
}

review_summary summarise_reviews(const std::vector<pr_review>& reviews)
{
    // Each reviewer's say, in the order they first appear.
    std::vector<std::pair<std::string, pr_review::verdict>> says;
    for (const pr_review& r : reviews)
    {
        if (r.who.empty()) continue;
        auto it =
            std::ranges::find(says, r.who, &std::pair<std::string, pr_review::verdict>::first);
        if (it == says.end())
        {
            says.emplace_back(r.who, r.say);
            continue;
        }
        const bool decided = it->second == pr_review::verdict::approved ||
                             it->second == pr_review::verdict::changes_requested;
        if (r.say == pr_review::verdict::commented && decided) continue;
        it->second = r.say;
    }
    review_summary out;
    for (auto& [who, say] : says) switch (say)
        {
        case pr_review::verdict::approved: out.approved.push_back(who); break;
        case pr_review::verdict::changes_requested: out.changes_requested.push_back(who); break;
        case pr_review::verdict::commented: out.commented.push_back(who); break;
        case pr_review::verdict::requested: out.waiting.push_back(who); break;
        }
    return out;
}

std::vector<std::string> pull_request_commits(const std::string& workdir,
                                              const std::string& head_ref,
                                              const std::string& base_ref, size_t limit)
{
    std::vector<std::string> out;
    // In process, newest first as rev-list gives them; git when libgit2
    // can't open the repository.
    if (auto r = repo::open(workdir))
    {
        const auto head = r->resolve(head_ref), base = r->resolve(base_ref);
        if (!head || !base) return out;
        for (const oid& id : r->rev_list({*head}, {*base}, limit)) out.push_back(id.hex());
        return out;
    }
    const git_result r = run_git(
        workdir,
        {"rev-list", "--max-count=" + std::to_string(limit), head_ref, "^" + base_ref, "--"},
        git_output::stdout_only);
    if (!r.ok()) return out;
    for (std::string_view rest = r.output; !rest.empty();)
    {
        const size_t end = rest.find('\n');
        const std::string_view line = rest.substr(0, end);
        rest.remove_prefix(end == std::string_view::npos ? rest.size() : end + 1);
        if (!line.empty()) out.emplace_back(line);
    }
    return out;
}

const forge_remote* remote_with_commit(const std::string& workdir, std::string_view sha,
                                       const std::vector<forge_remote>& remotes)
{
    if (remotes.empty() || sha.empty()) return nullptr;
    // In process (the forge panel asks as the log opens): the first remote
    // with a branch whose history holds it, as for-each-ref --contains.
    if (auto r = repo::open(workdir))
    {
        const auto id = r->resolve(std::string(sha));
        if (!id) return nullptr;
        for (const forge_remote& f : remotes)
            if (const auto refs = r->refs_under({"refs/remotes/" + f.name + "/"}))
                for (const repo::listed_ref& l : *refs)
                    if (r->is_ancestor(*id, l.target)) return &f;
        return nullptr;
    }
    std::vector<std::string> args{"for-each-ref", "--format=%(refname)", "--contains",
                                  std::string(sha)};
    for (const forge_remote& r : remotes) args.push_back("refs/remotes/" + r.name + "/");
    const git_result found = run_git(workdir, args, git_output::stdout_only);
    if (!found.ok()) return nullptr;
    for (const forge_remote& r : remotes)
    {
        const std::string prefix = "refs/remotes/" + r.name + "/";
        for (std::string_view rest = found.output; !rest.empty();)
        {
            const size_t end = rest.find('\n');
            const std::string_view line = rest.substr(0, end);
            rest.remove_prefix(end == std::string_view::npos ? rest.size() : end + 1);
            // Not <remote>/HEAD alone: it's one of the branches anyway.
            if (line.starts_with(prefix)) return &r;
        }
    }
    return nullptr;
}

}  // namespace tortilla
