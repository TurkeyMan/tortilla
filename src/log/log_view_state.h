// The log view carried when a page is rebuilt or a tab moves to another
// window: selection, panes, columns, focus and the filter row's input state.
// Derived row values (branch, journal and fetch status) come from the tab's
// repository and ongoing work when its new page is bound.
#pragma once

#include "changes/changes_view.h"
#include "git/repo.h"
#include "log/log_model.h"

#include <optional>
#include <string>
#include <vector>

namespace tortilla
{

// The commit list's selection, by commit (rows move): put back, and the
// current row left where it was on screen, or centred when it can't be.
struct kept_selection
{
    std::vector<oid> commits;
    bool working_tree = false;
    std::optional<oid> current;   // nullopt with working_tree: that row
    std::optional<float> offset;  // the current row's place in the view, if shown
};

struct log_view_state
{
    std::optional<kept_selection> selection;
    // Explicitly carried across a tear-off; pending text is flushed before capture.
    std::string filter_text;
    log_filter filter;
    std::optional<int64_t> since, until;
    int jump_type = 0;
    // The list over the rest, and the commit's text over its files; empty:
    // as built.
    std::vector<float> panes, details_panes;
    std::vector<float> column_widths;
    std::vector<size_t> column_order;
    std::vector<bool> column_visible;
    changes_view::column_state files;
    // The selected files, by path: selected again once the files are back.
    std::vector<std::string> selected_files;
    enum class focus
    {
        none,
        commits,
        search,
        path_filter,
        files,
    };
    focus focused = focus::none;
};

}  // namespace tortilla
