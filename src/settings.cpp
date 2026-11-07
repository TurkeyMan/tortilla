#include "settings.h"

#include <rapport/sys.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <io.h>
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace tortilla
{
namespace
{

namespace fs = std::filesystem;
using values = std::map<std::string, std::string>;

fs::path settings_file()
{
    return rap::sys::config_dir("tortilla") / "settings";
}

fs::path with_suffix(const fs::path& file, const std::string& suffix)
{
    fs::path out = file;
    out += rap::sys::path_from_utf8(suffix);
    return out;
}

// The lock writers hold across processes while they read, change and
// replace the file: a file of its own beside it, since the settings file
// is replaced, never written in place. Taking it waits, with no timeout,
// for the writer holding it: that one holds it only to read, write and
// rename one small file. Released on close, so also when a process dies
// holding it. Where the filesystem can't lock (some network shares), or the
// lock file can't be opened, writers go ahead unlocked: the file is still
// always whole, but a change made at the same moment by another process
// may be lost.
class writer_lock
{
public:
    explicit writer_lock(const fs::path& file)
    {
        const fs::path path = with_suffix(file, ".lock");
#ifdef _WIN32
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                               OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;
        OVERLAPPED at{};
        // Blocks until the other writer is done: it holds the lock only to
        // read and replace one small file.
        if (!LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK, 0, 1, 0, &at))
        {
            CloseHandle(h);
            return;
        }
        handle_ = h;
#else
        const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (fd < 0) return;
        int r;
        while ((r = ::flock(fd, LOCK_EX)) != 0 && errno == EINTR)
        {
        }
        if (r != 0)
        {
            ::close(fd);
            return;
        }
        fd_ = fd;
#endif
    }
    ~writer_lock()
    {
#ifdef _WIN32
        if (handle_) CloseHandle(handle_);
#else
        if (fd_ >= 0) ::close(fd_);
#endif
    }
    writer_lock(const writer_lock&) = delete;
    writer_lock& operator=(const writer_lock&) = delete;

private:
#ifdef _WIN32
    HANDLE handle_ = nullptr;
#else
    int fd_ = -1;
#endif
};

void parse(std::FILE* f, values& out)
{
    char line[4096];
    while (std::fgets(line, sizeof line, f))
    {
        std::string s = line;
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        auto eq = s.find('=');
        if (eq == std::string::npos || s.starts_with('#')) continue;
        out[s.substr(0, eq)] = s.substr(eq + 1);
    }
}

// The file as it is now: empty if there is none; nullopt if it is there but
// can't be read, so an update doesn't replace what it couldn't see.
std::optional<values> read_file(const fs::path& file)
{
    for (int attempt = 0;; ++attempt)
    {
        if (std::FILE* f = rap::sys::fopen(file, "rb"))
        {
            values out;
            parse(f, out);
            const bool ok = !std::ferror(f);
            std::fclose(f);
            if (ok) return out;
        }
        else
        {
            std::error_code ec;
            if (!fs::exists(file, ec) && !ec) return values{};
        }
        // On Windows, an open can meet a rename by a writer that isn't
        // holding the lock (one where locks don't work): try again shortly.
        if (attempt == 20) return std::nullopt;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

#ifdef _WIN32
bool sharing_error(DWORD e)
{
    return e == ERROR_SHARING_VIOLATION || e == ERROR_LOCK_VIOLATION || e == ERROR_ACCESS_DENIED;
}
#endif

// Renames `from` over `to`, in one step.
bool replace(const fs::path& from, const fs::path& to)
{
#ifdef _WIN32
    // A reader that opened the file without FILE_SHARE_DELETE (the C
    // runtime and std::ifstream, as the shell extension uses) holds off the
    // rename until it closes it, which is a moment: wait a little for it.
    for (int attempt = 0;; ++attempt)
    {
        if (MoveFileExW(from.c_str(), to.c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            return true;
        if (attempt == 200 || !sharing_error(GetLastError())) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
#else
    return std::rename(from.c_str(), to.c_str()) == 0;
#endif
}

// Writes `v` to a file beside `file` and renames it over `file`. On any
// failure `file` is untouched and the temporary file is gone.
bool write_file(const fs::path& file, const values& v)
{
    std::error_code ec;
    // A settings file that links elsewhere (kept with someone's dotfiles)
    // stays a link: the file it names is what's replaced.
    fs::path target = file;
    if (fs::is_symlink(file, ec))
        if (fs::path real = fs::weakly_canonical(file, ec); !ec) target = real;
    if (target.has_parent_path()) fs::create_directories(target.parent_path(), ec);

    const fs::path temp = settings_temp_file(target);
    std::FILE* f = rap::sys::fopen(temp, "wb");
    if (!f) return false;
#ifndef _WIN32
    // Kept as private as the file it replaces.
    if (struct stat st; ::stat(target.c_str(), &st) == 0) ::fchmod(::fileno(f), st.st_mode & 07777);
#endif
    for (auto& [k, val] : v) std::fprintf(f, "%s=%s\n", k.c_str(), val.c_str());
    bool ok = std::fflush(f) == 0 && !std::ferror(f);
    // On disk before the rename, so a crash leaves the old file or the new
    // one, not an empty one.
#ifdef _WIN32
    ok = ok && _commit(_fileno(f)) == 0;
#else
    ok = ok && ::fsync(::fileno(f)) == 0;
#endif
    ok = std::fclose(f) == 0 && ok;
    if (!ok || !replace(temp, target))
    {
        fs::remove(temp, ec);
        return false;
    }
    return true;
}

std::string numbers_value(const std::vector<float>& v)
{
    std::string s;
    for (float f : v) s += (s.empty() ? "" : ",") + std::to_string(int(std::lround(f)));
    return s;
}

}  // namespace

fs::path settings_temp_file(const fs::path& file)
{
#ifdef _WIN32
    const unsigned long pid = GetCurrentProcessId();
#else
    const unsigned long pid = static_cast<unsigned long>(::getpid());
#endif
    return with_suffix(file, "." + std::to_string(pid) + ".tmp");
}

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

void settings_changes::set(const std::string& key, const std::string& value)
{
    changes_.push_back([key, value](values& v) { v[key] = value; });
}

void settings_changes::remove(const std::string& key)
{
    changes_.push_back([key](values& v) { v.erase(key); });
}

void settings_changes::set_numbers(const std::string& key, const std::vector<float>& v)
{
    set(key, numbers_value(v));
}

void settings_changes::add_to_history(const std::string& key, const std::string& entry, size_t keep)
{
    // Added to the history as the file has it, so entries another process
    // added meanwhile stay.
    changes_.push_back(
        [key, entry, keep](values& v)
        {
            auto it = v.find(key);
            v[key] = history_with(it == v.end() ? std::string_view{} : it->second, entry, keep);
        });
}

void settings_changes::save_window(const std::string& prefix, const rap::window& w)
{
    const rap::size_f size = w.size();
    set_numbers(prefix + ".size", {size.w, size.h});
    if (auto pos = w.position()) set_numbers(prefix + ".position", {pos->x, pos->y});
}

settings& settings::get()
{
    static settings s;
    return s;
}

settings::settings()
    : file_(settings_file())
{
    values_ = read_file(file_).value_or(values{});
}

void settings::use_file(const fs::path& file)
{
    std::scoped_lock write(write_mutex_);
    values v = read_file(file).value_or(values{});
    std::unique_lock lock(mutex_);
    file_ = file;
    values_ = std::move(v);
}

bool settings::apply(const settings_changes& changes)
{
    if (changes.empty()) return true;
    std::scoped_lock write(write_mutex_);
    fs::path file;
    {
        std::shared_lock lock(mutex_);
        file = file_;
    }
    writer_lock across_processes(file);
    std::optional<values> now = read_file(file);
    if (!now) return false;
    values next = *now;
    for (auto& change : changes.changes_) change(next);
    const bool ok = next == *now || write_file(file, next);
    std::unique_lock lock(mutex_);
    values_ = ok ? std::move(next) : std::move(*now);
    return ok;
}

std::string settings::value(const std::string& key, const std::string& fallback) const
{
    std::shared_lock lock(mutex_);
    auto it = values_.find(key);
    return it == values_.end() ? fallback : it->second;
}

void settings::set(const std::string& key, const std::string& value)
{
    settings_changes c;
    c.set(key, value);
    apply(c);
}

void settings::remove(const std::string& key)
{
    settings_changes c;
    c.remove(key);
    apply(c);
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
    settings_changes c;
    c.add_to_history(key, entry, keep);
    apply(c);
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
    settings_changes c;
    c.set_numbers(key, v);
    apply(c);
}

void settings::save_window(const std::string& prefix, const rap::window& w)
{
    settings_changes c;
    c.save_window(prefix, w);
    apply(c);
}

void settings::restore_window(const std::string& prefix, rap::window& w) const
{
    if (auto v = numbers(prefix + ".size"); v.size() == 2 && v[0] > 0 && v[1] > 0)
        w.resize({v[0], v[1]});
    if (auto v = numbers(prefix + ".position"); v.size() == 2) w.move({v[0], v[1]});
}

}  // namespace tortilla
