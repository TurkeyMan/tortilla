// TortoiseGit's git-svn commands: SVN DCommit (CSVNDCommitDlg when
// svn.rmdir isn't set, then git svn dcommit), SVN Rebase (git svn fetch,
// then a fast-forward or the Rebase dialog onto the SVN branch), SVN Fetch
// (git svn fetch) and Import SVN Ignore (CSVNIgnoreTypeDlg, then git svn
// show-ignore or create-ignore). DCommit and Rebase offer to stash local
// changes first, and to pop them after. Each git svn runs in the progress
// dialog.
#pragma once

#include <rapport/rapport.h>

#include <functional>
#include <string>

namespace tortilla
{

// The window the dialogs belong to, asked each time one opens: a log closed
// meanwhile says nullptr, and they stand alone.
using parent_window = std::function<rap::window*()>;

// `workdir` is a git-svn repository's working tree. `done` gets whether it
// all succeeded, also when it was cancelled or git svn isn't installed (false).
void svn_dcommit(rap::ui&, const parent_window& parent, const std::string& workdir,
                 std::function<void(bool ok)> done);
void svn_rebase(rap::ui&, const parent_window& parent, const std::string& workdir,
                std::function<void(bool ok)> done);
void svn_fetch(rap::ui&, const parent_window& parent, const std::string& workdir,
               std::function<void(bool ok)> done);
void svn_import_ignore(rap::ui&, const parent_window& parent, const std::string& workdir,
                       std::function<void(bool ok)> done);

}  // namespace tortilla
