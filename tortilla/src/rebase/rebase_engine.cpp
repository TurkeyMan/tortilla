#include "rebase_engine.h"

#include <cstdio>
#include <cstring>

namespace tortilla
{
namespace
{

// Lines git writes around conflicting hunks.
bool hasConflictMarkers(const std::string& path)
{
    std::FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return false;
    char line[4096];
    bool found = false;
    while (!found && std::fgets(line, sizeof line, f))
        found = std::strncmp(line, "<<<<<<< ", 8) == 0 || std::strncmp(line, ">>>>>>> ", 8) == 0;
    std::fclose(f);
    return found;
}

}  // namespace

RebaseEngine::RebaseEngine(rap::Ui& ui, const std::string& workdir, const std::string& gitDir,
                           RebaseModel& model, Hooks hooks)
    : ui_(ui)
    , workdir_(workdir)
    , msgFile_(gitDir + "tortilla-rebase-msg")
    , model_(model)
    , hooks_(std::move(hooks))
{
}

// Runs a git command, echoing it and its output to the log.
void RebaseEngine::run(std::vector<std::string> args, Done done, bool quiet)
{
    busy_ = true;
    if (!quiet) hooks_.log(commandLine(args), true);
    auto output = std::make_shared<std::string>();
    process_ = GitProcess::start(
        ui_, workdir_, std::move(args),
        [this, output, quiet](const std::string& chunk)
        {
            *output += chunk;
            if (!quiet) hooks_.log(chunk, false);
        },
        [this, output, done](int code)
        {
            busy_ = false;
            done(code, *output);
        },
        quiet ? GitOutput::Stdout : GitOutput::All);  // quiet output is parsed
}

void RebaseEngine::start(const Start& s)
{
    start_ = s;
    stage_ = Stage::Running;
    hooks_.stageChanged();
    hooks_.log("Start Rebase\n", true);

    // Fast-forward: move the branch to the upstream and stop [spec 2.7.9].
    if (s.fastForward)
    {
        std::string onto = s.onto.hex();
        run({"checkout", "--no-track", "-f", "-B", s.branch, onto},
            [this, onto](int code, auto&)
            {
                if (code != 0) return stop(Stage::Error);
                run({"reset", "--hard", onto},
                    [this](int code, auto&)
                    {
                        if (code != 0) return stop(Stage::Error);
                        hooks_.log("Fast forward to \"" + start_.onto.hex(8) + "\"\n", true);
                        stop(Stage::Done);
                    });
            });
        return;
    }

    pos_ = int(model_.size()) - 1;
    run({"checkout", "-f", "--detach", s.onto.hex()},
        [this](int code, auto&)
        {
            if (code != 0) return stop(Stage::Error);
            step();
        });
}

void RebaseEngine::stop(Stage s)
{
    stage_ = s;
    hooks_.stageChanged();
}

// Applies the row at pos_, skipping Skip rows. A Pick or Edit with Squash
// rows above it starts a squash group: its members are picked without
// committing, and the group is committed once with a combined message.
void RebaseEngine::step()
{
    size_t total = model_.size();
    while (pos_ >= 0 && model_.row(size_t(pos_)).action == RebaseAction::Skip && !squash_.active)
    {
        model_.setState(size_t(pos_), true, false);
        --pos_;
    }
    if (pos_ < 0) return finish();

    auto i = size_t(pos_);
    const RebaseRow& row = model_.rows()[i];
    model_.setState(i, false, true);
    hooks_.progress(total - i, total);

    std::string hash = row.commit.id.hex();
    bool squashAbove = i > 0 && model_.rows()[i - 1].action == RebaseAction::Squash;

    if (row.action == RebaseAction::Squash || squashAbove)
    {
        if (!squash_.active)
        {
            squash_ = {true, i, row.commit.message};
        }
        else
        {
            if (!squash_.message.ends_with('\n')) squash_.message += '\n';
            squash_.message += "\n" + row.commit.message;
        }
        run({"cherry-pick", "--no-commit", hash},
            [this](int code, const std::string& out) { picked(code, out, true); });
        return;
    }

    if (row.action == RebaseAction::Edit)
    {
        run({"cherry-pick", hash},
            [this](int code, const std::string& out)
            {
                if (code != 0) return picked(code, out, false);
                hooks_.message(model_.rows()[size_t(pos_)].commit.message);
                stop(Stage::Edit);
            });
        return;
    }

    // --ff keeps the commit as is when it already sits on the new base.
    run({"cherry-pick", "--ff", hash},
        [this](int code, const std::string& out) { picked(code, out, false); });
}

// After a cherry-pick: on success carry on; on conflicts stop for the user;
// on other failures (an empty pick, say) ask whether to skip it.
void RebaseEngine::picked(int code, const std::string& output, bool inGroup)
{
    if (code == 0)
    {
        if (!inGroup) return advance();
        // Inside a squash group: keep picking until the top of the group.
        auto i = size_t(pos_);
        if (i > 0 && model_.rows()[i - 1].action == RebaseAction::Squash) return advance();
        hooks_.message(squash_.message);
        return stop(Stage::SquashEdit);
    }

    run(
        {"diff", "--name-only", "--diff-filter=U"},
        [this, output, inGroup](int, const std::string& u)
        {
            if (!u.empty())
            {
                hooks_.message(inGroup ? squash_.message
                                       : model_.rows()[size_t(pos_)].commit.message);
                return stop(inGroup ? Stage::SquashConflict : Stage::Conflict);
            }
            hooks_.askSkip(output,
                           [this](bool skip)
                           {
                               if (!skip) return stop(Stage::Error);
                               run({"reset", "--hard", "HEAD"},
                                   [this](int, auto&)
                                   {
                                       run({"cherry-pick", "--quit"},
                                           [this](int, auto&)
                                           {
                                               model_.row(size_t(pos_)).action = RebaseAction::Skip;
                                               advance();
                                           });
                                   });
                           });
        },
        true);
}

void RebaseEngine::advance()
{
    auto i = size_t(pos_);
    model_.setState(i, true, false);
    --pos_;
    stage_ = Stage::Running;
    hooks_.stageChanged();
    step();
}

void RebaseEngine::writeMessage(const std::string& message)
{
    if (std::FILE* f = std::fopen(msgFile_.c_str(), "w"))
    {
        std::fwrite(message.data(), 1, message.size(), f);
        std::fclose(f);
    }
}

std::string RebaseEngine::authorArg(const Commit& c) const
{
    return "--author=" + c.author + " <" + c.email + ">";
}

std::string RebaseEngine::dateArg(const Commit& c) const
{
    char tz[16];
    std::snprintf(tz, sizeof tz, "%+03d%02d", c.offsetMinutes / 60, std::abs(c.offsetMinutes % 60));
    return "--date=" + std::to_string(c.time) + " " + tz;
}

// Squash groups take the first commit's author and date [spec 2.7.5].
void RebaseEngine::commitSquash(const std::string& message)
{
    writeMessage(message);
    const Commit& first = model_.rows()[squash_.first].commit;
    run({"commit", "-F", msgFile_, authorArg(first), dateArg(first)},
        [this](int code, auto&)
        {
            if (code != 0) return stop(Stage::Error);
            // Every row of the group is applied now.
            for (size_t i = size_t(pos_); i <= squash_.first; ++i) model_.setState(i, true, false);
            squash_ = {};
            --pos_;
            stage_ = Stage::Running;
            hooks_.stageChanged();
            step();
        });
}

void RebaseEngine::resume(const std::string& message)
{
    switch (stage_)
    {
    case Stage::Conflict:
    case Stage::SquashConflict:
    {
        bool inGroup = stage_ == Stage::SquashConflict;
        // Unmerged files still holding conflict markers block the commit
        // [spec 2.7.3]; the rest count as resolved and are added.
        // TODO: per-file "Resolved" in the conflict list, as TortoiseGit has.
        run(
            {"diff", "--name-only", "--diff-filter=U"},
            [this, message, inGroup](int, const std::string& unmerged)
            {
                std::vector<std::string> files, marked;
                for (size_t at = 0; at < unmerged.size();)
                {
                    size_t nl = unmerged.find('\n', at);
                    if (nl == std::string::npos) nl = unmerged.size();
                    if (nl > at) files.push_back(unmerged.substr(at, nl - at));
                    at = nl + 1;
                }
                for (auto& f : files)
                    if (hasConflictMarkers(workdir_ + "/" + f)) marked.push_back(f);
                if (!marked.empty())
                {
                    std::string list;
                    for (auto& f : marked) list += f + "\n";
                    hooks_.warn("One or more files are in a conflicted state.\n\n" + list);
                    return;
                }
                std::vector<std::string> add{"add", "-u", "--"};
                add.insert(add.end(), files.begin(), files.end());
                run(add,
                    [this, message, inGroup](int code, auto&)
                    {
                        if (code != 0) return stop(Stage::Error);
                        if (inGroup)
                        {
                            squash_.message = message;
                            stage_ = Stage::Running;
                            return picked(0, {}, true);
                        }
                        const Commit& c = model_.rows()[size_t(pos_)].commit;
                        writeMessage(message);
                        run({"commit", "-F", msgFile_, authorArg(c), dateArg(c)},
                            [this](int code, auto&)
                            {
                                if (code != 0) return stop(Stage::Error);
                                advance();
                            });
                    });
            },
            true);
        break;
    }
    case Stage::Edit:
    {
        // Amend with the (possibly edited) message and any changes made.
        writeMessage(message);
        run({"commit", "--amend", "--all", "-F", msgFile_},
            [this](int code, auto&)
            {
                if (code != 0) return stop(Stage::Error);
                advance();
            });
        break;
    }
    case Stage::SquashEdit: commitSquash(message); break;
    default: break;
    }
}

// Move the branch to the rebased commits and check it out [spec 2.7.6].
void RebaseEngine::finish()
{
    run({"checkout", "-B", start_.branch},
        [this](int code, auto&)
        {
            if (code != 0) return stop(Stage::Error);
            std::remove(msgFile_.c_str());
            hooks_.log("Finished rebasing.\n", true);
            stop(Stage::Done);
        });
}

// The branch only moves at the end, so aborting is: drop any pick in
// progress and check out what was checked out before [spec 2.7.8].
void RebaseEngine::abort(std::function<void()> done)
{
    if (process_) process_->kill();
    if (!started() || stage_ == Stage::Done)
    {
        if (done) done();
        return;
    }
    hooks_.log("Abort\n", true);
    run({"cherry-pick", "--abort"},
        [this, done](int, auto&)
        {
            run({"reset", "--hard"},
                [this, done](int, auto&)
                {
                    run({"checkout", "-f", start_.checkedOut},
                        [this, done](int, auto&)
                        {
                            std::remove(msgFile_.c_str());
                            hooks_.log("Rebase aborted.\n", true);
                            stage_ = Stage::Done;
                            hooks_.stageChanged();
                            if (done) done();
                        });
                });
        });
}

}  // namespace tortilla
