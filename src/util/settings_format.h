// The settings file escapes keys only. Values keep their existing byte format.
// A literal '%' is always escaped. Only a version-marked file decodes keys;
// unmarked legacy keys remain literal and are encoded losslessly on next save.
#pragma once

#include <string>
#include <string_view>

namespace tortilla
{

inline constexpr std::string_view k_settings_format_marker = "#tortilla-settings 2";

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

// One instance per file. The first physical line alone selects the format.
// getline has removed LF; one terminal CR is accepted for a CRLF first line.
// No other whitespace/BOM normalization and no later-marker activation.
class settings_key_reader
{
public:
    bool read_header(std::string_view line)
    {
        if (!first_) return false;
        first_ = false;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        return version_two_ = line == k_settings_format_marker;
    }
    bool version_two() const { return version_two_; }
    std::string key(std::string_view stored) const
    { return version_two_ ? decode_settings_key(stored) : std::string(stored); }
private:
    bool first_ = true, version_two_ = false;
};

}  // namespace tortilla
