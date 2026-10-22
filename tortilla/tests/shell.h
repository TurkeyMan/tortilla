// Runs a POSIX shell command line for test fixtures, failing the test run if
// it fails. Windows has no /bin/sh, so it uses the sh.exe that ships with
// Git for Windows, found from git's own install directory.
#pragma once

#include <rapport/sys.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace tortilla::test
{

inline std::string shellPath()
{
#ifdef _WIN32
    // `git --exec-path`: <git root>/mingw64/libexec/git-core
    static const std::string path = []
    {
        auto r = rap::sys::Process::run("git", std::vector<std::string>{"--exec-path"});
        if (!r || r->exitCode != 0) return std::string("sh");
        std::string dir = r->output.substr(0, r->output.find_last_not_of("\r\n") + 1);
        auto root = rap::sys::pathFromUtf8(dir).parent_path().parent_path().parent_path();
        return rap::sys::pathToUtf8(root / "bin" / "sh.exe");
    }();
    return path;
#else
    return "sh";
#endif
}

inline void sh(const std::string& cmd)
{
    auto r = rap::sys::Process::run(shellPath(), std::vector<std::string>{"-c", cmd});
    if (r && !r->output.empty()) std::fputs(r->output.c_str(), stdout);
    if (!r || r->exitCode != 0)
    {
        std::fprintf(stderr, "command failed: %s%s\n", cmd.c_str(),
                     r ? "" : (" (" + r.error() + ")").c_str());
        std::exit(2);
    }
}

}  // namespace tortilla::test
