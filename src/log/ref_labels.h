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
    // Where its pull request's part starts (the hairline before "#12"; its
    // number and mark run to `right`); 0 when it has none.
    float pr_left = 0;
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
pr_mark mark_of(const pull_request&);

// What the mark means, for its tooltip: how long the checks took, which
// failed, which are still running and for how long, and why it can't be
// merged yet, if it can't. `now` in seconds since 1970.
std::string pr_tooltip(const pull_request&, std::int64_t now);
// Its parts: what the checks say ("Checks passed in 6 min"), and what stands
// in the way of a merge ("Needs an approving review", "Can be merged"; ""
// while only the checks do).
std::string checks_text(const pull_request&, std::int64_t now);
std::string merge_text(const pull_request&);
// "6 min 12 s", "45 s"; to the minute for what's still running.
std::string check_duration(std::int64_t seconds, bool to_the_minute = false);

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
// White or black, whichever reads on a status colour's fill.
rap::color ink_on(rap::color fill);

// The labels' colours, all in one place [COL:23-54]. A branch checked out
// in another worktree is like a remote one (it can't be changed from here),
// so it's pale too; each remote's default branch is a richer remote colour.
struct label_palette
{
    rap::color local{0, 195, 0};
    rap::color current{200, 0, 0};
    rap::color worktree{150, 215, 210};
    rap::color remote{255, 221, 170};
    rap::color remote_default{255, 196, 120};
    rap::color tag{255, 255, 0};
    rap::color stash{128, 128, 128};
    rap::color other{224, 224, 224};
};
inline constexpr label_palette label_colours{};
rap::color label_colour(const ref&);

// Draws a commit's labels from `x` across a cell `height` tall and returns
// the x after the last one (`x` itself when nothing was drawn). `show`
// filters labels (View > Labels); empty shows them all. `boxes`, if given,
// gets each label's extent. A label `pinned` says gets a small pin after
// its name.
float paint_ref_labels(rap::painter&, const std::vector<ref>& refs, float x, float height,
                       const std::function<bool(const ref&)>& show = {},
                       std::vector<label_box>* boxes = nullptr,
                       const std::function<std::optional<pr_mark>(const ref&)>& pr = {},
                       const std::function<bool(const ref&)>& pinned = {});

}  // namespace tortilla
