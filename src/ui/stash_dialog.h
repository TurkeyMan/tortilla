// TortoiseGit's Stash changes dialog (CStashSaveDlg): an optional message
// and whether to include untracked files, then git stash push in the
// progress dialog. Shared by the Log and the working-tree windows.
#pragma once

#include <rapport/rapport.h>

#include <functional>
#include <string>

namespace tortilla
{

// `done` gets whether the stash succeeded (not called if cancelled).
void stash_changes(rap::ui&, rap::window* parent, const std::string& workdir,
                   std::function<void(bool ok)> done);

}  // namespace tortilla
