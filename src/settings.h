// tortilla's user settings: a key=value file in the platform config directory
// (~/.config/tortilla/settings on Linux), read once and written on change.
//
// Any thread may read and write them (workers read the forge accounts and
// sync's choices while the UI changes them): each call is whole, behind a
// lock, and hands back a copy.
//
// Several Tortilla processes share the file (each file-manager command
// starts one), and the shell extension reads it for every menu. So a change
// is made to the file as it is now, not to this process's copy of it: under
// a lock held across processes the file is read again, the change applied,
// and the result written beside it and renamed over it, so a reader sees
// the old document or the new one, whole. This process's copy becomes what
// was written; when the write fails the file keeps its last good contents,
// and the copy what the file holds.
//
// A save that can't take the lock (its file can't be opened, or another
// writer holds it past the wait) saves nothing and says why. Where the
// filesystem has no locks at all, writers go ahead without one: the file is
// still always whole, but a change made at the same moment by another
// process may be lost.
#pragma once

#include <rapport/rapport.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>

namespace tortilla
{

// Changes made together and saved as one update (a window's geometry and
// panes on close, a dialog's choices on OK), applied in the order made.
class settings_changes
{
public:
    void set(const std::string& key, const std::string& value);
    // Back to the fallback.
    void remove(const std::string& key);
    void set_numbers(const std::string& key, const std::vector<float>&);
    void add_to_history(const std::string& key, const std::string& entry, size_t keep = 25);
    // "<prefix>.size" and, where the platform has one, "<prefix>.position".
    void save_window(const std::string& prefix, const rap::window&);

    bool empty() const { return changes_.empty(); }

private:
    friend class settings;
    std::vector<std::function<void(std::map<std::string, std::string>&)>> changes_;
};

class settings
{
public:
    static settings& get();

    std::string value(const std::string& key, const std::string& fallback = {}) const;
    void set(const std::string& key, const std::string& value);
    // Back to the fallback.
    void remove(const std::string& key);

    // Lists of numbers (sizes, positions, column widths), stored "a,b,c".
    std::vector<float> numbers(const std::string& key) const;
    void set_numbers(const std::string& key, const std::vector<float>&);

    // A history of what was typed (URLs cloned, pulled from, added as
    // submodules), newest first, stored tab-separated. Adding moves an entry
    // already there to the front and keeps the newest `keep`.
    std::vector<std::string> history(const std::string& key) const;
    void add_to_history(const std::string& key, const std::string& entry, size_t keep = 25);

    // A window's size and position as "<prefix>.size" / "<prefix>.position",
    // saved together. Restore before the window is first shown. Position
    // only where the platform lets apps place windows (not on Wayland).
    void save_window(const std::string& prefix, const rap::window&);
    void restore_window(const std::string& prefix, rap::window&) const;

    // Whether a save was made, and if not why (for the user: the file and
    // what went wrong).
    struct saved
    {
        std::string error;
        explicit operator bool() const { return error.empty(); }
    };

    // Saves `changes` as one update. Fails when the lock can't be taken in
    // time or the file can't be read or replaced: then nothing is written,
    // the file is as it was, and so is this process's copy of it (as the
    // file has it, when it could be read).
    saved apply(const settings_changes& changes);

    // For the tests: the settings read from, and written to, `file` instead
    // of the user's.
    void use_file(const std::filesystem::path& file);
    // For the tests: how long a save waits for another writer (in this
    // process or another) before it fails. 5 s.
    void set_lock_wait(std::chrono::milliseconds wait) { lock_wait_ = wait; }

private:
    settings();

    // values_ and file_, held only to read or swap them, so a reader never
    // waits on the disk or another process.
    mutable std::shared_mutex mutex_;
    // One update at a time from this process; the lock across processes
    // may be per process, not per thread. Waited for as long as that one.
    std::timed_mutex write_mutex_;
    std::atomic<std::chrono::milliseconds> lock_wait_{std::chrono::milliseconds(5000)};
    std::filesystem::path file_;
    std::map<std::string, std::string> values_;
};

// Appearance [Settings > General]: "light", "dark", or as the desktop has
// it (no value). Applied when the toolkit loads, and live from Settings.
inline constexpr const char* k_appearance_setting = "appearance";
rap::color_scheme appearance();
rap::color_scheme appearance_from(std::string_view stored);
std::string_view appearance_value(rap::color_scheme);

// The history format on its own, for the tests: a stored value's entries,
// and the value with `entry` added.
std::vector<std::string> history_entries(std::string_view stored);
std::string history_with(std::string_view stored, const std::string& entry, size_t keep);

// For the tests: the names an update to `file` may be written to before it
// is renamed over it, tried in order (per process). Each is created anew;
// one that is already there, whatever it is, is passed over and left alone.
std::vector<std::filesystem::path> settings_temp_files(const std::filesystem::path& file);
// For the tests: the lock writers to `file` hold, and the file it guards
// (`file`, or where the link it is leads).
std::filesystem::path settings_lock_file(const std::filesystem::path& file);

// For the tests: holds the writers' lock on `file`, as another process's
// save would, until destroyed. Check it with held().
class settings_lock_holder
{
public:
    explicit settings_lock_holder(const std::filesystem::path& file);
    ~settings_lock_holder();
    settings_lock_holder(const settings_lock_holder&) = delete;
    settings_lock_holder& operator=(const settings_lock_holder&) = delete;
    bool held() const { return held_; }

private:
    std::intptr_t handle_ = -1;  // fd, or HANDLE
    bool held_ = false;
};

}  // namespace tortilla
