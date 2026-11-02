// TortoiseGit's Log dialog (IDD_LOGMESSAGE). See docs/tortoisegit-spec.md
// section 1.
#pragma once

#include "changes/changes_view.h"
#include "forge/avatars.h"
#include "forge/pr_watch.h"
#include "git/command.h"
#include "git/history_edit.h"
#include "git/repo_worker.h"
#include "git/sync.h"
#include "log/forge_panel.h"
#include "log/log_model.h"
#include "rebase/rebase_window.h"

#include "ui/diff_window.h"
#include "ui/settings_dialog.h"

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
    // `path` is any path inside the repository; the history is limited to
    // it (Show Whole Project lifts that). `all_branches` starts with All
    // Branches checked. `outer`: the repository around a submodule at
    // `path`, rather than the submodule's own (Show log of this folder).
    static std::expected<std::unique_ptr<log_window>, std::string>
    open(rap::ui&, const std::string& path, bool all_branches = false, bool outer = false);

    void show() { window_->show(); }
    // Shows the history from `rev` (a ref or commit) instead of HEAD.
    void start_at(const std::string& rev);
    // Selects `rev`'s commit once it's loaded (at once if it is), scrolled
    // into view; later reloads keep it selected, as any selection.
    void select_commit(const std::string& rev);
    void on_closed(std::function<void()> f) { closed_ = std::move(f); }
    // Picking commits for another window (the rebase's Add): the title says
    // `purpose`, OK becomes `label` and hands it the selected commits, newest
    // first, then closes, and Cancel closes without. Merges and the working
    // tree row are left out, and OK waits for a commit it can hand over.
    void pick_commits(const std::string& purpose, const std::string& label,
                      std::function<void(std::vector<commit>)> chosen);

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
    // Where the walk starts [spec 1.9.1]: head (with `rev`, from there) is
    // the current branch preset, all the All branches one, local the Local.
    void set_walk(walk_refs refs, std::string rev = {});

    // What the log shows: the Show menu (the drop-down top left). Its
    // preset, pins, hide patterns and toggles (branch_filter.h), per
    // repository: the preset and toggles by working tree, the pins and
    // hide patterns by the repository's common directory, shared by its
    // worktrees.
    branch_filter show_;
    // The preset before Pick... chose a set for once: what's saved.
    std::optional<show_preset> before_pick_;
    bool prs_known_ = false;    // the pull request watch has answered
    bool pins_custom_ = false;  // the user changed the pins: saved, not the defaults
    void set_show(show_preset);
    // The filter, and what the forge knows, to the model for the next load.
    void apply_branches();
    void save_branches();
    void restore_branches();
    std::string branches_key(const char* what, bool shared) const;
    // "Pull requests + 2 pinned · tips only", for the status bar.
    std::string show_summary() const;
    void set_pinned(const ref&, bool pinned);
    void hide_branch(const ref&);
    void pick_branches();
    void edit_pins_and_hidden();
    // A branch checked out in another worktree: a log of that worktree.
    void open_worktree(const std::string& path);
    void schedule_filter();
    void apply_filter();
    void jump(bool up);
    void dates_changed();
    void save_layout();
    // The window closing: its layout kept, and View Patch's window closed
    // with it (on for next time).
    void closing();
    void restore_layout();

    // Commit-list context menu and its actions (log_actions.cpp).
    rap::menu context_menu(std::optional<size_t> row);
    void run_git_commands(const std::string& title, std::vector<std::vector<std::string>> commands);
    // Local branch/tag changes: no progress dialog, git's output only if
    // a command fails, and the labels refreshed instead of a full reload.
    void run_ref_commands(const std::string& title, std::vector<std::vector<std::string>> commands);
    // git commands with no progress dialog; `after` gets whether all
    // succeeded and their output. A failure is shown unless `show_failure`
    // is false (the caller handles it). `prompts`: never, for what runs
    // unasked.
    void run_quietly(const std::string& title, std::vector<std::vector<std::string>> commands,
                     std::function<void(bool ok, const std::string& output)> after,
                     bool show_failure = true, git_prompts prompts = git_prompts::ask);
    void refs_changed();
    // The forge's open pull requests: the branches that are their heads, for
    // the labels and the menus. pr_watch_ is made once the window is.
    std::unique_ptr<pr_watch> pr_watch_;
    std::unordered_map<std::string, open_pr> pr_heads_;
    void update_pull_requests();
    // A label's pull request: a remote branch's, if it's one's head; a
    // local branch's upstream's.
    const open_pr* pull_request_of(const ref&) const;
    void create_pull_request(const ref&);
    void merge_pull_request(const open_pr&);
    void close_pull_request(const open_pr&);
    // After the forge changed the repository: its remote fetched quietly,
    // the labels refreshed.
    void fetch_after_forge(const std::string& remote);
    // -- Sync: the Fetch button, top right (log_sync.cpp) --------------------
    // A click: the remotes fetched side by side (`only` that one, after a
    // forge action), never asking, the leading one first; the PRs asked
    // again; branches not checked out fast-forwarded after their remote's
    // fetch; the log reloaded when the leading remote is in and again when
    // all are; then the current branch, if it's behind its upstream, offered
    // forward (or moved, if set to and clean).
    void sync(const std::string& only = {});
    struct sync_state
    {
        std::string only, leading;
        size_t pending = 0;
        std::map<std::string, remote_result, std::less<>> results;
        std::vector<std::string> moved;  // fast-forwarded branches
    };
    std::shared_ptr<sync_state> sync_run_;
    // One remote fetched, and what moved after it.
    void remote_synced(const std::shared_ptr<sync_state>&, const std::string& remote, remote_result,
                       std::vector<std::string> moved);
    void sync_finished(const std::shared_ptr<sync_state>&);
    // Before a click: what the remotes have that a sync would bring, from
    // the PR watch's branch tips (forge remotes) and ls-remote (the rest,
    // every ten minutes). Compared with the local refs on a worker, and then
    // downloaded into refs/prefetch/ if allowed.
    void start_sync_watch();
    void poll_remotes();
    void tips_heard(std::map<std::string, remote_tips, std::less<>> tips);
    void recheck_changes();
    void prefetch(const std::string& remote);
    // A background contact's result for one remote: the button only.
    void contact_result(const std::string& remote, remote_result);
    // The button's icon and tooltip, as what's known says.
    void update_sync_badge();
    void show_sync_output();
    // The current branch against its upstream, on a worker: the hint bar.
    void update_branch_hint();
    void show_branch_hint(const current_state&);
    void fast_forward_current();
    // A sync's warning in the status bar, for a while.
    void set_sync_notice(std::string text, bool failure);
    void keep_pull_dialog();
    // The button's icon: the desktop's download icon unbadged (drawn where
    // there are no named icons, Windows), or drawn with its badge: changes
    // waiting (a dot with their count), a remote that failed (a red "!"),
    // or no network (greyed, Offline).
    enum class fetch_badge
    {
        none,
        changes,
        failure,
        offline,
    };
    static rap::icon fetch_icon(fetch_badge = fetch_badge::none, size_t count = 0);
    fetch_badge badge_shown_ = fetch_badge::none;
    size_t badge_count_ = 0;
    static void paint_failure_mark(rap::painter&, rap::size_f);
    rap::button* fetch_ = nullptr;
    rap::canvas* status_mark_ = nullptr;
    rap::widget* hint_bar_ = nullptr;
    rap::label* hint_text_ = nullptr;
    rap::button* hint_ff_ = nullptr;
    rap::button* hint_pull_ = nullptr;
    bool syncing_ = false;
    // The remotes' branches as last heard, by remote: the forge's answer, or
    // ls-remote's.
    std::map<std::string, remote_tips, std::less<>> known_tips_;
    std::vector<remote_changes> changes_;  // what a sync would bring, by remote
    // Remotes whose last contact failed, and why; until one succeeds.
    std::map<std::string, remote_result, std::less<>> failures_;
    std::string last_output_;  // the last fetch's, for the status bar's "!"
    std::string notice_;
    rap::subscription notice_timer_, poll_timer_;
    std::vector<std::string> prefetching_;
    bool rechecking_ = false, recheck_again_ = false, polling_ = false;
    bool after_sync_ = false;  // the next branch hint follows a sync
    current_state current_;
    std::string hint_hidden_;  // "branch upstream behind" the user hid

    // PRs set to merge when ready, by remote and number: fetched when the
    // watch no longer sees them open.
    std::vector<std::pair<std::string, int>> awaiting_merge_;
    void open_pull_request_menu(rap::menu&, const ref&);
    rap::menu pull_request_items(const open_pr&);
    void open_settings(settings_page);
    std::string title_text() const;
    bool path_limited() const;
    std::string path_text(size_t limit) const;
    std::string repo_location() const;
    void update_walk_button();
    bool one_file() const;
    void apply_follow();
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

    // Format Patch's Save As for several commits: a folder, then git
    // format-patch writing a numbered file per commit into it.
    void save_patch_series(rap::window* viewer, const patch_range&);
    // Show log...: another log window walking from `rev`, or a range.
    void show_log_at(const std::string& rev);
    // Compare change sets: each commit's patch, compared with the diff tool.
    void compare_change_sets(const std::string& a, const std::string& b);
    // Edit Notes: the commit's note in a dialog, saved with git notes.
    void edit_notes(const commit&);

    void copy_to_clipboard(const std::vector<size_t>& rows, int format);
    void show_branches_containing(const commit&);

    rap::menu walk_menu();
    rap::menu view_menu();
    // View Patch [spec 1.9.5]: the selection's patch in a window of its
    // own, following the selection; whether it's open is kept across runs.
    std::optional<patch_viewer> patch_;
    unsigned patch_generation_ = 0;  // drops a patch the selection has left
    rap::subscription patch_timer_;
    bool patch_restored_ = false;
    void show_patch(bool);
    void update_patch();
    // The Show menu: presets, toggles, pins and hidden, labels, and the
    // revision to start from (start_menu).
    rap::menu show_menu();
    rap::menu start_menu();
    rap::menu filter_menu();
    rap::menu header_menu();
    std::string filter_cue() const;

    rap::ui& ui_;
    // Expires with the log, for dialogs that outlive it (their reload).
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    // A git-svn repository's: whether git svn is installed, for SVN DCommit
    // (unset: not known yet, or not a git-svn repository).
    std::optional<bool> git_svn_ok_;
    repo repo_;  // UI-thread repository for details and file lists
    std::string path_;
    // The path the log was opened for, repository-relative, which the
    // history is limited to unless Show Whole Project; "" for the top.
    std::string rel_path_;
    log_model log_;
    std::unique_ptr<repo_worker> files_worker_;  // the selected commit's files
    changes_view files_;  // the selected commit's (or the working tree's) files
    // Before window_, so it goes after it: the window's lists use the panel's
    // models, and a list tells its model it's going as it goes.
    std::unique_ptr<forge_panel> forge_panel_;
    std::unique_ptr<rap::window> window_;
    std::function<void()> closed_;
    std::function<void(std::vector<commit>)> picked_;
    std::string pick_purpose_;
    rap::button* ok_ = nullptr;
    rap::button* cancel_ = nullptr;
    void update_pick_button();

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
    rap::check* only_path_ = nullptr;
    rap::entry* path_filter_ = nullptr;
    // The preset chosen, while following a file's renames walks from HEAD
    // instead: restored after, and what's saved.
    std::optional<show_preset> refs_before_follow_;
    rap::button* statistics_ = nullptr;
    rap::menu_button* walk_ = nullptr;
    rap::menu_button* view_ = nullptr;
    rap::progress_bar* progress_ = nullptr;
    rap::splitter* splitter_ = nullptr;
    // Under the list: the commit's text over its files, and the forge panel
    // beside both, a column of its own (shown when it has something to say).
    rap::splitter* details_splitter_ = nullptr;
    rap::splitter* forge_splitter_ = nullptr;
    std::vector<float> forge_sizes_;            // as last shown, for showing it again
    bool forge_panel_visible_ = false;
    void forge_panel_shown(bool);
    void fit_files();
    rap::subscription fit_timer_;

    // Options
    walk_options walk_options_;
    std::vector<std::string> ref_history_;  // refs picked, newest first
    log_filter filter_;
    bool show_unversioned_ = true;
    // Files outside the log's path in the file list [spec 1.9.5].
    file_view::unrelated_mode unrelated_ = file_view::gray_unrelated;
    // The file list's view from those and the path filter box.
    void update_file_view();
    std::optional<int64_t> since_, until_;  // explicit date limits
    rap::subscription filter_timer_, pulse_timer_, path_filter_timer_;
    // Reloading when something else changes the repository.
    void watch_repository();
    std::string repository_state() const;
    std::filesystem::path git_dir_, common_dir_;
    std::string state_;
    std::vector<std::unique_ptr<rap::sys::directory_watch>> watches_;
    std::shared_ptr<git_process> difftool_;
    std::shared_ptr<git_process> ref_command_;
    std::vector<std::unique_ptr<rebase_window>> rebases_;
    // `todo`: going on from a history edit's conflict, with its list.
    void open_rebase(const std::string& upstream, const rebase_todo* todo = nullptr);

    // -- History editing (history_actions.cpp) --------------------------------
    // The commit menu's Combine, Reword, Drop, Squash, Fixup, Amend, Move and
    // Undo last commit, greyed with why when they don't apply.
    void history_items(rap::menu&, const std::vector<size_t>& sel);
    // The leading remote's default branch, which edits never rewrite.
    std::string mainline_ref() const;
    // Dragging commits within the log: reordered on their branch.
    bool reorder_rows(const std::vector<size_t>& rows, size_t destination);
    using edit_maker = std::function<std::expected<history_edit, replay_failure>(const repo&)>;
    // The edit made on a worker, then the working tree asked about, then
    // applied there too; a conflict offers the rebase window with `fallback`.
    void start_history_edit(const std::string& title, edit_maker,
                            std::optional<rebase_todo> fallback = {}, bool undoing = false);
    void history_replayed(const std::string& title, std::expected<history_edit, replay_failure>,
                          tree_effect, std::optional<rebase_todo> fallback, bool undoing);
    void apply_history_edit(const std::string& title, history_edit, bool stash, bool undoing);
    // A question first when the commits are on the upstream already.
    void confirm_history_edit(const std::string& title, const std::string& question, bool pushed,
                              const branch_history&, std::function<void()> go);
    std::string pushed_warning(const branch_history&) const;
    void offer_force_push(const history_edit&);
    std::unique_ptr<repo_worker> edit_worker_;
    // The last edit, for the status bar's Undo while it shows.
    std::optional<history_edit> last_edit_;
    rap::button* undo_ = nullptr;
    rap::subscription undo_timer_;
    std::vector<oid> history_;  // selection history, for the "Selection History" jump
    // The selected commit's author, top right of the details [spec 1.2].
    void show_avatar(const commit*);
    std::shared_ptr<rap::image> avatar_;
    avatars avatars_{ui_};

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
