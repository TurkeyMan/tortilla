#include "menu_icons.h"

#include <array>
#include <cmath>
#include <numbers>
#include <optional>
#include <vector>

namespace tortilla
{
namespace
{

using rap::color;
using rap::point_f;

constexpr color k_blue{0x1c, 0x71, 0xd8}, k_green{0x26, 0xa2, 0x69}, k_red{0xd0, 0x1b, 0x24},
    k_orange{0xe6, 0x61, 0x00}, k_purple{0x91, 0x41, 0xac}, k_grey{0x77, 0x76, 0x7b},
    k_ink{0x4a, 0x48, 0x50}, k_paper{0xff, 0xff, 0xff}, k_folder{0xf6, 0xc3, 0x2d},
    k_folder_edge{0xb5, 0x83, 0x0c}, k_box{0xcd, 0xab, 0x8f}, k_box_edge{0x86, 0x5e, 0x3c};

// Drawing on a 16-unit square, scaled to the icon's size.
struct pen
{
    rap::painter& p;
    float x, y, u;

    point_f at(float px, float py) const { return {x + px * u, y + py * u}; }
    float w(float units) const { return units * u; }

    void line(float x0, float y0, float x1, float y1, color c, float width = 1.5f) const
    {
        p.line(at(x0, y0), at(x1, y1), c, w(width), rap::line_cap::round);
    }
    void box(float x0, float y0, float x1, float y1, color fill, color edge, float r = 0) const
    {
        p.rect({x + x0 * u, y + y0 * u, (x1 - x0) * u, (y1 - y0) * u}, w(r), rap::paint{fill},
               rap::paint{edge});
    }
    void disc(float cx, float cy, float r, color fill, std::optional<color> edge = {}) const
    {
        p.circle(at(cx, cy), w(r), rap::paint{fill},
                 edge ? std::optional<rap::paint>(*edge) : std::nullopt);
    }
    void ring(float cx, float cy, float r, color c) const
    {
        p.circle(at(cx, cy), w(r), std::nullopt, rap::paint{c});
    }
    void shape(std::vector<point_f> pts, color fill, std::optional<color> edge = {}) const
    {
        for (point_f& q : pts) q = at(q.x, q.y);
        p.polygon(pts, rap::paint{fill}, edge ? std::optional<rap::paint>(*edge) : std::nullopt,
                  w(1));
    }
    void arc(float cx, float cy, float r, float from, float to, color c, float width = 1.5f) const
    {
        constexpr float deg = std::numbers::pi_v<float> / 180;
        p.arc(at(cx, cy), w(r), from * deg, to * deg, c, w(width), rap::line_cap::round);
    }
    void glyph(float cx, float cy, const char* s, color c, float pt) const
    {
        const rap::text_style style{.fg = rap::paint{c}, .bold = true, .size = pt * u * 0.75f};
        const rap::size_f m = p.measure(s, style);
        p.text({x + cx * u - m.w / 2, y + cy * u - m.h / 2}, s, style);
    }
};

// -- motifs ---------------------------------------------------------------------

// A page with a folded corner; `lines` of text on it.
void page(const pen& d, float x0 = 2.5f, float y0 = 1.5f, float x1 = 12.5f, float y1 = 14.5f,
          int lines = 3)
{
    const float fold = 3;
    d.shape({{x0, y0}, {x1 - fold, y0}, {x1, y0 + fold}, {x1, y1}, {x0, y1}}, k_paper, k_ink);
    d.shape({{x1 - fold, y0}, {x1 - fold, y0 + fold}, {x1, y0 + fold}}, {0xde, 0xdd, 0xda}, k_ink);
    for (int i = 0; i < lines; ++i)
    {
        const float ly = y0 + 5 + i * 3.f;
        if (ly > y1 - 1.5f) break;
        d.line(x0 + 2, ly, x1 - 2 - (i % 2) * 2, ly, {0xa0, 0x9e, 0xa6}, 1);
    }
}

void folder(const pen& d, float x0 = 1.5f, float y0 = 2.5f, float x1 = 14.5f, float y1 = 13.5f)
{
    d.shape({{x0, y0}, {x0 + 5, y0}, {x0 + 6.5f, y0 + 1.5f}, {x1, y0 + 1.5f}, {x1, y1}, {x0, y1}},
            k_folder, k_folder_edge);
    d.line(x0 + 0.5f, y0 + 3.5f, x1 - 0.5f, y0 + 3.5f, k_folder_edge, 1);
}

// A thick arrow from (x0,y0) towards (x1,y1).
void arrow(const pen& d, float x0, float y0, float x1, float y1, color c, float width = 2)
{
    const float dx = x1 - x0, dy = y1 - y0, len = std::hypot(dx, dy);
    const float ux = dx / len, uy = dy / len, head = 3.6f;
    const float bx = x1 - ux * head, by = y1 - uy * head;
    d.line(x0, y0, bx, by, c, width);
    d.shape({{x1, y1}, {bx - uy * head * 0.8f, by + ux * head * 0.8f},
             {bx + uy * head * 0.8f, by - ux * head * 0.8f}},
            c, c);
}

// A small round badge in the bottom-right corner.
void badge(const pen& d, color c)
{
    d.disc(11.5f, 11.5f, 4, c, color{0xff, 0xff, 0xff});
}
void badge_plus(const pen& d, color c = k_green)
{
    badge(d, c);
    d.line(11.5f, 9.3f, 11.5f, 13.7f, k_paper, 1.6f);
    d.line(9.3f, 11.5f, 13.7f, 11.5f, k_paper, 1.6f);
}
void badge_minus(const pen& d, color c = k_red)
{
    badge(d, c);
    d.line(9.3f, 11.5f, 13.7f, 11.5f, k_paper, 1.6f);
}
void badge_cross(const pen& d, color c = k_red)
{
    badge(d, c);
    d.line(10, 10, 13, 13, k_paper, 1.6f);
    d.line(13, 10, 10, 13, k_paper, 1.6f);
}
void badge_check(const pen& d, color c = k_green)
{
    badge(d, c);
    d.line(9.6f, 11.6f, 11, 13, k_paper, 1.6f);
    d.line(11, 13, 13.5f, 10, k_paper, 1.6f);
}
void badge_arrow(const pen& d, bool up, color c)
{
    badge(d, c);
    if (up)
        arrow(d, 11.5f, 14, 11.5f, 8.8f, k_paper, 1.5f);
    else
        arrow(d, 11.5f, 9, 11.5f, 14.2f, k_paper, 1.5f);
}
void badge_letter(const pen& d, const char* s, color c)
{
    badge(d, c);
    d.glyph(11.5f, 11.6f, s, k_paper, 6.5f);
}
void badge_clock(const pen& d)
{
    badge(d, k_blue);
    d.line(11.5f, 11.5f, 11.5f, 9.2f, k_paper, 1.3f);
    d.line(11.5f, 11.5f, 13.2f, 12.3f, k_paper, 1.3f);
}
void badge_sync(const pen& d, color c = k_blue)
{
    badge(d, c);
    d.arc(11.5f, 11.5f, 2.2f, 200, 340, k_paper, 1.2f);
    d.arc(11.5f, 11.5f, 2.2f, 20, 160, k_paper, 1.2f);
}

// A commit: a dot on a line.
void commit_line(const pen& d, color c, bool vertical = false)
{
    if (vertical)
        d.line(8, 1, 8, 15, k_ink, 2);
    else
        d.line(1, 8, 15, 8, k_ink, 2);
    d.disc(8, 8, 3.6f, c, k_ink);
}

// A history: a line of commits and their summaries.
void history(const pen& d, color dots)
{
    d.line(3.5f, 2, 3.5f, 14, k_ink, 1);
    for (float y : {3.f, 8.f, 13.f})
    {
        d.disc(3.5f, y, 1.8f, dots, k_ink);
        d.line(7, y, y == 8.f ? 12 : 14.5f, y, k_ink, 2);
    }
}

// A cardboard box (a stash, a submodule).
void crate(const pen& d, float x0 = 1.5f, float y0 = 5, float x1 = 14.5f, float y1 = 14.5f)
{
    d.box(x0, y0, x1, y1, k_box, k_box_edge, 0.5f);
    d.box(x0 - 0.5f, y0 - 1.5f, x1 + 0.5f, y0 + 1.5f, k_box, k_box_edge, 0.5f);
    d.box((x0 + x1) / 2 - 1.5f, y0 + 3, (x0 + x1) / 2 + 1.5f, y0 + 4.5f, k_box_edge, k_box_edge);
}

// A cube (a submodule).
void cube(const pen& d)
{
    const color top{0x99, 0xc1, 0xf1}, left{0x62, 0xa0, 0xea}, right{0x35, 0x84, 0xe4};
    d.shape({{8, 1.5f}, {14, 4.5f}, {8, 7.5f}, {2, 4.5f}}, top, k_ink);
    d.shape({{2, 4.5f}, {8, 7.5f}, {8, 14.5f}, {2, 11.5f}}, left, k_ink);
    d.shape({{8, 7.5f}, {14, 4.5f}, {14, 11.5f}, {8, 14.5f}}, right, k_ink);
}

void magnifier(const pen& d, float cx, float cy, float r = 3.2f)
{
    d.line(cx + r * 0.7f, cy + r * 0.7f, cx + r * 1.7f, cy + r * 1.7f, k_ink, 2.2f);
    d.disc(cx, cy, r, {0xdb, 0xea, 0xfb}, k_ink);
}

void tag_shape(const pen& d, color c)
{
    d.shape({{1.5f, 3}, {9.5f, 3}, {14.5f, 8}, {9.5f, 13}, {1.5f, 13}}, c, k_ink);
    d.disc(4.5f, 8, 1.3f, k_paper, k_ink);
}

void branch_shape(const pen& d, color c)
{
    d.line(5, 2.5f, 5, 13.5f, k_ink, 2);
    d.p.curve(d.at(5, 11), d.at(5, 7), d.at(11.5f, 8), d.at(11.5f, 4.5f), k_ink, d.w(2));
    d.disc(5, 13, 1.9f, k_grey, k_ink);
    d.disc(5, 3, 1.9f, k_grey, k_ink);
    d.disc(11.5f, 3.5f, 2.2f, c, k_ink);
}

void merge_shape(const pen& d, color c)
{
    // Newest at the top, as in the log: two parents below join the merge.
    d.line(5, 2.5f, 5, 13.5f, k_ink, 2);
    d.p.curve(d.at(11.5f, 13), d.at(11.5f, 8), d.at(5, 9), d.at(5, 4.5f), k_ink, d.w(2));
    d.disc(5, 13, 1.9f, k_grey, k_ink);
    d.disc(11.5f, 12.5f, 1.9f, k_grey, k_ink);
    d.disc(5, 3.5f, 2.2f, c, k_ink);
}

void gear(const pen& d, color c)
{
    std::vector<point_f> pts;
    for (int i = 0; i < 32; ++i)
    {
        const float a = i * std::numbers::pi_v<float> / 16;
        const float r = (i / 2) % 2 ? 5 : 7;
        pts.push_back({8 + r * std::cos(a + 0.1f), 8 + r * std::sin(a + 0.1f)});
    }
    d.shape(pts, c, k_ink);
    d.disc(8, 8, 2.2f, k_paper, k_ink);
}

void envelope(const pen& d)
{
    d.box(1.5f, 3.5f, 14.5f, 12.5f, k_paper, k_ink);
    d.line(2, 4, 8, 9, k_ink, 1.1f);
    d.line(8, 9, 14, 4, k_ink, 1.1f);
}

void sync_arrows(const pen& d, color c)
{
    d.arc(8, 8, 5.2f, 200, 330, c, 2);
    d.shape({{13.9f, 4.3f}, {14.4f, 9}, {10.3f, 6.9f}}, c, c);
    d.arc(8, 8, 5.2f, 20, 150, c, 2);
    d.shape({{2.1f, 11.7f}, {1.6f, 7}, {5.7f, 9.1f}}, c, c);
}

// A patch: a page of +/- lines.
void patch_page(const pen& d)
{
    page(d, 2.5f, 1.5f, 12.5f, 14.5f, 0);
    d.line(5, 6, 10, 6, k_green, 2);
    d.line(5, 9, 9, 9, k_red, 2);
    d.line(5, 12, 10, 12, k_green, 2);
}

void two_pages(const pen& d)
{
    page(d, 1.5f, 1.5f, 9.5f, 11.5f, 0);
    d.line(3, 6, 7, 6, k_red, 2);
    d.line(3, 9, 6, 9, k_red, 2);
    page(d, 6.5f, 4.5f, 14.5f, 14.5f, 0);
    d.line(8, 9, 13, 9, k_green, 2);
    d.line(8, 12, 12, 12, k_green, 2);
}

// -- the icons --------------------------------------------------------------------

struct icon
{
    std::string_view group;
    void (*draw)(const pen&);
};

constexpr icon k_icons[] = {
    {"inaccessible",
     [](const pen& d)
     {
         d.shape({{8, 1.5f}, {15, 14}, {1, 14}}, {0xf5, 0xc2, 0x11}, k_ink);
         d.glyph(8, 9.6f, "!", k_ink, 9);
     }},
    {"clone",
     [](const pen& d)
     {
         folder(d, 1, 5, 15, 15);
         arrow(d, 8, 0.5f, 8, 11.5f, k_blue);
     }},
    {"pull",
     [](const pen& d)
     {
         d.line(2, 14, 14, 14, k_ink, 2);
         arrow(d, 8, 1, 8, 12.5f, k_green, 2);
     }},
    {"fetch",
     [](const pen& d)
     {
         d.line(2, 14, 14, 14, {0xa0, 0x9e, 0xa6}, 2);
         arrow(d, 8, 1, 8, 12.5f, k_blue, 2);
     }},
    {"push",
     [](const pen& d)
     {
         d.line(2, 2, 14, 2, k_ink, 2);
         arrow(d, 8, 15, 8, 3.5f, k_orange, 2);
     }},
    {"sync", [](const pen& d) { sync_arrows(d, k_blue); }},
    {"commit", [](const pen& d) { commit_line(d, k_green); }},
    {"svndcommit",
     [](const pen& d)
     {
         commit_line(d, k_green);
         badge_letter(d, "S", k_purple);
     }},
    {"svnrebase",
     [](const pen& d)
     {
         branch_shape(d, k_orange);
         badge_letter(d, "S", k_purple);
     }},
    {"svnfetch",
     [](const pen& d)
     {
         arrow(d, 6, 1, 6, 12, k_blue, 2);
         badge_letter(d, "S", k_purple);
     }},
    {"svnignore",
     [](const pen& d)
     {
         page(d);
         badge_letter(d, "S", k_purple);
     }},
    {"diff", [](const pen& d) { two_pages(d); }},
    {"difflater",
     [](const pen& d)
     {
         page(d);
         badge_clock(d);
     }},
    {"prevdiff",
     [](const pen& d)
     {
         two_pages(d);
         badge_letter(d, "<", k_blue);
     }},
    {"log", [](const pen& d) { history(d, k_blue); }},
    {"logsubmodule",
     [](const pen& d)
     {
         history(d, k_blue);
         badge(d, k_paper);
         d.box(9.5f, 9.5f, 13.5f, 13.5f, {0x62, 0xa0, 0xea}, k_ink);
     }},
    {"reflog",
     [](const pen& d)
     {
         history(d, k_grey);
         badge_sync(d, k_grey);
     }},
    {"refbrowse",
     [](const pen& d)
     {
         for (float y : {2.f, 6.5f, 11.f})
         {
             d.shape({{1.5f, y}, {5, y}, {7, y + 1.75f}, {5, y + 3.5f}, {1.5f, y + 3.5f}},
                     y == 2.f ? k_green : y == 6.5f ? k_orange : k_blue, k_ink);
             d.line(9, y + 1.75f, 14.5f, y + 1.75f, k_ink, 1.2f);
         }
     }},
    {"daemon",
     [](const pen& d)
     {
         for (float y : {1.5f, 6.f, 10.5f})
         {
             d.box(2, y, 14, y + 4, {0xde, 0xdd, 0xda}, k_ink, 0.8f);
             d.disc(11.5f, y + 2, 0.8f, k_green);
         }
     }},
    {"revisiongraph",
     [](const pen& d)
     {
         d.line(4, 3, 4, 13, k_ink, 1.3f);
         d.line(4, 5, 12, 8, k_ink, 1.3f);
         d.line(12, 8, 4, 11, k_ink, 1.3f);
         for (auto [cx, cy] : {std::pair{4.f, 3.f}, {4.f, 13.f}, {12.f, 8.f}})
             d.box(cx - 2, cy - 1.5f, cx + 2, cy + 1.5f, cx == 12 ? k_orange : k_blue, k_ink, 0.5f);
     }},
    {"repobrowser",
     [](const pen& d)
     {
         folder(d, 0.5f, 1.5f, 12.5f, 11);
         magnifier(d, 10, 10, 2.8f);
     }},
    {"repostatus",
     [](const pen& d)
     {
         page(d, 1.5f, 0.5f, 11.5f, 13.5f);
         magnifier(d, 10, 10, 2.8f);
     }},
    {"rebase",
     [](const pen& d)
     {
         branch_shape(d, k_orange);
         arrow(d, 13.5f, 9, 13.5f, 14.5f, k_orange, 1.6f);
     }},
    {"stashsave",
     [](const pen& d)
     {
         crate(d);
         arrow(d, 8, 0, 8, 7, k_blue, 2);
     }},
    {"stashapply",
     [](const pen& d)
     {
         crate(d);
         arrow(d, 8, 9, 8, 0.5f, k_green, 2);
     }},
    {"stashpop",
     [](const pen& d)
     {
         crate(d);
         arrow(d, 8, 9, 8, 0.5f, k_orange, 2);
     }},
    {"stashlist",
     [](const pen& d)
     {
         crate(d, 1.5f, 5, 9.5f, 14.5f);
         for (float y : {5.f, 8.f, 11.f}) d.line(11.5f, y, 15, y, k_ink, 1.2f);
     }},
    {"bisect",
     [](const pen& d)
     {
         commit_line(d, k_grey, true);
         d.line(1, 8, 15, 8, k_red, 1.5f);
     }},
    {"conflicteditor",
     [](const pen& d)
     {
         page(d);
         badge_letter(d, "!", k_orange);
     }},
    {"resolve",
     [](const pen& d)
     {
         page(d);
         badge_check(d);
     }},
    {"merge", [](const pen& d) { merge_shape(d, k_purple); }},
    {"rename",
     [](const pen& d)
     {
         d.box(1, 4.5f, 15, 11.5f, k_paper, k_ink, 0.8f);
         d.line(3, 8, 9, 8, k_ink, 1.3f);
         d.line(11, 3, 11, 13, k_blue, 1.3f);
         d.line(9.5f, 3, 12.5f, 3, k_blue, 1.3f);
         d.line(9.5f, 13, 12.5f, 13, k_blue, 1.3f);
     }},
    {"remove",
     [](const pen& d)
     {
         page(d);
         badge_cross(d);
     }},
    {"removekeep",
     [](const pen& d)
     {
         page(d);
         badge_minus(d);
     }},
    {"revert",
     [](const pen& d)
     {
         d.arc(8.5f, 8.5f, 5, 200, 470, k_red, 2.2f);
         d.shape({{0.8f, 7.8f}, {7, 7.5f}, {3.6f, 2.6f}}, k_red, k_red);
     }},
    {"cleanup",
     [](const pen& d)
     {
         d.line(13.5f, 1.5f, 8, 8, {0x86, 0x5e, 0x3c}, 1.8f);
         d.shape({{6.5f, 6.5f}, {9.5f, 9.5f}, {6, 15}, {1, 10}}, {0xf5, 0xc2, 0x11}, k_ink);
         d.line(3.5f, 12.5f, 7, 9, {0xb5, 0x83, 0x0c}, 0.9f);
     }},
    {"switch",
     [](const pen& d)
     {
         d.line(3, 2, 3, 14, k_ink, 2);
         d.line(13, 2, 13, 14, k_ink, 2);
         d.disc(3, 8, 1.8f, k_grey, k_ink);
         arrow(d, 4.5f, 8, 12.5f, 8, k_blue, 2);
     }},
    {"branch", [](const pen& d) { branch_shape(d, k_green); }},
    {"tag", [](const pen& d) { tag_shape(d, {0xf6, 0xd3, 0x2d}); }},
    {"export",
     [](const pen& d)
     {
         crate(d, 1.5f, 5, 10.5f, 14.5f);
         arrow(d, 6, 9.5f, 15.5f, 9.5f, k_blue, 2);
     }},
    {"repocreate",
     [](const pen& d)
     {
         folder(d);
         badge_plus(d);
     }},
    {"add",
     [](const pen& d)
     {
         page(d);
         badge_plus(d);
     }},
    {"blame",
     [](const pen& d)
     {
         page(d, 5.5f, 1.5f, 15, 14.5f);
         d.disc(3.5f, 5, 2.2f, k_blue, k_ink);
         d.shape({{0.5f, 13}, {0.8f, 9}, {3.5f, 8}, {6.2f, 9}, {6.5f, 13}}, k_blue, k_ink);
     }},
    {"ignore",
     [](const pen& d)
     {
         page(d);
         badge(d, k_grey);
         d.ring(11.5f, 11.5f, 2.3f, k_paper);
         d.line(10, 13, 13, 10, k_paper, 1.2f);
     }},
    {"worktreelist",
     [](const pen& d)
     {
         folder(d, 4, 1, 15.5f, 9);
         folder(d, 0.5f, 6, 12, 14.5f);
     }},
    {"subadd",
     [](const pen& d)
     {
         cube(d);
         badge_plus(d);
     }},
    {"subupdate",
     [](const pen& d)
     {
         cube(d);
         badge_arrow(d, false, k_green);
     }},
    {"subsync",
     [](const pen& d)
     {
         cube(d);
         badge_sync(d);
     }},
    {"formatpatch",
     [](const pen& d)
     {
         patch_page(d);
         badge_arrow(d, true, k_orange);
     }},
    {"importpatch",
     [](const pen& d)
     {
         patch_page(d);
         badge_arrow(d, false, k_green);
     }},
    {"patch", [](const pen& d) { patch_page(d); }},
    {"sendmail", [](const pen& d) { envelope(d); }},
    {"settings", [](const pen& d) { gear(d, {0x9a, 0x99, 0x96}); }},
    {"help",
     [](const pen& d)
     {
         d.disc(8, 8, 7, k_blue, k_ink);
         d.glyph(8, 8.2f, "?", k_paper, 11);
     }},
    {"about",
     [](const pen& d)
     {
         d.disc(8, 8, 7, k_blue, k_ink);
         d.glyph(8, 8.2f, "i", k_paper, 11);
     }},
};

constexpr auto k_groups = []
{
    std::array<std::string_view, std::size(k_icons)> out{};
    for (size_t i = 0; i < out.size(); ++i) out[i] = k_icons[i].group;
    return out;
}();

}  // namespace

void paint_menu_icon(rap::painter& p, std::string_view group, float x, float y, float size)
{
    for (const icon& i : k_icons)
        if (i.group == group) return i.draw(pen{p, x, y, size / 16});
}

std::span<const std::string_view> menu_icon_groups()
{
    return k_groups;
}

}  // namespace tortilla
