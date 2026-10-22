// Calendar time without the shared buffer of std::gmtime/std::localtime:
// gmtime_r/localtime_r on POSIX, the _s variants (arguments swapped) on
// Windows.
#pragma once

#include <ctime>

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

}  // namespace tortilla
