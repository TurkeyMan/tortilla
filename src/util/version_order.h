// Version order, as git's --sort=version:refname has it (git's
// versioncmp, which is glibc's strverscmp without versionsort.suffix):
// "v1.9" before "v1.10", "1.01" (a fraction) before "1.1".
#pragma once

#include <cctype>
#include <string_view>

namespace tortilla
{

inline int version_compare(std::string_view a, std::string_view b)
{
    enum : int
    {
        s_n = 0,  // normal
        s_i = 3,  // in an integer
        s_f = 6,  // in a fraction (after a leading zero)
        s_z = 9,  // in leading zeros
        cmp = 2,
        len = 3,
    };
    static constexpr int next_state[] = {
        /*        x    d    0 */
        /* s_n */ s_n, s_i, s_z,
        /* s_i */ s_n, s_i, s_i,
        /* s_f */ s_n, s_f, s_f,
        /* s_z */ s_n, s_f, s_z,
    };
    static constexpr int result_type[] = {
        /*        x/x  x/d  x/0  d/x  d/d  d/0  0/x  0/d  0/0 */
        /* s_n */ cmp, cmp, cmp, cmp, len, cmp, cmp, cmp, cmp,
        /* s_i */ cmp, -1,  -1,  +1,  len, len, +1,  len, len,
        /* s_f */ cmp, cmp, cmp, cmp, cmp, cmp, cmp, cmp, cmp,
        /* s_z */ cmp, +1,  +1,  -1,  cmp, cmp, -1,  cmp, cmp,
    };
    // As C strings: past the end reads '\0'.
    size_t i = 0, j = 0;
    auto at = [](std::string_view s, size_t k) -> unsigned char
    { return k < s.size() ? static_cast<unsigned char>(s[k]) : 0; };
    auto kind = [](unsigned char c) { return (c == '0') + (std::isdigit(c) != 0); };
    unsigned char c1 = at(a, i++), c2 = at(b, j++);
    int state = s_n + kind(c1);
    int diff;
    while ((diff = int(c1) - int(c2)) == 0)
    {
        if (c1 == 0) return 0;
        state = next_state[state];
        c1 = at(a, i++);
        c2 = at(b, j++);
        state += kind(c1);
    }
    state = result_type[state * 3 + kind(c2)];
    switch (state)
    {
    case cmp: return diff;
    case len:
        while (std::isdigit(at(a, i++)))
            if (!std::isdigit(at(b, j++))) return 1;
        return std::isdigit(at(b, j)) ? -1 : diff;
    default: return state;
    }
}

inline bool version_less(std::string_view a, std::string_view b)
{
    const int c = version_compare(a, b);
    return c != 0 ? c < 0 : a < b;
}

}  // namespace tortilla
