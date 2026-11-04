#include "settings.h"

#include <rapport/sys.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace tortilla
{
namespace
{

std::filesystem::path settings_file()
{
    return rap::sys::config_dir("tortilla") / "settings";
}

}  // namespace

rap::color_scheme appearance_from(std::string_view stored)
{
    return stored == "light"  ? rap::color_scheme::light
           : stored == "dark" ? rap::color_scheme::dark
                              : rap::color_scheme::system;
}

std::string_view appearance_value(rap::color_scheme s)
{
    return s == rap::color_scheme::light ? "light" : s == rap::color_scheme::dark ? "dark" : "";
}

rap::color_scheme appearance()
{
    return appearance_from(settings::get().value(k_appearance_setting));
}

settings& settings::get()
{
    static settings s;
    return s;
}

settings::settings()
    : file_(settings_file())
{
    load();
}

void settings::use_file(const std::filesystem::path& file)
{
    std::unique_lock lock(mutex_);
    file_ = file;
    values_.clear();
    load();
}

void settings::load()
{
    std::FILE* f = rap::sys::fopen(file_, "r");
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
    std::shared_lock lock(mutex_);
    return value_locked(key, fallback);
}

std::string settings::value_locked(const std::string& key, const std::string& fallback) const
{
    auto it = values_.find(key);
    return it == values_.end() ? fallback : it->second;
}

void settings::set(const std::string& key, const std::string& value)
{
    std::unique_lock lock(mutex_);
    set_locked(key, value);
}

void settings::set_locked(const std::string& key, const std::string& value)
{
    if (values_[key] == value) return;
    values_[key] = value;
    save();
}

void settings::remove(const std::string& key)
{
    std::unique_lock lock(mutex_);
    if (values_.erase(key)) save();
}

std::vector<std::string> history_entries(std::string_view stored)
{
    std::vector<std::string> out;
    for (size_t at = 0; at < stored.size();)
    {
        size_t end = stored.find('\t', at);
        if (end == std::string_view::npos) end = stored.size();
        if (end > at) out.emplace_back(stored.substr(at, end - at));
        at = end + 1;
    }
    return out;
}

std::string history_with(std::string_view stored, const std::string& entry, size_t keep)
{
    std::vector<std::string> entries = history_entries(stored);
    std::erase(entries, entry);
    entries.insert(entries.begin(), entry);
    if (entries.size() > keep) entries.resize(keep);
    std::string out;
    for (const std::string& e : entries) out += (out.empty() ? "" : "\t") + e;
    return out;
}

std::vector<std::string> settings::history(const std::string& key) const
{
    return history_entries(value(key));
}

void settings::add_to_history(const std::string& key, const std::string& entry, size_t keep)
{
    std::unique_lock lock(mutex_);  // read and written as one
    set_locked(key, history_with(value_locked(key, {}), entry, keep));
}

void settings::save() const
{
    std::FILE* f = rap::sys::fopen(file_, "w");
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
