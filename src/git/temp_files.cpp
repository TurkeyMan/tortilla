#include "temp_files.h"

#include "util/entry_path.h"

#include <rapport/sys.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <iterator>
#include <system_error>

namespace fs = std::filesystem;

namespace tortilla
{
namespace
{

// A folder that is one, not a link (or a junction) to one elsewhere.
bool real_folder(const fs::path& p)
{
    std::error_code ec;
    const fs::file_status s = fs::symlink_status(p, ec);
    return !ec && s.type() == fs::file_type::directory;
}

}  // namespace

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

fs::path unique_temp_dir(std::string_view prefix)
{
    for (int tries = 0; tries < 16; ++tries)
    {
        const fs::path dir = unique_temp_path(prefix, "");
        if (dir.empty()) return {};
        std::error_code ec;
        // Made here, now: one already there (another process's) isn't ours.
        if (fs::create_directory(dir, ec) && real_folder(dir)) return dir;
    }
    return {};
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
    // The path as a name under the revision's folder, or none: a git tree
    // can hold names this system reads as a drive, a root, a device or a
    // stream (C:\x, NUL), and none is trimmed into another file's name.
    const std::optional<fs::path> rel = entry_path(path);
    const fs::path root = tortilla_temp_dir();
    if (!rel || root.empty()) return {};
    return root / rap::sys::path_from_utf8(folder) / *rel;
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

bool write_temp_file(const fs::path& path, std::string_view content)
{
    const fs::path root = tortilla_temp_dir();
    if (root.empty() || !real_folder(root)) return false;
    // Under the temporary folder, by its names alone...
    const fs::path rel = path.lexically_normal().lexically_relative(root.lexically_normal());
    if (rel.empty() || rel.has_root_path()) return false;
    for (const fs::path& part : rel)
        if (part.empty() || part == "." || part == "..") return false;
    // ...and on the disk: each folder on the way made here or a real one,
    // never a link someone left there to send the file elsewhere.
    fs::path at = root;
    std::error_code ec;
    for (auto it = rel.begin(); it != rel.end(); ++it)
    {
        if (std::next(it) == rel.end()) break;  // the file itself
        at /= *it;
        if (!fs::exists(fs::symlink_status(at, ec))) fs::create_directory(at, ec);
        if (!real_folder(at)) return false;
    }
    const fs::path file = root / rel;
    const fs::file_status s = fs::symlink_status(file, ec);
    if (fs::is_symlink(s))
        fs::remove(file, ec);  // the link, never what it points at
    else if (fs::exists(s) && !fs::is_regular_file(s))
        return false;
    // With every link on the way ruled out, it can only be where it says.
    const fs::path parent = fs::weakly_canonical(file.parent_path(), ec);
    const fs::path inside = parent.lexically_relative(fs::weakly_canonical(root, ec));
    if (ec || inside.empty() || *inside.begin() == "..") return false;
    std::FILE* f = rap::sys::fopen(file, "wb");
    bool ok = f && std::fwrite(content.data(), 1, content.size(), f) == content.size();
    if (f) ok = std::fclose(f) == 0 && ok;
    return ok;
}

}  // namespace tortilla
