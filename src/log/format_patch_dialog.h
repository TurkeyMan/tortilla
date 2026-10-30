// TortoiseGit's Format Patch dialog (CFormatPatchDlg), for a selection in
// the log: the folder to write into and whether to sign off, then git
// format-patch in the progress dialog, which lists the files written.
#pragma once

#include "log/log_model.h"

#include <rapport/rapport.h>

#include <string>

namespace tortilla
{

// Writes `range` as patches; `what` describes it for the dialog.
void format_patch(rap::ui&, rap::window* parent, const std::string& workdir,
                  const patch_range& range, const std::string& what);

}  // namespace tortilla
