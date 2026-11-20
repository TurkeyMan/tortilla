// The settings file escapes keys only. Values keep their existing byte format.
// A literal '%' is always escaped, so decoding exactly once is unambiguous for
// new files. Legacy literal %XX keys are intentionally interpreted as escapes.
#pragma once

#include <string>
#include <string_view>

namespace tortilla
{

inline std::string encode_settings_key(std::string_view key)
{
    constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : key)
        if (c == '%' || c == '=' || c == '\r' || c == '\n')
        {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
        else out += char(c);
    return out;
}

inline std::string decode_settings_key(std::string_view key)
{
    auto hex = [](unsigned char c) -> int
    {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    std::string out;
    for (size_t i = 0; i < key.size(); ++i)
    {
        if (key[i] == '%' && i + 2 < key.size())
        {
            const int high = hex(key[i + 1]), low = hex(key[i + 2]);
            if (high >= 0 && low >= 0)
            {
                out += char(high * 16 + low);
                i += 2;
                continue;
            }
        }
        out += key[i];
    }
    return out;
}

}  // namespace tortilla
