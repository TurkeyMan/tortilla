// The Show menu's Pins and hidden dialog: the branches always shown and the
// patterns of those never shown. (Pick... is Browse References, checking
// branches: branch/browse_refs.h.)
#pragma once

#include "git/repo.h"

#include <rapport/rapport.h>

#include <functional>
#include <string>
#include <vector>

namespace tortilla
{

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
