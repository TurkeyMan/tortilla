#include "command.h"

#include <rapport/sys.h>

#include <atomic>
#include <thread>

namespace tortilla
{
namespace
{

using rap::sys::Process;

// Prompts are disabled: a GUI has no terminal to answer them on, and
// GIT_EDITOR=true never opens an editor (merge messages etc.).
Process::Options gitOptions(GitOutput output)
{
    return {.env = {{"GIT_TERMINAL_PROMPT", "0"}, {"GIT_EDITOR", "true"}},
            .mergeStderr = output == GitOutput::All};
}

std::vector<std::string> gitArgs(const std::string& dir, const std::vector<std::string>& args)
{
    std::vector<std::string> all{"-C", dir};
    all.insert(all.end(), args.begin(), args.end());
    return all;
}

}  // namespace

std::string commandLine(const std::vector<std::string>& args)
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

GitResult runGit(const std::string& dir, const std::vector<std::string>& args, GitOutput output)
{
    GitResult r;
    auto run = Process::run("git", gitArgs(dir, args), gitOptions(output));
    if (!run)
    {
        r.output = "could not start git: " + run.error();
        return r;
    }
    r.exitCode = run->exitCode;
    r.output = std::move(run->output);
    return r;
}

struct GitProcess::State
{
    std::shared_ptr<Process> process;
    std::atomic<bool> dropped{false};
};

std::shared_ptr<GitProcess> GitProcess::start(rap::Ui& ui, const std::string& dir,
                                              std::vector<std::string> args,
                                              std::function<void(const std::string&)> output,
                                              std::function<void(int)> done, GitOutput mode)
{
    auto p = std::shared_ptr<GitProcess>(new GitProcess);
    auto state = p->state_ = std::make_shared<State>();
    auto started = Process::start("git", gitArgs(dir, args), gitOptions(mode));
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
            Process& proc = *state->process;
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

GitProcess::~GitProcess()
{
    if (!state_) return;
    state_->dropped = true;
    kill();
}

void GitProcess::kill()
{
    // Safe from the UI thread while the reader thread blocks in read().
    if (state_ && state_->process) state_->process->kill();
}

}  // namespace tortilla
