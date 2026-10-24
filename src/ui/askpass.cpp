#include "askpass.h"

#include "git/askpass_env.h"

#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#endif

namespace tortilla
{
namespace
{

enum class kind
{
    secret,
    username,
    yes_no,       // answered "yes" or "no" (ssh's unknown host)
    yes_no_exit,  // answered by the exit code (Git for Windows' GIT_ASK_YESNO)
};

kind kind_of(const std::string& q)
{
    if (q.find("(yes/no") != std::string::npos) return kind::yes_no;
    // "Unlink of file '...' failed. Should I try again?" and the like.
    if (q.find("Should I try again?") != std::string::npos) return kind::yes_no_exit;
    if (q.starts_with("Username") || q.find("username for") != std::string::npos)
        return kind::username;
    return kind::secret;
}

// The answer, to whoever started us (a pipe; a GUI program's stdout may
// not be set up, so the handle directly on Windows).
void write_answer(const std::string& answer)
{
    const std::string line = answer + "\n";
#ifdef _WIN32
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), line.data(), DWORD(line.size()), &written, nullptr);
#else
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fflush(stdout);
#endif
}

}  // namespace

bool started_as_askpass(int argc)
{
    return argc == 2 && std::getenv(k_askpass_marker);
}

int run_askpass(rap::ui& ui, const std::string& question)
{
    const kind k = kind_of(question);
    int code = 1;
    if (k == kind::yes_no || k == kind::yes_no_exit)
    {
        // ssh's question about a host it hasn't seen ("...(yes/no/[fingerprint])?").
        ui.message(nullptr,
                   {.icon = rap::message_spec::icon::question,
                    .title = "Tortilla",
                    .text = question,
                    .buttons = {rap::dialog_role::yes, rap::dialog_role::no}},
                   [&](rap::dialog_role r)
                   {
                       const bool yes = r == rap::dialog_role::yes;
                       if (k == kind::yes_no)
                           write_answer(yes ? "yes" : "no"), code = 0;
                       else
                           code = yes ? 0 : 1;
                       ui.quit();
                   });
        ui.run();
        return code;
    }

    using namespace rap::layout;
    using namespace rap::literals;
    rap::entry* answer = nullptr;
    auto window =
        ui.create(dialog{"Tortilla",
                         {vbox{
                             label{question},
                             entry{}.bind(answer).password(k == kind::secret).width(40_ch),
                             buttons{ok, cancel},
                         }}});
    window->on_finished(
        [&](rap::dialog_role r)
        {
            if (r == rap::dialog_role::ok)
            {
                write_answer(answer->text());
                code = 0;
            }
            ui.quit();
        });
    window->show();
    answer->focus();
    ui.run();
    return code;
}

}  // namespace tortilla
