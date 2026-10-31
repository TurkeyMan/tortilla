#include "repo_icon.h"

#include "git/repo.h"

#include <rapport/sys.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
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

// -- which colour: by what's open now ------------------------------------------
//
// Colours tell apart the repositories on screen at once, so they're handed
// out among Tortilla's running processes, not kept: the first repository
// open gets the icon's own colours (slot 0), each other one open meanwhile
// the lowest free palette slot, and a slot frees when its windows close.
//
// Each process holding a slot keeps a file locked for its life, named
// <slot>-<repository hash>-<pid>, in Tortilla's cache folder: the system
// drops the lock when the process ends, however it ends, so a file nobody
// holds is left over and goes. The name says all; no one reads another's
// file. A short lock on "registry" keeps two starting processes from taking
// the same slot.

rap::sys::mapped_file g_slot;  // ours, held until exit

std::string hash_hex(const std::string& s)
{
    uint64_t h = 1469598103934665603ull;  // FNV-1a
    for (unsigned char c : s) h = (h ^ c) * 1099511628211ull;
    char out[17];
    std::snprintf(out, sizeof out, "%016llx", static_cast<unsigned long long>(h));
    return out;
}

// The slot for the repository keyed `key`; 0 is plain.
size_t take_slot(const std::string& key)
{
    namespace fs = std::filesystem;
    const fs::path dir = rap::sys::cache_dir("tortilla") / "colors";
    std::error_code ec;
    fs::create_directories(dir, ec);
    auto registry =
        rap::sys::mapped_file::open(dir / "registry", rap::sys::mapped_file::mode::read_write);
    if (!registry) return 0;
    for (int tries = 0; !registry->try_lock(); ++tries)
    {
        if (tries == 50) return 0;  // someone's stuck: plain rather than wait
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    const std::string mine = hash_hex(key);
    std::vector<size_t> users(1 + std::size(k_palette));
    std::optional<size_t> same;
    for (const fs::directory_entry& e : fs::directory_iterator(dir, ec))
    {
        const std::string name = rap::sys::path_to_utf8(e.path().filename());
        size_t slot;
        char hash[17];
        if (std::sscanf(name.c_str(), "%zu-%16[0-9a-f]-", &slot, hash) != 2 || slot >= users.size())
            continue;
        // Held: its process is running. Not: left over.
        auto f = rap::sys::mapped_file::open(e.path(), rap::sys::mapped_file::mode::read);
        if (f && f->try_lock())
        {
            f->unlock();
            *f = {};
            fs::remove(e.path(), ec);
            continue;
        }
        ++users[slot];
        if (hash == mine) same = slot;
    }
    size_t slot = same ? *same : 0;
    if (!same)
    {
        // The lowest free; with every colour taken, the least used.
        auto it = std::ranges::find(users, size_t(0));
        slot =
            it != users.end()
                ? size_t(it - users.begin())
                : size_t(std::ranges::min_element(users.begin() + 1, users.end()) - users.begin());
    }
    const std::string name =
        std::to_string(slot) + "-" + mine + "-" + std::to_string(current_pid());
    if (auto f = rap::sys::mapped_file::open(dir / name, rap::sys::mapped_file::mode::read_write);
        f && f->try_lock())
        g_slot = std::move(*f);
    registry->unlock();
    return slot;
}

}  // namespace

void use_repo_icon(rap::ui& ui, const std::string& path)
{
    auto r = repo::open(path);
    if (!r) return;
    const std::string dir = r->bare() ? r->git_dir() : r->workdir();
    const size_t slot = take_slot(dir);
    if (slot == 0) return;  // the program's own icon
    const rap::color c = k_palette[slot - 1];

    // Smallest first, as k_icon_layers lists them.
    std::vector<std::pair<int, std::shared_ptr<rap::image>>> images;
    for (const icon_layers& l : k_icon_layers)
        if (auto image = tinted(ui, l, c)) images.emplace_back(l.size, std::move(image));
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
