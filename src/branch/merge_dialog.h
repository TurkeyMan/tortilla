// TortoiseGit's Merge dialog (CMergeDlg) [spec 8.4]: what to merge into
// the current branch, the options and message; git merge in the progress
// dialog. And Abort Merge (CMergeAbortDlg) [spec 8.5]: a reset to HEAD of
// the kind chosen.
#pragma once

#include "branch/ref_chooser.h"

#include <rapport/rapport.h>

#include <expected>
#include <functional>
#include <memory>
#include <string>

namespace tortilla
{

class merge_dialog
{
public:
    // `from` preselects what to merge (a ref or hash); empty: the current
    // branch's push target, remotes/<remote>/<branch> [spec 8.4.3].
    static std::expected<std::unique_ptr<merge_dialog>, std::string>
    open(rap::ui&, const std::string& path, const std::string& from = {});
    void show() { window_->show(); }
    void on_closed(std::function<void()> f) { closed_ = std::move(f); }
    void on_done(std::function<void()> f) { done_ = std::move(f); }

private:
    merge_dialog(rap::ui&, std::string workdir);
    void build(const std::string& from);
    void update_state();
    void ok();

    rap::ui& ui_;
    std::string workdir_;
    ref_chooser from_;
    std::unique_ptr<rap::window> window_;
    std::function<void()> closed_, done_;

    rap::check* squash_ = nullptr;
    rap::check* log_ = nullptr;
    rap::spin* log_count_ = nullptr;
    rap::check* no_ff_ = nullptr;
    rap::check* ff_only_ = nullptr;
    rap::check* no_commit_ = nullptr;
    rap::drop_down* strategy_ = nullptr;
    rap::drop_down* strategy_option_ = nullptr;
    rap::entry* strategy_parameter_ = nullptr;
    rap::text_view* message_ = nullptr;
};

class abort_merge_dialog
{
public:
    static std::expected<std::unique_ptr<abort_merge_dialog>, std::string>
    open(rap::ui&, const std::string& path);
    void show() { window_->show(); }
    void on_closed(std::function<void()> f) { closed_ = std::move(f); }
    void on_done(std::function<void()> f) { done_ = std::move(f); }

private:
    abort_merge_dialog(rap::ui&, std::string workdir);
    void build();
    void ok();

    rap::ui& ui_;
    std::string workdir_;
    std::unique_ptr<rap::window> window_;
    std::function<void()> closed_, done_;
    rap::drop_down* type_ = nullptr;
};

}  // namespace tortilla
