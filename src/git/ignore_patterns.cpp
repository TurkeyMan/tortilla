#include "ignore_patterns.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <set>
#include <system_error>

namespace tortilla
{
namespace
{

// "*.txt" for "notes.txt"; "" for a folder or a name without a dot.
std::string extension_mask(const ignore_item& i)
{
    if (i.folder) return {};
    const size_t dot = i.name.rfind('.');
    if (dot == std::string::npos || dot == 0) return {};
    return "*" + literal_pattern(i.name.substr(dot));
}

}  // namespace

std::string literal_pattern(const std::string& name)
{
    std::string out;
    for (char c : name)
    {
        if (c == '*' || c == '?' || c == '[' || c == '\\') out += '\\';
        out += c;
    }
    if (!out.empty() && (out.front() == '#' || out.front() == '!')) out.insert(0, 1, '\\');
    // Trailing spaces count only escaped, each one.
    size_t end = out.size();
    while (end > 0 && out[end - 1] == ' ')
    {
        out.insert(end - 1, 1, '\\');
        end -= 1;
    }
    return out;
}

std::vector<std::pair<std::string, std::vector<std::string>>>
ignore_patterns(const std::vector<ignore_item>& items, bool by_extension, bool everywhere,
                ignore_target target)
{
    std::vector<std::pair<std::string, std::vector<std::string>>> out;
    for (const ignore_item& i : items)
    {
        std::string pattern = by_extension ? extension_mask(i) : literal_pattern(i.name);
        if (pattern.empty()) continue;
        if (!by_extension && i.folder) pattern += '/';
        if (!everywhere)
        {
            // Anchored to the ignore file's own folder.
            const std::string rel = target == ignore_target::folder ? std::string() : i.dir;
            pattern = "/" + (rel.empty() ? "" : rel + "/") + pattern;
        }
        const std::string file = target == ignore_target::exclude ? std::string()
                                 : target == ignore_target::folder && !i.dir.empty()
                                     ? i.dir + "/.gitignore"
                                     : std::string(".gitignore");
        auto it = std::ranges::find_if(out, [&](auto& p) { return p.first == file; });
        if (it == out.end()) it = out.insert(out.end(), {file, {}});
        if (std::ranges::find(it->second, pattern) == it->second.end()) it->second.push_back(pattern);
    }
    return out;
}

std::string append_ignore_lines(const std::filesystem::path& file, const std::vector<std::string>& lines)
{
    std::string existing;
    {
        std::ifstream in(file, std::ios::binary);
        existing.assign(std::istreambuf_iterator<char>(in), {});
    }
    std::set<std::string> present;
    for (size_t at = 0; at < existing.size();)
    {
        size_t end = existing.find('\n', at);
        if (end == std::string::npos) end = existing.size();
        std::string l = existing.substr(at, end - at);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        present.insert(l);
        at = end + 1;
    }
    std::string add;
    for (const std::string& l : lines)
        if (present.insert(l).second) add += l + '\n';
    if (add.empty()) return {};
    if (!existing.empty() && existing.back() != '\n') add.insert(0, 1, '\n');
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::binary | std::ios::app);
    out << add;
    return out ? std::string() : "couldn't write " + file.string();
}

}  // namespace tortilla
