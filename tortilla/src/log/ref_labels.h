// TortoiseGit's ref labels, shared by the Log and Rebase lists.
#pragma once

#include "git/repo.h"

#include <rapport/rapport.h>

#include <functional>

namespace tortilla
{

// Where a label was drawn, for finding the one under the pointer.
struct LabelBox
{
    float left, right;
    Ref ref;
};

// Draws a commit's labels from `x` across a cell `height` tall and returns
// the x after the last one (`x` itself when nothing was drawn). `show`
// filters labels (View > Labels); empty shows them all. `boxes`, if given,
// gets each label's extent.
float paintRefLabels(rap::Painter&, const std::vector<Ref>& refs, float x, float height,
                     const std::function<bool(const Ref&)>& show = {},
                     std::vector<LabelBox>* boxes = nullptr);

}  // namespace tortilla
