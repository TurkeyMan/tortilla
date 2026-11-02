// Explorer's Create Patch Serial (TortoiseGit's CFormatPatchDlg): a folder
// and which commits, then git format-patch in the progress dialog, a
// numbered .patch file per commit. And Apply Patch Serial
// (CImportPatchDlg): patch files in order and git am's options, then git am
// in the progress dialog, a commit per patch; where it stops, Resolved,
// Skip or Abort.
#pragma once

#include <rapport/rapport.h>

#include <functional>
#include <string>
#include <vector>

namespace tortilla
{

// The patches go into `folder` by default (the one clicked). `done` gets
// whether git succeeded (false when cancelled).
void create_patch_serial(rap::ui&, rap::window* parent, const std::string& workdir,
                         const std::string& folder, std::function<void(bool ok)> done);

// `patches` start the list (the files selected); more are added from the
// dialog. When a git am has stopped part way, offers to go on with it
// instead.
void apply_patch_serial(rap::ui&, rap::window* parent, const std::string& workdir,
                        std::vector<std::string> patches, std::function<void(bool ok)> done);

}  // namespace tortilla
