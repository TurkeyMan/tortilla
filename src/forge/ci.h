// A commit's CI on its forge: the runs of its workflows and their jobs (a
// matrix's included), why a failed job failed (the step, the annotations,
// its log's tail), and the build artifacts the runs left. For the log's
// forge panel, and for anything else that wants a commit's checks (a
// label's mark, later).
//
// Everything that talks to a forge blocks: for a worker thread.
#pragma once

#include "forge/forge.h"

#include <atomic>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace tortilla
{

// One of a run's jobs: a check run (GitHub), a pipeline's job (GitLab), or
// a commit status (Gitea, and GitHub's statuses).
struct ci_job
{
    std::string name;  // "build (windows-latest)"
    check_state state = check_state::none;
    // Seconds since 1970; 0 when the forge doesn't say.
    std::int64_t started_at = 0, completed_at = 0;
    std::string url;      // its page on the forge
    std::int64_t id = 0;  // GitHub's check run (an Actions job's id too), GitLab's job; 0: a status
    std::string failure_reason;  // GitLab's, for a failed job
};

// A workflow's run on the commit (GitHub Actions, Forgejo), a pipeline's
// stage (GitLab), or a forge's statuses.
struct ci_workflow
{
    std::string name;  // "CI", "test" (a stage), or "" for statuses
    std::string url;   // the run's page; may be empty
    std::int64_t run_id = 0;
    std::int64_t run_number = 0;
    std::vector<ci_job> jobs;
};

struct commit_ci
{
    std::vector<ci_workflow> workflows;
    bool empty() const;
    // Combined: failure if any failed, else pending if any runs, else
    // success; none when there's nothing.
    check_state state() const;
};

// What CI says of `sha`: its runs on the forge, failures first (see
// order_failures_first). One request on GitHub; two on GitLab (the
// commit's latest pipeline, then its jobs); one on Gitea.
std::expected<commit_ci, std::string> commit_checks(forge_kind, const forge_repo&,
                                                    std::string_view sha, std::string_view token);

// A build artifact left by a run on the commit.
struct ci_artifact
{
    std::string name;
    std::string workflow;   // the run's workflow (GitLab: the job)
    std::int64_t size = 0;  // bytes; 0 when the forge doesn't say
    bool expired = false;   // gone: it can't be downloaded
    std::string download_url;
};

// The artifacts of the commit's runs: GitHub's a request a workflow run;
// GitLab's are its jobs' (no request); Forgejo's a request for the
// commit's runs, then one a run.
std::expected<std::vector<ci_artifact>, std::string> commit_artifacts(forge_kind, const forge_repo&,
                                                                      std::string_view sha,
                                                                      const commit_ci&,
                                                                      std::string_view token);

// Downloads it to `to` (a zip; GitHub's and Forgejo's are always zips). ""
// when done; else why not. The forge's reply is held whole before it's
// written, so it stops at `max_bytes`.
constexpr std::int64_t k_artifact_max_bytes = std::int64_t(1) << 30;
std::string download_artifact(forge_kind, const ci_artifact&, const std::filesystem::path& to,
                              std::string_view token, const std::atomic<bool>* cancelled = nullptr);

// Why a job failed, for its row in the panel.
struct ci_annotation
{
    std::string path;  // "src/main.c"; may be empty
    int line = 0;
    std::string message;
    std::string url;  // the file at that line on the forge; may be empty
};
struct job_failure
{
    std::string step;  // the step that failed ("Build"); empty when unknown
    int step_number = 0;
    std::vector<ci_annotation> annotations;  // the failure-level ones
    std::string log_tail;                    // the failed step's last lines, or the log's
    std::string log_url;                     // the whole log's page
};
// GitHub: three requests (the job's steps, its annotations, its log);
// GitLab: one (the job's trace). Gitea's API says nothing more than the
// status: only its page.
std::expected<job_failure, std::string> job_failure_details(forge_kind, const forge_repo&,
                                                            const ci_job&, std::string_view token);

// -- for the panel, and its tests ---------------------------------------------

// Workflows with a failure first, then those still running, then the rest;
// in each, failed jobs, then running ones, then the rest. Otherwise as the
// forge listed them.
void order_failures_first(commit_ci&);

// A log's last `lines` lines, without GitHub's timestamps and group marks
// or GitLab's colours and section marks: the failed step's (the one with
// GitHub's "##[error]", up to that line), else the log's end.
std::string log_tail(std::string_view log, size_t lines);

// A matrix job's name, split: "build (windows-latest)" is "build" and
// "windows-latest"; a plain job's is its name and "".
std::pair<std::string_view, std::string_view> matrix_parts(std::string_view job);

}  // namespace tortilla
