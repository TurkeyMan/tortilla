#pragma once

#include <chrono>
#include <expected>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace tortilla::credential_helper
{
struct request
{
    std::string program = "git";  // Test-only override points at an isolated dummy child.
    std::string directory;
    std::vector<std::string> arguments;
    std::vector<std::pair<std::string, std::string>> environment;
    std::string input;
    std::chrono::milliseconds timeout{10000};
    size_t output_limit = 65536;
};
struct reply
{
    int exit_code = -1;
    std::string output;
    bool timed_out = false, exceeded_limit = false;
};
// A dedicated stdin-capable, headless child runner. No Git transport path
// uses this. stderr is discarded, and stdout is bounded and never logged.
using runner = std::function<std::expected<reply, std::string>(const request&)>;
std::expected<reply, std::string> run(const request&);
void set_runner(runner);  // Offline tests only; empty restores native execution.
}  // namespace tortilla::credential_helper
