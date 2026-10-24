// TortoiseGit's Add, Revert and Resolve dialogs (CAddDlg, CRevertDlg,
// CResolveDlg) [spec 6]: the working tree's files of one kind in the shared
// list, a box per file, and OK to act on the checked ones in the git
// progress dialog.
//   Add      unversioned files (and ignored ones on request): git add
//   Revert   changed files: back to HEAD, renames undone, additions unstaged
//   Resolve  conflicted files: marked resolved (git add)
#pragma once

#include "changes/changes_view.h"
#include "changes/working_tree.h"
#include "git/repo.h"
#include "git/repo_worker.h"

#include <rapport/rapport.h>

#include <expected>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace tortilla
{

enum class file_action
{
    add,
    revert,
    resolve,
};

class file_action_window
{
public:
    // `paths` are files or folders in one repository.
    static std::expected<std::unique_ptr<file_action_window>, std::string>
    open(rap::ui&, file_action, const std::vector<std::string>& paths);
    ~file_action_window();

    // Add given only files adds them without asking [spec 6.2.1].
    void show();
    // After it closes; `done` says whether the action ran.
    void on_closed(std::function<void(bool done)> f) { closed_ = std::move(f); }

private:
    file_action_window(rap::ui&, file_action, repo, std::vector<std::string> relative);

    void build();
    void reload();
    void loaded(working_tree_changes);
    void update_state();
    void run(const std::vector<file_change>& files);
    void open_commit();
    void close(bool done);
    std::string name() const;  // "Add", "Revert", "Resolve"
    std::string settings_key() const;
    bool shows(const file_change&) const;
    bool starts_checked(const file_change&) const;

    rap::ui& ui_;
    file_action action_;
    repo repo_;
    std::string workdir_;
    std::vector<std::string> paths_;        // repository-relative; empty: the whole repository
    std::set<std::string> explicit_files_;  // paths given as files, not folders
    std::unique_ptr<repo_worker> worker_;
    std::shared_ptr<git_canceller> load_;
    changes_view files_;
    std::unique_ptr<rap::window> window_;
    std::function<void(bool)> closed_;
    std::set<std::string> seen_;  // rows that have had their first check state
    bool running_ = false;
    bool shown_ = false;  // Add of files runs without showing it
    std::unique_ptr<class commit_window> commit_;  // the follow-up Commit, while open

    rap::check* select_all_ = nullptr;
    rap::check_state all_state_ = rap::check_state::unchecked;  // what it last showed
    rap::check* ignored_ = nullptr;                             // Add: "Include ignored files"
    rap::label* note_ = nullptr;
    rap::button* ok_ = nullptr;
};

}  // namespace tortilla
