// Which repository each window belongs to, for its icon and taskbar group
// (ui/repo_icon.h). The log windows' host has several repositories' windows
// open at once, so it's a window's, not the process's: a dialog takes its
// parent window's, whatever repository was opened since. Kept apart from
// the windows themselves (keyed by address only) so it's tested without a
// UI.
#pragma once

#include <rapport/rapport.h>

#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>

namespace tortilla
{

// A repository's look on the taskbar, shared by its windows.
struct repo_identity
{
    std::string repo;      // its folder (its git folder, if bare)
    rap::icon icon;        // Tortilla's, coloured or not
    std::string app_id;    // its own taskbar group; "" keeps Tortilla's
    // What holds its colour against other processes (repo_icon.cpp's lock
    // file): let go once nothing has this identity, no window and not
    // window_identities::current.
    std::shared_ptr<void> lease;
};

class window_identities
{
public:
    // The identity windows made from now on take when they have no parent
    // (or one this doesn't know); nullptr: none (Tortilla's own).
    void use(std::shared_ptr<const repo_identity>);
    const std::shared_ptr<const repo_identity>& current() const { return current_; }

    // Window `w` is being made with `parent` (or none): its identity, its
    // parent's if that's recorded, else the scope's (identity_scope) if one
    // is open, else current(). Recorded until destroyed(w).
    std::shared_ptr<const repo_identity> created(const void* w, const void* parent);
    void destroyed(const void* w);

    // What `w` was given; nullopt if it isn't recorded (nullptr: it was
    // given none, Tortilla's own).
    std::optional<std::shared_ptr<const repo_identity>> recorded(const void* w) const;
    std::shared_ptr<const repo_identity> of(const void* w) const { return recorded(w).value_or(nullptr); }
    size_t size() const { return by_window_.size(); }

private:
    friend class identity_scope;
    std::shared_ptr<const repo_identity> current_;
    // While an identity_scope is open: what windows without a parent take.
    std::optional<std::shared_ptr<const repo_identity>> scope_;
    std::map<const void*, std::shared_ptr<const repo_identity>> by_window_;
};

// `w`, just made with `parent` (or none), recorded as created() does, and
// let go (destroyed()) when rapport destroys it: `w`'s on_destroyed is
// taken. `ids` must outlive every window it tracks, as they may be torn
// down at exit. Returns `w`'s identity.
std::shared_ptr<const repo_identity> track_window(window_identities& ids, rap::window& w,
                                                  rap::window* parent);

// While it lives, windows made without a parent take `id`, not current():
// a window's own actions (Commit, Rebase, a log from a log) open windows of
// its repository, whichever was opened since. Nested scopes restore the one
// outside them. An unset `id` (a window not recorded) leaves things as they
// are.
class identity_scope
{
public:
    identity_scope(window_identities&, std::optional<std::shared_ptr<const repo_identity>> id);
    ~identity_scope();
    identity_scope(const identity_scope&) = delete;
    identity_scope& operator=(const identity_scope&) = delete;

private:
    window_identities& ids_;
    std::optional<std::shared_ptr<const repo_identity>> outer_;
    bool open_ = false;
};

// `m` with each action, submenus' too, run in an identity_scope of `id`
// (as it was when the menu was made); unchanged for an unset `id`.
rap::menu in_identity_scope(window_identities&,
                            std::optional<std::shared_ptr<const repo_identity>> id, rap::menu m);
// `f` run in an identity_scope of `id`, whenever it runs: for what a scoped
// action does later (a posted callback, a worker's result), after its own
// scope has closed. Capture `id` on the UI thread, where this lives.
std::function<void()> in_identity_scope(window_identities&,
                                        std::optional<std::shared_ptr<const repo_identity>> id,
                                        std::function<void()> f);

}  // namespace tortilla
