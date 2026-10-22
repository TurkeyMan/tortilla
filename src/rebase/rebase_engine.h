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

class rebase_engine
{
public:
    enum class rebase_stage
    {
        choose,          // not started
        running,         // applying commits
        conflict,        // a pick conflicted; Commit when resolved
        edit,            // an Edit row is applied; Amend to go on
        squash_edit,      // a squash group is staged; Commit its message
        squash_conflict,  // a pick inside a squash group conflicted
        error,           // a pick failed without conflicts; only Abort
        done,
    };

    struct start_args
    {
        std::string branch;      // local branch being rebased
        oid branch_head;          // its tip before the rebase
        oid onto;                // upstream (or --onto) commit
        std::string checked_out;  // what to return to on abort (branch name or hash)
        bool fast_forward = false;
    };

    struct hooks
    {
        std::function<void(const std::string& text, bool command)> log;
        std::function<void()> stage_changed;
        std::function<void(size_t applied, size_t total)> progress;
        // The message to show (and, when stopped, edit) in the message tab.
        std::function<void(const std::string&)> message;
        // Something needs saying in a message box.
        std::function<void(const std::string&)> warn;
        // Ask whether to skip a commit that failed without conflicts.
        std::function<void(const std::string& output, std::function<void(bool skip)>)> ask_skip;
    };

    rebase_engine(rap::ui&, const std::string& workdir, const std::string& git_dir, rebase_model&,
                 hooks);

    void start(const start_args&);
    // Continue / Commit / Amend, with the message tab's text.
    void resume(const std::string& message);
    // Undoes everything and returns to the original checkout.
    void abort(std::function<void()> done);

    rebase_stage stage() const { return stage_; }
    bool busy() const { return busy_; }
    bool started() const { return stage_ != rebase_stage::choose; }

private:
    using done_fn = std::function<void(int code, const std::string& output)>;
    void run(std::vector<std::string> args, done_fn done, bool quiet = false);

    void step();
    void advance();
    void picked(int code, const std::string& output, bool in_group);
    void stop(rebase_stage);
    void finish();
    void commit_squash(const std::string& message);
    void write_message(const std::string& message);
    std::string author_arg(const commit&) const;
    std::string date_arg(const commit&) const;

    rap::ui& ui_;
    std::string workdir_, msg_file_;
    rebase_model& model_;
    hooks hooks_;
    start_args start_;
    rebase_stage stage_ = rebase_stage::choose;
    bool busy_ = false;
    std::shared_ptr<git_process> process_;

    int pos_ = -1;  // row being applied; rows go bottom (size-1) to top (0)
    struct squash
    {
        bool active = false;
        size_t first = 0;  // the row the group squashes into
        std::string message;
    } squash_;
};

}  // namespace tortilla
