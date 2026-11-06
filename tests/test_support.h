#pragma once

#include "git/command.h"

#include <cstdio>

namespace tortilla::test
{

inline int failures = 0;

inline void check(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

inline std::string git_out(const std::string& dir, const std::vector<std::string>& args)
{
    const tortilla::git_result r = tortilla::run_git(dir, args, tortilla::git_output::stdout_only);
    std::string out = r.ok() ? r.output : "";
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}

}  // namespace tortilla::test
