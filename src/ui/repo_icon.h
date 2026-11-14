// Telling repositories' windows apart on the taskbar: their icon is
// Tortilla's with the taco shell coloured. The colours go to the
// repositories open at the time: the first keeps the icon's own, each other
// one open meanwhile gets a free colour, freed again when it closes.
// Worktrees and submodules count as repositories of their own.
#pragma once

#include "ui/window_identities.h"

#include <rapport/rapport.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace tortilla
{

// The icon at one size, whole and split in two (tools/make_icons.cpp): the
// icon without its shell, and the shell's grey shading. Built in from
// data/icons/layers (CMakeLists.txt).
struct icon_layers
{
    int size;
    std::span<const uint8_t> icon, body, shell;
};
extern const std::vector<icon_layers> k_icon_layers;

// From now on, windows made with create_window without a parent carry the
// icon of the repository at `path` (anything inside it; with `outer`, the
// repository around a submodule at `path`). Outside one, they keep
// Tortilla's.
void use_repo_icon(rap::ui&, const std::string& path, bool outer = false);

class repo;

// The repository's own identity, as use_repo_icon would choose it, without
// changing what create_window gives windows: several repositories' at once
// in one process (a log window's tabs). Repositories open in this process
// keep the colour they have.
std::shared_ptr<const repo_identity> repo_identity_of(rap::ui&, const repo&);

// What `window` carries (made by create_window); nullopt when it isn't
// recorded. For something that opens later, after the scope it was asked
// in has closed: a follow-up's window, a dialog's done.
std::optional<std::shared_ptr<const repo_identity>> repo_identity_of(rap::window*);

// A window several repositories share, one shown at a time (a log window's
// tabs): what it's recorded as from now on, which dialogs made on it take,
// and repo_identity_scope(window): the repository on show. Its own icon and
// taskbar group are the caller's to set.
void set_repo_identity(rap::window*, std::shared_ptr<const repo_identity>);

// While it lives, windows create_window makes without a parent take
// `window`'s repository (or `id`), not the latest use_repo_icon's: a log's
// Commit, Rebase or Show Log are its own repository's, whichever was
// opened since. A window not recorded leaves things as they are.
class repo_identity_scope
{
public:
    explicit repo_identity_scope(rap::window* window);
    explicit repo_identity_scope(std::optional<std::shared_ptr<const repo_identity>> id);
    // And windows made on `shared` (set_repo_identity's, a log window that
    // may show another tab by now) take `id` too, until this ends: a tab's
    // work, a hidden one's late work included.
    repo_identity_scope(std::shared_ptr<const repo_identity> id, rap::window* shared);
    ~repo_identity_scope();
    repo_identity_scope(const repo_identity_scope&) = delete;
    repo_identity_scope& operator=(const repo_identity_scope&) = delete;

private:
    identity_scope scope_;
    rap::window* shared_ = nullptr;
    std::optional<std::shared_ptr<const repo_identity>> shared_was_;
};

// The menu with each action (submenus' too) run in `window`'s scope, as it
// is when the menu is made: for a window's menus, whose actions open
// windows.
rap::menu in_repo_scope(rap::window* window, rap::menu);
// `f` run in the scope of `id` (repo_identity_of, taken on the UI thread
// when the action starts), whenever it runs: for what an action opens
// later, from a posted callback or a worker's result.
std::function<void()> repo_scoped(std::optional<std::shared_ptr<const repo_identity>> id,
                                  std::function<void()> f);

// ui.create, with the repository's icon: its parent's, for a window made
// with one by create_window (ui/window_identities.h), else the scope's, else
// the one use_repo_icon chose last, else Tortilla's own. Set, not left to
// the desktop to find by name (which an uninstalled build, or WSLg,
// doesn't). The window holds its identity, and with it its repository's
// colour, until it's destroyed: its on_destroyed is taken, so set it on none
// of these.
std::unique_ptr<rap::window> create_window(rap::ui&, const rap::window_spec&,
                                           rap::window* parent = nullptr);

}  // namespace tortilla
