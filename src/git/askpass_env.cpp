#include "askpass_env.h"

#include <rapport/sys.h>

namespace tortilla
{

std::vector<std::pair<std::string, std::string>> askpass_environment()
{
    static const std::vector<std::pair<std::string, std::string>> env = []
    {
        const std::string self = rap::sys::path_to_utf8(rap::sys::executable_path());
        std::vector<std::pair<std::string, std::string>> e{
            {"GIT_ASKPASS", self},
            {"SSH_ASKPASS", self},
            // Git for Windows' "Should I try again?" (a file it can't replace).
            {"GIT_ASK_YESNO", self},
            // OpenSSH 8.4+: the askpass program even without a terminal or
            // DISPLAY to ask on.
            {"SSH_ASKPASS_REQUIRE", "force"},
            {k_askpass_marker, "1"},
        };
#ifdef _WIN32
        // WSL's ssh (the "WSL ssh" client) gets them too, SSH_ASKPASS as a
        // Linux path, and calls back out to tortilla.exe.
        std::string wslenv = rap::sys::env("WSLENV").value_or("");
        for (const char* v : {"SSH_ASKPASS/p", "SSH_ASKPASS_REQUIRE", "TORTILLA_ASKPASS"})
            wslenv += (wslenv.empty() ? "" : ":") + std::string(v);
        e.emplace_back("WSLENV", wslenv);
#endif
        return e;
    }();
    return env;
}

}  // namespace tortilla
