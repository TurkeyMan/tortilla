// Explorer's "Add to ignore list" and "Delete and add to ignore list"
// [spec 4.4, TortoiseGit's CIgnoreDlg]: the items by name or by extension,
// either only where they are or in every folder, written to the
// repository's .gitignore, each item's folder's, or .git/info/exclude (this
// clone only). "Delete and" also stops tracking them, leaving them on disk.
#pragma once

#include <rapport/rapport.h>

#include <functional>
#include <string>
#include <vector>

namespace tortilla
{

// `paths` are files or folders in one repository; `done` says whether the
// patterns were written.
void show_ignore_dialog(rap::ui&, std::vector<std::string> paths, bool by_extension, bool untrack,
                        std::function<void(bool done)> done);

}  // namespace tortilla
