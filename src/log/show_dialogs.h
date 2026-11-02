// The Show menu's dialogs: Pick... (a checklist of branches, shown for
// once), and Pins and hidden (the branches always shown and the patterns
// of those never shown).
#pragma once

#include "git/repo.h"

#include <rapport/rapport.h>

#include <functional>
#include <string>
#include <vector>

namespace tortilla
{

// Every branch, local then remote, by full name; `checked` start ticked.
// `chosen` gets the ones ticked on OK.
void pick_branches_dialog(rap::ui&, rap::window* parent, const ref_map&,
                          const std::vector<std::string>& checked,
                          std::function<void(std::vector<std::string>)> chosen);

struct pins_and_hidden
{
    std::vector<std::string> pins;    // full names; "HEAD" for what's checked out
    std::vector<std::string> hidden;  // glob patterns
};

// The two lists, each with Add and Remove; Defaults puts `defaults` back
// as the pins. `done` gets them on OK.
void pins_hidden_dialog(rap::ui&, rap::window* parent, const ref_map&, pins_and_hidden current,
                        std::vector<std::string> defaults,
                        std::function<void(pins_and_hidden)> done);

}  // namespace tortilla
