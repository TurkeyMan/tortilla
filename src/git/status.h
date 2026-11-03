// Working-tree status from the git CLI. libgit2's status/diff of the working
// tree was several times slower than git's own on big or slow trees (git
// keeps stat caches in the index), so the log's "Working tree changes" row
// asks git:
//   git status --porcelain -z --untracked-files=normal   (what changed)
//   git diff HEAD --numstat -z                           (line counts)
#pragma once

#include "git/command.h"
#include "git/repo.h"

#include <optional>
#include <string>
#include <vector>

namespace tortilla
{

inline const std::vector<std::string> k_status_args{"status", "--porcelain", "-z",
                                                  "--untracked-files=normal"};
inline const std::vector<std::string> k_numstat_args{"diff", "HEAD", "--numstat", "-z"};

// Combines the two outputs into one file_change per path. Status letters:
// A D M R C T, '?' untracked (directories end in '/'), 'U' conflicted.
std::vector<file_change> parse_status(const std::string& porcelain, const std::string& numstat);

// The status family's answers through libgit2, for when git isn't
// installed: `git status --porcelain -z [--untracked-files=no|normal|all]
// [--ignored=matching]`, `git diff HEAD --numstat -z` and `git ls-files -z`
// / `-v -z`, each with `-- <paths>` (":(top)" ones too), as git would
// print them. nullopt for any other command. Slower than git's on big trees
// (no fsmonitor or untracked cache): git's is used while it's there.
std::optional<git_result> status_in_process(const std::string& workdir,
                                            const std::vector<std::string>& args);
// git <args> while git is installed, else status_in_process's answer.
git_result run_status(const std::string& workdir, const std::vector<std::string>& args,
                      git_canceller* = nullptr);

}  // namespace tortilla
