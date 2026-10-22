// TortoiseGit's ref labels, shared by the Log and Rebase lists.
#pragma once

#include "git/repo.h"

#include <rapport/rapport.h>

#include <functional>

namespace tortilla
{

// Where a label was drawn, for finding the one under the pointer.
struct label_box
{
    float left, right;
    tortilla::ref ref;
};

// Draws a commit's labels from `x` across a cell `height` tall and returns
// the x after the last one (`x` itself when nothing was drawn). `show`
// filters labels (View > Labels); empty shows them all. `boxes`, if given,
// gets each label's extent.
float paint_ref_labels(rap::painter&, const std::vector<ref>& refs, float x, float height,
                     const std::function<bool(const ref&)>& show = {},
                     std::vector<label_box>* boxes = nullptr);

}  // namespace tortilla
