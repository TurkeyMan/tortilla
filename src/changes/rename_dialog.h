// Explorer's "Rename..." (TortoiseGit's RenameCommand and CRenameDlg)
// [spec 4.2, row 36]: a new name in the same folder, then git mv. A
// change of case alone (readme.md to README.md) is passed with -f, as git
// needs on a filesystem that doesn't tell them apart.
#pragma once

#include <rapport/rapport.h>

#include <functional>
#include <string>

namespace tortilla
{

// `done` gets whether it was renamed.
void rename_item(rap::ui&, const std::string& path, std::function<void(bool renamed)> done);

}  // namespace tortilla
