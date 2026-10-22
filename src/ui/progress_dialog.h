// TortoiseGit's git progress dialog (CProgressDlg): runs git commands one
// after another, echoing each command in bold followed by its output, then
// "Success" or "Failed" with the time taken. Close is enabled when done;
// Abort kills the running command.
#pragma once

#include <rapport/rapport.h>

#include <functional>
#include <string>
#include <vector>

namespace tortilla
{

using git_command = std::vector<std::string>;

// Owns itself; `done` gets whether every command succeeded once the user
// closes the dialog.
void run_progress_dialog(rap::ui&, rap::window* parent, const std::string& title,
                       const std::string& dir, std::vector<git_command> commands,
                       std::function<void(bool ok)> done = {});

}  // namespace tortilla
