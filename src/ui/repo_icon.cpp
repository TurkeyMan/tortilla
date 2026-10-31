#include "repo_icon.h"

#include "git/command.h"
#include "git/remotes.h"
#include "git/repo.h"
#include "settings.h"

#include <rapport/sys.h>

#include <algorithm>
#include <cstdio>

namespace tortilla
{

namespace
{

// Distinct on a taskbar, light and dark, and none the shell's own tan.
constexpr rap::color k_palette[] = {
    {240, 110, 170},  // pink
    {80, 200, 210},   // teal
    {170, 120, 240},  // purple
    {250, 140, 60},   // orange
    {120, 200, 110},  // green
    {90, 140, 240},   // blue
    {235, 80, 80},    // red
    {150, 160, 175},  // slate
};

std::optional<rap::color> parse(const std::string& s)
{
    unsigned r, g, b;
    if (s.size() == 7 && std::sscanf(s.c_str(), "#%2x%2x%2x", &r, &g, &b) == 3)
        return rap::color{uint8_t(r), uint8_t(g), uint8_t(b)};
    return std::nullopt;
}

// Premultiplied RGBA, 0-255, whichever way the platform decoded it.
std::vector<float> premultiplied(const rap::sys::decoded_image& d)
{
    std::vector<float> out(d.pixels.size());
    for (size_t i = 0; i + 3 < d.pixels.size(); i += 4)
    {
        const float a = d.pixels[i + 3];
        if (d.format == rap::pixel_format::rgba8)
            for (int c = 0; c < 3; ++c) out[i + c] = d.pixels[i + c] * a / 255;
        else  // bgra8_premultiplied
            for (int c = 0; c < 3; ++c) out[i + c] = d.pixels[i + 2 - c];
        out[i + 3] = a;
    }
    return out;
}

// One size: the body plus the shell's shading times the colour. The shell's
// usual tone is 200 (tools/make_shell_layer.py), so a colour lands as given
// there, lighter on its highlights and darker on its spots.
std::unique_ptr<rap::image> tinted(rap::ui& ui, const icon_layers& l, rap::color c)
{
    auto body = rap::sys::decode_image(l.body), shell = rap::sys::decode_image(l.shell);
    if (!body || !shell || body->width != l.size || shell->width != l.size ||
        body->height != l.size || shell->height != l.size)
        return nullptr;
    const std::vector<float> b = premultiplied(*body), s = premultiplied(*shell);
    const float tint[3] = {c.r / 200.f, c.g / 200.f, c.b / 200.f};
    std::vector<uint8_t> out(b.size());
    for (size_t i = 0; i + 3 < b.size(); i += 4)
    {
        const float a = std::min(b[i + 3] + s[i + 3], 255.f);
        for (int ch = 0; ch < 3; ++ch)
            out[i + 2 - ch] = uint8_t(std::min(b[i + ch] + s[i + ch] * tint[ch], a) + 0.5f);
        out[i + 3] = uint8_t(a + 0.5f);
    }
    return ui.create_image(l.size, l.size, rap::pixel_format::bgra8_premultiplied, out.data(),
                           size_t(l.size) * 4);
}

rap::icon g_icon;

}  // namespace

std::optional<rap::color> repo_color(const std::string& workdir)
{
    const std::string set = config_value(workdir, "tortilla.color");
    if (set == "plain") return std::nullopt;
    if (auto c = parse(set)) return c;

    // The next colour, counted across repositories in Tortilla's settings:
    // the first is plain, then round the palette.
    settings& st = settings::get();
    const std::vector<float> next = st.numbers("repoColor.next");
    const size_t n = next.empty() ? 0 : size_t(std::max(next.front(), 0.f));
    st.set_numbers("repoColor.next", {float(n + 1)});
    std::optional<rap::color> c;
    std::string value = "plain";
    if (n > 0)
    {
        c = k_palette[(n - 1) % std::size(k_palette)];
        char hex[8];
        std::snprintf(hex, sizeof hex, "#%02x%02x%02x", c->r, c->g, c->b);
        value = hex;
    }
    run_git(workdir, {"config", "tortilla.color", value});
    return c;
}

void use_repo_icon(rap::ui& ui, const std::string& path)
{
    auto r = repo::open(path);
    if (!r) return;
    const std::string dir = r->bare() ? r->git_dir() : r->workdir();
    const std::optional<rap::color> c = repo_color(dir);
    if (!c) return;  // the program's own icon

    // Smallest first, as k_icon_layers lists them.
    std::vector<std::pair<int, std::shared_ptr<rap::image>>> images;
    for (const icon_layers& l : k_icon_layers)
        if (auto image = tinted(ui, l, *c)) images.emplace_back(l.size, std::move(image));
    if (images.empty()) return;
    g_icon = rap::icon::drawn(
        [images](rap::painter& p, rap::size_f size)
        {
            // The smallest at least as big as the device pixels asked for.
            const float want = std::max(size.w, size.h) * p.device_scale();
            auto it = std::ranges::find_if(images, [&](const auto& e) { return e.first >= want; });
            const auto& [n, image] = it != images.end() ? *it : images.back();
            p.image(*image, {0, 0, float(n), float(n)}, {0, 0, size.w, size.h});
        });
}

std::unique_ptr<rap::window> create_window(rap::ui& ui, const rap::window_spec& spec,
                                           rap::window* parent)
{
    auto w = ui.create(spec, parent);
    if (w && g_icon) w->set_icon(g_icon);
    return w;
}

}  // namespace tortilla
