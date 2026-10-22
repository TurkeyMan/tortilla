#include "ref_labels.h"

#include <algorithm>

namespace tortilla
{
namespace
{

using rap::Color;
using rap::Paint;
using rap::RectF;

// Ref label colours [COL:23-54].
Color labelColour(const Ref& r)
{
    switch (r.kind)
    {
    case RefKind::LocalBranch: return r.current ? Color{200, 0, 0} : Color{0, 195, 0};
    case RefKind::RemoteBranch: return {255, 221, 170};
    case RefKind::Tag: return {255, 255, 0};
    case RefKind::Stash: return {128, 128, 128};
    case RefKind::Head:
    case RefKind::Other: break;
    }
    return {224, 224, 224};
}

// Mix toward white / black by amount/255 [COL:85-109].
Color lighten(Color c, int amount)
{
    auto f = [&](uint8_t v) { return uint8_t(v + (255 - v) * amount / 255); };
    return {f(c.r), f(c.g), f(c.b), c.a};
}

Color darken(Color c, int amount)
{
    auto f = [&](uint8_t v) { return uint8_t(v * (255 - amount) / 255); };
    return {f(c.r), f(c.g), f(c.b), c.a};
}

// Dark labels get white text [GLLB:534-539].
bool isDark(Color c)
{
    return c.r * 30 + c.g * 59 + c.b * 11 <= 12800;
}

}  // namespace

// [GLLB:523-657]: full-height rectangles, text width + 8, 1 px apart.
// Branches with an upstream are rounded with a drop shadow and their
// upstream's label follows them; annotated tags get an arrow tip;
// everything else is bevelled.
float paintRefLabels(rap::Painter& p, const std::vector<Ref>& refs, float x, float h,
                     const std::function<bool(const Ref&)>& show, std::vector<LabelBox>* boxes)
{
    if (boxes) boxes->clear();
    auto visible = [&](const Ref& r) { return !show || show(r); };

    // Upstream labels move next to their tracking branch.
    std::vector<std::pair<const Ref*, bool>> order;  // ref, rounded
    std::vector<const Ref*> placed;
    for (const Ref& r : refs)
    {
        if (!visible(r) || std::ranges::find(placed, &r) != placed.end()) continue;
        bool tracking = r.kind == RefKind::LocalBranch && !r.upstream.empty();
        order.push_back({&r, tracking});
        placed.push_back(&r);
        if (!tracking) continue;
        for (const Ref& up : refs)
        {
            if (up.fullName != r.upstream || !visible(up)) continue;
            order.push_back({&up, true});
            placed.push_back(&up);
        }
    }

    float start = x;
    for (auto [ref, rounded] : order)
    {
        Color fill = labelColour(*ref);
        rap::TextStyle ts;
        ts.fg = isDark(fill) ? Color{255, 255, 255} : Color{0, 0, 0};
        rap::SizeF t = p.measure(ref->name, ts);
        float w = t.w + 8;
        RectF box{x, 0, w, h};

        if (rounded)
        {
            p.rect({box.x + 2 + 1, box.y + 2 + 1, box.w - 2 - 2, box.h - 2 - 2}, 4,
                   Paint{darken(fill, 100)}, std::nullopt);
            p.rect({box.x, box.y, box.w - 2, box.h - 2}, 4, Paint{fill}, std::nullopt);
        }
        else
        {
            p.rect(box, 0, Paint{fill}, std::nullopt);
            // Two-step bevel: light top/left, dark bottom/right.
            for (int step = 0; step < 2; ++step)
            {
                float i = float(step) + 0.5f;
                Color light = lighten(fill, step ? 50 : 100), dark = darken(fill, step ? 50 : 100);
                p.line({box.x + i, box.y + h - i}, {box.x + i, box.y + i}, Paint{light}, 1);
                p.line({box.x + i, box.y + i}, {box.x + w - i, box.y + i}, Paint{light}, 1);
                p.line({box.x + w - i, box.y + i}, {box.x + w - i, box.y + h - i}, Paint{dark}, 1);
                p.line({box.x + w - i, box.y + h - i}, {box.x + i, box.y + h - i}, Paint{dark}, 1);
            }
        }
        if (ref->kind == RefKind::Tag && ref->annotated)
        {
            p.polygon({{box.x + w, 0}, {box.x + w + 8, h / 2}, {box.x + w, h}}, Paint{fill},
                      std::nullopt);
            p.line({box.x + w, 0}, {box.x + w + 8, h / 2}, Paint{lighten(fill, 50)}, 2);
            p.line({box.x + w + 8, h / 2}, {box.x + w, h}, Paint{darken(fill, 50)}, 2);
            w += 8;
        }
        p.text({box.x + 4, (h - t.h) / 2 - (rounded ? 1 : 0)}, ref->name, ts);
        if (boxes) boxes->push_back({x, x + w, *ref});
        x += w + 1;
    }
    return order.empty() ? start : x;
}

}  // namespace tortilla
