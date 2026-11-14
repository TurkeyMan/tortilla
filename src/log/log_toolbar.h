// The window's shared filter row. Values live with the tab; widgets live with the frame.
#pragma once
#include "log/log_page.h"
namespace tortilla
{
struct log_toolbar_state
{
    std::string branch = "HEAD", branch_tip;
    bool undo = false, redo = false;
    bool up = true, down = true;
    std::string undo_tip, redo_tip;
    fetch_badge badge = fetch_badge::none;
    size_t badge_count = 0;
    std::string fetch_tip;
};
struct log_toolbar
{
    rap::button* fetch = nullptr;
    rap::menu_button* branch = nullptr;
    rap::tool_button* undo = nullptr;
    rap::tool_button* redo = nullptr;
    rap::entry* search = nullptr;
    rap::entry_action* dates = nullptr;
    rap::drop_down* jump_type = nullptr;
    rap::button* up = nullptr;
    rap::button* down = nullptr;
};
}  // namespace tortilla
