// Create Pull Request: a branch proposed for merging into another on its
// forge (GitHub, GitLab, Gitea), from the log's branch menu. The branch is
// pushed first when the forge doesn't have it yet, or has an older copy.
#pragma once

#include "forge/pr_watch.h"

#include <rapport/rapport.h>

#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace tortilla
{

class pull_request_dialog
{
public:
    // `branch`: a local branch or a remote one; `remotes`: the repository's
    // remotes on forges Tortilla has accounts for (forge_remotes).
    static std::expected<std::unique_ptr<pull_request_dialog>, std::string>
    open(rap::ui&, const std::string& workdir, const ref& branch,
         std::vector<forge_remote> remotes);
    void show();
    void on_closed(std::function<void()> f) { closed_ = std::move(f); }
    // After it opened one (and pushed, if it did).
    void on_done(std::function<void(const pull_request&)> f) { done_ = std::move(f); }

private:
    pull_request_dialog(rap::ui&, std::string workdir);
    void build();
    void fill_into();
    void base_changed();
    void create();
    void send();
    // What Into names: a remote and its branch ("upstream/main", or just
    // "main" with one remote).
    struct target
    {
        const forge_remote* remote;
        std::string branch;
    };
    target into() const;

    rap::ui& ui_;
    std::string workdir_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    std::vector<forge_remote> remotes_;
    std::unique_ptr<rap::window> window_;
    std::function<void()> closed_;
    std::function<void(const pull_request&)> done_;

    // The branch: the local one (if it's local) and where it is on the forge.
    std::string local_;        // "" for a remote branch
    size_t head_remote_ = 0;   // into remotes_
    std::string head_branch_;  // its name there
    std::string tip_;          // the commit proposed
    bool on_forge_ = true;     // the forge has the branch (maybe an older copy)
    bool needs_push_ = false;  // the forge's copy isn't the branch as it is here
    bool sets_upstream_ = false;          // a local branch with no upstream yet
    std::string auto_title_, auto_body_;  // as last filled, to refill if untouched

    size_t preferred_ = 0;        // the remote Into means without a prefix
    std::vector<size_t> listed_;  // the remotes Into lists: the branch's forge's
    // The repositories the branch's can open PRs on (itself and what it's a
    // fork of), once the forge has said: Into lists only their remotes.
    std::optional<std::vector<forge_repo>> family_;
    void ask_family();
    rap::combo* base_ = nullptr;
    rap::entry* title_ = nullptr;
    rap::text_view* body_ = nullptr;
    rap::check* draft_ = nullptr;
    rap::check* push_ = nullptr;  // newer commits pushed first: when it's on the forge already
    rap::check* browse_ = nullptr;
    rap::label* commits_ = nullptr;
    rap::label* status_ = nullptr;
    rap::button* ok_ = nullptr;
};

}  // namespace tortilla
