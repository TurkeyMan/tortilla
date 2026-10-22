// TortoiseGit's rebase engine [spec 2.7]: instead of git rebase -i, the
// commits are replayed one by one with git cherry-pick, from the bottom of
// the list up, on a detached HEAD at the upstream. That lets the dialog stop
// for conflicts, edits and squash messages and resume from its own buttons.
// When everything is applied the branch is moved to the result.
#pragma once

#include "git/command.h"
#include "rebase/rebase_model.h"

#include <rapport/rapport.h>

#include <functional>
#include <memory>
#include <string>

namespace tortilla
{

class RebaseEngine
{
public:
    enum class Stage
    {
        Choose,          // not started
        Running,         // applying commits
        Conflict,        // a pick conflicted; Commit when resolved
        Edit,            // an Edit row is applied; Amend to go on
        SquashEdit,      // a squash group is staged; Commit its message
        SquashConflict,  // a pick inside a squash group conflicted
        Error,           // a pick failed without conflicts; only Abort
        Done,
    };

    struct Start
    {
        std::string branch;      // local branch being rebased
        Oid branchHead;          // its tip before the rebase
        Oid onto;                // upstream (or --onto) commit
        std::string checkedOut;  // what to return to on abort (branch name or hash)
        bool fastForward = false;
    };

    struct Hooks
    {
        std::function<void(const std::string& text, bool command)> log;
        std::function<void()> stageChanged;
        std::function<void(size_t applied, size_t total)> progress;
        // The message to show (and, when stopped, edit) in the message tab.
        std::function<void(const std::string&)> message;
        // Something needs saying in a message box.
        std::function<void(const std::string&)> warn;
        // Ask whether to skip a commit that failed without conflicts.
        std::function<void(const std::string& output, std::function<void(bool skip)>)> askSkip;
    };

    RebaseEngine(rap::Ui&, const std::string& workdir, const std::string& gitDir, RebaseModel&,
                 Hooks);

    void start(const Start&);
    // Continue / Commit / Amend, with the message tab's text.
    void resume(const std::string& message);
    // Undoes everything and returns to the original checkout.
    void abort(std::function<void()> done);

    Stage stage() const { return stage_; }
    bool busy() const { return busy_; }
    bool started() const { return stage_ != Stage::Choose; }

private:
    using Done = std::function<void(int code, const std::string& output)>;
    void run(std::vector<std::string> args, Done done, bool quiet = false);

    void step();
    void advance();
    void picked(int code, const std::string& output, bool inGroup);
    void stop(Stage);
    void finish();
    void commitSquash(const std::string& message);
    void writeMessage(const std::string& message);
    std::string authorArg(const Commit&) const;
    std::string dateArg(const Commit&) const;

    rap::Ui& ui_;
    std::string workdir_, msgFile_;
    RebaseModel& model_;
    Hooks hooks_;
    Start start_;
    Stage stage_ = Stage::Choose;
    bool busy_ = false;
    std::shared_ptr<GitProcess> process_;

    int pos_ = -1;  // row being applied; rows go bottom (size-1) to top (0)
    struct Squash
    {
        bool active = false;
        size_t first = 0;  // the row the group squashes into
        std::string message;
    } squash_;
};

}  // namespace tortilla
