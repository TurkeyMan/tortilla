// Checks that are running, asked after often and cheaply: GitHub's REST
// check runs and statuses for a commit, with If-None-Match, so an answer
// that hasn't changed (a 304) costs nothing against the rate limit. How
// often follows how long the workflow took last time: gently at first, more
// often as its end nears. pr_watch runs it for the pending PRs and the one
// on show.
//
// What talks to a forge blocks: for a worker thread.
#pragma once

#include "forge/forge.h"

#include <chrono>
#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tortilla
{

// Replies kept by URL with their ETags, to ask again conditionally.
struct etag_cache
{
    struct entry
    {
        std::string etag, body;
    };
    std::map<std::string, entry, std::less<>> by_url;
};

// A GET that sends the ETag last seen for `url`: the body (the kept one on
// a 304), whether it changed, and how long the forge asks to wait (a 403
// or 429 with Retry-After, or a secondary rate limit), when it does.
struct conditional_reply
{
    std::string body;
    bool changed = false;
    std::chrono::seconds retry_after{0};
};
std::expected<conditional_reply, std::string> conditional_get(forge_kind, const std::string& url,
                                                              std::string_view token, etag_cache&);

// A commit's checks by GitHub's REST API (check runs, then statuses), each
// conditional: whether anything changed since last asked, and the runs.
struct commit_runs
{
    std::vector<check_run> runs;
    std::vector<std::int64_t> ids;  // the check runs' ids, as `runs` (0 for a status)
    check_state state = check_state::none;
    bool changed = false;
    std::chrono::seconds retry_after{0};
};
std::expected<commit_runs, std::string> poll_commit_runs(const forge_repo&, std::string_view sha,
                                                         std::string_view token, etag_cache&);

// How long the commit's workflows took when they last passed, as far as
// the forge says: the longest of them, by its workflow runs (one request,
// then one a workflow not yet in `known`, which it fills). 0 when unknown.
std::int64_t expected_duration(const forge_repo&, std::string_view sha, std::string_view token,
                               std::map<std::int64_t, std::int64_t>& known);

// -- the schedule (no forge: for tests) ---------------------------------------

// How long to wait before asking again about checks that started `elapsed`
// seconds ago and are expected to take `expected` (0: unknown): the
// watched commit every 10 s at first, every 5 s from 80% of the expected
// time on; the others every 30 s, then 15 s.
std::chrono::seconds fast_interval(std::int64_t elapsed, std::int64_t expected, bool watched);

struct pending_head
{
    std::string sha;
    std::int64_t started_at = 0;  // the earliest pending run's start; 0: unknown
    std::int64_t expected = 0;    // seconds; 0: unknown
};
struct fast_poll
{
    std::string sha;
    std::chrono::seconds interval;
    bool watched = false;
};
// Which commits are asked after quickly, and how often: the watched one
// (when its checks run) first, then the pending PR heads in their order
// (newest updated first), up to `cap` in all. Without a token only the
// watched one, and only while `anonymous_left` (GitHub's budget for
// unsigned requests) is above 30.
constexpr size_t k_fast_poll_cap = 20;
std::vector<fast_poll> plan_fast_polls(const std::vector<pending_head>& pending,
                                       const std::optional<pending_head>& watched, std::int64_t now,
                                       bool signed_in, int anonymous_left,
                                       size_t cap = k_fast_poll_cap);

}  // namespace tortilla
