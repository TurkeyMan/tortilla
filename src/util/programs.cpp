#include "programs.h"

#include <rapport/sys.h>

#include <algorithm>
#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

namespace tortilla
{

std::string find_program(const std::string& name)
{
    std::error_code ec;
    const fs::path p = rap::sys::path_from_utf8(name);
    if (p.has_parent_path()) return fs::is_regular_file(p, ec) ? name : "";
#ifdef _WIN32
    const char sep = ';';
    const std::vector<std::string> exts{".exe", ".cmd", ".bat", ""};
#else
    const char sep = ':';
    const std::vector<std::string> exts{""};
#endif
    const std::string path = rap::sys::env("PATH").value_or("");
    for (size_t at = 0; at <= path.size();)
    {
        size_t end = std::min(path.find(sep, at), path.size());
        if (end > at)
            for (const std::string& ext : exts)
            {
                fs::path candidate = rap::sys::path_from_utf8(path.substr(at, end - at)) /
                                     rap::sys::path_from_utf8(name + ext);
                if (fs::is_regular_file(candidate, ec)) return rap::sys::path_to_utf8(candidate);
            }
        at = end + 1;
    }
    return "";
}

std::vector<std::string> install_roots()
{
    std::vector<std::string> roots;
#ifdef _WIN32
    for (const char* var : {"ProgramFiles", "ProgramFiles(x86)", "ProgramW6432", "LOCALAPPDATA"})
        if (auto v = rap::sys::env(var))
            roots.push_back(*v + (std::string(var) == "LOCALAPPDATA" ? "\\Programs" : ""));
#endif
    return roots;
}

std::string forward_slashes(std::string path)
{
    std::replace(path.begin(), path.end(), '\\', '/');
    return path;
}

}  // namespace tortilla
