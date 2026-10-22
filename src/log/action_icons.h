// Icons for the log's Actions column: one per kind of change (modified,
// added, deleted, renamed, conflicted), in several styles chosen under
// View > Action Icons.
//
// They're vector-drawn at the row's size, so everything that can be is
// snapped to device pixels: boxes and bars land on whole pixels and 1 px
// strokes on pixel centres (at any scale). Only diagonals stay
// anti-aliased.
#pragma once

#include "git/repo.h"

#include <rapport/rapport.h>

#include <algorithm>
#include <cmath>

namespace tortilla
{

enum class action_icon_style
{
    glyph,     // bare coloured symbols
    document,  // page with a folded corner and a coloured mark
    bevel,     // raised tile with a letter, like the ref labels
    line,      // thin outlined symbols
};

namespace detail
{

using rap::color;
using rap::paint;
using rap::point_f;

using grid = rap::pixel_grid;

inline color mix(color c, color to, float t)
{
    auto f = [&](uint8_t a, uint8_t b) { return uint8_t(a + (b - a) * t); };
    return {f(c.r, to.r), f(c.g, to.g), f(c.b, to.b), c.a};
}

inline color action_colour(action_bits a)
{
    switch (a)
    {
    case action_modified: return {0xd8, 0x8c, 0x00};
    case action_added: return {0x26, 0xa2, 0x69};
    case action_deleted: return {0xd0, 0x1b, 0x24};
    case action_renamed: return {0x1c, 0x71, 0xd8};
    case action_conflicted: return {0xe6, 0x61, 0x00};
    }
    return {0x80, 0x80, 0x80};
}

inline const char* action_letter(action_bits a)
{
    switch (a)
    {
    case action_modified: return "M";
    case action_added: return "A";
    case action_deleted: return "D";
    case action_renamed: return "R";
    case action_conflicted: return "C";
    }
    return "?";
}

// The filled symbol for an action in the s x s box at (ox, oy), which is
// already on the pixel grid. `t` is the stroke thickness.
inline void symbol(rap::painter& p, const grid& g, action_bits a, float ox, float oy, float s, color c,
                   float t)
{
    t = g.size(t);
    // Bars are pixel-aligned rectangles, centred in the box.
    auto hbar = [&](float x0, float x1)
    {
        float y = g.snap(oy + (s - t) / 2);
        p.rect({g.snap(x0), y, g.snap(x1) - g.snap(x0), t}, 0, paint{c}, std::nullopt);
    };
    auto vbar = [&](float y0, float y1)
    {
        float x = g.snap(ox + (s - t) / 2);
        p.rect({x, g.snap(y0), t, g.snap(y1) - g.snap(y0)}, 0, paint{c}, std::nullopt);
    };

    switch (a)
    {
    case action_modified:
    {
        const float k = 0.7071f;
        point_f tip{ox + s * 0.08f, oy + s * 0.92f};
        auto at = [&](float along, float side)
        { return point_f{tip.x + k * along + k * side, tip.y - k * along + k * side}; };
        float len = s * 1.12f, w = s * 0.18f, point = s * 0.3f;
        p.polygon({at(point, -w), at(len, -w), at(len, w), at(point, w)}, paint{c}, std::nullopt);
        p.polygon({at(0, 0), at(point, -w), at(point, w)}, paint{mix(c, {0, 0, 0}, 0.55f)},
                  std::nullopt);
        break;
    }
    case action_added:
        hbar(ox, ox + s);
        vbar(oy, oy + s);
        break;
    case action_deleted:
    {
        // A cross: two diagonal bars.
        const float k = 0.7071f, half = s * 0.5f, w = t / 2;
        point_f c0{ox + s / 2, oy + s / 2};
        for (float sx : {1.f, -1.f})
        {
            point_f d{k * sx, k}, n{-k, k * sx};
            auto at = [&](float along, float side)
            { return point_f{c0.x + d.x * along + n.x * side, c0.y + d.y * along + n.y * side}; };
            p.polygon({at(-half, -w), at(half, -w), at(half, w), at(-half, w)}, paint{c},
                      std::nullopt);
        }
        break;
    }
    case action_renamed:
    {
        hbar(ox, ox + s * 0.55f);
        float hx = g.snap(ox + s * 0.45f);
        p.polygon({{hx, g.snap(oy)}, {g.snap(ox + s), oy + s / 2}, {hx, g.snap(oy + s)}}, paint{c},
                  std::nullopt);
        break;
    }
    case action_conflicted:
    {
        p.polygon({{ox + s / 2, g.snap(oy)},
                   {g.snap(ox + s), g.snap(oy + s)},
                   {g.snap(ox), g.snap(oy + s)}},
                  paint{c}, std::nullopt);
        color white{255, 255, 255};
        float bw = g.size(s * 0.14f), bx = g.snap(ox + (s - bw) / 2);
        p.rect({bx, g.snap(oy + s * 0.32f), bw, g.size(s * 0.34f)}, 0, paint{white}, std::nullopt);
        p.rect({bx, g.snap(oy + s * 0.74f), bw, bw}, 0, paint{white}, std::nullopt);
        break;
    }
    }
}

}  // namespace detail

// Draws the icon for `action` in the s x s box at (ox, oy).
inline void paint_action_icon(rap::painter& p, action_bits action, float ox, float oy, float s,
                            action_icon_style style)
{
    using namespace detail;
    grid g(p);
    ox = g.snap(ox);
    oy = g.snap(oy);
    s = g.snap(s);
    color c = action_colour(action);

    switch (style)
    {
    case action_icon_style::glyph: symbol(p, g, action, ox, oy, s, c, s * 0.22f); break;

    case action_icon_style::document:
    {
        // Page on the left, mark overlapping its bottom right.
        float w = g.snap(s * 0.6f), h = g.snap(s * 0.85f), f = g.snap(s * 0.22f);
        float lw = g.stroke_width(1);
        float x0 = g.stroke(ox, 1), y0 = g.stroke(oy, 1);
        float x1 = g.stroke(ox + w, 1), y1 = g.stroke(oy + h, 1);
        color edge{0x6e, 0x6e, 0x6e};
        p.polygon({{x0, y0}, {x1 - f, y0}, {x1, y0 + f}, {x1, y1}, {x0, y1}},
                  paint{color{0xff, 0xff, 0xff}}, paint{edge});
        p.polygon({{x1 - f, y0}, {x1 - f, y0 + f}, {x1, y0 + f}}, paint{color{0xd8, 0xd8, 0xd8}},
                  paint{edge});
        for (int i = 0; i < 3; ++i)
        {
            float y = g.stroke(oy + s * (0.4f + i * 0.15f), 1);
            p.line({g.snap(ox + s * 0.12f), y}, {g.snap(ox + w - s * 0.1f), y},
                   paint{color{0xa8, 0xa8, 0xa8}}, lw);
        }
        float m = g.snap(s * 0.74f);  // the mark is what reads at row size
        symbol(p, g, action, ox + s - m, oy + s - m, m, c, m * 0.28f);
        break;
    }

    case action_icon_style::bevel:
    {
        float t = g.snap(s * 0.92f), x = g.snap(ox + (s - t) / 2), y = g.snap(oy + (s - t) / 2);
        p.rect({x, y, t, t}, 0, paint{c}, std::nullopt);
        color light = mix(c, {255, 255, 255}, 0.45f), dark = mix(c, {0, 0, 0}, 0.45f);
        float lw = g.stroke_width(1), l = g.stroke(x, 1), tp = g.stroke(y, 1);
        float r = g.stroke(x + t - lw, 1), b = g.stroke(y + t - lw, 1);
        p.line({l, y + t}, {l, y}, paint{light}, lw);
        p.line({x, tp}, {x + t, tp}, paint{light}, lw);
        p.line({r, y}, {r, y + t}, paint{dark}, lw);
        p.line({x + t, b}, {x, b}, paint{dark}, lw);
        rap::text_style ts;
        ts.bold = true;
        ts.fg = color{255, 255, 255};
        rap::size_f sz = p.measure(action_letter(action), ts);
        p.text({g.snap(x + (t - sz.w) / 2), g.snap(y + (t - sz.h) / 2)}, action_letter(action), ts);
        break;
    }

    case action_icon_style::line:
    {
        color muted = mix(c, {0x60, 0x60, 0x60}, 0.25f);
        float lw = g.stroke_width(std::max(1.f, s * 0.11f));
        float cx = g.stroke(ox + s / 2, lw), cy = g.stroke(oy + s / 2, lw);
        float r = g.snap(s * 0.42f);
        switch (action)
        {
        case action_modified:
            p.circle({ox + s / 2, oy + s / 2}, r, std::nullopt, paint{muted});
            p.circle({ox + s / 2, oy + s / 2}, r * 0.35f, paint{muted}, std::nullopt);
            break;
        case action_added:
            p.line({cx - r, cy}, {cx + r, cy}, paint{muted}, lw);
            p.line({cx, cy - r}, {cx, cy + r}, paint{muted}, lw);
            break;
        case action_deleted: p.line({cx - r, cy}, {cx + r, cy}, paint{muted}, lw); break;
        case action_renamed:
            p.line({cx - r, cy}, {cx + r, cy}, paint{muted}, lw);
            p.line({cx + r * 0.3f, cy - r * 0.7f}, {cx + r, cy}, paint{muted}, lw);
            p.line({cx + r * 0.3f, cy + r * 0.7f}, {cx + r, cy}, paint{muted}, lw);
            break;
        case action_conflicted:
            p.polygon({{cx, cy - r}, {cx + r, cy + r * 0.9f}, {cx - r, cy + r * 0.9f}},
                      std::nullopt, paint{muted});
            p.line({cx, cy - r * 0.3f}, {cx, cy + r * 0.3f}, paint{muted}, lw);
            break;
        }
        break;
    }
    }
}

}  // namespace tortilla
