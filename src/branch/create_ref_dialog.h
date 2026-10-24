// TortoiseGit's Create Branch / Create Tag dialog (CCreateBranchTagDlg)
// [spec 8.2]: a name, what it's based on, and the options; from Explorer
// on HEAD, from the log on the row chosen.
#pragma once

#include "branch/ref_chooser.h"

#include <rapport/rapport.h>

#include <expected>
#include <functional>
#include <memory>
#include <string>

namespace tortilla
{

class create_ref_dialog
{
public:
    // `base` preselects what it's based on (a ref or hash); `name` fills
    // the name; `switch_to` ticks "Switch to new branch".
    static std::expected<std::unique_ptr<create_ref_dialog>, std::string>
    open(rap::ui&, const std::string& path, bool tag, const std::string& base = {},
         const std::string& name = {}, bool switch_to = false);
    void show();
    void on_closed(std::function<void()> f) { closed_ = std::move(f); }
    // After it made a branch or tag (the log reloads).
    void on_done(std::function<void()> f) { done_ = std::move(f); }

private:
    create_ref_dialog(rap::ui&, std::string workdir, bool tag);
    void build(const std::string& base, const std::string& name, bool switch_to);
    void base_changed();
    void name_changed();
    void ok();
    void create();

    rap::ui& ui_;
    std::string workdir_;
    bool tag_;
    ref_chooser base_;
    std::string auto_name_;  // the name last filled from a remote branch
    std::unique_ptr<rap::window> window_;
    std::function<void()> closed_, done_;

    rap::entry* name_ = nullptr;
    rap::check* track_ = nullptr;
    rap::check* force_ = nullptr;
    rap::check* switch_ = nullptr;
    rap::check* sign_ = nullptr;
    rap::check* push_ = nullptr;
    rap::text_view* message_ = nullptr;
};

}  // namespace tortilla
