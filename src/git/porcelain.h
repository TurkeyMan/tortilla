// Reading `git status --porcelain -z` (v1): the entries alone, as views of
// the output. What they mean (the changes list's letters, the file
// manager's badges) is each caller's.
#pragma once

#include <string_view>

namespace tortilla
{

struct porcelain_entry
{
    char x = ' ', y = ' ';      // index, working tree: "M ", "??", "UU"...
    std::string_view path;      // a folder git collapses ends in '/'
    std::string_view old_path;  // a rename's or copy's, else empty
};

// Each NUL-terminated field of `text` in turn.
template <class F>
void each_nul_field(std::string_view text, F f)
{
    for (size_t at = 0; at < text.size();)
    {
        size_t end = text.find('\0', at);
        if (end == std::string_view::npos) end = text.size();
        f(text.substr(at, end - at));
        at = end + 1;
    }
}

// Each "XY path" entry in turn; a rename's or copy's old path (the next
// field, for R or C on either side) goes with it. Fields too short to be an
// entry are skipped.
template <class F>
void each_porcelain_entry(std::string_view porcelain, F f)
{
    porcelain_entry pending;
    bool want_old = false;
    each_nul_field(porcelain,
                   [&](std::string_view field)
                   {
                       if (want_old)
                       {
                           want_old = false;
                           pending.old_path = field;
                           f(pending);
                           return;
                       }
                       if (field.size() < 4) return;
                       pending = {field[0], field[1], field.substr(3), {}};
                       want_old = pending.x == 'R' || pending.x == 'C' || pending.y == 'R' ||
                                  pending.y == 'C';
                       if (!want_old) f(pending);
                   });
    if (want_old) f(pending);  // cut short: no old path
}

}  // namespace tortilla
