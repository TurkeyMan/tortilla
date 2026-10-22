#include "settings.h"

#include <rapport/sys.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace tortilla
{
namespace
{

std::filesystem::path settingsFile()
{
    return rap::sys::configDir("tortilla") / "settings";
}

}  // namespace

Settings& Settings::get()
{
    static Settings s;
    return s;
}

Settings::Settings()
{
    std::FILE* f = rap::sys::fopen(settingsFile(), "r");
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

std::string Settings::value(const std::string& key, const std::string& fallback) const
{
    auto it = values_.find(key);
    return it == values_.end() ? fallback : it->second;
}

void Settings::set(const std::string& key, const std::string& value)
{
    if (values_[key] == value) return;
    values_[key] = value;
    save();
}

void Settings::save() const
{
    std::FILE* f = rap::sys::fopen(settingsFile(), "w");
    if (!f) return;
    for (auto& [k, v] : values_) std::fprintf(f, "%s=%s\n", k.c_str(), v.c_str());
    std::fclose(f);
}

std::vector<float> Settings::numbers(const std::string& key) const
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

void Settings::setNumbers(const std::string& key, const std::vector<float>& v)
{
    std::string s;
    for (float f : v) s += (s.empty() ? "" : ",") + std::to_string(int(std::lround(f)));
    set(key, s);
}

void Settings::saveWindow(const std::string& prefix, const rap::Window& w)
{
    const rap::SizeF size = w.size();
    setNumbers(prefix + ".size", {size.w, size.h});
    if (auto pos = w.position()) setNumbers(prefix + ".position", {pos->x, pos->y});
}

void Settings::restoreWindow(const std::string& prefix, rap::Window& w) const
{
    if (auto v = numbers(prefix + ".size"); v.size() == 2 && v[0] > 0 && v[1] > 0)
        w.resize({v[0], v[1]});
    if (auto v = numbers(prefix + ".position"); v.size() == 2) w.move({v[0], v[1]});
}

}  // namespace tortilla
