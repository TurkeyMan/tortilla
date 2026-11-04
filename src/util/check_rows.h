// A click on a row's checkbox, or Space on the cursor row, in a list with a
// multi-selection: which rows it sets, and to what. Inside the selection
// the whole selection takes the row's new state; outside it, the row alone.
// The one rule for Browse References and the Commit dialog's file list.
#pragma once

#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>

namespace tortilla
{

struct check_change
{
    std::vector<size_t> rows;
    bool on = false;
};

// `selection`: the list's selected rows; `row_checked`: the clicked row's
// state now.
inline check_change box_clicked(std::vector<size_t> selection, size_t row, bool row_checked)
{
    if (!std::ranges::contains(selection, row)) selection = {row};
    return {std::move(selection), !row_checked};
}

}  // namespace tortilla
