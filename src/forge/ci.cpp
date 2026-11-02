#include "ci.h"

#include "forge/forge_detail.h"
#include "i18n.h"
#include "util/json.h"

#include <algorithm>
#include <ctime>
#include <fstream>

namespace tortilla
{

using namespace forge_detail;

namespace
{

// Its id as JSON has it (a number), for a URL.
std::string id_text(std::int64_t id)
{
    return std::to_string(id);
}

rap::sys::http_response get(forge_kind k, const std::string& url, std::string_view token,
                            std::size_t max_bytes = std::size_t(4) << 20)
{
    return rap::sys::http_get(url, {.headers = api_headers(k, token), .max_bytes = max_bytes});
}

// -- GitHub -------------------------------------------------------------------

std::expected<commit_ci, std::string> github_checks(const forge_repo& r, std::string_view sha,
                                                    std::string_view token)
{
    // The commit's check suites (a workflow run each, or an app's), their
    // runs, and the statuses set on it: one request.
    constexpr std::string_view query =
        "query($owner:String!,$name:String!,$oid:GitObjectID!){repository(owner:$owner,name:$name){"
        "object(oid:$oid){... on Commit{"
        "checkSuites(first:30){nodes{app{name} workflowRun{workflow{name} runNumber databaseId "
        "url} "
        "checkRuns(first:100){nodes{__typename name status conclusion startedAt completedAt "
        "detailsUrl databaseId}}}} "
        "status{contexts{__typename context state createdAt targetUrl}}}}}}";
    const std::string body =
        "{\"query\":" + json::quoted(query) + ",\"variables\":{\"owner\":" + json::quoted(r.owner) +
        ",\"name\":" + json::quoted(r.name) + ",\"oid\":" + json::quoted(sha) + "}}";
    auto headers = api_headers(forge_kind::github, token);
    headers.push_back({"Content-Type", "application/json"});
    const auto reply =
        rap::sys::http_request("POST", graphql_url(r.host), body, {.headers = headers});
    const auto j = json::parse(reply.body);
    if (reply.status != 200 || !j) return std::unexpected(failure(reply, j));
    if (const json::value errors = (*j)["errors"]; errors.kind() == json::value::type::array)
        for (const json::value& e : errors.elements())
            return std::unexpected(e["message"].str(_("GitHub refused the query.")));
    const json::value commit = (*j)["data"]["repository"]["object"];
    commit_ci out;
    for (const json::value& suite : commit["checkSuites"]["nodes"].elements())
    {
        const json::value run = suite["workflowRun"];
        ci_workflow w{.name = run ? run["workflow"]["name"].str() : suite["app"]["name"].str(),
                      .url = run["url"].str(),
                      .run_id = run["databaseId"].integer().value_or(0),
                      .run_number = run["runNumber"].integer().value_or(0)};
        for (const json::value& c : suite["checkRuns"]["nodes"].elements())
        {
            const check_run cr = github_run(c);
            w.jobs.push_back({.name = cr.name,
                              .state = cr.state,
                              .started_at = cr.started_at,
                              .completed_at = cr.completed_at,
                              .url = cr.url,
                              .id = c["databaseId"].integer().value_or(0)});
        }
        // An app that was asked but ran nothing (a service catalogue).
        if (!w.jobs.empty()) out.workflows.push_back(std::move(w));
    }
    ci_workflow statuses;
    for (const json::value& c : commit["status"]["contexts"].elements())
    {
        const check_run cr = github_run(c);
        statuses.jobs.push_back({.name = cr.name,
                                 .state = cr.state,
                                 .started_at = cr.started_at,
                                 .completed_at = cr.completed_at,
                                 .url = cr.url});
    }
    if (!statuses.jobs.empty()) out.workflows.push_back(std::move(statuses));
    return out;
}

// As no one (GraphQL wants a token): REST, the commit's check runs and its
// workflow runs to name them, two requests of the hour's sixty, only while
// plenty is left.
std::expected<commit_ci, std::string> github_rest_checks(const forge_repo& r, std::string_view sha)
{
    if (!budget_allows(anonymous_budget(), 2, 20, std::int64_t(std::time(nullptr))))
        return std::unexpected(std::string(_("GitHub's allowance for asking as no one is short.")));
    const std::string root = repo_api(forge_kind::github, r);
    const auto runs = get(forge_kind::github,
                          root + "/commits/" + std::string(sha) + "/check-runs?per_page=100", {});
    note_budget(runs);
    const auto j = json::parse(runs.body);
    if (runs.status != 200 || !j) return std::unexpected(failure(runs, j));
    const auto flows = get(forge_kind::github,
                           root + "/actions/runs?per_page=50&head_sha=" + std::string(sha), {});
    note_budget(flows);
    const auto f = json::parse(flows.body);
    commit_ci out;
    for (const json::value& c : (*j)["check_runs"].elements())
    {
        // Its workflow run, by its check suite; an app's runs on their own.
        const std::int64_t suite = c["check_suite"]["id"].integer().value_or(0);
        ci_workflow found{.name = c["app"]["name"].str()};
        if (f && flows.status == 200)
            for (const json::value& run : (*f)["workflow_runs"].elements())
                if (run["check_suite_id"].integer().value_or(-1) == suite)
                    found = {.name = run["name"].str(),
                             .url = run["html_url"].str(),
                             .run_id = run["id"].integer().value_or(0),
                             .run_number = run["run_number"].integer().value_or(0)};
        auto w = std::ranges::find(out.workflows, found.name, &ci_workflow::name);
        if (w == out.workflows.end())
        {
            out.workflows.push_back(std::move(found));
            w = out.workflows.end() - 1;
        }
        const json::value conclusion = c["conclusion"];
        ci_job job{.name = c["name"].str(),
                   .started_at = time_of(c["started_at"]),
                   .completed_at = time_of(c["completed_at"]),
                   .url = c["html_url"].str(),
                   .id = c["id"].integer().value_or(0)};
        if (!c["status"].str_is("completed"))
            job.state = check_state::pending;
        else if (conclusion.str_is("success") || conclusion.str_is("neutral") ||
                 conclusion.str_is("skipped"))
            job.state = check_state::success;
        else
            job.state = check_state::failure;
        w->jobs.push_back(std::move(job));
    }
    return out;
}

std::expected<std::vector<ci_artifact>, std::string>
github_artifacts(const forge_repo& r, const commit_ci& ci, std::string_view token)
{
    // Downloading one takes a token: as no one, none are listed.
    if (token.empty()) return std::vector<ci_artifact>{};
    std::vector<ci_artifact> out;
    for (const ci_workflow& w : ci.workflows)
    {
        if (!w.run_id) continue;
        const auto reply = get(forge_kind::github,
                               repo_api(forge_kind::github, r) + "/actions/runs/" +
                                   id_text(w.run_id) + "/artifacts?per_page=100",
                               token);
        const auto j = json::parse(reply.body);
        if (reply.status != 200 || !j) return std::unexpected(failure(reply, j));
        for (const json::value& a : (*j)["artifacts"].elements())
            out.push_back({.name = a["name"].str(),
                           .workflow = w.name,
                           .size = a["size_in_bytes"].integer().value_or(0),
                           .expired = a["expired"].boolean(),
                           .download_url = a["archive_download_url"].str()});
    }
    return out;
}

std::expected<job_failure, std::string> github_failure(const forge_repo& r, const ci_job& job,
                                                       std::string_view token)
{
    job_failure out{.log_url = job.url};
    if (!job.id) return out;  // a status: its page is all there is
    const std::string root = repo_api(forge_kind::github, r);
    // The job's steps: which failed. (A check run that isn't an Actions
    // job has none: a 404, and only its annotations.)
    const auto steps_reply =
        get(forge_kind::github, root + "/actions/jobs/" + id_text(job.id), token);
    const auto steps = json::parse(steps_reply.body);
    const bool actions_job = steps && steps_reply.status == 200;
    if (actions_job)
        for (const json::value& s : (*steps)["steps"].elements())
            if (s["conclusion"].str_is("failure") || s["conclusion"].str_is("timed_out") ||
                s["conclusion"].str_is("cancelled"))
            {
                out.step = s["name"].str();
                out.step_number = int(s["number"].integer().value_or(0));
                break;
            }
    const auto notes_reply =
        get(forge_kind::github,
            root + "/check-runs/" + id_text(job.id) + "/annotations?per_page=50", token);
    if (const auto notes = json::parse(notes_reply.body); notes && notes_reply.status == 200)
        for (const json::value& a : notes->elements())
            if (a["annotation_level"].str_is("failure"))
            {
                ci_annotation n{.path = a["path"].str(),
                                .line = int(a["start_line"].integer().value_or(0)),
                                .message = a["message"].str(),
                                .url = a["blob_href"].str()};
                // Actions' own notes name the workflow file as ".github".
                if (n.path == ".github")
                {
                    n.path.clear();
                    n.line = 0;
                    n.url.clear();
                }
                out.annotations.push_back(std::move(n));
            }
    // The log takes a token, even a public repository's.
    if (!actions_job || token.empty()) return out;
    // The log redirects to a signed address elsewhere; libcurl doesn't send
    // the token there (another host), and the signed address needs none.
    const auto log = get(forge_kind::github, root + "/actions/jobs/" + id_text(job.id) + "/logs",
                         token, std::size_t(64) << 20);
    if (log.status == 200) out.log_tail = log_tail(log.body, 40);
    return out;
}

// -- GitLab -------------------------------------------------------------------

std::string gitlab_project(const forge_repo& r)
{
    return repo_api(forge_kind::gitlab, r);
}

// The commit's latest pipeline's jobs, as JSON (the reply's body), and the
// pipeline's page.
struct gitlab_jobs
{
    std::string body, pipeline_url;
};
std::expected<gitlab_jobs, std::string>
gitlab_pipeline_jobs(const forge_repo& r, std::string_view sha, std::string_view token)
{
    const auto pipes =
        get(forge_kind::gitlab, gitlab_project(r) + "/pipelines?per_page=1&sha=" + std::string(sha),
            token);
    const auto p = json::parse(pipes.body);
    if (pipes.status != 200 || !p) return std::unexpected(failure(pipes, p));
    const json::value latest = (*p)[size_t(0)];
    if (!latest) return gitlab_jobs{};
    const auto jobs = get(forge_kind::gitlab,
                          gitlab_project(r) + "/pipelines/" + std::string(latest["id"].text()) +
                              "/jobs?per_page=100",
                          token);
    const auto j = json::parse(jobs.body);
    if (jobs.status != 200 || !j) return std::unexpected(failure(jobs, j));
    return gitlab_jobs{jobs.body, latest["web_url"].str()};
}

std::expected<commit_ci, std::string> gitlab_checks(const forge_repo& r, std::string_view sha,
                                                    std::string_view token)
{
    auto jobs = gitlab_pipeline_jobs(r, sha, token);
    if (!jobs) return std::unexpected(jobs.error());
    commit_ci out;
    const auto j = json::parse(jobs->body);
    if (!j) return out;
    // Newest first in GitLab's list: the stages as the pipeline runs them.
    std::vector<json::value> listed;
    for (const json::value& job : j->elements()) listed.push_back(job);
    std::ranges::reverse(listed);
    for (const json::value& job : listed)
    {
        const std::string stage = job["stage"].str();
        auto w = std::ranges::find(out.workflows, stage, &ci_workflow::name);
        if (w == out.workflows.end())
        {
            out.workflows.push_back({.name = stage, .url = jobs->pipeline_url});
            w = out.workflows.end() - 1;
        }
        const std::string status = job["status"].str();
        // A manual job left alone, or one skipped, passes nothing and fails
        // nothing.
        w->jobs.push_back({.name = job["name"].str(),
                           .state = status == "manual" || status == "skipped" ? check_state::none
                                                                              : status_word(status),
                           .started_at = time_of(job["started_at"]),
                           .completed_at = time_of(job["finished_at"]),
                           .url = job["web_url"].str(),
                           .id = job["id"].integer().value_or(0),
                           .failure_reason = job["failure_reason"].str()});
    }
    return out;
}

std::expected<std::vector<ci_artifact>, std::string>
gitlab_artifacts(const forge_repo& r, std::string_view sha, std::string_view token)
{
    auto jobs = gitlab_pipeline_jobs(r, sha, token);
    if (!jobs) return std::unexpected(jobs.error());
    std::vector<ci_artifact> out;
    const auto j = json::parse(jobs->body);
    if (!j) return out;
    for (const json::value& job : j->elements())
    {
        const json::value file = job["artifacts_file"];
        if (!file || file.is_null()) continue;
        const std::string name = job["name"].str();
        out.push_back({.name = name,
                       .workflow = job["stage"].str(),
                       .size = file["size"].integer().value_or(0),
                       // Kept until a date; once gone, the list stops naming it.
                       .expired = false,
                       .download_url = gitlab_project(r) + "/jobs/" +
                                       std::string(job["id"].text()) + "/artifacts"});
    }
    return out;
}

std::expected<job_failure, std::string> gitlab_failure(const forge_repo& r, const ci_job& job,
                                                       std::string_view token)
{
    job_failure out{.log_url = job.url};
    if (!job.id) return out;
    const auto trace =
        get(forge_kind::gitlab, gitlab_project(r) + "/jobs/" + id_text(job.id) + "/trace", token,
            std::size_t(64) << 20);
    if (trace.status != 200) return std::unexpected(failure(trace, std::nullopt));
    out.log_tail = log_tail(trace.body, 40);
    // The last section begun is the one it stopped in: its name, as the
    // job's page shows it ("step_script").
    const std::string_view body = trace.body;
    if (const size_t at = body.rfind("section_start:"); at != std::string_view::npos)
    {
        std::string_view rest = body.substr(at + 14);
        rest.remove_prefix(std::min(rest.size(), rest.find(':') + 1));
        out.step = std::string(rest.substr(0, rest.find_first_of("\r\n[")));
    }
    return out;
}

// -- Gitea and Forgejo --------------------------------------------------------

std::expected<commit_ci, std::string> gitea_checks(const forge_repo& r, std::string_view sha,
                                                   std::string_view token)
{
    const auto reply =
        get(forge_kind::gitea,
            repo_api(forge_kind::gitea, r) + "/commits/" + std::string(sha) + "/status", token);
    const auto s = json::parse(reply.body);
    if (reply.status != 200 || !s) return std::unexpected(failure(reply, s));
    ci_workflow w;
    for (const json::value& c : (*s)["statuses"].elements())
    {
        json::value word = c["status"];  // older Gitea: "state"
        if (!word) word = c["state"];
        ci_job job{.name = c["context"].str(),
                   .state = word.str_is("skipped") || word.str_is("warning")
                                ? check_state::success
                                : status_word(word.str()),
                   .url = c["target_url"].str()};
        if (job.state == check_state::pending)
            job.started_at = time_of(c["created_at"]);
        else
            job.completed_at = time_of(c["updated_at"]);
        w.jobs.push_back(std::move(job));
    }
    commit_ci out;
    if (!w.jobs.empty()) out.workflows.push_back(std::move(w));
    return out;
}

// Forgejo's Actions API (Codeberg's): the commit's runs, then each one's
// artifacts. Gitea before 1.24 has neither: nothing, without a word.
std::expected<std::vector<ci_artifact>, std::string>
gitea_artifacts(const forge_repo& r, std::string_view sha, std::string_view token)
{
    const std::string root = repo_api(forge_kind::gitea, r);
    const auto runs =
        get(forge_kind::gitea, root + "/actions/runs?head_sha=" + std::string(sha), token);
    const auto j = json::parse(runs.body);
    std::vector<ci_artifact> out;
    if (runs.status != 200 || !j) return out;
    for (const json::value& run : (*j)["workflow_runs"].elements())
    {
        const auto reply =
            get(forge_kind::gitea,
                root + "/actions/runs/" + std::string(run["id"].text()) + "/artifacts", token);
        const auto a = json::parse(reply.body);
        if (reply.status != 200 || !a) continue;
        // Forgejo's is a list; Gitea's an object holding one.
        const json::value list = a->kind() == json::value::type::array ? *a : (*a)["artifacts"];
        std::string workflow = run["workflow_id"].str();  // the file: "ci.yml"
        if (workflow.empty()) workflow = run["title"].str();
        for (const json::value& art : list.elements())
            out.push_back({.name = art["name"].str(),
                           .workflow = workflow,
                           .size = art["size_in_bytes"].integer().value_or(0),
                           .expired = art["expired"].boolean(),
                           .download_url = art["archive_download_url"].str()});
    }
    return out;
}

int rank(check_state s)
{
    switch (s)
    {
    case check_state::failure: return 0;
    case check_state::pending: return 1;
    default: return 2;
    }
}

}  // namespace

bool commit_ci::empty() const
{
    return std::ranges::all_of(workflows, [](const ci_workflow& w) { return w.jobs.empty(); });
}

check_state commit_ci::state() const
{
    bool any = false, running = false;
    for (const ci_workflow& w : workflows)
        for (const ci_job& j : w.jobs)
        {
            if (j.state == check_state::failure) return check_state::failure;
            running = running || j.state == check_state::pending;
            any = any || j.state != check_state::none;
        }
    return running ? check_state::pending : any ? check_state::success : check_state::none;
}

std::expected<commit_ci, std::string> commit_checks(forge_kind k, const forge_repo& r,
                                                    std::string_view sha, std::string_view token)
{
    std::expected<commit_ci, std::string> out;
    switch (k)
    {
    case forge_kind::github:
        out = token.empty() ? github_rest_checks(r, sha) : github_checks(r, sha, token);
        break;
    case forge_kind::gitlab: out = gitlab_checks(r, sha, token); break;
    case forge_kind::gitea: out = gitea_checks(r, sha, token); break;
    }
    if (out) order_failures_first(*out);
    return out;
}

std::expected<std::vector<ci_artifact>, std::string>
commit_artifacts(forge_kind k, const forge_repo& r, std::string_view sha, const commit_ci& ci,
                 std::string_view token)
{
    switch (k)
    {
    case forge_kind::github: return github_artifacts(r, ci, token);
    case forge_kind::gitlab: return gitlab_artifacts(r, sha, token);
    case forge_kind::gitea: return gitea_artifacts(r, sha, token);
    }
    return {};
}

std::string download_artifact(forge_kind k, const ci_artifact& a, const std::filesystem::path& to,
                              std::string_view token, const std::atomic<bool>* cancelled,
                              download_progress progress)
{
    // GitHub's redirects to a signed address on another host; rapport sends
    // the token only to the host it was asked of.
    const auto reply = rap::sys::http_download(a.download_url, to,
                                               {.headers = api_headers(k, token),
                                                .timeout = std::chrono::seconds(30),
                                                .stall = std::chrono::seconds(60),
                                                .cancelled = cancelled,
                                                .progress = std::move(progress)});
    if (reply.status < 200 || reply.status >= 300) return failure(reply, std::nullopt);
    return reply.error;  // "" when whole
}

std::expected<job_failure, std::string>
job_failure_details(forge_kind k, const forge_repo& r, const ci_job& job, std::string_view token)
{
    switch (k)
    {
    case forge_kind::github: return github_failure(r, job, token);
    case forge_kind::gitlab: return gitlab_failure(r, job, token);
    case forge_kind::gitea: return job_failure{.log_url = job.url};
    }
    return {};
}

void order_failures_first(commit_ci& ci)
{
    auto worst = [](const ci_workflow& w)
    {
        int best = 2;
        for (const ci_job& j : w.jobs) best = std::min(best, rank(j.state));
        return best;
    };
    for (ci_workflow& w : ci.workflows)
        std::ranges::stable_sort(w.jobs, {}, [](const ci_job& j) { return rank(j.state); });
    std::ranges::stable_sort(ci.workflows, {}, worst);
}

std::string log_tail(std::string_view log, size_t lines)
{
    // The lines, cleaned: GitHub's "2026-11-01T01:06:06.1477295Z " dropped,
    // its group marks too, and GitLab's colours and section marks.
    std::vector<std::string> kept;
    size_t error_line = std::string::npos;
    for (std::string_view rest = log; !rest.empty();)
    {
        const size_t end = rest.find('\n');
        std::string_view line = rest.substr(0, end);
        rest.remove_prefix(end == std::string_view::npos ? rest.size() : end + 1);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (const size_t z = line.find("Z ");
            z != std::string_view::npos && z < 36 && line[4] == '-' && line[10] == 'T')
            line.remove_prefix(z + 2);
        // GitLab's carriage returns redraw a line: what's after the last.
        if (const size_t cr = line.rfind('\r'); cr != std::string_view::npos)
            line.remove_prefix(cr + 1);
        std::string clean;
        clean.reserve(line.size());
        for (size_t i = 0; i < line.size(); ++i)
        {
            if (line[i] == '\x1b' && i + 1 < line.size() && line[i + 1] == '[')
            {
                i += 2;
                while (i < line.size() && !(line[i] >= '@' && line[i] <= '~')) ++i;
                continue;
            }
            clean += line[i];
        }
        std::string_view c = clean;
        if (c.starts_with("section_start:") || c.starts_with("section_end:")) continue;
        if (c.starts_with("##[group]") || c.starts_with("##[endgroup]")) continue;
        if (c.starts_with("##[error]"))
        {
            clean.erase(0, 9);
            if (error_line == std::string::npos) error_line = kept.size();
        }
        kept.push_back(std::move(clean));
    }
    // To the first error, it included; else the end.
    size_t end = error_line == std::string::npos ? kept.size() : error_line + 1;
    while (end > 0 && kept[end - 1].empty()) --end;
    const size_t begin = end > lines ? end - lines : 0;
    std::string out;
    for (size_t i = begin; i < end; ++i) out += kept[i] + (i + 1 < end ? "\n" : "");
    return out;
}

std::pair<std::string_view, std::string_view> matrix_parts(std::string_view job)
{
    const size_t open = job.find(" (");
    if (open == std::string_view::npos || !job.ends_with(')')) return {job, {}};
    return {job.substr(0, open), job.substr(open + 2, job.size() - open - 3)};
}

}  // namespace tortilla
