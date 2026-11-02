// The log's forge panel: a column beside the commit's text and files with
// what the forge knows of the selected commit, in two parts, each shown only
// when it has something to say (and the column only when either does):
// - Pull request: the PR the commit belongs to (its head, or one of its
//   commits), with whether it can be merged, its head's checks in a line,
//   its reviews, its description and its actions.
// - Build: the commit's own CI, by workflow and job (a matrix grouped), a
//   failed job's step, annotations and log's tail, and the artifacts its
//   runs left, to download.
// Everything the forge is asked goes on a worker thread, only for what's on
// show, and is kept: a finished run doesn't change.
#pragma once

#include "forge/ci.h"
#include "forge/ci_poll.h"
#include "forge/pr_watch.h"

#include <rapport/rapport.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace tortilla
{

class forge_panel
{
public:
    struct host
    {
        rap::ui& ui;
        std::function<rap::window*()> window;  // for the save dialog
        std::string workdir;
        // A pull request's actions (Open, Merge..., Close...): the log's
        // menu for it, which the buttons follow.
        std::function<rap::menu(const open_pr&)> actions;
        // The column shown or hidden.
        std::function<void(bool)> shown;
        // The commit on show, where its CI is, and whether its checks run:
        // for pr_watch's quick asks, which come back through commit_checks.
        std::function<void(std::optional<std::string>, std::optional<forge_remote>, bool)> watch;
    };

    explicit forge_panel(host);
    ~forge_panel();
    forge_panel(const forge_panel&) = delete;
    forge_panel& operator=(const forge_panel&) = delete;

    // The column, for the window's layout.
    rap::node layout();

    // The open pull requests and the forge remotes, as pr_watch last heard;
    // which commits are each one's is worked out on a worker.
    void set_pull_requests(const std::vector<open_pr>&, const std::vector<forge_remote>&);
    // The selected commit (its id), or none: the working tree, several.
    void show_commit(std::optional<std::string> sha);
    // The commit's runs as a quick ask found them (pr_watch's): the matrix
    // follows them job by job.
    void commit_checks(const std::string& sha, const commit_runs&);

private:
    struct shared;
    class matrix_model;
    class artifact_model;
    class annotation_model;

    struct pr_entry
    {
        open_pr pr;
        std::string head_ref, base_ref;    // refs/remotes/...
        std::vector<std::string> commits;  // newest first, its head's tip first
        bool known = false;                // commits worked out
    };

    // What's kept for a commit's build.
    struct build
    {
        bool asked = false, answered = false;
        bool asking = false;  // on its way (again, perhaps: what's kept shows meanwhile)
        bool local = false;   // on no forge remote: nothing to ask
        std::string error;
        commit_ci ci;
        std::vector<ci_artifact> artifacts;
        bool artifacts_known = false;
        forge_remote remote;
        std::chrono::steady_clock::time_point at{};
    };

    void work_out_commits();
    void forget_misplaced_builds();
    void update();
    void update_pull_request();
    void update_build();
    void update_failure();
    void update_watch();
    void ask_details(const pr_entry&);
    void ask_avatar(const std::string& url);
    void ask_build(const std::string& sha);
    void ask_failure(size_t workflow, size_t job);
    void download_selected();
    const pr_entry* pull_request_of(const std::string& sha, bool& head,
                                    std::vector<int>* others) const;
    // Runs `work` on a worker with the remote's token; `done` back on the UI
    // thread unless the panel's gone.
    void with_token(const forge_remote&,
                    std::function<std::function<void()>(std::string token)> work);

    host host_;
    std::shared_ptr<shared> shared_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);

    std::vector<forge_remote> remotes_;
    std::vector<pr_entry> prs_;
    // Commits worked out, by "head ref tip base ref": not asked again until
    // the branch moves.
    std::map<std::string, std::vector<std::string>> commits_cache_;
    std::optional<std::string> sha_;
    // PRs' descriptions and reviews, by "remote #number"; asked once.
    std::map<std::string, pull_request> details_;
    std::set<std::string> details_asked_;
    // Authors' pictures, by address; null: none.
    std::map<std::string, std::shared_ptr<rap::image>> pictures_;
    std::set<std::string> pictures_asked_;
    std::map<std::string, build> builds_;
    // A failed job's why, by its id: asked once.
    std::map<std::int64_t, job_failure> failures_;
    std::map<std::int64_t, std::string> failure_errors_;
    std::set<std::int64_t> failures_asked_;
    std::optional<ci_job> failed_job_;  // the job picked in the matrix
    bool visible_ = false;
    bool matrix_open_ = false, matrix_opened_by_user_ = false;
    std::string matrix_sha_;
    rap::subscription poll_timer_, download_timer_;
    std::chrono::steady_clock::time_point poll_after_{};  // the answer the poll timer follows
    bool downloading_ = false;
    std::shared_ptr<std::atomic<bool>> download_cancel_;

    std::unique_ptr<matrix_model> matrix_;
    std::unique_ptr<artifact_model> artifact_list_;
    std::unique_ptr<annotation_model> annotation_list_;

    // Widgets
    rap::widget* root_ = nullptr;
    rap::widget* pr_part_ = nullptr;
    rap::label* pr_title_ = nullptr;
    rap::canvas* pr_avatar_ = nullptr;
    rap::label* pr_author_ = nullptr;
    rap::canvas* pr_merge_mark_ = nullptr;
    rap::label* pr_merge_ = nullptr;
    rap::canvas* pr_checks_mark_ = nullptr;
    rap::label* pr_checks_ = nullptr;
    rap::label* pr_reviews_ = nullptr;
    rap::label* pr_body_ = nullptr;
    rap::label* pr_more_ = nullptr;
    rap::label* pr_also_ = nullptr;
    std::array<rap::button*, 3> pr_buttons_{};
    std::vector<std::function<void()>> pr_actions_;
    rap::widget* separator_ = nullptr;
    rap::widget* build_part_ = nullptr;
    rap::canvas* build_mark_ = nullptr;
    rap::label* build_summary_ = nullptr;
    rap::label* build_toggle_ = nullptr;
    rap::list* matrix_list_ = nullptr;
    rap::widget* failure_part_ = nullptr;
    rap::label* failure_title_ = nullptr;
    rap::list* annotations_ = nullptr;
    rap::text_view* log_tail_ = nullptr;
    rap::label* full_log_ = nullptr;
    rap::widget* artifact_part_ = nullptr;
    rap::list* artifacts_ = nullptr;
    rap::button* download_ = nullptr;
    rap::progress_bar* download_progress_ = nullptr;
    rap::label* download_status_ = nullptr;

    // What the marks show.
    check_state merge_mark_state_ = check_state::none;
    int merge_mark_kind_ = 0;  // mergeable
    check_state checks_mark_state_ = check_state::none;
    check_state build_mark_state_ = check_state::none;
    std::shared_ptr<rap::image> picture_;
    std::string full_log_url_;
    std::string pr_url_;
    std::string pr_body_text_;
};

}  // namespace tortilla
