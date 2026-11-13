// What a log tab asks of the window it's in (log_window): a parent for its
// dialogs, its title shown, the window closed by its OK and Cancel, and
// View Patch, which is the window's.
#pragma once

#include <rapport/rapport.h>

namespace tortilla
{

class log_tab;

class tab_host
{
public:
    // The window, the parent of the tab's dialogs and messages.
    virtual rap::window* window() = 0;
    // The tab's title (log_tab::title_text) has changed.
    virtual void tab_changed(log_tab&) = 0;
    // OK (`ok`) or Cancel clicked: done with the window. Picking commits, OK
    // has handed them over first.
    virtual void finish(bool ok) = 0;
    // View Patch [spec 1.9.5]: a window of its own, following the selection
    // of the tab on show (log_tab::patch_request).
    virtual bool patch_shown() const = 0;
    virtual void show_patch(bool on) = 0;
    // The tab's selection has changed: View Patch follows it.
    virtual void selection_moved(log_tab&) = 0;

protected:
    ~tab_host() = default;
};

}  // namespace tortilla
