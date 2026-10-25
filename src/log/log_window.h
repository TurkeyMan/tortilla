// TortoiseGit's Log dialog (IDD_LOGMESSAGE). See docs/tortoisegit-spec.md
// section 1.
#pragma once

#include "changes/changes_view.h"
#include "git/command.h"
#include "git/repo_worker.h"
#include "log/log_model.h"
#include "rebase/rebase_window.h"

#include "ui/settings_dialog.h"
#include "util/gravatar.h"

#include <rapport/rapport.h>
#include <rapport/sys.h>

#include <chrono>
#include <filesystem>
#include <memory>

namespace tortilla
{

class log_window
{
public:
    // `path` is any path inside the repository.
    // `all_branches` starts with All Branches checked.
    static std::expected<std::unique_ptr<log_window>, std::string>
    open(rap::ui&, const std::string& path, bool all_branches = false);

    void show() { window_->show(); }
    // Shows the history from `rev` (a ref or commit) instead of HEAD.
    void start_at(const std::string& rev);
    void on_closed(std::function<void()> f) { closed_ = std::move(f); }

private:
    log_window(rap::ui&, repo, std::string path);

    void build();
    void reload();
    void loaded(const std::string& error);
    void restore_selection();
    void selection_changed();
    void show_commit(size_t row);
    void update_status();
    void update_ref_button();
    // Where the walk starts; the All Branches box follows [spec 1.9.1].
    void set_walk(walk_refs refs, std::string rev = {});
    void schedule_filter();
    void apply_filter();
    void jump(bool up);
    void dates_changed();
    void save_layout();
    void restore_layout();

    // Commit-list context menu and its actions (log_actions.cpp).
    rap::menu context_menu(std::optional<size_t> row);
    void run_git_commands(const std::string& title, std::vector<std::vector<std::string>> commands);
    // Local branch/tag changes: no progress dialog, git's output only if
    // a command fails, and the labels refreshed instead of a full reload.
    void run_ref_commands(const std::string& title, std::vector<std::vector<std::string>> commands);
    // git commands with no progress dialog; `after` gets whether all
    // succeeded and their output. A failure is shown unless `show_failure`
    // is false (the caller handles it).
    void run_quietly(const std::string& title, std::vector<std::vector<std::string>> commands,
                     std::function<void(bool ok, const std::string& output)> after,
                     bool show_failure = true);
    void refs_changed();
    void open_settings(settings_page);
    void difftool(std::vector<std::string> revisions, std::vector<std::string> paths = {});
    void confirm_then_run(const std::string& question, const std::string& title,
                          std::vector<std::vector<std::string>> commands);
    void create_branch(const commit&, const std::string& name = {}, bool switch_to = false,
                       const std::string& start_point = {});
    void create_tag(const commit&);
    void stash_changes();
    void reset_to(const commit&);
    // A branch or tag label dropped on `target`: confirm, then move it.
    void move_ref(const ref&, const commit& target);
    // Dragging a branch or tag label onto another commit moves it there.
    rap::list::cell_drag label_drag();
    // Double-clicking a label: a local branch is switched to (asking what to
    // do with uncommitted changes), a remote one opens Create Branch for a
    // local branch tracking it. The label comes from the last press, which
    // label_drag's begin sees.
    struct label_press
    {
        size_t row;
        std::string full_name;
        std::chrono::steady_clock::time_point at;
    };
    std::optional<label_press> last_label_press_;
    void row_activated(size_t row);
    enum class switch_mode
    {
        plain,
        stash,
        bring_along,
        discard,
    };
    void switch_to(const std::string& target, bool detach);
    void run_switch(const std::string& target, bool detach, switch_mode);
    struct choice
    {
        std::string label, description;
        std::function<void()> act;
    };
    void choose(const std::string& title, const std::string& question, std::vector<choice> choices);
    void offer_stash_pop();
    rap::menu cell_menu(size_t row, size_t column, rap::point_f at);
    std::optional<std::string> clicked_ref_;  // while building a label's context menu
    void merge_from(const commit&, const std::string& what);
    std::string remote_or_hash(const commit&);
    template <class Dialog>
    void keep_dialog(std::expected<std::unique_ptr<Dialog>, std::string>);

    void copy_to_clipboard(const std::vector<size_t>& rows, int format);
    void show_branches_containing(const commit&);

    rap::menu walk_menu();
    rap::menu view_menu();
    rap::menu ref_menu();
    rap::menu filter_menu();
    rap::menu header_menu();
    std::string filter_cue() const;

    rap::ui& ui_;
    // Expires with the log, for dialogs that outlive it (their reload).
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    repo repo_;  // UI-thread repository for details and file lists
    std::string path_;
    log_model log_;
    std::unique_ptr<repo_worker> files_worker_;  // the selected commit's files
    changes_view files_;  // the selected commit's (or the working tree's) files
    std::unique_ptr<rap::window> window_;
    std::function<void()> closed_;

    // Widgets
    rap::menu_button* ref_button_ = nullptr;
    rap::date_picker* from_ = nullptr;
    rap::date_picker* to_ = nullptr;
    rap::entry* search_ = nullptr;
    rap::drop_down* jump_type_ = nullptr;
    rap::list* commit_list_ = nullptr;
    rap::text_view* details_ = nullptr;
    rap::canvas* avatar_canvas_ = nullptr;
    rap::label* status_ = nullptr;
    rap::check* all_branches_ = nullptr;
    rap::button* statistics_ = nullptr;
    rap::menu_button* walk_ = nullptr;
    rap::menu_button* view_ = nullptr;
    rap::progress_bar* progress_ = nullptr;
    rap::splitter* splitter_ = nullptr;

    // Options
    walk_options walk_options_;
    std::vector<std::string> ref_history_;  // refs picked, newest first
    log_filter filter_;
    bool show_unversioned_ = true;
    std::optional<int64_t> since_, until_;  // explicit date limits
    rap::subscription filter_timer_, pulse_timer_;
    // Reloading when something else changes the repository.
    void watch_repository();
    std::string repository_state() const;
    std::filesystem::path git_dir_, common_dir_;
    std::string state_;
    std::vector<std::unique_ptr<rap::sys::directory_watch>> watches_;
    std::shared_ptr<git_process> difftool_;
    std::shared_ptr<git_process> ref_command_;
    std::vector<std::unique_ptr<rebase_window>> rebases_;
    void open_rebase(const std::string& upstream);
    std::vector<oid> history_;  // selection history, for the "Selection History" jump
    // The selected commit's author, top right of the details [spec 1.2].
    void show_avatar(const std::string& email);
    std::shared_ptr<rap::image> avatar_;
    gravatar gravatar_{ui_};

    // The selection across a reload, by commit (rows move): put back, and
    // the current row scrolled into view, once loading finishes.
    struct kept_selection
    {
        std::vector<oid> commits;
        bool working_tree = false;
        std::optional<oid> current;  // nullopt with working_tree: that row
    };
    std::optional<kept_selection> kept_;
    size_t history_pos_ = 0;
    bool navigating_ = false;
};

}  // namespace tortilla
