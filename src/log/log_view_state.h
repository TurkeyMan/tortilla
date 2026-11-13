// What a log page shows that its tab's state doesn't say: the selection,
// the panes, the columns and the focus, carried from the page that goes to
// the one built after it (log_tab::capture_view, log_tab::bind_page). The
// rest (the filter box's text, the jump combo, the forge column) is the
// tab's own state, which a new page is drawn from.
#pragma once

#include "changes/changes_view.h"
#include "git/repo.h"

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
