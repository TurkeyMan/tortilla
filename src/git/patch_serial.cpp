#include "patch_serial.h"

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

}  // namespace tortilla
