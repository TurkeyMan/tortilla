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

// [GLLB:523-657]: full-height rectangles, text width + 8, 1 px apart.
// Branches with an upstream are rounded with a drop shadow and their
// upstream's label follows them; annotated tags get an arrow tip;
// everything else is bevelled.
float paint_ref_labels(rap::painter& p, const std::vector<ref>& refs, float x, float h,
                     const std::function<bool(const ref&)>& show, std::vector<label_box>* boxes)
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
        p.text({box.x + 4, (h - t.h) / 2 - (rounded ? 1 : 0)}, ref->name, ts);
        if (boxes) boxes->push_back({x, x + w, *ref});
        x += w + 1;
    }
    return order.empty() ? start : x;
}

}  // namespace tortilla
