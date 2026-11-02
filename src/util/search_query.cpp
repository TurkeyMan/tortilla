#include "search_query.h"

#include <algorithm>

namespace tortilla
{

search_query parse_search_query(std::string_view s)
{
    search_query out;
    size_t i = 0;
    while (i < s.size() && s[i] == ' ') ++i;
    if (i < s.size() && s[i] == '!')
    {
        out.invert = true;
        ++i;
    }
    while (i < s.size())
    {
        while (i < s.size() && s[i] == ' ') ++i;
        if (i >= s.size()) break;
        bool exclude = false;
        if (s[i] == '-' || s[i] == '+')
        {
            exclude = s[i] == '-';
            ++i;
        }
        std::string term;
        if (i < s.size() && s[i] == '"')
        {
            // To the closing quote, or the end; "" inside is one quote.
            for (++i; i < s.size(); ++i)
            {
                if (s[i] != '"')
                {
                    term += s[i];
                    continue;
                }
                if (i + 1 < s.size() && s[i + 1] == '"')
                {
                    term += '"';
                    ++i;
                    continue;
                }
                ++i;
                break;
            }
        }
        else
        {
            const size_t end = std::min(s.find(' ', i), s.size());
            term = s.substr(i, end - i);
            i = end;
        }
        if (!term.empty()) out.terms.push_back({std::move(term), exclude});
    }
    return out;
}

}  // namespace tortilla
