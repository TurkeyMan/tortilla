// Bisect [spec 1.11, 4.2 rows 28-32]: git bisect's steps in the progress
// dialog, where git says how many revisions are left to test; and, for
// Explorer, TortoiseGit's Bisect start dialog (CBisectStartDlg), naming the
// last good and the first bad revision. The log starts it from two
// selected commits instead.
#pragma once

#include "branch/ref_chooser.h"
#include "log/log_model.h"

#include <rapport/rapport.h>

#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tortilla
{

// `git bisect <step> [revs]` in the progress dialog; `done` once it closes.
void run_bisect(rap::ui&, rap::window* parent, const std::string& workdir, bisect_step,
                std::vector<std::string> revs = {}, std::function<void(bool ok)> done = {});

class bisect_start_dialog
{
public:
    // `good` and `bad` preselect the revisions; empty: HEAD is bad, its
    // parent good.
    static std::expected<std::unique_ptr<bisect_start_dialog>, std::string>
    open(rap::ui&, const std::string& path, const std::string& good = {},
         const std::string& bad = {});
    void show() { window_->show(); }
    void on_closed(std::function<void()> f) { closed_ = std::move(f); }
    void on_done(std::function<void()> f) { done_ = std::move(f); }

private:
    bisect_start_dialog(rap::ui&, std::string workdir);
    void build(const std::string& good, const std::string& bad);
    void ok();

    rap::ui& ui_;
    std::string workdir_;
    ref_chooser good_, bad_;
    std::unique_ptr<rap::window> window_;
    std::function<void()> closed_, done_;
};

}  // namespace tortilla
