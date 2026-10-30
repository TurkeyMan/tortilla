// Explorer's Worktrees (TortoiseGit's worktreelist [spec 4.2, row 52]): the
// repository's working trees, with what each has checked out and whether
// it's locked or can be pruned; adding one, removing, locking, pruning, and
// the log or the file manager at one.
#pragma once

#include "git/worktrees.h"
#include "ui/progress_dialog.h"

#include <rapport/rapport.h>

#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tortilla
{

class git_process;

class worktree_window
{
public:
    static std::expected<std::unique_ptr<worktree_window>, std::string>
    open(rap::ui&, const std::string& path, rap::window* parent = nullptr);
    ~worktree_window();
    void show() { window_->show(); }
    void on_closed(std::function<void()> f) { closed_ = std::move(f); }

private:
    class rows;        // the list's model
    class add_dialog;  // Add...

    worktree_window(rap::ui&, std::string dir, rap::window* parent);
    void build();
    // The worktrees again, keeping the one at `select` selected.
    void reload(const std::string& select = {});
    void update_state();
    const worktree_info* chosen() const;

    rap::menu context_menu();
    void show_log(const worktree_info&);
    void explore(const worktree_info&);
    void add();
    void remove(const worktree_info&);
    void lock(const worktree_info&, bool lock);
    void prune();
    void run(const std::string& title, std::vector<git_command> commands,
             const std::string& select = {}, std::vector<progress_follow_up> follow_ups = {});

    rap::ui& ui_;
    std::string dir_;   // where git runs: the main working tree, or a bare repository
    std::string here_;  // the worktree it was opened in, if any
    rap::window* parent_;
    std::unique_ptr<rows> rows_;  // before the window: its list goes first
    std::unique_ptr<rap::window> window_;
    std::unique_ptr<add_dialog> add_;
    std::shared_ptr<git_process> locking_;
    std::function<void()> closed_;
    // Callbacks that outlive the window (a progress dialog, a confirmation)
    // check this is still there.
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);

    rap::list* list_ = nullptr;
    rap::label* info_ = nullptr;
};

}  // namespace tortilla
