#include "entry_path.h"

#include <rapport/sys.h>

#include <array>
#include <cctype>
#include <string>

namespace tortilla
{
namespace
{

bool ieq(std::string_view a, std::string_view b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}

// CON, NUL, COM1, "nul.txt", "Con .log": Windows opens the device, whatever
// the folder and the extension.
bool device_name(std::string_view part)
{
    std::string_view stem = part.substr(0, part.find('.'));
    while (!stem.empty() && stem.back() == ' ') stem.remove_suffix(1);
    static constexpr std::array<std::string_view, 6> fixed{"CON", "PRN",    "AUX",
                                                           "NUL", "CONIN$", "CONOUT$"};
    for (std::string_view d : fixed)
        if (ieq(stem, d)) return true;
    if (stem.size() == 4 && (ieq(stem.substr(0, 3), "COM") || ieq(stem.substr(0, 3), "LPT")))
        return stem[3] >= '0' && stem[3] <= '9';
    // COM¹, LPT²...: superscript digits, as UTF-8.
    if (stem.size() == 5 && (ieq(stem.substr(0, 3), "COM") || ieq(stem.substr(0, 3), "LPT")))
        return stem.substr(3) == "\xc2\xb9" || stem.substr(3) == "\xc2\xb2" ||
               stem.substr(3) == "\xc2\xb3";
    return false;
}

bool windows_part_ok(std::string_view part)
{
    for (char c : part)
        if (static_cast<unsigned char>(c) < 0x20 ||
            std::string_view("\\:*?\"<>|").find(c) != std::string_view::npos)
            return false;
    if (part.back() == '.' || part.back() == ' ') return false;
    return !device_name(part);
}

}  // namespace

std::optional<std::filesystem::path> entry_path(std::string_view name, path_rules rules)
{
    if (name.ends_with('/')) name.remove_suffix(1);  // a folder's entry
    if (name.empty() || name.front() == '/') return std::nullopt;
    std::filesystem::path out;
    for (size_t at = 0; at <= name.size();)
    {
        size_t end = name.find('/', at);
        if (end == std::string_view::npos) end = name.size();
        const std::string_view part = name.substr(at, end - at);
        if (part.empty() || part == "." || part == "..") return std::nullopt;
        if (rules == path_rules::windows && !windows_part_ok(part)) return std::nullopt;
        out /= rap::sys::path_from_utf8(part);
        at = end + 1;
    }
    // Belt and braces: what the system makes of it is still relative, and
    // still that many plain names.
    if (out.has_root_name() || out.has_root_directory()) return std::nullopt;
    for (const std::filesystem::path& p : out)
        if (p.empty() || p == "." || p == "..") return std::nullopt;
    return out;
}

}  // namespace tortilla
