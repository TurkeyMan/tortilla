// Comparing revisions, with the tool the Settings dialog's Diff Viewer page
// chose: tortilla's unified diff window, git difftool with diff.tool, or a
// command of the user's.
#pragma once

#include "git/command.h"

#include <rapport/rapport.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tortilla
{

// `revisions` are as for git diff (one means against the working tree);
// `paths` are relative to the repository at `dir`. With no tool set up it
// offers the Diff Viewer page and calls `open_settings` if the user agrees.
// `done` runs once nothing more will happen: the diff window closed, the tool
// exited, or a message was dismissed. Keep the returned process (null for
// the diff window) while the tool runs; dropping it kills the tool.
std::shared_ptr<git_process>
compare_revisions(rap::ui&, rap::window* parent, const std::string& dir,
                  std::vector<std::string> revisions, std::vector<std::string> paths,
                  std::function<void()> open_settings, std::function<void()> done = {});

}  // namespace tortilla
