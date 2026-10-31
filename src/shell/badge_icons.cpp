#include "badge_icons.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace tortilla
{
namespace
{

using rap::color;
using rap::point_f;

constexpr color k_clean{0x26, 0xa2, 0x69}, k_modified{0xd0, 0x1b, 0x24},
    k_conflicted{0xf5, 0xa6, 0x0f}, k_added{0x1c, 0x71, 0xd8}, k_white{0xff, 0xff, 0xff},
    k_edge{0x24, 0x1f, 0x31, 0xc0}, k_ink{0x24, 0x1f, 0x31};

constexpr std::array<std::string_view, 4> k_kinds{"conflicted", "modified", "added", "clean"};

// A small "!" on whole pixels: a two-pixel bar from `top` to `bottom`, a
// pixel's gap, and a two-pixel dot.
void exclaim(rap::painter& p, float mid, float top, float bottom, color c)
{
    p.rect({mid - 1, top, 2, bottom - top}, 0, rap::paint{c}, std::nullopt);
    p.rect({mid - 1, bottom + 1, 2, 2}, 0, rap::paint{c}, std::nullopt);
}

}  // namespace

std::span<const std::string_view> badge_kinds()
{
    return k_kinds;
}

float badge_size(float icon)
{
    // Even, so the marks sit on whole pixels either side of the middle.
    const float b = icon <= 48 ? std::max(10.f, icon * 0.5f) : icon * 0.4f;
    return std::min(icon, 2 * std::round(b / 2));
}

void paint_badge(rap::painter& p, std::string_view kind, float x, float y, float size)
{
    // On a 16-unit square: a disc (or for conflicts a triangle) with a dark
    // edge, so it reads over any icon, and its mark in it. Big enough, a
    // white rim inside the edge; small, marks two pixels wide and square-
    // ended, on whole pixels.
    const bool small = size < 15;
    const float u = size / 16;
    auto at = [&](float px, float py) { return point_f{x + px * u, y + py * u}; };
    const float stroke = small ? 2.f : std::max(2.f, 1.6f * u);
    const auto cap = small ? rap::line_cap::butt : rap::line_cap::round;
    const float mid = x + size / 2;  // a whole pixel boundary: size is even

    if (kind == "conflicted")
    {
        p.polygon({at(8, 0.2f), at(16, 15.6f), at(0, 15.6f)}, rap::paint{k_edge}, std::nullopt, 0);
        if (!small)
            p.polygon({at(8, 1.5f), at(14.9f, 14.6f), at(1.1f, 14.6f)}, rap::paint{k_white},
                      std::nullopt, 0);
        p.polygon(small ? std::vector<point_f>{at(8, 2.2f), at(14.4f, 14.6f), at(1.6f, 14.6f)}
                        : std::vector<point_f>{at(8, 2.8f), at(13.9f, 13.9f), at(2.1f, 13.9f)},
                  rap::paint{k_conflicted}, std::nullopt, 0);
        if (small)
            exclaim(p, mid, y + std::round(size * 0.36f), y + std::round(size * 0.66f), k_ink);
        else
        {
            p.line({mid, y + 5.6f * u}, {mid, y + 9.6f * u}, k_ink, stroke, cap);
            p.circle(at(8, 12.1f), stroke * 0.65f, rap::paint{k_ink}, std::nullopt);
        }
        return;
    }

    const color fill = kind == "clean" ? k_clean : kind == "modified" ? k_modified : k_added;
    const point_f c{mid, y + size / 2};
    p.circle(c, size / 2, rap::paint{k_edge}, std::nullopt);
    if (small)
        p.circle(c, size / 2 - 1, rap::paint{fill}, std::nullopt);
    else
    {
        p.circle(c, 7.0f * u, rap::paint{k_white}, std::nullopt);
        p.circle(c, 5.9f * u, rap::paint{fill}, std::nullopt);
    }
    if (kind == "clean")
    {
        p.line(at(4.8f, 8.3f), at(7.1f, 10.6f), k_white, stroke, rap::line_cap::round);
        p.line(at(7.1f, 10.6f), at(11.3f, 5.6f), k_white, stroke, rap::line_cap::round);
    }
    else if (kind == "modified")
    {
        if (small)
            exclaim(p, mid, y + std::round(size * 0.2f), y + std::round(size * 0.6f), k_white);
        else
        {
            p.line({mid, y + 4.2f * u}, {mid, y + 8.8f * u}, k_white, stroke, cap);
            p.circle(at(8, 11.2f), stroke * 0.6f, rap::paint{k_white}, std::nullopt);
        }
    }
    else  // added
    {
        const float arm = small ? std::round(size * 0.3f) : 3.2f * u;
        p.line({mid, c.y - arm}, {mid, c.y + arm}, k_white, stroke, cap);
        p.line({mid - arm, c.y}, {mid + arm, c.y}, k_white, stroke, cap);
    }
}

}  // namespace tortilla
