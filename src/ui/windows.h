// The program's top-level windows: the one a command opened, and those a
// progress dialog's follow-ups open (the log after a pull, the rebase after
// a fetch). Each is kept until it closes; the program ends when the last
// one does.
#pragma once

#include <rapport/rapport.h>

#include <cstddef>
#include <functional>
#include <memory>

namespace tortilla::windows
{

// Before the first keep(): the ui to quit when none are left.
void init(rap::ui&);

// Keeps `owner` alive until release(id).
size_t hold(std::shared_ptr<void> owner);
// Lets go of a window (after the callback that closed it returns); the
// program ends if it was the last.
void release(size_t id);

// Keeps and shows a window (log_window, commit_window...: anything with
// show() and on_closed()); `closed` runs too when it closes.
template <class W>
W* keep(std::unique_ptr<W> window, std::function<void()> closed = {})
{
    W* raw = window.get();
    const size_t id = hold(std::shared_ptr<W>(std::move(window)));
    raw->on_closed(
        [id, closed](auto&&...)
        {
            if (closed) closed();
            release(id);
        });
    raw->show();
    return raw;
}

}  // namespace tortilla::windows
