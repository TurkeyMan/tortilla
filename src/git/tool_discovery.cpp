#include "tool_discovery.h"

#include "util/programs.h"

#include <rapport/sys.h>

#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

namespace tortilla
{

std::vector<std::string> install_candidates(std::initializer_list<const char*> relative,
                                            const char* on_path)
{
    std::vector<std::string> out;
    for (const std::string& root : install_roots())
        for (const char* r : relative) out.push_back(root + "\\" + r);
    if (on_path) out.push_back(on_path);
    return out;
}

std::vector<jetbrains_ide> jetbrains_ides()
{
    std::vector<jetbrains_ide> out;
    for (const std::string& root : install_roots())
    {
        std::error_code ec;
        for (const auto& ide :
             fs::directory_iterator(rap::sys::path_from_utf8(root + "\\JetBrains"), ec))
            for (const char* launcher :
                 {"idea64.exe", "clion64.exe", "rider64.exe", "pycharm64.exe", "webstorm64.exe",
                  "goland64.exe", "phpstorm64.exe", "rustrover64.exe"})
            {
                const fs::path exe = ide.path() / "bin" / launcher;
                if (fs::is_regular_file(exe, ec))
                    out.push_back({rap::sys::path_to_utf8(ide.path().filename()),
                                   rap::sys::path_to_utf8(exe)});
            }
    }
    return out;
}

std::string installed_program(const std::vector<std::string>& programs)
{
    for (const std::string& program : programs)
        if (!find_program(program).empty())
            return program.find(' ') != std::string::npos ? "\"" + program + "\"" : program;
    return {};
}

std::string flatpak_command(const char* app)
{
#ifdef _WIN32
    (void)app;
    return {};
#else
    std::error_code ec;
    const std::string home = rap::sys::env("HOME").value_or("");
    for (const std::string dir :
         {home + "/.local/share/flatpak/app/", std::string("/var/lib/flatpak/app/")})
        if (fs::is_directory(dir + app, ec)) return std::string("flatpak run ") + app;
    return {};
#endif
}

}  // namespace tortilla
