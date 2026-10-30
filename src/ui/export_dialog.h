// TortoiseGit's Export dialog (CExportDlg), from the log's "Export this
// version..." and Explorer's Export: a revision's tracked files, by git
// archive, to a .zip or into a folder, in the progress dialog, which then
// offers to open the folder.
#pragma once

#include <rapport/rapport.h>

#include <functional>
#include <string>

namespace tortilla
{

// `workdir` is the repository's top (a bare one's folder); `rev` is
// preselected ("HEAD" if empty). `done` runs once it's all over: cancelled,
// failed, or written.
void export_revision(rap::ui&, rap::window* parent, const std::string& workdir,
                     const std::string& rev, std::function<void()> done = {});

}  // namespace tortilla
