#include "status.h"

#include <charconv>
#include <unordered_map>

namespace tortilla
{
namespace
{

// Splits on NULs; the trailing empty field after the last NUL is dropped.
std::vector<std::string> fields(const std::string& s)
{
    std::vector<std::string> out;
    size_t at = 0;
    while (at < s.size())
    {
        size_t end = s.find('\0', at);
        if (end == std::string::npos) end = s.size();
        out.push_back(s.substr(at, end - at));
        at = end + 1;
    }
    return out;
}

char status_letter(char x, char y)
{
    if (x == '?' && y == '?') return '?';
    if (x == '!' && y == '!') return '!';
    // Unmerged: either side U, or both added/deleted.
    if (x == 'U' || y == 'U' || (x == 'A' && y == 'A') || (x == 'D' && y == 'D')) return 'U';
    char c = x != ' ' ? x : y;
    switch (c)
    {
    case 'A':
    case 'D':
    case 'R':
    case 'C':
    case 'T': return c;
    default: return 'M';
    }
}

}  // namespace

std::vector<file_change> parse_status(const std::string& porcelain, const std::string& numstat)
{
    struct line_counts
    {
        size_t added = 0, removed = 0;
        bool binary = false;
    };
    // numstat -z: "added\tremoved\tpath\0", or for renames
    // "added\tremoved\t\0old\0new\0". Binary files count as "-\t-".
    std::unordered_map<std::string, line_counts> counts;
    auto nf = fields(numstat);
    for (size_t i = 0; i < nf.size(); ++i)
    {
        const std::string& f = nf[i];
        size_t t1 = f.find('\t'), t2 = t1 == std::string::npos ? t1 : f.find('\t', t1 + 1);
        if (t2 == std::string::npos) continue;
        line_counts c;
        std::string a = f.substr(0, t1), r = f.substr(t1 + 1, t2 - t1 - 1);
        c.binary = a == "-";
        // Never throws on odd output: unparsable counts stay 0.
        if (!c.binary)
        {
            std::from_chars(a.data(), a.data() + a.size(), c.added);
            std::from_chars(r.data(), r.data() + r.size(), c.removed);
        }
        std::string path = f.substr(t2 + 1);
        if (path.empty() && i + 2 < nf.size())
        {
            path = nf[i + 2];  // rename: old, new
            i += 2;
        }
        counts[path] = c;
    }

    // porcelain v1 -z: "XY path\0", renames and copies "XY new\0old\0".
    std::vector<file_change> out;
    auto pf = fields(porcelain);
    for (size_t i = 0; i < pf.size(); ++i)
    {
        const std::string& e = pf[i];
        if (e.size() < 4) continue;
        file_change f;
        f.status = status_letter(e[0], e[1]);
        f.staged = e[0] != ' ' && e[0] != '?' && e[0] != '!';
        f.path = e.substr(3);
        if ((e[0] == 'R' || e[0] == 'C') && i + 1 < pf.size()) f.old_path = pf[++i];
        if (auto it = counts.find(f.path); it != counts.end())
        {
            f.added = it->second.added;
            f.removed = it->second.removed;
            f.binary = it->second.binary;
        }
        out.push_back(std::move(f));
    }
    return out;
}

}  // namespace tortilla
