// Unified diffs through libgit2, for when git isn't installed: the text
// `git diff HEAD`, `git diff <a> <b>` and `git show --format=fuller <c>`
// print (each with `-- <paths>`), near enough to read the same.
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace tortilla
{

// nullopt for commands it doesn't make, or revisions that don't resolve.
std::optional<std::string> patch_in_process(const std::string& dir,
                                            const std::vector<std::string>& args);

}  // namespace tortilla
