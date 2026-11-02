#include "ref_labels.h"

#include "i18n.h"

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

std::string count(std::int64_t n)
{
    return std::to_string(n);
}

// "6 min 12 s", "5 min", "45 s", "1 h 3 min"; to the minute, once it's
// that long, for what's still running (the poll's a minute behind anyway).
std::string duration_text(std::int64_t s, bool to_the_minute)
{
    s = std::max<std::int64_t>(s, 0);
    if (s >= 3600)
        return s % 3600 / 60
                   ? rap::i18n::format(_("{0} h {1} min"), count(s / 3600), count(s % 3600 / 60))
                   : rap::i18n::format(_("{0} h"), count(s / 3600));
    if (s >= 60)
        return s % 60 && !to_the_minute
                   ? rap::i18n::format(_("{0} min {1} s"), count(s / 60), count(s % 60))
                   : rap::i18n::format(_("{0} min"), count(s / 60));
    return rap::i18n::format(_("{0} s"), count(s));
}

// The first few runs' names: "build, test and 3 more".
std::string run_names(const std::vector<const check_run*>& runs)
{
    constexpr size_t shown = 4;
    std::string out;
    for (size_t i = 0; i < runs.size() && i < shown; ++i) out += (i ? ", " : "") + runs[i]->name;
    if (runs.size() <= shown) return out;
    const size_t more = runs.size() - shown;
    return rap::i18n::format(N_("{0} and {1} more", "{0} and {1} more", more), out, count(more));
}

std::string block_text(merge_block b, const pull_request& pr)
{
    switch (b)
    {
    case merge_block::conflicts:
        return pr.base_branch.empty() ? std::string(_("Conflicts with its base"))
                                      : rap::i18n::format(_("Conflicts with {0}"), pr.base_branch);
    case merge_block::review_required: return _("Needs an approving review");
    case merge_block::changes_requested: return _("Changes requested");
    case merge_block::behind:
        return pr.base_branch.empty() ? std::string(_("Behind its base"))
                                      : rap::i18n::format(_("Behind {0}"), pr.base_branch);
    case merge_block::draft: return _("Draft");
    case merge_block::discussions: return _("Unresolved threads");
    case merge_block::blocked_other: return _("The repository's rules stop a merge");
    }
    return {};
}

}  // namespace

pr_mark mark_of(const pull_request& pr)
{
    return {.number = pr.number,
            .checks = pr.checks,
            .draft = pr.draft,
            .blocked = pr.blocked,
            .conflicts = pr.conflicts};
}

std::string pr_tooltip(const pull_request& pr, std::int64_t now)
{
    // The runs that ran (or will): a GitLab job skipped or left to start by
    // hand doesn't count.
    std::vector<const check_run*> failed, running;
    std::int64_t counted = 0, first_start = 0, last_end = 0, running_since = 0;
    for (const check_run& r : pr.runs)
    {
        if (r.state == check_state::none) continue;
        ++counted;
        if (r.state == check_state::failure) failed.push_back(&r);
        if (r.state == check_state::pending)
        {
            running.push_back(&r);
            if (r.started_at && (!running_since || r.started_at < running_since))
                running_since = r.started_at;
        }
        if (r.started_at && (!first_start || r.started_at < first_start))
            first_start = r.started_at;
        if (r.completed_at > last_end) last_end = r.completed_at;
    }
    if (!running_since) running_since = first_start;

    std::vector<std::string> parts{"#" + count(pr.number)};
    const bool pipe = pr.pipeline;
    switch (pr.checks)
    {
    case check_state::none: parts.push_back(_("No checks")); break;
    case check_state::success:
        // From the first to start to the last to finish.
        if (first_start && last_end > first_start)
            parts.push_back(
                rap::i18n::format(pipe ? _("Pipeline passed in {0}") : _("Checks passed in {0}"),
                                  duration_text(last_end - first_start, false)));
        else
            parts.push_back(pipe ? _("Pipeline passed") : _("Checks passed"));
        break;
    case check_state::failure:
        if (failed.empty())
            parts.push_back(pipe ? _("Pipeline failed") : _("Checks failed"));
        else if (pipe)
            parts.push_back(rap::i18n::format(N_("Pipeline failed: {0} of {1} job: {2}",
                                                 "Pipeline failed: {0} of {1} jobs: {2}", counted),
                                              count(failed.size()), count(counted),
                                              run_names(failed)));
        else
            parts.push_back(rap::i18n::format(
                N_("{0} of {1} check failed: {2}", "{0} of {1} checks failed: {2}", counted),
                count(failed.size()), count(counted), run_names(failed)));
        if (!running.empty())
            parts.push_back(
                rap::i18n::format(N_("{0} still running", "{0} still running", running.size()),
                                  count(running.size())));
        break;
    case check_state::pending:
    {
        std::string text =
            running.empty() ? std::string(pipe ? _("Pipeline running") : _("Checks running"))
            : pipe
                ? rap::i18n::format(N_("Pipeline running: {0} job left",
                                       "Pipeline running: {0} jobs left", running.size()),
                                    count(running.size()))
                : rap::i18n::format(N_("{0} check running", "{0} checks running", running.size()),
                                    count(running.size()));
        if (running_since && now > running_since)
            text += rap::i18n::format(_(", for {0}"), duration_text(now - running_since, true));
        if (!running.empty()) text += ": " + run_names(running);
        parts.push_back(std::move(text));
        if (!failed.empty())
            parts.push_back(
                rap::i18n::format(N_("{0} failed: {1}", "{0} failed: {1}", failed.size()),
                                  count(failed.size()), run_names(failed)));
        break;
    }
    }

    // What stands in the way of a merge; that nothing does only once the
    // checks are through, or when there are none.
    if (pr.blocks.empty())
    {
        if (pr.checks == check_state::success || pr.checks == check_state::none)
            parts.push_back(_("Can be merged"));
    }
    else
        for (merge_block b : pr.blocks) parts.push_back(block_text(b, pr));

    std::string out;
    for (const std::string& p : parts) out += (out.empty() ? "" : " · ") + p;
    return out;
}

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
        if (boxes)
            boxes->push_back(
                {.left = x, .right = x + w, .ref = *ref, .pr_left = mark ? x + 4 + t.w + 2.5f : 0});
        x += w + 1;
    }
    return order.empty() ? start : x;
}

}  // namespace tortilla
