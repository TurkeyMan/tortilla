// The notice that git isn't installed: what still works (history, read
// only), what needs git, and how to install it here.
#pragma once

#include <rapport/rapport.h>

#include <functional>

namespace tortilla
{

// Shows the notice: from Settings, or for a command that needs git.
// `done` runs once it's closed.
void show_git_missing(rap::ui&, rap::window* parent, std::function<void()> done = {});

// Shows it when git is missing, once a session (not again for some hours,
// however many windows open meanwhile); else nothing.
void notice_if_git_missing(rap::ui&, rap::window* parent = nullptr);

}  // namespace tortilla
