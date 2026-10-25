// TortoiseGit's Push dialog (CPushDlg) [spec 7.3]: which branch to which
// remote (or URL) and branch, the options, then git push in the progress
// dialog.
#pragma once

#include <rapport/rapport.h>

#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tortilla
{

class push_dialog
{
public:
    // `local` preselects what's pushed (a branch, refs/tags/<tag>).
    static std::expected<std::unique_ptr<push_dialog>, std::string>
    open(rap::ui&, const std::string& path, const std::string& local = {});
    void show() { window_->show(); }
    void on_closed(std::function<void()> f) { closed_ = std::move(f); }

private:
    push_dialog(rap::ui&, std::string workdir);
    void build(const std::string& local);
    void local_changed();  // the remote and branch it pushes to
    void update_state();
    int first_remote() const;
    void manage_remotes();
    void refresh_remotes();
    bool by_url() const;
    bool all_remotes() const;
    void ok();
    void push();

    rap::ui& ui_;
    std::string workdir_;
    std::vector<std::string> remotes_;
    std::unique_ptr<rap::window> window_;
    std::function<void()> closed_;

    rap::check* all_branches_ = nullptr;
    rap::combo* local_ = nullptr;
    rap::combo* remote_branch_ = nullptr;
    rap::drop_down* remote_ = nullptr;
    rap::combo* url_ = nullptr;
    rap::check* force_with_lease_ = nullptr;
    rap::check* force_ = nullptr;
    rap::check* tags_ = nullptr;
    rap::check* set_upstream_ = nullptr;
    rap::drop_down* recurse_ = nullptr;
    rap::entry* push_option_ = nullptr;
};

}  // namespace tortilla
