// TortoiseGit's simple search syntax [MAN-LOG:715-764], shared by the log's
// filter and Browse References': space-separated terms that must all match,
// -term excludes, +term re-includes, "quoted phrases" literal ("" is a
// quote), a leading ! inverts everything. What each term is matched against,
// and how (case, regular expressions), stays with the caller.
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace tortilla
{

struct search_term
{
    std::string text;
    bool exclude = false;  // -term
    bool operator==(const search_term&) const = default;
};

struct search_query
{
    bool invert = false;  // a leading !
    std::vector<search_term> terms;
};

search_query parse_search_query(std::string_view);

}  // namespace tortilla
