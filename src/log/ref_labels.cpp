#include "ref_labels.h"

#include <algorithm>

namespace tortilla
{
namespace
{

using rap::color;
using rap::paint;
using rap::rect_f;

// Ref label colours [COL:23-54].
color label_colour(const ref& r)
{
    switch (r.kind)
    {
    case ref_kind::local_branch: return r.current ? color{200, 0, 0} : color{0, 195, 0};
    case ref_kind::remote_branch: return {255, 221, 170};
    case ref_kind::tag: return {255, 255, 0};
    case ref_kind::stash: return {128, 128, 128};
    case ref_kind::head:
    case ref_kind::other: break;
    }
    return {224, 224, 224};
}

// Mix toward white / black by amount/255 [COL:85-109].
color lighten(color c, int amount)
{
    auto f = [&](uint8_t v) { return uint8_t(v + (255 - v) * amount / 255); };
    return {f(c.r), f(c.g), f(c.b), c.a};
}

color darken(color c, int amount)
{
    auto f = [&](uint8_t v) { return uint8_t(v * (255 - amount) / 255); };
    return {f(c.r), f(c.g), f(c.b), c.a};
}

// Dark labels get white text [GLLB:534-539].
bool is_dark(color c)
{
    return c.r * 30 + c.g * 59 + c.b * 11 <= 12800;
}

}  // namespace

void paint_check_mark(rap::painter& p, rap::point_f c, float d, check_state s, mergeable m)
{
    const float r = d / 2;
    const color white{255, 255, 255};
    const color grey{110, 119, 129}, yellow{232, 178, 0};
    // No checks, but it can't be merged: a "!", grey for conflicts, yellow
    // for the forge's rules.
    if (s == check_state::none && m != mergeable::yes)
    {
        p.circle(c, r, paint{m == mergeable::conflicts ? grey : yellow}, paint{white});
        const float w = std::max(1.5f, d / 7);
        p.line({c.x, c.y - r * 0.5f}, {c.x, c.y + r * 0.12f}, paint{white}, w,
               rap::line_cap::round);
        p.circle({c.x, c.y + r * 0.45f}, w * 0.6f, paint{white}, std::nullopt);
        return;
    }
    switch (s)
    {
    case check_state::success:
        p.circle(c, r,
                 paint{m == mergeable::conflicts ? grey
                       : m == mergeable::not_yet ? yellow
                                                 : color{26, 127, 55}},
                 paint{white});
        {
            const float w = std::max(1.5f, d / 8);
            const rap::point_f a{c.x - r * 0.45f, c.y + r * 0.02f},
                b{c.x - r * 0.12f, c.y + r * 0.36f}, e{c.x + r * 0.48f, c.y - r * 0.33f};
            p.line(a, b, paint{white}, w, rap::line_cap::round);
            p.line(b, e, paint{white}, w, rap::line_cap::round);
            break;
        }
    case check_state::failure:
    {
        p.circle(c, r, paint{color{207, 34, 46}}, paint{white});
        const float a = r * 0.36f, w = std::max(1.5f, d / 8);
        p.line({c.x - a, c.y - a}, {c.x + a, c.y + a}, paint{white}, w);
        p.line({c.x - a, c.y + a}, {c.x + a, c.y - a}, paint{white}, w);
        break;
    }
    case check_state::pending:
        p.circle(c, r, paint{color{191, 135, 0}}, paint{white});
        p.circle(c, r * 0.3f, paint{white}, std::nullopt);
        break;
    case check_state::none: break;
    }
}


// [GLLB:523-657]: full-height rectangles, text width + 8, 1 px apart.
// Branches with an upstream are rounded with a drop shadow and their
// upstream's label follows them; annotated tags get an arrow tip;
// everything else is bevelled.
float paint_ref_labels(rap::painter& p, const std::vector<ref>& refs, float x, float h,
                       const std::function<bool(const ref&)>& show, std::vector<label_box>* boxes,
                       const std::function<std::optional<pr_mark>(const ref&)>& pr)
{
    if (boxes) boxes->clear();
    auto visible = [&](const ref& r) { return !show || show(r); };

    // Upstream labels move next to their tracking branch.
    std::vector<std::pair<const ref*, bool>> order;  // ref, rounded
    std::vector<const ref*> placed;
    for (const ref& r : refs)
    {
        if (!visible(r) || std::ranges::find(placed, &r) != placed.end()) continue;
        bool tracking = r.kind == ref_kind::local_branch && !r.upstream.empty();
        order.push_back({&r, tracking});
        placed.push_back(&r);
        if (!tracking) continue;
        for (const ref& up : refs)
        {
            if (up.full_name != r.upstream || !visible(up)) continue;
            order.push_back({&up, true});
            placed.push_back(&up);
        }
    }

    float start = x;
    for (auto [ref, rounded] : order)
    {
        color fill = label_colour(*ref);
        rap::text_style ts;
        ts.fg = is_dark(fill) ? color{255, 255, 255} : color{0, 0, 0};
        rap::size_f t = p.measure(ref->name, ts);
        float w = t.w + 8;
        // A pull request's "#12" and its checks, after the name.
        const std::optional<pr_mark> mark = pr ? pr(*ref) : std::nullopt;
        const std::string number = mark ? "#" + std::to_string(mark->number) : "";
        rap::text_style number_style = ts;
        if (mark && mark->draft)  // a draft's number fainter
            number_style.fg = is_dark(fill) ? color{255, 255, 255, 150} : color{0, 0, 0, 150};
        const float number_w = mark ? p.measure(number, number_style).w : 0;
        const float dot = std::clamp(h - 7, 8.0f, 14.0f);
        const bool has_dot =
            mark && (mark->checks != check_state::none || mark->conflicts || mark->blocked);
        if (mark) w += 5 + number_w + (has_dot ? dot + 3 : 0);
        rect_f box{x, 0, w, h};

        if (rounded)
        {
            p.rect({box.x + 2 + 1, box.y + 2 + 1, box.w - 2 - 2, box.h - 2 - 2}, 4,
                   paint{darken(fill, 100)}, std::nullopt);
            p.rect({box.x, box.y, box.w - 2, box.h - 2}, 4, paint{fill}, std::nullopt);
        }
        else
        {
            p.rect(box, 0, paint{fill}, std::nullopt);
            // Two-step bevel: light top/left, dark bottom/right.
            for (int step = 0; step < 2; ++step)
            {
                float i = float(step) + 0.5f;
                color light = lighten(fill, step ? 50 : 100), dark = darken(fill, step ? 50 : 100);
                p.line({box.x + i, box.y + h - i}, {box.x + i, box.y + i}, paint{light}, 1);
                p.line({box.x + i, box.y + i}, {box.x + w - i, box.y + i}, paint{light}, 1);
                p.line({box.x + w - i, box.y + i}, {box.x + w - i, box.y + h - i}, paint{dark}, 1);
                p.line({box.x + w - i, box.y + h - i}, {box.x + i, box.y + h - i}, paint{dark}, 1);
            }
        }
        if (ref->kind == ref_kind::tag && ref->annotated)
        {
            p.polygon({{box.x + w, 0}, {box.x + w + 8, h / 2}, {box.x + w, h}}, paint{fill},
                      std::nullopt);
            p.line({box.x + w, 0}, {box.x + w + 8, h / 2}, paint{lighten(fill, 50)}, 2);
            p.line({box.x + w + 8, h / 2}, {box.x + w, h}, paint{darken(fill, 50)}, 2);
            w += 8;
        }
        const float ty = (h - t.h) / 2 - (rounded ? 1 : 0);
        p.text({box.x + 4, ty}, ref->name, ts);
        if (mark)
        {
            const float nx = box.x + 4 + t.w + 5;
            // A hairline before the number: the label's two parts.
            p.line({nx - 2.5f, 3}, {nx - 2.5f, h - (rounded ? 5 : 3)}, paint{darken(fill, 60)}, 1);
            p.text({nx, ty}, number, number_style);
            if (has_dot)
                paint_check_mark(p, {nx + number_w + 3 + dot / 2, (h - (rounded ? 2 : 0)) / 2}, dot,
                                 mark->checks,
                                 mark->conflicts ? mergeable::conflicts
                                 : mark->blocked ? mergeable::not_yet
                                                 : mergeable::yes);
        }
        if (boxes) boxes->push_back({x, x + w, *ref});
        x += w + 1;
    }
    return order.empty() ? start : x;
}

}  // namespace tortilla
