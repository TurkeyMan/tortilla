// TortoiseGit's Rebase dialog (IDD_REBASE) [spec 2].
#pragma once

#include "changes/changes_view.h"
#include "changes/file_model.h"
#include "git/repo_worker.h"
#include "rebase/rebase_engine.h"
#include "rebase/rebase_model.h"

#include <rapport/rapport.h>

#include <memory>

namespace tortilla
{

class rebase_window
{
public:
    // `upstream` preselects the upstream (a ref or hash); empty = the
    // current branch's upstream.
    static std::expected<std::unique_ptr<rebase_window>, std::string>
    open(rap::ui&, const std::string& path, const std::string& upstream = {});

    void show() { window_->show(); }
    void on_closed(std::function<void()> f) { closed_ = std::move(f); }
    // "Show log" from the post-action menu when the rebase is done.
    void on_show_log(std::function<void()> f) { show_log_ = std::move(f); }

private:
    rebase_window(rap::ui&, repo);

    void build(const std::string& upstream);
    void reload_list();
    void pick_upstream();
    void toggle_onto();
    void selection_changed();
    void update_controls();
    void set_status(const std::string&, bool warning = false);
    void continue_pressed();
    void abort_pressed();
    void close();
    void start_rebase();
    void move_rows(bool up, bool to_end);
    bool drop_rows(const std::vector<size_t>& rows, size_t destination);
    void set_action(rebase_action);
    bool key(const rap::key_event&);
    rap::menu context_menu(std::optional<size_t> row);
    rap::menu set_all_menu();
    void append_log(const std::string& text, bool command);
    void refresh_conflicts();
    changes_view::host files_host();

    rap::ui& ui_;
    repo repo_;
    rebase_model model_;
    changes_view revision_files_, conflict_files_;
    std::unique_ptr<rebase_engine> engine_;
    std::unique_ptr<rap::window> window_;
    std::function<void()> closed_, show_log_;

    std::vector<std::string> branches_, upstreams_;
    std::optional<oid> branch_head_, upstream_head_;
    std::string onto_;  // --onto, as picked; "" for the upstream itself
    // Callbacks that outlive the window (a picker) check this.
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    rap::button* add_ = nullptr;
    // Add: a log of every branch to pick commits from [spec 2.5].
    void add_commits();
    // Commits added (Add, or dropped from a log) as picks in front of row
    // `at`; false when none went in.
    bool can_add() const;
    bool add_commits_at(size_t at, const std::vector<std::string>& hashes);
    bool fast_forward_ = false;
    bool can_start_ = false;
    bool stashed_ = false;
    bool closing_ = false;
    // Off the UI thread, newest job only (git there is slow on Windows over
    // \\wsl.localhost): the clicked commit's files, and the list's git
    // cherry and labels. Last, so they go before what their results touch.
    std::unique_ptr<repo_worker> files_worker_, list_worker_;

    // Widgets
    rap::drop_down* branch_ = nullptr;
    rap::drop_down* upstream_ = nullptr;
    rap::button* swap_ = nullptr;
    rap::button* onto_button_ = nullptr;
    rap::list* list_ = nullptr;
    rap::menu_button* set_all_ = nullptr;
    rap::button* up_ = nullptr;
    rap::button* down_ = nullptr;
    rap::check* preserve_merges_ = nullptr;
    rap::check* force_ = nullptr;
    rap::tabs* tabs_ = nullptr;
    rap::widget* revision_page_ = nullptr;
    rap::widget* conflict_page_ = nullptr;
    rap::widget* message_page_ = nullptr;
    rap::widget* log_page_ = nullptr;
    rap::text_view* message_ = nullptr;
    rap::text_view* log_ = nullptr;
    rap::progress_bar* progress_ = nullptr;
    rap::label* status_ = nullptr;
    rap::menu_button* post_ = nullptr;
    rap::button* continue_ = nullptr;
    rap::button* abort_ = nullptr;
};

}  // namespace tortilla
