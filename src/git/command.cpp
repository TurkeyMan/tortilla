#include "command.h"

#include <rapport/sys.h>

#include <atomic>
#include <thread>

namespace tortilla
{
namespace
{

using rap::sys::process;

// Prompts are disabled: a GUI has no terminal to answer them on, and
// GIT_EDITOR=true never opens an editor (merge messages etc.).
process::options git_options(git_output output)
{
    return {.env = {{"GIT_TERMINAL_PROMPT", "0"}, {"GIT_EDITOR", "true"}},
            .merge_stderr = output == git_output::all};
}

std::vector<std::string> git_command_line(const std::string& dir, const std::vector<std::string>& args)
{
    std::vector<std::string> all{"-C", dir};
    all.insert(all.end(), args.begin(), args.end());
    return all;
}

}  // namespace

std::string command_line(const std::vector<std::string>& args)
{
    std::string s = "git";
    for (auto& a : args)
    {
        bool quote = a.empty() || a.find_first_of(" \t\"'") != std::string::npos;
        s += ' ';
        s += quote ? "\"" + a + "\"" : a;
    }
    return s;
}

git_result run_git(const std::string& dir, const std::vector<std::string>& args, git_output output)
{
    git_result r;
    auto run = process::run("git", git_command_line(dir, args), git_options(output));
    if (!run)
    {
        r.output = "could not start git: " + run.error();
        return r;
    }
    r.exit_code = run->exit_code;
    r.output = std::move(run->output);
    return r;
}

struct git_process::state
{
    std::shared_ptr<rap::sys::process> process;
    std::atomic<bool> dropped{false};
};

std::shared_ptr<git_process> git_process::start(rap::ui& ui, const std::string& dir,
                                              std::vector<std::string> args,
                                              std::function<void(const std::string&)> output,
                                              std::function<void(int)> done, git_output mode)
{
    auto p = std::shared_ptr<git_process>(new git_process);
    auto state = p->state_ = std::make_shared<git_process::state>();
    auto started = process::start("git", git_command_line(dir, args), git_options(mode));
    if (!started)
    {
        ui.post(
            [output, done, error = started.error()]
            {
                if (output) output("could not start git: " + error + "\n");
                if (done) done(-1);
            });
        return p;
    }
    state->process = std::move(*started);

    // Callbacks only run while the handle is alive.
    std::thread(
        [&ui, state, output, done]
        {
            process& proc = *state->process;
            char buf[4096];
            while (size_t n = proc.read(buf, sizeof buf))
                ui.post(
                    [state, output, chunk = std::string(buf, n)]
                    {
                        if (!state->dropped && output) output(chunk);
                    });
            int code = proc.wait();
            ui.post(
                [state, done, code]
                {
                    if (!state->dropped && done) done(code);
                });
        })
        .detach();
    return p;
}

git_process::~git_process()
{
    if (!state_) return;
    state_->dropped = true;
    kill();
}

void git_process::kill()
{
    // Safe from the UI thread while the reader thread blocks in read().
    if (state_ && state_->process) state_->process->kill();
}

}  // namespace tortilla
