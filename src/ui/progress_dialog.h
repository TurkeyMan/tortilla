// TortoiseGit's git progress dialog (CProgressDlg): runs git commands one
// after another, echoing each command in bold followed by its output, then
// "Success" or "Failed" with the time taken. Close is enabled when done;
// Abort kills the running command. After success a menu button offers the
// follow-ups, if any (TortoiseGit's post-command menu: "Commit..." after Add).
#pragma once

#include <rapport/rapport.h>

#include <functional>
#include <string>
#include <vector>

namespace tortilla
{

using git_command = std::vector<std::string>;

// A follow-up: more git commands, made when it's chosen and run in the
// dialog, or something to do as it closes (open the Commit window).
struct progress_follow_up
{
    std::string label;
    std::function<std::vector<git_command>()> commands;
    std::function<void()> after_close;
    // When it's offered, from the outcome; unset: after success.
    std::function<bool(bool ok)> offered;
};

// Owns itself; `done` gets whether every command succeeded once the user
// closes the dialog (after a chosen follow-up's after_close, so a window it
// opens is there before the caller finishes).
void run_progress_dialog(rap::ui&, rap::window* parent, const std::string& title,
                         const std::string& dir, std::vector<git_command> commands,
                         std::function<void(bool ok)> done = {},
                         std::vector<progress_follow_up> follow_ups = {});

}  // namespace tortilla
