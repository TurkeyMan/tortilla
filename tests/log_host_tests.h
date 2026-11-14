#pragma once

#include <rapport/rapport.h>

#include <string>

namespace tortilla::test
{
// `repo`: a repository of the suite's own, for the paths handed over; `ui`:
// the null backend's, for windows of its own.
void run_log_host_tests(rap::ui& ui, const std::string& repo);
}
