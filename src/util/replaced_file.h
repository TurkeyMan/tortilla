// Opening a file that other processes replace by renaming a new copy over
// it (the settings). On Windows an open that meets the rename can fail for
// a moment though the file is there; the open is tried again, shortly and a
// bounded number of times, so a reader doesn't take a file being replaced
// for a missing one. Header-only: the shell extension uses it too.
#pragma once

#include <chrono>
#include <filesystem>
#include <system_error>
#include <thread>

namespace tortilla
{

// What `open()` returns (a FILE*, a std::ifstream: anything that tests
// false when the open failed), tried again while it fails and `file` is
// there, `attempts` times in all. Failed as the last try left it otherwise.
template <class Open>
auto open_replaced_file(const std::filesystem::path& file, Open open, int attempts,
                        std::chrono::milliseconds pause)
{
    for (int attempt = 1;; ++attempt)
    {
        auto opened = open();
        if (opened || attempt >= attempts) return opened;
        std::error_code ec;
        // Not there: not being replaced, but missing.
        if (!std::filesystem::exists(file, ec) && !ec) return opened;
        std::this_thread::sleep_for(pause);
    }
}

}  // namespace tortilla
