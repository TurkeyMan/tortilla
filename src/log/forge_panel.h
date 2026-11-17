// The log's lower pages: Commit (supplied by the log tab), then the PR,
// selected commit's CI, and artifacts. Conditional pages retain their
// indices and the user's choice while visible. Finished forge answers are
// cached; unchanged presentation does not mutate the native controls.
#pragma once

#include "forge/ci.h"
#include "forge/ci_poll.h"
#include "forge/pr_watch.h"
#include "ui/page_guard.h"

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
        // The commit on show, where its CI is, and whether its checks run:
        // for pr_watch's quick asks, which come back through commit_checks.
        std::function<void(std::optional<std::string>, std::optional<forge_remote>, bool)> watch;
    };

    explicit forge_panel(host);
    ~forge_panel();
    forge_panel(const forge_panel&) = delete;
    forge_panel& operator=(const forge_panel&) = delete;

    // The lower pane: Commit content and conditional forge pages.
    // Choice belongs to this log tab and survives a page rebuild/tear-off.
    rap::node layout(rap::node commit = {});
    void built();
    void unbind();
    int selected_page() const { return selected_page_; }
    int remembered_page() const { return remembered_page_; }
    // Restore a carried user choice at bind/adopt; ordinary fallbacks never write it.
    void restore_page_choice(int page);
    // Actual full lower-area width, from the host's existing resize watch.
    void set_available_width(float width);

    // The open pull requests and the forge remotes, as pr_watch last heard;
    // which commits are each one's is worked out on a worker.
    void set_pull_requests(const std::vector<open_pr>&, const std::vector<forge_remote>&);
    // The selected commit (its id), or none: the working tree, several.
    void show_commit(std::optional<std::string> sha, std::string author_name = {},
                     std::string author_email = {});
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
    void sync_pages();
    void user_page_changed(int page);
    void remember_for_row();
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
    // The widgets' callbacks, from layout() until unbind().
    page_guard page_guard_;

    std::vector<forge_remote> remotes_;
    std::vector<pr_entry> prs_;
    // Commits worked out, by "head ref tip base ref": not asked again until
    // the branch moves.
    std::map<std::string, std::vector<std::string>> commits_cache_;
    std::optional<std::string> sha_;
    std::string author_name_, author_email_;  // its author's
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
    rap::tabs* tabs_ = nullptr;
    int selected_page_ = 0, remembered_page_ = 0;
    std::uint64_t row_revision_ = 0, user_choice_revision_ = 0;
    struct page_restore
    {
        std::uint64_t row, choice;
        int page;
    };
    std::optional<page_restore> restore_page_;
    bool changing_pages_ = false;
    bool pr_present_ = false, ci_present_ = false, artifacts_present_ = false;
    rap::subscription poll_timer_, download_timer_;
    std::chrono::steady_clock::time_point poll_after_{};  // the answer the poll timer follows
    bool downloading_ = false;
    bool download_pulsing_ = false;  // its size unknown: the bar says it's busy
    float download_fraction_ = 0;
    // Under the artifacts: what's downloading, where it was saved, or why not.
    std::string download_text_;
    void download_started(std::string text, bool pulsing);
    void download_progressed(float fraction);
    void download_ended(const std::string& to, const std::string& error);
    void show_download();
    // Last values sent to this page. Background answers often leave the
    // presentation unchanged; do not ask native controls to lay it out again.
    struct drawn_widget
    {
        std::optional<std::string> text, tooltip;
        std::optional<bool> visible, enabled;
    };
    std::map<rap::widget*, drawn_widget> drawn_;
    void text(rap::label*, const std::string&);
    void text(rap::button*, const std::string&);
    void text(rap::text_view*, const std::string&);
    void show(rap::widget*, bool);
    void enable(rap::widget*, bool);
    void tooltip(rap::widget*, const std::string&);
    friend struct forge_panel_test;  // tests/log_page_tests.cpp
    std::shared_ptr<std::atomic<bool>> download_cancel_;

    std::unique_ptr<matrix_model> matrix_;
    std::unique_ptr<artifact_model> artifact_list_;
    std::unique_ptr<annotation_model> annotation_list_;

    // Widgets
    rap::widget* root_ = nullptr;
    rap::widget* pr_part_ = nullptr;
    rap::label* pr_title_ = nullptr;
    rap::stack* pr_labels_ = nullptr;
    std::vector<pr_label> labels_;
    rap::canvas* labels_canvas_ = nullptr;
    struct label_plan
    {
        std::vector<size_t> shown;
        size_t hidden = 0;
        float width = 0;
        float height = 0;
        bool operator==(const label_plan&) const = default;
    };
    float labels_available_width_ = 0;
    std::vector<rap::size_f> label_sizes_, overflow_sizes_;
    bool labels_measured_ = false, labels_layout_queued_ = false;
    label_plan label_plan_;
    std::optional<label_plan> labels_rendered_;
    unsigned labels_revision_ = 0;
    unsigned labels_generation_ = 0;
    void layout_labels();
    void plan_labels();
    rap::canvas* pr_avatar_ = nullptr;
    rap::label* pr_author_ = nullptr;
    rap::canvas* pr_merge_mark_ = nullptr;
    rap::label* pr_merge_ = nullptr;
    rap::canvas* pr_checks_mark_ = nullptr;
    rap::label* pr_checks_ = nullptr;
    rap::label* pr_reviews_ = nullptr;
    rap::text_view* pr_body_ = nullptr;
    rap::label* pr_also_ = nullptr;
    std::array<rap::button*, 3> pr_buttons_{};
    std::vector<std::function<void()>> pr_actions_;
    rap::widget* build_body_ = nullptr;
    rap::widget* build_part_ = nullptr;
    rap::canvas* build_mark_ = nullptr;
    rap::label* build_summary_ = nullptr;
    rap::list* matrix_list_ = nullptr;
    rap::widget* failure_part_ = nullptr;
    rap::text_view* failure_title_ = nullptr;
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
