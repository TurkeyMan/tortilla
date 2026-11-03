// The busy pointer while a program Tortilla started comes up (a diff tool,
// a file opened in its app), as TortoiseGit shows Windows' app-starting
// pointer: the click was heard, and Tortilla stays usable meanwhile.
#pragma once

#include <rapport/rapport.h>

#include <functional>

namespace tortilla
{

// How long it shows at most: about how long a tool takes to show its window.
inline constexpr int busy_pointer_ms = 2000;

// Shows the busy pointer until the returned function is called (on the UI
// thread; again, it does nothing) or `ms` have passed. While several are
// showing, it shows until the last ends.
std::function<void()> show_busy_pointer(rap::ui&, int ms = busy_pointer_ms);

}  // namespace tortilla
