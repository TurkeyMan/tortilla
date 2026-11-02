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

}  // namespace tortilla
