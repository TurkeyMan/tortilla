// Explorer's Review/apply single patch, which TortoiseGit hands to
// TortoiseGitMerge: the files a patch changes and whether each applies;
// Review shows one as it is against what the patch would make it, in the
// diff tool; then git apply, for every file or those selected, in the
// progress dialog.
#pragma once

#include <rapport/rapport.h>

#include <functional>
#include <string>

namespace tortilla
{

// `patch` is a path to a unified diff (absolute); its paths are taken from
// the top of the working tree at `workdir`. `done` gets whether it was
// applied.
void review_patch(rap::ui&, rap::window* parent, const std::string& workdir,
                  const std::string& patch, std::function<void(bool applied)> done);

}  // namespace tortilla
