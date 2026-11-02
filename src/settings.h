// tortilla's user settings: a key=value file in the platform config directory
// (~/.config/tortilla/settings on Linux), read once and written on change.
#pragma once

#include <rapport/rapport.h>

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace tortilla
{

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

    // A window's size and position as "<prefix>.size" / "<prefix>.position".
    // Restore before the window is first shown. Position only where the
    // platform lets apps place windows (not on Wayland).
    void save_window(const std::string& prefix, const rap::window&);
    void restore_window(const std::string& prefix, rap::window&) const;

private:
    settings();
    void save() const;

    std::map<std::string, std::string> values_;
};

// The history format on its own, for the tests: a stored value's entries,
// and the value with `entry` added.
std::vector<std::string> history_entries(std::string_view stored);
std::string history_with(std::string_view stored, const std::string& entry, size_t keep);

}  // namespace tortilla
