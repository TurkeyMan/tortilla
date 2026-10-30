// TortoiseGit's submodule dialogs: Submodule Add (CSubmoduleAddDlg), a
// repository URL, the path it goes to and its options, then git submodule
// add; Submodule Update (CSubmoduleUpdateDlg), the submodules with a box
// each and the options, then git submodule update; and Submodule Sync,
// which has no dialog. Each runs in the progress dialog.
#pragma once

#include <rapport/rapport.h>

#include <functional>
#include <string>

namespace tortilla
{

// `folder` is the folder chosen in the working tree at `workdir`: the new
// submodule goes inside it. `done` gets whether git succeeded (not called
// if cancelled).
void add_submodule(rap::ui&, rap::window* parent, const std::string& workdir,
                   const std::string& folder, std::function<void(bool ok)> done);

// The submodules inside `folder` start checked (all of them if none is).
void update_submodules(rap::ui&, rap::window* parent, const std::string& workdir,
                       const std::string& folder, std::function<void(bool ok)> done);

void sync_submodules(rap::ui&, rap::window* parent, const std::string& workdir,
                     std::function<void(bool ok)> done);

}  // namespace tortilla
