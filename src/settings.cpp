#include "settings.h"

#include <rapport/sys.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace tortilla
{
namespace
{

std::filesystem::path settings_file()
{
    return rap::sys::config_dir("tortilla") / "settings";
}

}  // namespace

settings& settings::get()
{
    static settings s;
    return s;
}

settings::settings()
{
    std::FILE* f = rap::sys::fopen(settings_file(), "r");
    if (!f) return;
    char line[4096];
    while (std::fgets(line, sizeof line, f))
    {
        std::string s = line;
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        auto eq = s.find('=');
        if (eq == std::string::npos || s.starts_with('#')) continue;
        values_[s.substr(0, eq)] = s.substr(eq + 1);
    }
    std::fclose(f);
}

std::string settings::value(const std::string& key, const std::string& fallback) const
{
    auto it = values_.find(key);
    return it == values_.end() ? fallback : it->second;
}

void settings::set(const std::string& key, const std::string& value)
{
    if (values_[key] == value) return;
    values_[key] = value;
    save();
}

void settings::save() const
{
    std::FILE* f = rap::sys::fopen(settings_file(), "w");
    if (!f) return;
    for (auto& [k, v] : values_) std::fprintf(f, "%s=%s\n", k.c_str(), v.c_str());
    std::fclose(f);
}

std::vector<float> settings::numbers(const std::string& key) const
{
    const std::string s = value(key);
    std::vector<float> out;
    for (size_t at = 0; at < s.size();)
    {
        size_t comma = s.find(',', at);
        if (comma == std::string::npos) comma = s.size();
        out.push_back(std::strtof(s.substr(at, comma - at).c_str(), nullptr));
        at = comma + 1;
    }
    return out;
}

void settings::set_numbers(const std::string& key, const std::vector<float>& v)
{
    std::string s;
    for (float f : v) s += (s.empty() ? "" : ",") + std::to_string(int(std::lround(f)));
    set(key, s);
}

void settings::save_window(const std::string& prefix, const rap::window& w)
{
    const rap::size_f size = w.size();
    set_numbers(prefix + ".size", {size.w, size.h});
    if (auto pos = w.position()) set_numbers(prefix + ".position", {pos->x, pos->y});
}

void settings::restore_window(const std::string& prefix, rap::window& w) const
{
    if (auto v = numbers(prefix + ".size"); v.size() == 2 && v[0] > 0 && v[1] > 0)
        w.resize({v[0], v[1]});
    if (auto v = numbers(prefix + ".position"); v.size() == 2) w.move({v[0], v[1]});
}

}  // namespace tortilla
