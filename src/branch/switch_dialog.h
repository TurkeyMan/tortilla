// TortoiseGit's Switch/Checkout dialog (CGitSwitchDlg) [spec 8.3]: what to
// switch to, optionally as a new branch, and how; git checkout in the
// progress dialog.
#pragma once

#include "branch/ref_chooser.h"

#include <rapport/rapport.h>

#include <expected>
#include <functional>
#include <memory>
#include <string>

namespace tortilla
{

class switch_dialog
{
public:
    // `target` preselects what to switch to (a ref or hash).
    static std::expected<std::unique_ptr<switch_dialog>, std::string>
    open(rap::ui&, const std::string& path, const std::string& target = {});
    void show() { window_->show(); }
    void on_closed(std::function<void()> f) { closed_ = std::move(f); }
    void on_done(std::function<void()> f) { done_ = std::move(f); }

private:
    switch_dialog(rap::ui&, std::string workdir);
    void build(const std::string& target);
    void target_changed();
    void update_state();
    void ok();

    rap::ui& ui_;
    std::string workdir_;
    ref_chooser target_;
    std::unique_ptr<rap::window> window_;
    std::function<void()> closed_, done_;

    rap::check* new_branch_ = nullptr;
    rap::entry* name_ = nullptr;
    rap::check* force_ = nullptr;
    rap::check* merge_ = nullptr;
    rap::check* track_ = nullptr;
    rap::check* override_ = nullptr;
};

}  // namespace tortilla
