// Edit conflicts [spec 9.1]: one conflicted file in the merge tool, then
// whether it's resolved. A delete/modify conflict asks instead whether to
// keep the file.
#pragma once

#include "ui/settings_dialog.h"

#include <rapport/rapport.h>

#include <functional>
#include <string>

namespace tortilla
{

// `path` is repository-relative. `done(changed)` runs once nothing more
// will happen; `changed` when the index may differ (a list reloads).
// `open_settings` offers the Diff Viewer page when no merge tool is set up.
void edit_conflict(rap::ui&, rap::window* parent, const std::string& workdir,
                   const std::string& path, std::function<void(bool changed)> done,
                   std::function<void(settings_page)> open_settings);

}  // namespace tortilla
