// TortoiseGit's Reset dialog (CResetDlg): soft, mixed or hard, then git
// reset in the progress dialog. Shared by the Log and the RefLog.
#pragma once

#include <rapport/rapport.h>

#include <functional>
#include <string>

namespace tortilla
{

// Resets `branch` (what HEAD names, for the text) to `hash`, described by
// `summary`. `done` gets whether the reset succeeded (not called if
// cancelled).
void reset_to(rap::ui&, rap::window* parent, const std::string& workdir, const std::string& branch,
              const std::string& hash, const std::string& summary,
              std::function<void(bool ok)> done);

}  // namespace tortilla
