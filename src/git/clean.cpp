#include "clean.h"

#include <string_view>

namespace tortilla
{
namespace
{

// A path as git prints it: as it is, or in quotes with C escapes when it has
// unusual characters.
std::string unquote(std::string_view s)
{
    if (s.size() < 2 || s.front() != '"' || s.back() != '"') return std::string(s);
    s = s.substr(1, s.size() - 2);
    std::string out;
    for (size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] != '\\' || i + 1 == s.size())
        {
            out += s[i];
            continue;
        }
        const char c = s[++i];
        if (c >= '0' && c <= '7' && i + 2 < s.size())
        {
            out += char((c - '0') * 64 + (s[i + 1] - '0') * 8 + (s[i + 2] - '0'));
            i += 2;
        }
        else
            out += c == 'n'   ? '\n'
                   : c == 't' ? '\t'
                   : c == 'r' ? '\r'
                   : c == 'a' ? '\a'
                   : c == 'b' ? '\b'
                   : c == 'f' ? '\f'
                   : c == 'v' ? '\v'
                              : c;
    }
    return out;
}

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
            out.push_back(prefix + unquote(line.substr(13)));
    }
    return out;
}

}  // namespace tortilla
