// Git LFS without git-lfs: git clones and checks out such a repository
// without a word, and its LFS files are left as small pointer files.
// Tortilla says so, once per repository.
#pragma once

#include <rapport/rapport.h>

#include <functional>
#include <string>

namespace tortilla
{

// Whether the repository's index holds files with the lfs filter (a
// filter=lfs attribute in one of its .gitattributes).
bool uses_lfs(const std::string& workdir);
// Whether `git lfs` runs; asked once per process.
bool lfs_installed();
// A repository using LFS where git-lfs isn't installed.
bool lfs_missing(const std::string& workdir);

// When lfs_missing(workdir), tells the user, once per repository per run.
// `done` runs when that message closes, or at once when there's none.
void warn_if_lfs_missing(rap::ui&, rap::window* parent, const std::string& workdir,
                         std::function<void()> done = {});

}  // namespace tortilla
