// Explorer's Delete and Delete (keep local) [spec 4.4, TortoiseGit's remove
// command]. Delete sends the items to the Recycle Bin (Trash) and stages
// their removal, where TortoiseGit's git rm loses them for good; keep local
// stops tracking them and leaves them on disk (git rm --cached). Both ask
// first.
#pragma once

#include <rapport/rapport.h>

#include <functional>
#include <string>
#include <vector>

namespace tortilla
{

// `paths` are files or folders in one repository; `done` says whether they
// were removed (not when cancelled or failed, which says why itself).
void remove_files(rap::ui&, const std::vector<std::string>& paths, bool keep_local,
                  std::function<void(bool done)> done);

}  // namespace tortilla
