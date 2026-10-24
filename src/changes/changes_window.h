// TortoiseGit's Check for modifications (CChangedDlg) [spec 5.3]: the
// working tree's changes under some paths, in the shared changes list, with
// filters, statistics and Stash/Commit/Refresh. Explorer's Diff on a folder
// opens it too [spec 5.1].
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

class changes_window
{
public:
    // `paths` are files or folders in one repository; the repository's
    // root (or nothing) means the whole repository.
    static std::expected<std::unique_ptr<changes_window>, std::string>
    open(rap::ui&, const std::vector<std::string>& paths);
    ~changes_window();

    void show() { window_->show(); }
    void on_closed(std::function<void()> f) { closed_ = std::move(f); }

private:
    changes_window(rap::ui&, repo, std::vector<std::string> relative);

    void build();
    void reload();
    void loaded(working_tree_changes);
    void update_title();
    void update_stats();
    void set_loading(bool);
    rap::menu stash_menu();
    void show_unified_diff_of_paths();
    void open_commit();

    rap::ui& ui_;
    repo repo_;
    std::string workdir_;
    std::vector<std::string> paths_;  // repository-relative; empty: the whole repository
    bool root_ = false;               // opened on the repository root: whole project, fixed
    bool one_folder_ = false;         // one folder (or the root): unversioned/ignored filters apply
    bool loading_ = false;
    rap::subscription loading_delay_;  // shows the loading state only if it's slow
    // Stops the running load's git: a newer reload restarts rather than waits.
    std::shared_ptr<git_canceller> load_;
    std::unique_ptr<repo_worker> worker_;
    changes_view files_;
    std::vector<file_change> loaded_;  // everything loaded, before the filters
    std::unique_ptr<rap::window> window_;
    std::function<void()> closed_;
    std::unique_ptr<class commit_window> commit_window_;

    rap::label* branch_ = nullptr;
    rap::label* stats_ = nullptr;
    rap::label* note_ = nullptr;  // "Please wait..." / nothing to show
    rap::check* unversioned_ = nullptr;
    rap::check* local_ignored_ = nullptr;
    rap::check* ignored_ = nullptr;
    rap::check* all_staged_ = nullptr;
    rap::check* whole_project_ = nullptr;
    rap::button* refresh_ = nullptr;
};

}  // namespace tortilla
