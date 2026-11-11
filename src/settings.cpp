#include "settings.h"

#include "i18n.h"
#include "util/replaced_file.h"

#include <rapport/sys.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <system_error>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <fcntl.h>
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
using steady = std::chrono::steady_clock;

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

std::string shown(const fs::path& p)
{
    return rap::sys::path_to_utf8(p);
}

// The last system error, for the user.
std::string system_error()
{
#ifdef _WIN32
    return std::system_category().message(int(GetLastError()));
#else
    return std::generic_category().message(errno);
#endif
}

// Where the settings are really kept: `file`, or where the link it is
// leads (a settings file kept with someone's dotfiles stays a link). The
// lock, the read and the replace all use this one path, so two links to
// one file share one lock. A link to a file not there yet is kept, and the
// file made where it leads, if that folder is there.
struct settings_target
{
    fs::path path;
    bool linked = false;
};

std::optional<settings_target> resolve(const fs::path& file, std::string& error)
{
    settings_target t{file};
    for (int hops = 0;; ++hops)
    {
        std::error_code ec;
        const fs::file_status st = fs::symlink_status(t.path, ec);
        if (st.type() != fs::file_type::symlink) return t;
        const fs::path to = fs::read_symlink(t.path, ec);
        if (ec || hops == 40)
        {
            error = rap::i18n::format(_("{0} is a link that doesn't lead to a file."), shown(file));
            return std::nullopt;
        }
        t.path = (to.is_absolute() ? to : t.path.parent_path() / to).lexically_normal();
        t.linked = true;
    }
}

// The lock writers hold across processes while they read, change and
// replace the file: a file of its own beside it, since the settings file
// is replaced, never written in place. A writer waits for it until its
// deadline (another writer holds it only to read, write and rename one
// small file), then fails: a writer stopped while it holds the lock (in a
// debugger, say) costs a save, not a hang. Released on close, so also when
// a process dies holding it. A lock file that can't be opened, or is
// anything but a file, fails the save. Only where the filesystem has no
// locks (some network shares) do writers go ahead unlocked.
struct lock_taken
{
    std::intptr_t handle = -1;  // fd, or HANDLE; -1 when not held
    std::string error;          // why not, when it failed
};

lock_taken take_lock(const fs::path& lock_file, steady::time_point deadline)
{
    auto busy = [&]
    {
        return lock_taken{
            .error = rap::i18n::format(
                _("Another Tortilla is saving its settings and hasn't finished ({0} is held)."),
                shown(lock_file))};
    };
    auto failed = [&]
    {
        return lock_taken{.error = rap::i18n::format(_("Couldn't lock the settings ({0}): {1}"),
                                                     shown(lock_file), system_error())};
    };
    std::chrono::milliseconds pause(1);
#ifdef _WIN32
    HANDLE h =
        CreateFileW(lock_file.c_str(), GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) return failed();
    if (GetFileType(h) != FILE_TYPE_DISK)
    {
        CloseHandle(h);
        SetLastError(ERROR_INVALID_FUNCTION);
        return failed();
    }
    for (;;)
    {
        OVERLAPPED at{};
        if (LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &at))
            return {.handle = reinterpret_cast<std::intptr_t>(h)};
        const DWORD e = GetLastError();
        if (e == ERROR_NOT_SUPPORTED || e == ERROR_INVALID_FUNCTION)
        {
            CloseHandle(h);  // no locks here: unlocked
            return {};
        }
        if (e != ERROR_LOCK_VIOLATION && e != ERROR_IO_PENDING)
        {
            lock_taken out = failed();
            CloseHandle(h);
            return out;
        }
        if (steady::now() >= deadline)
        {
            CloseHandle(h);
            return busy();
        }
        std::this_thread::sleep_for(pause);
        pause = std::min(pause * 2, std::chrono::milliseconds(10));
    }
#else
    // O_NOFOLLOW: a link planted in the lock's place isn't followed.
    const int fd = ::open(lock_file.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return failed();
    for (;;)
    {
        if (::flock(fd, LOCK_EX | LOCK_NB) == 0) return {.handle = fd};
        const int e = errno;
        if (e == EINTR) continue;
        if (e == ENOLCK || e == EOPNOTSUPP || e == ENOSYS || e == EINVAL)
        {
            ::close(fd);  // no locks here: unlocked
            return {};
        }
        if (e != EWOULDBLOCK)
        {
            lock_taken out = failed();
            ::close(fd);
            return out;
        }
        if (steady::now() >= deadline)
        {
            ::close(fd);
            return busy();
        }
        std::this_thread::sleep_for(pause);
        pause = std::min(pause * 2, std::chrono::milliseconds(10));
    }
#endif
}

void release_lock(std::intptr_t handle)
{
    if (handle == -1) return;
#ifdef _WIN32
    CloseHandle(reinterpret_cast<HANDLE>(handle));
#else
    ::close(int(handle));
#endif
}

class writer_lock
{
public:
    writer_lock(const fs::path& lock_file, steady::time_point deadline)
        : taken_(take_lock(lock_file, deadline))
    {
    }
    ~writer_lock() { release_lock(taken_.handle); }
    writer_lock(const writer_lock&) = delete;
    writer_lock& operator=(const writer_lock&) = delete;
    const std::string& error() const { return taken_.error; }

private:
    lock_taken taken_;
};

void parse(std::string_view text, values& out)
{
    // Whole lines, however long: every save writes every key back.
    for (size_t at = 0; at < text.size();)
    {
        size_t end = text.find('\n', at);
        if (end == std::string_view::npos) end = text.size();
        std::string_view s = text.substr(at, end - at);
        at = end + 1;
        while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) s.remove_suffix(1);
        const size_t eq = s.find('=');
        if (eq == std::string_view::npos || s.starts_with('#')) continue;
        out[std::string(s.substr(0, eq))] = std::string(s.substr(eq + 1));
    }
}

// The file as it is now: empty if there is none; nullopt if it is there but
// can't be read, so an update doesn't replace what it couldn't see.
std::optional<values> read_file(const fs::path& file)
{
    std::FILE* f = open_replaced_file(
        file, [&] { return rap::sys::fopen(file, "rb"); }, 20, std::chrono::milliseconds(10));
    if (!f)
    {
        std::error_code ec;
        if (!fs::exists(file, ec) && !ec) return values{};
        return std::nullopt;
    }
    std::string text;
    char buf[8192];
    while (size_t n = std::fread(buf, 1, sizeof buf, f)) text.append(buf, n);
    const bool ok = !std::ferror(f);
    std::fclose(f);
    if (!ok) return std::nullopt;
    values out;
    parse(text, out);
    return out;
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

// A temporary file beside `target`, made anew (never one already there,
// whatever it is: a leftover, a link, a folder), open for writing, private
// until it takes the mode of the file it replaces.
std::FILE* create_temp(const fs::path& target, fs::path& made, std::string& error)
{
    for (const fs::path& name : settings_temp_files(target))
    {
#ifdef _WIN32
        HANDLE h = CreateFileW(name.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
        {
            const DWORD e = GetLastError();
            std::error_code ec;
            if (e == ERROR_FILE_EXISTS || e == ERROR_ALREADY_EXISTS ||
                fs::symlink_status(name, ec).type() != fs::file_type::not_found)
                continue;  // someone else's
            error = system_error();
            return nullptr;
        }
        const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(h), _O_WRONLY | _O_BINARY);
        if (fd < 0)
        {
            error = system_error();
            CloseHandle(h);
            DeleteFileW(name.c_str());
            return nullptr;
        }
        std::FILE* f = _fdopen(fd, "wb");
        if (!f)
        {
            error = system_error();
            _close(fd);
            DeleteFileW(name.c_str());
            return nullptr;
        }
#else
        const int fd =
            ::open(name.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd < 0)
        {
            if (errno == EEXIST) continue;  // someone else's
            error = system_error();
            return nullptr;
        }
        // Kept as private as the file it replaces.
        if (struct stat st;
            ::stat(target.c_str(), &st) == 0 && ::fchmod(fd, st.st_mode & 07777) != 0)
        {
            error = system_error();
            ::close(fd);
            ::unlink(name.c_str());
            return nullptr;
        }
        std::FILE* f = ::fdopen(fd, "wb");
        if (!f)
        {
            error = system_error();
            ::close(fd);
            ::unlink(name.c_str());
            return nullptr;
        }
#endif
        made = name;
        return f;
    }
    error = _("every name for it is taken");
    return nullptr;
}

// Writes `v` to a file beside `target` and renames it over `target`. On any
// failure `target` is untouched, the temporary file is gone, and the error
// is said.
std::string write_file(const fs::path& target, const values& v)
{
    fs::path temp;
    std::string why;
    std::FILE* f = create_temp(target, temp, why);
    if (!f)
        return rap::i18n::format(_("Couldn't make a file beside {0} to save the settings to: {1}"),
                                 shown(target), why);
    bool ok = true;
    for (auto& [k, val] : v)
        ok = ok && std::fwrite(k.data(), 1, k.size(), f) == k.size() && std::fputc('=', f) != EOF &&
             std::fwrite(val.data(), 1, val.size(), f) == val.size() && std::fputc('\n', f) != EOF;
    ok = ok && std::fflush(f) == 0 && !std::ferror(f);
    // On disk before the rename, so a crash leaves the old file or the new
    // one, not an empty one.
#ifdef _WIN32
    ok = ok && _commit(_fileno(f)) == 0;
#else
    ok = ok && ::fsync(::fileno(f)) == 0;
#endif
    if (!ok) why = system_error();
    ok = std::fclose(f) == 0 && ok;
    if (ok && !replace(temp, target))
    {
        why = system_error();
        ok = false;
    }
    if (!ok)
    {
        std::error_code ec;
        fs::remove(temp, ec);  // ours: made above
        return rap::i18n::format(_("Couldn't save the settings to {0}: {1}"), shown(target), why);
    }
    return {};
}

std::string numbers_value(const std::vector<float>& v)
{
    std::string s;
    for (float f : v) s += (s.empty() ? "" : ",") + std::to_string(int(std::lround(f)));
    return s;
}

}  // namespace

std::vector<fs::path> settings_temp_files(const fs::path& file)
{
#ifdef _WIN32
    const unsigned long pid = GetCurrentProcessId();
#else
    const unsigned long pid = static_cast<unsigned long>(::getpid());
#endif
    std::vector<fs::path> out;
    for (int n = 0; n < 16; ++n)
        out.push_back(
            with_suffix(file, "." + std::to_string(pid) + "." + std::to_string(n) + ".tmp"));
    return out;
}

fs::path settings_lock_file(const fs::path& file)
{
    std::string error;
    return with_suffix(resolve(file, error).value_or(settings_target{file}).path, ".lock");
}

settings_lock_holder::settings_lock_holder(const fs::path& file)
{
    lock_taken t = take_lock(settings_lock_file(file), steady::now());
    handle_ = t.handle;
    held_ = t.handle != -1;
}

settings_lock_holder::~settings_lock_holder()
{
    release_lock(handle_);
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

settings::saved settings::apply(const settings_changes& changes)
{
    if (changes.empty()) return {};
    // One wait for both locks: this process's other writers and others'.
    const steady::time_point deadline = steady::now() + lock_wait_.load();
    std::unique_lock write(write_mutex_, deadline);
    fs::path file;
    {
        std::shared_lock lock(mutex_);
        file = file_;
    }
    if (!write.owns_lock())
        return {rap::i18n::format(
            _("Another save of the settings in this Tortilla hasn't finished ({0})."),
            shown(file))};
    std::string error;
    const std::optional<settings_target> target = resolve(file, error);
    if (!target) return {error};
    std::error_code ec;
    const fs::path folder = target->path.parent_path();
    if (!folder.empty() && !fs::is_directory(folder, ec))
    {
        // The config folder on a first save; never a folder a link leads to.
        if (target->linked)
            return {rap::i18n::format(_("{0} is a link to {1}, in a folder that isn't there."),
                                      shown(file), shown(target->path))};
        fs::create_directories(folder, ec);
    }
    writer_lock across_processes(with_suffix(target->path, ".lock"), deadline);
    if (!across_processes.error().empty()) return {across_processes.error()};
    std::optional<values> now = read_file(target->path);
    if (!now)
        return {rap::i18n::format(_("Couldn't read the settings in {0}: {1}"), shown(target->path),
                                  system_error())};
    values next = *now;
    for (auto& change : changes.changes_) change(next);
    if (next != *now) error = write_file(target->path, next);
    std::unique_lock lock(mutex_);
    values_ = error.empty() ? std::move(next) : std::move(*now);
    return {error};
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
