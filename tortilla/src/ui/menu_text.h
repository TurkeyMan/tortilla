// Menu labels use '_' to mark access keys, so text from outside (branch
// names, commit subjects) has its underscores doubled before going in.
#pragma once

#include <string>

namespace tortilla
{

inline std::string menuText(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s)
    {
        out += c;
        if (c == '_') out += '_';
    }
    return out;
}

}  // namespace tortilla
