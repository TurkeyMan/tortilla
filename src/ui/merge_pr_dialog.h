// Merge Pull Request and Close Pull Request, from the log's "Pull request #N"
// menu. The forge does the merging, as its own page would; Tortilla asks it
// first what's allowed (the methods, whether it can be merged now, whether
// the user may bypass the rules) and offers only that.
#pragma once

#include "forge/pr_watch.h"

#include <rapport/rapport.h>

#include <functional>
#include <memory>
#include <string>

namespace tortilla
{

class merge_pr_dialog
{
public:
    static std::unique_ptr<merge_pr_dialog> open(rap::ui&, const open_pr&);
    void show();
    void on_closed(std::function<void()> f) { closed_ = std::move(f); }
    // After the forge merged it (`now`), or set it to merge when ready.
    void on_done(std::function<void(bool now)> f) { done_ = std::move(f); }

private:
    merge_pr_dialog(rap::ui&, open_pr);
    void build();
    void ask();
    void answered(std::expected<merge_state, std::string>);
    void method_changed();
    void update();
    void merge(bool when_ready);
    void send(bool when_ready);

    rap::ui& ui_;
    open_pr pr_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    std::unique_ptr<rap::window> window_;
    std::function<void()> closed_;
    std::function<void(bool)> done_;
    std::optional<merge_state> state_;
    int asked_ = 0;  // asks while the forge is still working it out
    rap::subscription again_;
    bool busy_ = false;
    bool can_delete_ = false;  // the branch is the repository's, and the forge keeps merged ones
    std::string auto_title_;

    rap::label* status_ = nullptr;
    rap::drop_down* method_ = nullptr;
    rap::entry* title_ = nullptr;
    rap::text_view* message_ = nullptr;
    rap::check* delete_ = nullptr;
    rap::check* bypass_ = nullptr;
    rap::button* merge_ = nullptr;
    rap::button* when_ready_ = nullptr;
};

class close_pr_dialog
{
public:
    static std::unique_ptr<close_pr_dialog> open(rap::ui&, const open_pr&);
    void show();
    void on_closed(std::function<void()> f) { closed_ = std::move(f); }
    void on_done(std::function<void()> f) { done_ = std::move(f); }

private:
    close_pr_dialog(rap::ui&, open_pr);
    void close_it();

    rap::ui& ui_;
    open_pr pr_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    std::unique_ptr<rap::window> window_;
    std::function<void()> closed_, done_;
    rap::text_view* reason_ = nullptr;
    rap::check* delete_ = nullptr;
    rap::label* status_ = nullptr;
    rap::button* ok_ = nullptr;
};

}  // namespace tortilla
