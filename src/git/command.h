// Running the git CLI. Actions (reset, merge, cherry-pick, rebase...) shell
// out to git as TortoiseGit does; reads use libgit2 (repo.h).
#pragma once

#include <rapport/rapport.h>
#include <rapport/sys.h>

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace tortilla
{

// What a git command's output holds: everything, for showing to the user,
// or stdout alone, for parsing (git warns on stderr, e.g. about line
// endings on Windows, and a warning must not read as a file name).
enum class git_output
{
    all,
    stdout_only,
};

struct git_result
{
    int exit_code = -1;
    std::string output;  // stdout, and stderr interleaved with git_output::all
    bool ok() const { return exit_code == 0; }
};

// Runs `git <args>` in `dir` and waits. For quick queries only.
git_result run_git(const std::string& dir, const std::vector<std::string>& args,
                 git_output = git_output::all);

// Lets another thread stop a series of run_git calls: cancel() kills the git
// running now, and every later call fails at once.
class git_canceller
{
public:
    void cancel();
    bool cancelled() const;

private:
    friend git_result run_git(const std::string&, const std::vector<std::string>&, git_output,
                              git_canceller&);
    mutable std::mutex mutex_;
    std::shared_ptr<rap::sys::process> current_;
    bool cancelled_ = false;
};

git_result run_git(const std::string& dir, const std::vector<std::string>& args, git_output,
                   git_canceller&);

// Runs `git <args>` in `dir` on a worker thread. Output arrives in chunks on
// the UI thread; `done` gets the exit code. The process is killed if the
// handle is dropped before it finishes.
class git_process
{
public:
    static std::shared_ptr<git_process> start(rap::ui&, const std::string& dir,
                                             std::vector<std::string> args,
                                             std::function<void(const std::string&)> output,
                                             std::function<void(int exit_code)> done,
                                             git_output = git_output::all);
    ~git_process();
    void kill();

private:
    struct state;
    std::shared_ptr<state> state_;
};

// "git reset --hard abc123", for showing what is about to run.
std::string command_line(const std::vector<std::string>& args);

}  // namespace tortilla
