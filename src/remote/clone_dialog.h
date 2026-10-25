// TortoiseGit's Clone dialog (CCloneDlg) [spec 7.1]: a URL, the folder to
// clone into (named after the URL as it's typed), the options, then git
// clone in the progress dialog.
#pragma once

#include <rapport/rapport.h>

#include <functional>
#include <memory>
#include <string>

namespace tortilla
{

class clone_dialog
{
public:
    // `folder` is where the clone goes (inside it); `url` prefills.
    static std::unique_ptr<clone_dialog> open(rap::ui&, const std::string& folder,
                                              const std::string& url = {});
    void show() { window_->show(); }
    void on_closed(std::function<void()> f) { closed_ = std::move(f); }

private:
    clone_dialog(rap::ui&);
    void build(const std::string& folder, const std::string& url);
    void url_changed(const std::string& url);
    void update_state();
    void ok();

    rap::ui& ui_;
    std::unique_ptr<rap::window> window_;
    std::function<void()> closed_;
    std::string module_;  // the folder name last derived from the URL
    // The clipboard's answer comes later (GTK); it checks this is still there.
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);

    rap::combo* url_ = nullptr;
    rap::entry* directory_ = nullptr;
    rap::check* depth_ = nullptr;
    rap::spin* depth_value_ = nullptr;
    rap::check* recursive_ = nullptr;
    rap::check* bare_ = nullptr;
    rap::check* no_checkout_ = nullptr;
    rap::check* branch_ = nullptr;
    rap::entry* branch_name_ = nullptr;
    rap::check* origin_ = nullptr;
    rap::entry* origin_name_ = nullptr;
};

}  // namespace tortilla
