// Explorer's Create Patch Serial (TortoiseGit's CFormatPatchDlg): a folder
// and which commits, then git format-patch in the progress dialog, a
// numbered .patch file per commit.
#pragma once

#include <rapport/rapport.h>

#include <functional>
#include <string>

namespace tortilla
{

// The patches go into `folder` by default (the one clicked). `done` gets
// whether git succeeded (false when cancelled).
void create_patch_serial(rap::ui&, rap::window* parent, const std::string& workdir,
                         const std::string& folder, std::function<void(bool ok)> done);

}  // namespace tortilla
