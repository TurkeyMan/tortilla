// The Log window: a window with a repository's log in it (log_tab.h), and
// what's the window's own: its size and place, its keys, its closing, and
// View Patch.
#pragma once

#include "log/log_tab.h"
#include "log/tab_host.h"

#include <rapport/rapport.h>

#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace tortilla
{

class log_window : tab_host
{
public:
    // `path` is any path inside the repository; the history is limited to
    // it (Show Whole Project lifts that). `all_branches` starts with All
    // Branches checked. `outer`: the repository around a submodule at
    // `path`, rather than the submodule's own (Show log of this folder).
    static std::expected<std::unique_ptr<log_window>, std::string>
    open(rap::ui&, const std::string& path, bool all_branches = false, bool outer = false);
    ~log_window();

    void show() { window_->show(); }
    // Shows the history from `rev` (a ref or commit) instead of HEAD.
    void start_at(const std::string& rev) { tab_->start_at(rev); }
    // Selects `rev`'s commit once it's loaded (at once if it is), scrolled
    // into view; later reloads keep it selected, as any selection.
    void select_commit(const std::string& rev) { tab_->select_commit(rev); }
    void on_closed(std::function<void()> f) { closed_ = std::move(f); }
    // Picking commits for another window (the rebase's Add): the title says
    // `purpose`, OK becomes `label` and hands it the selected commits, newest
    // first, then closes, and Cancel closes without. Merges and the working
    // tree row are left out, and OK waits for a commit it can hand over.
    void pick_commits(const std::string& purpose, const std::string& label,
                      std::function<void(std::vector<commit>)> chosen)
    {
        tab_->pick_commits(purpose, label, std::move(chosen));
    }

private:
    log_window(rap::ui&, std::unique_ptr<log_tab>);

    void build();
    // The window closing: its size and place kept, the tab's settings too,
    // and View Patch's window closed with it (on for next time).
    void closing();

    rap::window* window() override { return window_.get(); }
    void tab_changed(log_tab&) override;
    void finish(bool ok) override;
    bool patch_shown() const override { return patch_.has_value(); }
    void show_patch(bool on) override;
    void selection_moved(log_tab&) override;

    // View Patch [spec 1.9.5]: the selection's patch in a window of its
    // own, following the selection; whether it's open is kept across runs
    // (log.viewPatch), for every log window alike.
    std::optional<patch_viewer> patch_;
    unsigned patch_generation_ = 0;  // drops a patch the selection has left
    rap::subscription patch_timer_;
    void update_patch();
    void close_patch();

    rap::ui& ui_;
    // Expires with the window, for what can call back after it has gone.
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    // Before window_, so it goes after it: the window's lists use the tab's
    // models, and a list tells its model it's going as it goes.
    std::unique_ptr<log_tab> tab_;
    std::unique_ptr<rap::window> window_;
    std::function<void()> closed_;
};

}  // namespace tortilla
