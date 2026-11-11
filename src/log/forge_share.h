// The forge panel's column keeps its share of the log window's width. The
// splitter it shares with the commit's text and files spans the window, so
// whatever the window gains or loses, so does the splitter: its width is
// followed through the window's, which every backend reports as laid out.
// (Win32's splitter reports the sizes last asked of it rather than those
// laid out, so their sum doesn't change as the window does.)
#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

namespace tortilla
{

struct forge_share
{
    float total = 0;   // the splitter's width when the column was last fitted
    float window = 0;  // the window's width then; 0: never fitted

    // Whether the window's width has changed since the last fit.
    bool moved(float window_now) const { return window <= 0 || std::abs(window_now - window) >= 1; }

    // The splitter's width now: the last fit's, moved as the window has
    // since; before any, `measured` (its panes' sizes summed, as laid out).
    float width_now(float window_now, float measured) const
    {
        return window > 0 ? total + (window_now - window) : measured;
    }

    // The column's width out of `width`: the share it was last dragged to
    // (`dragged`, the column's size last), else a third, 480 at most.
    static float side(float width, const std::vector<float>& dragged)
    {
        if (dragged.size() == 2 && dragged[0] > 0 && dragged[1] > 0)
            return width * dragged[1] / (dragged[0] + dragged[1]);
        return std::min(width / 3, 480.f);
    }

    void fitted(float width, float window_now)
    {
        total = width;
        window = window_now;
    }
};

}  // namespace tortilla
