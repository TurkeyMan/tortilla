// TortoiseGit's ref labels, shared by the Log and Rebase lists.
#pragma once

#include "forge/forge.h"
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

// A branch that's a pull request's head wears its number and its checks:
// a tick, a cross, or a ring while they run.
struct pr_mark
{
    int number = 0;
    check_state checks = check_state::none;
    bool draft = false;
    bool blocked = false;    // checks passed, but the forge's rules stop a merge yet
    bool conflicts = false;  // it can't be merged at all
};

// The checks' mark, in a circle `diameter` across at `centre`, ringed in
// white to stand out on any label: a tick, a cross, or a ring while they run.
// A tick is green when it can be merged, amber when the forge's rules stop
// it yet (a review), and grey when it conflicts.
enum class mergeable
{
    yes,
    not_yet,
    conflicts,
};
void paint_check_mark(rap::painter&, rap::point_f centre, float diameter, check_state,
                      mergeable = mergeable::yes);

// Draws a commit's labels from `x` across a cell `height` tall and returns
// the x after the last one (`x` itself when nothing was drawn). `show`
// filters labels (View > Labels); empty shows them all. `boxes`, if given,
// gets each label's extent.
float paint_ref_labels(rap::painter&, const std::vector<ref>& refs, float x, float height,
                       const std::function<bool(const ref&)>& show = {},
                       std::vector<label_box>* boxes = nullptr,
                       const std::function<std::optional<pr_mark>(const ref&)>& pr = {});

}  // namespace tortilla
