// TortoiseGit's Commit dialog (CCommitDlg) [spec 5.4]: the message, its
// options, and the working tree's changes in the shared list with a box per
// file; the checked files are what's committed.
#pragma once

#include "changes/changes_view.h"
#include "changes/working_tree.h"
#include "git/repo.h"
#include "git/repo_worker.h"

#include <rapport/rapport.h>

#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tortilla
{

class commit_window
{
public:
    // `paths` are files or folders in one repository; the root (or none)
    // means the whole repository.
    static std::expected<std::unique_ptr<commit_window>, std::string>
    open(rap::ui&, const std::vector<std::string>& paths);
    ~commit_window();

    void show() { window_->show(); }
    // After it closes; `committed` says whether a commit was made.
    void on_closed(std::function<void(bool committed)> f) { closed_ = std::move(f); }

private:
    commit_window(rap::ui&, repo, std::vector<std::string> relative);

    void build();
    void reload();
    void loaded(working_tree_changes);
    void update_title();
    void update_state();
    void amend_toggled(bool on);
    void add_signoff();
    void commit();
    void close(bool committed);
    std::string config(const std::string& key) const;

    rap::ui& ui_;
    repo repo_;
    std::string workdir_;
    std::vector<std::string> paths_;  // repository-relative; empty: the whole repository
    bool root_ = false;
    std::unique_ptr<repo_worker> worker_;
    std::shared_ptr<git_canceller> load_;
    changes_view files_;
    std::unique_ptr<rap::window> window_;
    std::function<void(bool)> closed_;
    std::string pre_amend_message_;  // the message before Amend replaced it
    bool committing_ = false;
    bool can_commit_ = false;  // Commit's enabled state

    rap::label* branch_ = nullptr;
    rap::check* new_branch_ = nullptr;
    rap::entry* new_branch_name_ = nullptr;
    rap::text_view* message_ = nullptr;
    rap::check* amend_ = nullptr;
    rap::check* set_date_ = nullptr;
    rap::date_picker* date_ = nullptr;
    rap::entry* time_ = nullptr;
    rap::check* set_author_ = nullptr;
    rap::entry* author_ = nullptr;
    rap::check* unversioned_ = nullptr;
    rap::check* whole_project_ = nullptr;
    rap::check* message_only_ = nullptr;
    rap::label* stats_ = nullptr;
    rap::button* commit_ = nullptr;
};

}  // namespace tortilla
