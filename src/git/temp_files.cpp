#include "temp_files.h"

#include "util/entry_path.h"

#include <rapport/sys.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <iterator>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

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

// revision_temp_dir()'s folders, and the others sweep_temp_dirs() removes
// once stale (Review Patch's temp_dir).
constexpr std::string_view swept_prefixes[] = {revision_dir_prefix, "review-patch"};

// A name unique_temp_dir(prefix) gives: "<prefix>-<time>-<n>".
bool unique_name(std::string_view name, std::string_view prefix)
{
    return name.size() > prefix.size() + 1 && name.starts_with(prefix) &&
           name[prefix.size()] == '-' &&
           name.find_first_not_of("0123456789-", prefix.size()) == std::string_view::npos;
}

// A folder's lock: beside it, not in it, where a scratch tree's own file
// (a repository's ".lock") could be.
fs::path lock_path(const fs::path& folder)
{
    fs::path p = folder;
    p += ".lock";
    return p;
}

constexpr std::intptr_t no_lock = -1;

#ifdef _WIN32
// Open with no sharing: while the handle lives, no other open of it, in
// this process or another, succeeds, and it can't be deleted.
std::intptr_t open_lock(const fs::path& p, bool create)
{
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                           create ? CREATE_NEW : OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    return h == INVALID_HANDLE_VALUE ? no_lock : reinterpret_cast<std::intptr_t>(h);
}

void close_lock(std::intptr_t lock)
{
    CloseHandle(reinterpret_cast<HANDLE>(lock));
}
#else
// flock: an open file description's, so another open of the file, in this
// process too, can't take it while this one holds it.
std::intptr_t open_lock(const fs::path& p, bool create)
{
    const int fd =
        ::open(p.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW | (create ? O_CREAT | O_EXCL : 0), 0600);
    if (fd < 0) return no_lock;
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0)
    {
        ::close(fd);
        return no_lock;
    }
    return fd;
}

void close_lock(std::intptr_t lock)
{
    ::close(static_cast<int>(lock));
}
#endif

// Whether `folder` may be swept: no lock beside it (an Open's), or one no
// one holds (its owner gone, a crash).
bool unowned(const fs::path& folder)
{
    const fs::path lock = lock_path(folder);
    std::error_code ec;
    const fs::file_status s = fs::symlink_status(lock, ec);
    if (s.type() == fs::file_type::not_found) return true;
    if (!fs::is_regular_file(s)) return false;  // not ours to judge
    const std::intptr_t held = open_lock(lock, false);
    if (held == no_lock) return false;  // its owner's still
    close_lock(held);
    fs::remove(lock, ec);
    return true;
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

temp_dir::temp_dir(std::string_view prefix)
    : path_(unique_temp_dir(prefix))
{
    // Held from the start: the folder is new, so no sweep takes it before.
    if (!path_.empty()) lock_ = open_lock(lock_path(path_), true);
}

temp_dir::~temp_dir()
{
    if (path_.empty()) return;
    std::error_code ec;
    fs::remove_all(path_, ec);  // links in it go, not what they point at
    if (lock_ != no_lock) close_lock(lock_);
    fs::remove(lock_path(path_), ec);
}

fs::path revision_temp_path(const fs::path& dir, std::string_view rev, std::string_view path)
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
    if (!rel || dir.empty()) return {};
    return dir / rap::sys::path_from_utf8(folder) / *rel;
}

fs::path revision_temp_dir()
{
    return unique_temp_dir(revision_dir_prefix);
}

fs::path write_revision_file(std::string_view rev, std::string_view path, std::string_view content)
{
    const fs::path dir = revision_temp_dir();
    const fs::path file = revision_temp_path(dir, rev, path);
    if (!file.empty() && write_temp_file(file, content)) return file;
    std::error_code ec;
    if (!dir.empty()) fs::remove_all(dir, ec);
    return {};
}

void sweep_temp_dirs(std::chrono::hours age)
{
    const fs::path root = tortilla_temp_dir();
    if (root.empty() || !real_folder(root)) return;
    const auto before = fs::file_time_type::clock::now() - age;
    std::error_code ec;
    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
    {
        // Only the folders unique_temp_dir() named for these, nothing else.
        const std::string name = rap::sys::path_to_utf8(it->path().filename());
        // A lock whose folder is gone (its owner stopped between the two):
        // its own name's folder decides.
        const bool lock = name.ends_with(".lock");
        const std::string_view stem =
            lock ? std::string_view(name).substr(0, name.size() - 5) : std::string_view(name);
        bool ours = false;
        for (std::string_view prefix : swept_prefixes) ours = ours || unique_name(stem, prefix);
        if (!ours) continue;
        std::error_code e;
        if (lock)
        {
            const fs::path folder = it->path().parent_path() / rap::sys::path_from_utf8(stem);
            const auto written = fs::last_write_time(it->path(), e);
            if (!e && written < before && !fs::exists(fs::symlink_status(folder, e)))
                unowned(folder);  // removes the lock if no one holds it
            continue;
        }
        if (!real_folder(it->path())) continue;
        const auto written = fs::last_write_time(it->path(), e);
        // However old, one a temp_dir holds is in use (a diff tool left open
        // for days); the lock is gone with it once it's taken.
        if (!e && written < before && unowned(it->path()))
            fs::remove_all(it->path(), e);  // a link in it, not its target
    }
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
