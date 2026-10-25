// TortoiseGit's Clean up (CCleanTypeDlg): untracked files under the chosen
// folders removed with git clean. The ones git doesn't ignore, the ignored
// ones, or both; untracked folders too or not; to the Recycle Bin (Trash)
// or for good; submodules too; or a dry run that only lists them.
#pragma once

#include "git/clean.h"

#include <rapport/rapport.h>

#include <functional>
#include <string>
#include <vector>

namespace tortilla
{

// Asks how, then cleans `paths` (files or folders in one repository; the
// repository's top when empty is passed as its folder). `done` says whether
// anything ran (not when cancelled).
void clean_up(rap::ui&, rap::window* parent, std::vector<std::string> paths,
              std::function<void(bool done)> done);

}  // namespace tortilla
