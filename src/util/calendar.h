// Calendar time without the shared buffer of std::gmtime/std::localtime:
// gmtime_r/localtime_r on POSIX, the _s variants (arguments swapped) on
// Windows.
#pragma once

#include <cstdint>
#include <ctime>
#include <string>

namespace tortilla
{

inline std::tm utc_calendar(std::time_t t)
{
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    return tm;
}

inline std::tm local_calendar(std::time_t t)
{
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    return tm;
}

// A commit's time as the log and the rebase list show it, in its own zone:
// "2024-05-01 13:45".
inline std::string commit_date_text(int64_t time, int offset_minutes)
{
    std::tm tm = utc_calendar(std::time_t(time + int64_t(offset_minutes) * 60));
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &tm);
    return buf;
}

}  // namespace tortilla
