// The modal dialogs and messages open over a window, counted: a log
// window's tab isn't moved to another window while one is (the dialog is
// over the window it would leave, and its answer would land in the other).
// Counted are the modal dialogs create_window makes with a parent (until
// they're destroyed), the messages show_message shows and the file dialogs
// pick_files opens (until they're answered, or cancelled). Every message and
// file dialog in the app goes through those two.
#pragma once

#include <rapport/rapport.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tortilla
{

// `parent` counted as having one more open over it, for as long as the
// token is kept. (Null: nothing counted.)
std::shared_ptr<const void> count_modal(const rap::window* parent);
// How many are open over `w` now.
size_t modals_over(const rap::window* w);

// ui.message, counted over `parent` until it's answered.
void show_message(rap::ui&, rap::window* parent, const rap::message_spec&,
                  std::function<void(rap::dialog_role)> done = {});
// ui.choose_files (open, save or folder), counted over `parent` until it's
// answered or cancelled.
void pick_files(rap::ui&, rap::window* parent, const rap::file_dialog_spec&,
                std::function<void(std::vector<std::string>)> done);

}  // namespace tortilla
