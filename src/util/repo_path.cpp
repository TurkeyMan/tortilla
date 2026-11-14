#include "repo_path.h"

#include <rapport/sys.h>

#include <algorithm>
#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

namespace tortilla
{
namespace
{

// `rel` as git names it: '/'-separated, converted through UTF-8, never the
// code page (.string() on Windows); std::nullopt if it climbs out.
std::optional<std::string> git_form(const fs::path& rel)
{
    if (rel.empty() || *rel.begin() == "..") return std::nullopt;
    std::string out = rap::sys::path_to_utf8(rel);
#ifdef _WIN32
    // Elsewhere \ is a character a name can hold.
    std::ranges::replace(out, '\\', '/');
#endif
    while (out.ends_with('/')) out.pop_back();
    if (out == ".") out.clear();
    return out;
}

}  // namespace

std::optional<std::string> repo_relative(std::string_view workdir, std::string_view path)
{
    std::error_code ec;
    fs::path abs = fs::absolute(rap::sys::path_from_utf8(path), ec).lexically_normal();
    if (ec) return std::nullopt;
    if (!abs.has_filename()) abs = abs.parent_path();  // "sub/" is sub
    const fs::path root = fs::weakly_canonical(rap::sys::path_from_utf8(workdir), ec);
    if (ec) return std::nullopt;
    // The entry selected is the entry itself: a link (to a file or a folder,
    // a junction, inside or out, or to nothing) is the link git tracks, never
    // its target. Only the folders it's in are resolved, so /tmp/x and its
    // target agree.
    if (abs.has_relative_path())
    {
        const fs::path parent = fs::weakly_canonical(abs.parent_path(), ec);
        const fs::path rel = parent.lexically_relative(root);
        if (!ec && !rel.empty() && *rel.begin() != "..")
            return git_form(rel == "." ? abs.filename() : rel / abs.filename());
    }
    // The top itself, by any name (a link to it outside it too).
    if (fs::weakly_canonical(abs, ec) == root && !ec) return std::string();
    return std::nullopt;
}

std::string folder_text(std::string path)
{
    const size_t root = rap::sys::path_to_utf8(rap::sys::path_from_utf8(path).root_path()).size();
    while (path.size() > std::max<size_t>(root, 1) && (path.back() == '/' || path.back() == '\\'))
        path.pop_back();
    return path;
}

}  // namespace tortilla
