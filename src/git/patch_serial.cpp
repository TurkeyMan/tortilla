#include "patch_serial.h"

#include "git/ignore_patterns.h"

#include <rapport/sys.h>

#include <filesystem>

namespace tortilla
{

std::vector<std::string> format_patch_serial_args(const format_patch_options& o)
{
    std::vector<std::string> args{"format-patch", "-o", o.folder};
    switch (o.commits)
    {
    case format_patch_options::which::since: args.push_back(o.since); break;
    case format_patch_options::which::number:
        args.push_back("-" + std::to_string(o.number < 1 ? 1 : o.number));
        break;
    case format_patch_options::which::range:
        args.push_back(o.from + ".." + (o.to.empty() ? std::string("HEAD") : o.to));
        break;
    }
    return args;
}

std::vector<std::string> am_args(const am_options& o, const std::vector<std::string>& patches)
{
    std::vector<std::string> args{"am"};
    if (o.three_way) args.push_back("--3way");
    if (o.ignore_space) args.push_back("--ignore-space-change");
    if (o.keep_cr) args.push_back("--keep-cr");
    if (o.signoff) args.push_back("--signoff");
    args.push_back("--");
    args.insert(args.end(), patches.begin(), patches.end());
    return args;
}

bool am_in_progress(const std::string& git_dir)
{
    std::error_code ec;
    return std::filesystem::exists(rap::sys::path_from_utf8(git_dir) / "rebase-apply" / "applying",
                                   ec);
}

std::vector<std::string> numstat_paths(std::string_view numstat)
{
    // "added\tdeleted\tpath\0", or for a rename "added\tdeleted\t\0old\0new\0".
    std::vector<std::string> out;
    auto field = [&](std::string_view& s)
    {
        const size_t end = s.find('\0');
        std::string_view f = s.substr(0, end);
        s = end == std::string_view::npos ? std::string_view() : s.substr(end + 1);
        return f;
    };
    while (!numstat.empty())
    {
        std::string_view record = field(numstat);
        const size_t tab = record.find('\t');
        const size_t tab2 = tab == std::string_view::npos ? tab : record.find('\t', tab + 1);
        if (tab2 == std::string_view::npos) continue;
        std::string_view path = record.substr(tab2 + 1);
        if (path.empty())
        {
            field(numstat);  // the old name
            path = field(numstat);
        }
        if (!path.empty()) out.emplace_back(path);
    }
    return out;
}

std::vector<std::string> apply_args(const std::string& patch, const std::vector<std::string>& only,
                                    bool check)
{
    std::vector<std::string> args{"apply"};
    if (check) args.push_back("--check");
    // --include takes a pattern (git's wildmatch, as .gitignore's): each
    // name escaped, so [ab].txt is that file and never a.txt and b.txt.
    for (const std::string& p : only) args.push_back("--include=" + literal_pattern(p));
    args.insert(args.end(), {"--", patch});
    return args;
}

}  // namespace tortilla
