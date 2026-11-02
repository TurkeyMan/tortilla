#include "temp_files.h"

#include <rapport/sys.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <system_error>

namespace fs = std::filesystem;

namespace tortilla
{

fs::path tortilla_temp_dir()
{
    std::error_code ec;
    const fs::path temp = fs::temp_directory_path(ec);
    if (ec || temp.empty()) return {};
    const fs::path dir = temp / "tortilla";
    fs::create_directories(dir, ec);
    return dir;
}

fs::path unique_temp_path(std::string_view prefix, std::string_view extension)
{
    const fs::path dir = tortilla_temp_dir();
    if (dir.empty()) return {};
    static std::atomic<unsigned> serial = 0;
    const auto stamp = std::chrono::system_clock::now().time_since_epoch().count();
    std::string name(prefix);
    name += "-" + std::to_string(stamp) + "-" + std::to_string(++serial);
    name += extension;
    return dir / rap::sys::path_from_utf8(name);
}

fs::path revision_temp_path(std::string_view rev, std::string_view path)
{
    // A hash is shortened as everywhere else; any other name (HEAD,
    // origin/main, main~2) is kept, with what a folder name can't hold as _.
    const bool hash =
        rev.size() >= 8 && rev.find_first_not_of("0123456789abcdef") == std::string_view::npos;
    std::string folder(hash ? rev.substr(0, 8) : rev);
    for (char& c : folder)
        if (std::string_view("/\\:*?\"<>|").find(c) != std::string_view::npos ||
            static_cast<unsigned char>(c) < 0x20)
            c = '_';
    if (folder.empty() || folder == "." || folder == "..") folder = "_";
    fs::path out = tortilla_temp_dir() / rap::sys::path_from_utf8(folder);
    // Each of the path's parts in turn: git's paths use /, and none is "..".
    for (size_t at = 0; at <= path.size();)
    {
        size_t end = path.find('/', at);
        if (end == std::string_view::npos) end = path.size();
        const std::string_view part = path.substr(at, end - at);
        if (!part.empty() && part != "." && part != "..") out /= rap::sys::path_from_utf8(part);
        at = end + 1;
    }
    return out;
}

bool write_file(const fs::path& path, std::string_view content)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::FILE* f = rap::sys::fopen(path, "wb");
    bool ok = f && std::fwrite(content.data(), 1, content.size(), f) == content.size();
    if (f) ok = std::fclose(f) == 0 && ok;
    return ok;
}

}  // namespace tortilla
