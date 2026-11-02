// A path as git prints it where it has no NUL-separated (-z) output: as it
// is, or in double quotes with C escapes when it has unusual characters
// (core.quotepath: bytes over 0x7f as octal, and ", \, tabs, newlines).
#pragma once

#include <string>
#include <string_view>

namespace tortilla
{

inline std::string unquote_path(std::string_view s)
{
    if (s.size() < 2 || s.front() != '"' || s.back() != '"') return std::string(s);
    s = s.substr(1, s.size() - 2);
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] != '\\' || i + 1 == s.size())
        {
            out += s[i];
            continue;
        }
        const char c = s[++i];
        if (c >= '0' && c <= '7' && i + 2 < s.size())
        {
            out += char((c - '0') * 64 + (s[i + 1] - '0') * 8 + (s[i + 2] - '0'));
            i += 2;
        }
        else
            out += c == 'n'   ? '\n'
                   : c == 't' ? '\t'
                   : c == 'r' ? '\r'
                   : c == 'a' ? '\a'
                   : c == 'b' ? '\b'
                   : c == 'f' ? '\f'
                   : c == 'v' ? '\v'
                              : c;
    }
    return out;
}

}  // namespace tortilla
