// TortoiseGit's Pull and Fetch dialog (CPullFetchDlg) [spec 7.2]: the
// remote (or a URL) and branch, the options, then git pull or git fetch in
// the progress dialog, with "Launch Rebase After Fetch" fetching and then
// opening the Rebase dialog.
#pragma once

#include <rapport/rapport.h>

#include <expected>
#include <functional>
#include <memory>
#include <string>

namespace tortilla
{

class pull_fetch_dialog
{
public:
    static std::expected<std::unique_ptr<pull_fetch_dialog>, std::string>
    open(rap::ui&, const std::string& path, bool pull);
    void show();
    void on_closed(std::function<void()> f) { closed_ = std::move(f); }

private:
    pull_fetch_dialog(rap::ui&, std::string workdir, bool pull);
    void build();
    void remote_changed();
    std::string remote() const;  // the chosen remote's name, or the URL; "" for all remotes
    int first_remote() const;
    bool by_url() const;
    bool all_remotes() const;
    void ok();
    void finished();

    rap::ui& ui_;
    std::string workdir_;
    bool pull_;
    std::string branch_;  // checked out; "" when detached
    std::vector<std::string> remotes_;
    std::unique_ptr<rap::window> window_;
    std::function<void()> closed_;

    rap::drop_down* remote_ = nullptr;
    rap::combo* url_ = nullptr;
    rap::combo* remote_branch_ = nullptr;
    rap::check* squash_ = nullptr;
    rap::check* no_ff_ = nullptr;
    rap::check* no_commit_ = nullptr;
    rap::check* ff_only_ = nullptr;
    rap::check* depth_ = nullptr;
    rap::spin* depth_value_ = nullptr;
    rap::drop_down* tags_ = nullptr;
    rap::drop_down* prune_ = nullptr;
    rap::check* rebase_ = nullptr;
    bool rebase_from_config_ = false;  // pull.rebase / branch.<b>.rebase said so
};

}  // namespace tortilla
