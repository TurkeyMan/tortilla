#include "forge/credential_helper.h"
#include "test_support.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <thread>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace tortilla::test
{
// This mode exits before any UI/backend/repository setup. It never runs Git,
// contacts a helper, reads a credential store, or accesses the network.
int azure_credential_child(int argc, char** argv)
{
    if (argc < 3 || std::string_view(argv[1]) != "--azure-dummy-child") return -1;
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    const std::string mode = argv[2];
    if (mode == "timeout")
    {
        std::fclose(stdout);  // Exercises timeout even after the output pipe closes.
        std::this_thread::sleep_for(std::chrono::seconds(60));
        return 0;
    }
    if (mode == "limit")
    {
        std::cout << std::string(70000, 'x') << std::flush;
        return 0;
    }
    const std::string input{std::istreambuf_iterator<char>(std::cin), {}};
    std::cout << input;
    for (const char* key : {"GCM_INTERACTIVE", "GIT_TERMINAL_PROMPT", "GIT_ASKPASS", "SSH_ASKPASS"})
    {
        const char* value = std::getenv(key);
        std::cout << key << '=' << (value ? value : "<unset>") << '\n';
    }
    return 0;
}

void run_azure_process_tests(const std::string& executable)
{
    credential_helper::request r;
    r.program = std::filesystem::absolute(executable).string();
    r.arguments = {"--azure-dummy-child", "echo"};
    r.input = "protocol=https\nhost=fixture.invalid\npath=synthetic\n\n";
    r.environment = {{"GCM_INTERACTIVE", "never"},
                     {"GIT_TERMINAL_PROMPT", "0"},
                     {"GIT_ASKPASS", ""},
                     {"SSH_ASKPASS", ""}};
    const auto echo = credential_helper::run(r);
    check(echo && echo->exit_code == 0 &&
              echo->output == r.input + "GCM_INTERACTIVE=never\nGIT_TERMINAL_PROMPT=0\nGIT_ASKPASS="
                                        "<unset>\nSSH_ASKPASS=<unset>\n",
          "azure helper process: isolated dummy child receives exact stdin and noninteractive "
          "child-only environment");
    r.arguments.back() = "timeout";
    r.timeout = std::chrono::milliseconds(150);
    const auto begin = std::chrono::steady_clock::now();
    const auto timeout = credential_helper::run(r);
    check(timeout && timeout->timed_out &&
              std::chrono::steady_clock::now() - begin < std::chrono::seconds(5),
          "azure helper process: closed-stdout child is killed within its deadline using its own "
          "group/job");
    r.arguments.back() = "limit";
    r.timeout = std::chrono::seconds(5);
    r.output_limit = 128;
    const auto capped = credential_helper::run(r);
    check(capped && capped->exceeded_limit && capped->output.size() <= r.output_limit,
          "azure helper process: actual dummy child output is capped and stopped without a "
          "credential-store probe");
}
}  // namespace tortilla::test
