#include "ci_poll.h"

#include "forge/forge_detail.h"
#include "i18n.h"
#include "util/json.h"

#include <algorithm>
#include <charconv>
#include <cmath>

namespace tortilla
{

using namespace forge_detail;

namespace
{

// A header's value, by name in any case; "" when it isn't there.
std::string_view header(const rap::sys::http_headers& headers, std::string_view name)
{
    for (const auto& [n, v] : headers)
        if (std::ranges::equal(
                n, name, [](char a, char b)
                { return std::tolower((unsigned char)a) == std::tolower((unsigned char)b); }))
            return v;
    return {};
}

std::int64_t number(std::string_view s)
{
    std::int64_t v = 0;
    std::from_chars(s.data(), s.data() + s.size(), v);
    return v;
}

check_state rest_state(const json::value& run)
{
    if (!run["status"].str_is("completed")) return check_state::pending;
    const json::value c = run["conclusion"];
    if (c.str_is("success") || c.str_is("neutral") || c.str_is("skipped"))
        return check_state::success;
    return check_state::failure;
}

}  // namespace

std::expected<conditional_reply, std::string>
conditional_get(forge_kind k, const std::string& url, std::string_view token, etag_cache& cache)
{
    auto headers = api_headers(k, token);
    auto kept = cache.by_url.find(url);
    if (kept != cache.by_url.end() && !kept->second.etag.empty())
        headers.emplace_back("If-None-Match", kept->second.etag);
    const auto reply = rap::sys::http_get(url, {.headers = std::move(headers)});
    if (token.empty()) note_budget(reply);  // a 304 costs nothing; a 200, one
    conditional_reply out;
    // Asked to slow down: a secondary rate limit, or the hour's spent.
    if (reply.status == 403 || reply.status == 429)
    {
        std::int64_t wait = number(header(reply.headers, "Retry-After"));
        if (!wait && header(reply.headers, "X-RateLimit-Remaining") == "0")
            wait = std::max<std::int64_t>(60, number(header(reply.headers, "X-RateLimit-Reset")) -
                                                  std::int64_t(std::time(nullptr)));
        out.retry_after = std::chrono::seconds(std::max<std::int64_t>(wait, 60));
        if (kept != cache.by_url.end()) out.body = kept->second.body;
        return out;
    }
    if (reply.status == 304 && kept != cache.by_url.end())
    {
        out.body = kept->second.body;
        return out;
    }
    if (reply.status != 200) return std::unexpected(failure(reply, json::parse(reply.body)));
    out.changed = kept == cache.by_url.end() || kept->second.body != reply.body;
    out.body = reply.body;
    cache.by_url[url] = {std::string(header(reply.headers, "ETag")), reply.body};
    return out;
}

std::expected<commit_runs, std::string> poll_commit_runs(const forge_repo& r, std::string_view sha,
                                                         std::string_view token, etag_cache& cache)
{
    const std::string root = repo_api(forge_kind::github, r) + "/commits/" + std::string(sha);
    commit_runs out;
    auto runs =
        conditional_get(forge_kind::github, root + "/check-runs?per_page=100", token, cache);
    if (!runs) return std::unexpected(runs.error());
    out.retry_after = runs->retry_after;
    if (out.retry_after.count()) return out;
    auto statuses = conditional_get(forge_kind::github, root + "/status", token, cache);
    if (!statuses) return std::unexpected(statuses.error());
    out.retry_after = statuses->retry_after;
    out.changed = runs->changed || statuses->changed;
    bool any = false, failed = false, pending = false;
    auto count = [&](check_state s)
    {
        any = true;
        failed = failed || s == check_state::failure;
        pending = pending || s == check_state::pending;
    };
    if (const auto j = json::parse(runs->body))
        for (const json::value& c : (*j)["check_runs"].elements())
        {
            check_run run{.name = c["name"].str(),
                          .state = rest_state(c),
                          .started_at = time_of(c["started_at"]),
                          .completed_at = time_of(c["completed_at"]),
                          .url = c["details_url"].str()};
            if (c["conclusion"].str_is("skipped")) run.started_at = run.completed_at = 0;
            count(run.state);
            out.runs.push_back(std::move(run));
            out.ids.push_back(c["id"].integer().value_or(0));
        }
    if (const auto j = json::parse(statuses->body))
        for (const json::value& c : (*j)["statuses"].elements())
        {
            check_run run{.name = c["context"].str(),
                          .state = status_word(c["state"].str()),
                          .url = c["target_url"].str()};
            (run.state == check_state::pending ? run.started_at : run.completed_at) =
                time_of(c["updated_at"]);
            count(run.state);
            out.runs.push_back(std::move(run));
            out.ids.push_back(0);
        }
    out.state = failed    ? check_state::failure
                : pending ? check_state::pending
                : any     ? check_state::success
                          : check_state::none;
    return out;
}

std::int64_t expected_duration(const forge_repo& r, std::string_view sha, std::string_view token,
                               std::map<std::int64_t, std::int64_t>& known)
{
    const std::string root = repo_api(forge_kind::github, r) + "/actions";
    const auto headers = api_headers(forge_kind::github, token);
    const auto runs = rap::sys::http_get(
        root + "/runs?head_sha=" + std::string(sha) + "&per_page=50", {.headers = headers});
    const auto j = json::parse(runs.body);
    if (runs.status != 200 || !j) return 0;
    std::int64_t longest = 0;
    for (const json::value& run : (*j)["workflow_runs"].elements())
    {
        const std::int64_t workflow = run["workflow_id"].integer().value_or(0);
        if (!workflow) continue;
        if (!known.contains(workflow))
        {
            // Its last successful run's length; kept for the window's life.
            const auto last = rap::sys::http_get(root + "/workflows/" + std::to_string(workflow) +
                                                     "/runs?status=success&per_page=1",
                                                 {.headers = headers});
            std::int64_t took = 0;
            if (const auto l = json::parse(last.body); l && last.status == 200)
            {
                const json::value prev = (*l)["workflow_runs"][size_t(0)];
                took = std::max<std::int64_t>(0, time_of(prev["updated_at"]) -
                                                     time_of(prev["run_started_at"]));
            }
            known[workflow] = took;
        }
        longest = std::max(longest, known[workflow]);
    }
    return longest;
}

std::chrono::seconds fast_interval(std::int64_t elapsed, std::int64_t expected, bool watched)
{
    const double slow = watched ? 10 : 30, fast = watched ? 5 : 15;
    if (expected <= 0) return std::chrono::seconds(int(watched ? 8 : 20));
    // From slow to fast over the expected time, steepest near its end; fast
    // once it's 80% through, and after.
    const double through = std::clamp(double(elapsed) / (0.8 * double(expected)), 0.0, 1.0);
    return std::chrono::seconds(int(std::lround(slow - (slow - fast) * through * through)));
}

std::vector<fast_poll> plan_fast_polls(const std::vector<pending_head>& pending,
                                       const std::optional<pending_head>& watched, std::int64_t now,
                                       bool signed_in, int anonymous_left, size_t cap)
{
    std::vector<fast_poll> out;
    auto add = [&](const pending_head& h, bool is_watched)
    {
        if (out.size() >= cap) return;
        if (std::ranges::any_of(out, [&](const fast_poll& p) { return p.sha == h.sha; })) return;
        const std::int64_t elapsed =
            h.started_at ? std::max<std::int64_t>(0, now - h.started_at) : 0;
        out.push_back({.sha = h.sha,
                       .interval = fast_interval(elapsed, h.expected, is_watched),
                       .watched = is_watched});
    };
    if (!signed_in)
    {
        if (watched && anonymous_left > 30) add(*watched, true);
        return out;
    }
    if (watched) add(*watched, true);
    for (const pending_head& h : pending) add(h, false);
    return out;
}

}  // namespace tortilla
