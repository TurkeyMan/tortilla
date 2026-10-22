// A unified diff viewer, standing in for TortoiseGitUDiff: shows the output
// of a git diff/show command with diff colouring.
#pragma once

#include <rapport/rapport.h>

#include <functional>
#include <string>
#include <vector>

namespace tortilla
{

// Owns itself; closes on Close or the window's close box, then calls
// `closed`.
void showUnifiedDiff(rap::Ui&, const std::string& title, const std::string& dir,
                     const std::vector<std::string>& gitArgs, std::function<void()> closed = {});

// Colour spans for unified diff text: headers bold, hunks in the accent
// colour, additions and removals in the theme's positive/negative colours.
std::vector<rap::TextView::Span> diffSpans(const std::string& text);

// Byte offsets of every occurrence of `needle` in `text` (ASCII case
// folding unless `matchCase`).
std::vector<size_t> findAll(const std::string& text, const std::string& needle, bool matchCase);

}  // namespace tortilla
