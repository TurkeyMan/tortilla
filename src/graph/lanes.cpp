#include "lanes.h"

#include <algorithm>

namespace tortilla
{

uint16_t lane_builder::free_lane(std::span<const uint16_t> exclude)
{
    for (size_t k = 0; k < lanes_.size(); ++k)
        if (!lanes_[k] && !std::ranges::contains(exclude, uint16_t(k))) return uint16_t(k);
    lanes_.emplace_back();
    return uint16_t(lanes_.size() - 1);
}

graph_row lane_builder::next(const oid& id, std::span<const oid> parents)
{
    graph_row row;
    row.merge = parents.size() > 1;

    // Lanes waiting for this commit converge into its node.
    std::vector<uint16_t> incoming;
    for (size_t k = 0; k < lanes_.size(); ++k)
        if (lanes_[k] == id) incoming.push_back(uint16_t(k));

    if (incoming.empty())
    {
        row.node = free_lane();
        row.tip = true;
    }
    else
    {
        row.node = incoming.front();
    }
    row.fork = incoming.size() > 1;

    for (size_t k = 0; k < lanes_.size(); ++k)
    {
        if (!lanes_[k]) continue;
        auto lane = uint16_t(k);
        if (lanes_[k] == id)
            row.top.push_back({lane, row.node});
        else
        {
            // Lanes waiting for something else pass straight through.
            row.top.push_back({lane, lane});
            row.bottom.push_back({lane, lane});
        }
    }
    for (uint16_t k : incoming) lanes_[k].reset();

    for (size_t i = 0; i < parents.size(); ++i)
    {
        const oid& p = parents[i];
        // The first parent always continues the node's lane, even when
        // another lane already waits for it: both converge there later (a
        // fork point), which keeps the first-parent line straight.
        if (i == 0 && !lanes_[row.node])
        {
            lanes_[row.node] = p;
            row.bottom.push_back({row.node, row.node});
            continue;
        }
        auto existing = std::ranges::find(lanes_, std::optional<oid>(p));
        if (existing != lanes_.end())
        {
            // Another lane already leads to this parent: join it.
            auto j = uint16_t(existing - lanes_.begin());
            row.bottom.push_back({row.node, j});
            continue;
        }
        // Other parents open new lanes, but not in a column that closed in
        // this very row: back-to-back branches (one ends where the next
        // starts, as in a run of merged pull requests) then get different
        // columns and colours, as in TortoiseGit/qgit, instead of looking
        // like one long branch.
        std::vector<uint16_t> closed;
        for (uint16_t k : incoming)
            if (k != row.node) closed.push_back(k);
        uint16_t lane = free_lane(closed);
        lanes_[lane] = p;
        row.bottom.push_back({row.node, lane});
    }

    while (!lanes_.empty() && !lanes_.back()) lanes_.pop_back();

    uint16_t width = row.node + 1;
    for (auto* segs : {&row.top, &row.bottom})
        for (auto& s : *segs) width = std::max<uint16_t>(width, std::max(s.from, s.to) + 1);
    row.width = width;
    return row;
}

}  // namespace tortilla
