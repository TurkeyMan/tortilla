#include "clean.h"

#include "git/quoted_path.h"

#include <string_view>

namespace tortilla
{
namespace
{

std::vector<std::string> flags(const clean_options& o, bool dry_run)
{
    std::vector<std::string> f{dry_run ? "-n" : "-f"};
    if (o.directories) f.push_back("-d");
    if (o.files == clean_options::ignored) f.push_back("-X");
    if (o.files == clean_options::all) f.push_back("-x");
    return f;
}

}  // namespace

std::vector<std::string> clean_args(const clean_options& o, bool dry_run, const std::string& folder,
                                    const std::vector<std::string>& files)
{
    std::vector<std::string> args;
    if (!folder.empty() && folder != ".") args = {"-C", folder};
    // Paths printed as they are, not escaped, for the listing to read.
    args.insert(args.end(), {"-c", "core.quotepath=false", "clean"});
    for (std::string& f : flags(o, dry_run)) args.push_back(std::move(f));
    if (!files.empty())
    {
        args.push_back("--");
        args.insert(args.end(), files.begin(), files.end());
    }
    return args;
}

std::vector<std::string> submodule_clean_args(const clean_options& o, bool dry_run)
{
    std::vector<std::string> args{
        "submodule", "foreach", "--recursive", "git", "-c", "core.quotepath=false", "clean"};
    for (std::string& f : flags(o, dry_run)) args.push_back(std::move(f));
    return args;
}

std::vector<std::string> parse_clean_listing(const std::string& output)
{
    std::vector<std::string> out;
    std::string prefix;
    size_t at = 0;
    while (at < output.size())
    {
        size_t end = output.find('\n', at);
        if (end == std::string::npos) end = output.size();
        std::string_view line(output.data() + at, end - at);
        at = end + 1;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.starts_with("Entering '") && line.ends_with("'"))
            prefix = std::string(line.substr(10, line.size() - 11)) + "/";
        else if (line.starts_with("Would remove "))
            out.push_back(prefix + unquote_path(line.substr(13)));
    }
    return out;
}

}  // namespace tortilla
