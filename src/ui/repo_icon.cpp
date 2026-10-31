#include "repo_icon.h"

#include "git/repo.h"

#include <rapport/sys.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <random>
#include <string_view>
#include <thread>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

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

unsigned long current_pid()
{
#ifdef _WIN32
    return static_cast<unsigned long>(_getpid());
#else
    return static_cast<unsigned long>(getpid());
#endif
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
// The icon as it is.
std::unique_ptr<rap::image> plain(rap::ui& ui, const icon_layers& l)
{
    auto icon = rap::sys::decode_image(l.icon);
    if (!icon || icon->width != l.size || icon->height != l.size) return nullptr;
    return ui.create_image(l.size, l.size, icon->format, icon->pixels.data(), size_t(l.size) * 4);
}

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

// The icon drawn from `colored`'s images (or the plain ones), at the size
// nearest above what's asked; empty if none decode.
rap::icon icon_of(rap::ui& ui, std::optional<rap::color> colored)
{
    // Smallest first, as k_icon_layers lists them.
    std::vector<std::pair<int, std::shared_ptr<rap::image>>> images;
    for (const icon_layers& l : k_icon_layers)
        if (auto image = colored ? tinted(ui, l, *colored) : plain(ui, l))
            images.emplace_back(l.size, std::move(image));
    if (images.empty()) return {};
    return rap::icon::drawn(
        [images](rap::painter& p, rap::size_f size)
        {
            // The smallest at least as big as the device pixels asked for.
            const float want = std::max(size.w, size.h) * p.device_scale();
            auto it = std::ranges::find_if(images, [&](const auto& e) { return e.first >= want; });
            const auto& [n, image] = it != images.end() ? *it : images.back();
            p.image(*image, {0, 0, float(n), float(n)}, {0, 0, size.w, size.h});
        });
}

rap::icon g_icon;

// -- which colour: by what's open now ------------------------------------------
//
// Colours tell apart the repositories on screen at once, so they're handed
// out among Tortilla's running processes, not kept: the first repository
// open gets the icon's own colours, each other one open meanwhile a colour
// none of the others has (the palette's first, then made up to stand apart
// from those in use), and it's free again when its windows close.
//
// Each process with a colour keeps a file locked for its life, named
// <rrggbb or "plain">-<repository hash>-<pid>, in Tortilla's cache folder:
// the system drops the lock when the process ends, however it ends, so a
// file nobody holds is left over and goes. The name says all; no one reads
// another's file. A short lock on "registry" keeps two starting processes
// from taking the same colour.

rap::sys::mapped_file g_held;  // ours, held until exit

std::string hash_hex(const std::string& s)
{
    uint64_t h = 1469598103934665603ull;  // FNV-1a
    for (unsigned char c : s) h = (h ^ c) * 1099511628211ull;
    char out[17];
    std::snprintf(out, sizeof out, "%016llx", static_cast<unsigned long long>(h));
    return out;
}

std::string hex(rap::color c)
{
    char out[7];
    std::snprintf(out, sizeof out, "%02x%02x%02x", c.r, c.g, c.b);
    return out;
}

// How different two colours look: RGB weighted by the mean red ("redmean").
float distance(rap::color a, rap::color b)
{
    const float r = (a.r + b.r) / 2.f, dr = float(a.r) - b.r, dg = float(a.g) - b.g,
                db = float(a.b) - b.b;
    return std::sqrt((2 + r / 256) * dr * dr + 4 * dg * dg + (2 + (255 - r) / 256) * db * db);
}

rap::color from_hsl(float h, float s, float l)
{
    const float c = (1 - std::abs(2 * l - 1)) * s, m = l - c / 2;
    auto channel = [&](float n)
    {
        const float k = std::fmod(n + h / 30, 12.f);
        return uint8_t(std::lround(
            255 * (m + c * (0.5f - 0.5f * std::clamp(std::min(k - 3, 9 - k), -1.f, 1.f)))));
    };
    return {channel(0), channel(8), channel(4)};
}

// A colour for a repository newly open beside those `in_use`: the first of
// the palette none has, else the most apart of a few made up, apart from
// the shell's own tan too: any hue, saturation 50-100%, lightness 25-90%
// (dark and near white; darker flattens the shell's spots).
rap::color new_color(const std::vector<rap::color>& in_use)
{
    auto apart = [&](rap::color c)
    {
        float d = distance(c, {253, 212, 100});  // the shell's own
        for (rap::color u : in_use) d = std::min(d, distance(c, u));
        return d;
    };
    for (rap::color c : k_palette)
        if (std::ranges::none_of(in_use, [&](rap::color u) { return distance(c, u) < 1; }))
            return c;
    std::mt19937 random{std::random_device{}()};
    std::uniform_real_distribution<float> hue(0, 360), sat(0.5f, 1), light(0.25f, 0.9f);
    rap::color best{};
    float best_apart = -1;
    for (int i = 0; i < 48; ++i)
        if (const rap::color c = from_hsl(hue(random), sat(random), light(random));
            apart(c) > best_apart)
        {
            best = c;
            best_apart = apart(c);
        }
    return best;
}

// The colour for the repository keyed `key`; nullopt: the icon's own.
std::optional<rap::color> take_color(const std::string& key)
{
    namespace fs = std::filesystem;
    const fs::path dir = rap::sys::cache_dir("tortilla") / "colors";
    std::error_code ec;
    fs::create_directories(dir, ec);
    auto registry =
        rap::sys::mapped_file::open(dir / "registry", rap::sys::mapped_file::mode::read_write);
    if (!registry) return std::nullopt;
    for (int tries = 0; !registry->try_lock(); ++tries)
    {
        if (tries == 50) return std::nullopt;  // someone's stuck: plain rather than wait
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    const std::string mine = hash_hex(key);
    std::vector<rap::color> in_use;
    bool plain_used = false;
    std::optional<std::string> same;  // the colour's part of the name
    for (const fs::directory_entry& e : fs::directory_iterator(dir, ec))
    {
        const std::string name = rap::sys::path_to_utf8(e.path().filename());
        char what[8], hash[17];
        if (std::sscanf(name.c_str(), "%7[0-9a-z]-%16[0-9a-f]-", what, hash) != 2) continue;
        unsigned r, g, b;
        const bool plain = std::string_view(what) == "plain";
        if (!plain && std::sscanf(what, "%2x%2x%2x", &r, &g, &b) != 3) continue;
        // Held: its process is running. Not: left over.
        auto f = rap::sys::mapped_file::open(e.path(), rap::sys::mapped_file::mode::read);
        if (f && f->try_lock())
        {
            f->unlock();
            *f = {};
            fs::remove(e.path(), ec);
            continue;
        }
        if (plain)
            plain_used = true;
        else
            in_use.push_back({uint8_t(r), uint8_t(g), uint8_t(b)});
        if (hash == mine) same = what;
    }

    std::optional<rap::color> color;
    std::string what = "plain";
    if (same)
    {
        what = *same;
        unsigned r, g, b;
        if (what != "plain" && std::sscanf(what.c_str(), "%2x%2x%2x", &r, &g, &b) == 3)
            color = rap::color{uint8_t(r), uint8_t(g), uint8_t(b)};
    }
    else if (plain_used)
    {
        color = new_color(in_use);
        what = hex(*color);
    }
    const std::string name = what + "-" + mine + "-" + std::to_string(current_pid());
    if (auto f = rap::sys::mapped_file::open(dir / name, rap::sys::mapped_file::mode::read_write);
        f && f->try_lock())
        g_held = std::move(*f);
    registry->unlock();
    return color;
}

}  // namespace

void use_repo_icon(rap::ui& ui, const std::string& path, bool outer)
{
    // As the log finds it: a path gone from the working tree is looked
    // for from the nearest folder still there; `outer` from the folder above.
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path at = fs::absolute(rap::sys::path_from_utf8(path), ec);
    if (outer) at = (at.has_filename() ? at : at.parent_path()).parent_path();
    while (!fs::exists(at, ec) && at.has_parent_path() && at.parent_path() != at)
        at = at.parent_path();
    auto r = repo::open(rap::sys::path_to_utf8(at));
    if (!r) return;
    const std::string dir = r->bare() ? r->git_dir() : r->workdir();
    g_icon = icon_of(ui, take_color(dir));
}

std::unique_ptr<rap::window> create_window(rap::ui& ui, const rap::window_spec& spec,
                                           rap::window* parent)
{
    auto w = ui.create(spec, parent);
    if (!g_icon) g_icon = icon_of(ui, std::nullopt);
    if (w && g_icon) w->set_icon(g_icon);
    return w;
}

}  // namespace tortilla
