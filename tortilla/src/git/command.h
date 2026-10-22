// Running the git CLI. Actions (reset, merge, cherry-pick, rebase...) shell
// out to git as TortoiseGit does; reads use libgit2 (repo.h).
#pragma once

#include <rapport/rapport.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tortilla
{

// What a git command's output holds: everything, for showing to the user,
// or stdout alone, for parsing (git warns on stderr, e.g. about line
// endings on Windows, and a warning must not read as a file name).
enum class GitOutput
{
    All,
    Stdout,
};

struct GitResult
{
    int exitCode = -1;
    std::string output;  // stdout, and stderr interleaved with GitOutput::All
    bool ok() const { return exitCode == 0; }
};

// Runs `git <args>` in `dir` and waits. For quick queries only.
GitResult runGit(const std::string& dir, const std::vector<std::string>& args,
                 GitOutput = GitOutput::All);

// Runs `git <args>` in `dir` on a worker thread. Output arrives in chunks on
// the UI thread; `done` gets the exit code. The process is killed if the
// handle is dropped before it finishes.
class GitProcess
{
public:
    static std::shared_ptr<GitProcess> start(rap::Ui&, const std::string& dir,
                                             std::vector<std::string> args,
                                             std::function<void(const std::string&)> output,
                                             std::function<void(int exitCode)> done,
                                             GitOutput = GitOutput::All);
    ~GitProcess();
    void kill();

private:
    struct State;
    std::shared_ptr<State> state_;
};

// "git reset --hard abc123", for showing what is about to run.
std::string commandLine(const std::vector<std::string>& args);

}  // namespace tortilla
