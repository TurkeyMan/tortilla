// Small text helpers shared across the dialogs and git wrappers.
#pragma once

#include <cctype>
#include <string>
#include <string_view>

namespace tortilla
{

// `s` without the white space (ASCII: spaces, tabs, newlines) around it, as
// a view of it.
inline std::string_view trimmed_view(std::string_view s)
{
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    return s;
}

// The same, as a string of its own: for an entry's text, or git's output.
inline std::string trimmed(std::string_view s)
{
    return std::string(trimmed_view(s));
}

// `s` with A-Z as a-z: for matching names and searches without case. Bytes
// beyond ASCII are kept, so offsets stay the same.
inline std::string ascii_lower(std::string_view s)
{
    std::string out(s);
    for (char& c : out)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return out;
}

// Every `from` in `s` replaced with `to`, left to right; what's put in isn't
// looked at again.
inline void replace_all(std::string& s, std::string_view from, std::string_view to)
{
    if (from.empty()) return;
    for (size_t at = s.find(from); at != std::string::npos; at = s.find(from, at + to.size()))
        s.replace(at, from.size(), to);
}

}  // namespace tortilla
