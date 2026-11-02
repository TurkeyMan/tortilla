// The log's history editing dialogs: a commit message to edit (Combine to
// one commit, Reword, Squash into the commit below), and where to move
// commits to (Move commits to branch).
#pragma once

#include <rapport/rapport.h>

#include <functional>
#include <string>
#include <vector>

namespace tortilla
{

// `text` to edit under `intro`; `warning` (pushed commits) shown under it
// when there is one. `done` gets the text on OK.
void history_message_dialog(rap::ui&, rap::window* parent, const std::string& title,
                            const std::string& intro, const std::string& text,
                            const std::string& warning, std::function<void(std::string)> done);

// Move commits to branch: one of `branches` (short names), or a new branch
// named in the box.
struct move_target
{
    std::string branch;  // short name
    bool create = false;
};
void move_commits_dialog(rap::ui&, rap::window* parent, const std::string& intro,
                         std::vector<std::string> branches, const std::string& warning,
                         std::function<void(move_target)> done);

}  // namespace tortilla
