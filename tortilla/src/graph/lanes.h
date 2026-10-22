// Commit graph lane assignment.
//
// Commits arrive newest first in topological order. Each lane is a column
// waiting for a particular commit; a row's node sits in the lane that was
// waiting for it. Every row records the line segments to draw in its top
// half (incoming, lane -> node) and bottom half (outgoing, node -> parent
// lanes), so a row can be painted on its own without looking at others.
#pragma once

#include "git/repo.h"

#include <optional>
#include <span>
#include <vector>

namespace tortilla
{

// TortoiseGit colours lines by lane column, so a segment's colour is the
// lane its vertical part runs in.
struct Segment
{
    uint16_t from, to;  // lane indices: top edge -> middle, or middle -> bottom edge
};

struct GraphRow
{
    uint16_t node = 0;
    uint16_t width = 0;  // lanes in use, for sizing the column
    bool merge = false;  // more than one parent
    bool fork = false;   // more than one child lane converges here
    bool tip = false;    // nothing above leads into this commit
    std::vector<Segment> top, bottom;
};

class LaneBuilder
{
public:
    GraphRow next(const Oid& id, std::span<const Oid> parents);

private:
    // The lowest free lane, skipping `exclude`.
    uint16_t freeLane(std::span<const uint16_t> exclude = {});

    std::vector<std::optional<Oid>> lanes_;  // commit each lane is waiting for
};

}  // namespace tortilla
