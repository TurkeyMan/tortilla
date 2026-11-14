// The Log window: repositories' logs (log_tab.h) as document tabs in one
// window, one page each in a stack, the strip choosing which shows. What's
// the window's own: its size and place, its keys, its closing, View Patch,
// and opening, closing, ordering and moving the tabs (torn off into windows
// of their own, dropped into other log windows).
#pragma once

#include "log/log_host.h"
#include "log/log_tab.h"
#include "log/tab_host.h"

#include <rapport/rapport.h>

#include <cstdint>
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
    // The window has that one tab.
    static std::expected<std::unique_ptr<log_window>, std::string>
    open(rap::ui&, const std::string& path, bool all_branches = false, bool outer = false);
    ~log_window();

    // Shown, and brought to the front if it's open already.
    void show();
    // As show(), and restored if minimised, raised and activated where the
    // platform lets it (rap::window::present): for a log another process
    // asked for, which the user is waiting to see.
    void present();
    // A log another `tortilla log` handed over (log/log_host.h), opened as
    // it asked: unless r.new_window, a tab of forward_target() (or that
    // window's tab showing it already, selected, as open_tab does);
    // otherwise, or with no such window, a log window of its own, given to
    // `keep`. Either way the window is present()ed after: restored if
    // minimised, brought to the front. r.select selected in it either
    // way. The window it's in; the error says why the repository can't be
    // shown (nothing opened then).
    static std::expected<log_window*, std::string>
    open_forwarded(rap::ui&, const log_request& r,
                   const std::function<void(std::unique_ptr<log_window>)>& keep);
    // The log window a log handed over goes into as a tab: of those that
    // take tabs (not picking commits) and have no dialog open over them, the
    // one shown, or its tab selected or a key pressed in it, last. Null when
    // there's none.
    static log_window* forward_target();
    // Shows the history from `rev` (a ref or commit) instead of HEAD; in
    // the tab the window opened with.
    void start_at(const std::string& rev) { tabs_.front()->start_at(rev); }
    // Selects `rev`'s commit once it's loaded (at once if it is), scrolled
    // into view; later reloads keep it selected, as any selection. In the
    // tab the window opened with.
    void select_commit(const std::string& rev) { tabs_.front()->select_commit(rev); }
    void on_closed(std::function<void()> f) { closed_ = std::move(f); }
    // Picking commits for another window (the rebase's Add): the title says
    // `purpose`, OK becomes `label` and hands it the selected commits, newest
    // first, then closes, and Cancel closes without. Merges and the working
    // tree row are left out, and OK waits for a commit it can hand over.
    // The window keeps its one tab: no strip, and none opened.
    void pick_commits(const std::string& purpose, const std::string& label,
                      std::function<void(std::vector<commit>)> chosen);

    // -- Tabs -------------------------------------------------------------------
    // `path` in a tab of its own, last, selected and loading; or, when a tab
    // shows it already (the same repository, limited to the same path),
    // that tab selected. The error says why it isn't a repository.
    std::expected<log_tab*, std::string> open_tab(const std::string& path,
                                                  bool all_branches = false, bool outer = false);
    // + and Ctrl+T: the repository to open asked for (recent ones, or
    // Browse...), opened in a tab once chosen.
    void new_tab();
    void select_tab(const std::string& id);
    // Its settings kept, its work given up and it gone, the next one
    // shown; the last closes the window (once this has returned). False,
    // and said why, while it can't close (log_tab::busy_reason).
    bool close_tab(const std::string& id);
    // Where the user dragged it to: `id` now at `index` in the strip.
    void reorder(const std::string& id, int index);
    // -- Moving tabs between windows ---------------------------------------------
    // Log windows' tabs are dragged out into windows of their own (or moved
    // there from the tabs' menu), and from one log window into another's
    // strip; the windows picking commits take no part.
    //
    // The tab `id` taken out of the window, to go into another (adopt): its
    // view kept, its page gone, its work going on without one. Null, and the
    // window as it was, while it can't move (move_refusal). The window's
    // last tab gone, the window closes (once the caller has returned).
    std::unique_ptr<log_tab> detach(const std::string& id);
    // Why `id` can't move to another window now (picking commits, git at
    // work in it, a dialog open over the window); empty when it can.
    std::string move_refusal(const std::string& id) const;
    // A tab from another window (detach) made part of this one, at `index`
    // in the strip (-1: last), selected and drawn as it was. Only while
    // takes_tabs().
    log_tab& adopt(std::unique_ptr<log_tab>, int index = -1);
    // Whether tabs may move in: not while picking commits or closing.
    bool takes_tabs() const { return !picking_ && !closing_ && !emptied_; }
    // The tabs' tearing off (rap::document_tabs::detach_fn): a window of
    // its own for `id`, sized as this one, at `at` (or, from the menu, a
    // little down and right of this one). Not shown: rapport shows it.
    rap::window* tear_off(const std::string& id, std::optional<rap::point_f> at);
    // A tab dropped in from another log window's strip (or sent home to
    // this one): moved here, at t.index. False, and both as they were, if
    // t.source isn't a log window or the tab can't move.
    bool receive(const rap::document_tabs::transfer& t);
    // The log window whose frame is `w` (null: none).
    static log_window* of(const rap::window* w);
    // Tests only: what keeps a torn-off window (else windows::hold, until
    // it closes).
    static inline std::function<void(std::unique_ptr<log_window>)>* keep_for_tests = nullptr;
    // In the strip's order.
    const std::vector<std::unique_ptr<log_tab>>& tabs() const { return tabs_; }
    log_tab* current_tab() const;
    rap::window* frame() const { return window_.get(); }

private:
    explicit log_window(rap::ui&);

    // Where a torn-off window goes: sized as the window it came from, and
    // put there.
    struct placement
    {
        rap::size_f size;
        std::optional<rap::point_f> at;
    };
    void build(std::unique_ptr<log_tab> first, std::optional<placement> = std::nullopt);
    // A tab made part of the window: its page in the stack (drawn as its
    // carried view had it, if it's come from another window), its tab in
    // the strip (at `index`; -1 last); not selected.
    log_tab& place(std::unique_ptr<log_tab>, int index = -1);
    // The tab `id` on show: its page, and the window's title, icon and View
    // Patch from it. (The strip has it selected already.)
    void show_tab(const std::string& id);
    rap::document_tab strip_item(const log_tab&) const;
    std::vector<std::unique_ptr<log_tab>>::iterator find(const std::string& id);
    // The window closing: its size and place kept, each tab's settings
    // too (the current tab's last, so its are what's left), and View
    // Patch's window closed with it (on for next time).
    void closing();
    // Closed from here (OK, the last tab's closing): as its close box does.
    void close_window();

    rap::window* window() override { return window_.get(); }
    void tab_changed(log_tab&) override;
    void finish(log_tab&, bool ok) override;
    bool patch_shown() const override { return patch_.has_value(); }
    void show_patch(bool on) override;
    void selection_moved(log_tab&) override;

    // View Patch [spec 1.9.5]: the current tab's selection's patch in a
    // window of its own, following the selection and the tabs; whether it's
    // open is kept across runs (log.viewPatch), for every log window alike.
    std::optional<patch_viewer> patch_;
    unsigned patch_generation_ = 0;  // drops a patch the selection has left
    rap::subscription patch_timer_;
    void update_patch();
    void close_patch();
    void follow_patch(const log_tab&);

    rap::ui& ui_;
    // Expires with the window, for what can call back after it has gone.
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    // Before window_, so they go after it: the window's lists use the tabs'
    // models, and a list tells its model it's going as it goes.
    std::vector<std::unique_ptr<log_tab>> tabs_;
    std::string current_;  // the tab on show's id
    bool picking_ = false;
    bool closing_ = false;  // closed, or about to be
    // Its last tab moved to another window: closing once the handler that
    // moved it has returned, taking no other.
    bool emptied_ = false;
    std::unique_ptr<rap::window> window_;
    rap::document_tabs* strip_ = nullptr;
    rap::stack* stack_ = nullptr;
    std::function<void()> closed_;
    // When it was last used (shown, a tab selected, a key pressed), in
    // the order of all log windows': forward_target's choice. (rapport says
    // nothing of a window's activation.)
    uint64_t last_used_ = 0;
    void used();
};

}  // namespace tortilla
