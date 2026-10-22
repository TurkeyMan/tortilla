// Working-tree status from the git CLI. libgit2's status/diff of the working
// tree was several times slower than git's own on big or slow trees (git
// keeps stat caches in the index), so the log's "Working tree changes" row
// asks git:
//   git status --porcelain -z --untracked-files=normal   (what changed)
//   git diff HEAD --numstat -z                           (line counts)
#pragma once

#include "git/repo.h"

#include <string>
#include <vector>

namespace tortilla
{

inline const std::vector<std::string> kStatusArgs{"status", "--porcelain", "-z",
                                                  "--untracked-files=normal"};
inline const std::vector<std::string> kNumstatArgs{"diff", "HEAD", "--numstat", "-z"};

// Combines the two outputs into one FileChange per path. Status letters:
// A D M R C T, '?' untracked (directories end in '/'), 'U' conflicted.
std::vector<FileChange> parseStatus(const std::string& porcelain, const std::string& numstat);

}  // namespace tortilla
