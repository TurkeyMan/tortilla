// Version 2 escapes record delimiters in keys and values, plus key equals signs.
// Literal percent bytes are escaped; decoding happens exactly once, only in
// marked files. Unmarked legacy keys and values migrate losslessly on save.
#pragma once

#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace tortilla
{

inline constexpr std::string_view k_settings_format_marker = "#tortilla-settings 2";

inline std::string encode_settings_field(std::string_view key, bool is_key)
{
    constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : key)
        if (c == '%' || (is_key && c == '=') || c == '\r' || c == '\n')
        {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
        else out += char(c);
    return out;
}

inline std::string encode_settings_key(std::string_view key)
{ return encode_settings_field(key, true); }
inline std::string encode_settings_value(std::string_view value)
{ return encode_settings_field(value, false); }

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

inline constexpr std::string_view k_settings_generation_prefix = "#tortilla-settings-generation ";

// Generation is metadata only in the second physical line of a v2 document.
// Legacy documents have generation zero; malformed claimed metadata is unsafe.
inline std::optional<uint64_t> settings_document_generation(std::string_view bytes)
{
    auto line = [&]() {
        const auto end = bytes.find('\n');
        auto out = bytes.substr(0, end);
        bytes = end == bytes.npos ? std::string_view{} : bytes.substr(end + 1);
        if (!out.empty() && out.back() == '\r') out.remove_suffix(1);
        return out;
    };
    if (line() != k_settings_format_marker) return 0;
    auto generation = line();
    if (!generation.starts_with(k_settings_generation_prefix)) return 0;
    generation.remove_prefix(k_settings_generation_prefix.size());
    uint64_t value = 0;
    const auto parsed = std::from_chars(generation.data(), generation.data() + generation.size(), value);
    if (generation.empty() || parsed.ec != std::errc{} || parsed.ptr != generation.data() + generation.size())
        return {};
    return value;
}

inline bool settings_document_has_generation(std::string_view bytes)
{
    const auto first_end = bytes.find('\n');
    auto first = bytes.substr(0, first_end);
    if (!first.empty() && first.back() == '\r') first.remove_suffix(1);
    if (first != k_settings_format_marker || first_end == bytes.npos) return false;
    bytes.remove_prefix(first_end + 1);
    auto second = bytes.substr(0, bytes.find('\n'));
    if (!second.empty() && second.back() == '\r') second.remove_suffix(1);
    return second.starts_with(k_settings_generation_prefix);
}
inline std::string settings_generation_line(uint64_t generation)
{ return std::string(k_settings_generation_prefix) + std::to_string(generation) + "\n"; }

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
    std::string value(std::string_view stored) const
    { return version_two_ ? decode_settings_key(stored) : std::string(stored); }
private:
    bool first_ = true, version_two_ = false;
};

}  // namespace tortilla
