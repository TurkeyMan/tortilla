#include "repo_path.h"

#include <rapport/sys.h>

#include <algorithm>
#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

namespace tortilla
{

std::optional<std::string> repo_relative(std::string_view workdir, std::string_view path)
{
    std::error_code ec;
    const fs::path abs = fs::absolute(rap::sys::path_from_utf8(path), ec);
    if (ec) return std::nullopt;
    // fs::relative resolves links on both sides first, so /tmp/x and its
    // target agree; the result is converted through UTF-8, never the code
    // page (.string() on Windows).
    const fs::path rel = fs::relative(abs, rap::sys::path_from_utf8(workdir), ec);
    if (ec || rel.empty()) return std::nullopt;
    if (*rel.begin() == "..") return std::nullopt;
    std::string out = rap::sys::path_to_utf8(rel);
#ifdef _WIN32
    // Elsewhere \ is a character a name can hold.
    std::ranges::replace(out, '\\', '/');
#endif
    while (out.ends_with('/')) out.pop_back();
    if (out == ".") out.clear();
    return out;
}

}  // namespace tortilla
