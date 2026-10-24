#include "rebase_engine.h"

#include <cstdio>
#include <cstring>

namespace tortilla
{
namespace
{

// Lines git writes around conflicting hunks.
bool has_conflict_markers(const std::string& path)
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

rebase_engine::rebase_engine(rap::ui& ui, const std::string& workdir, const std::string& git_dir,
                             rebase_model& model, hooks hooks)
    : ui_(ui)
    , workdir_(workdir)
    , msg_file_(git_dir + "tortilla-rebase-msg")
    , active_file_(git_dir + "tortilla-rebase-active")
    , model_(model)
    , hooks_(std::move(hooks))
{
}

// Runs a git command, echoing it and its output to the log.
void rebase_engine::run(std::vector<std::string> args, done_fn done, bool quiet)
{
    busy_ = true;
    if (!quiet) hooks_.log(command_line(args), true);
    auto output = std::make_shared<std::string>();
    process_ = git_process::start(
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
        quiet ? git_output::stdout_only : git_output::all);  // quiet output is parsed
}

void rebase_engine::start(const start_args& s)
{
    start_ = s;
    if (std::FILE* f = std::fopen(active_file_.c_str(), "w"))
    {
        std::fprintf(f, "onto=%s\nbranch=%s\n", s.onto_name.c_str(), s.branch.c_str());
        std::fclose(f);
    }
    stage_ = rebase_stage::running;
    hooks_.stage_changed();
    hooks_.log("Start Rebase\n", true);

    // Fast-forward: move the branch to the upstream and stop [spec 2.7.9].
    if (s.fast_forward)
    {
        std::string onto = s.onto.hex();
        run({"checkout", "--no-track", "-f", "-B", s.branch, onto},
            [this, onto](int code, auto&)
            {
                if (code != 0) return stop(rebase_stage::error);
                run({"reset", "--hard", onto},
                    [this](int code, auto&)
                    {
                        if (code != 0) return stop(rebase_stage::error);
                        hooks_.log("Fast forward to \"" + start_.onto.hex(8) + "\"\n", true);
                        stop(rebase_stage::done);
                    });
            });
        return;
    }

    pos_ = int(model_.size()) - 1;
    run({"checkout", "-f", "--detach", s.onto.hex()},
        [this](int code, auto&)
        {
            if (code != 0) return stop(rebase_stage::error);
            step();
        });
}

void rebase_engine::stop(rebase_stage s)
{
    stage_ = s;
    hooks_.stage_changed();
}

// Applies the row at pos_, skipping Skip rows. A Pick or Edit with Squash
// rows above it starts a squash group: its members are picked without
// committing, and the group is committed once with a combined message.
void rebase_engine::step()
{
    size_t total = model_.size();
    while (pos_ >= 0 && model_.row(size_t(pos_)).action == rebase_action::skip && !squash_.active)
    {
        model_.set_state(size_t(pos_), true, false);
        --pos_;
    }
    if (pos_ < 0) return finish();

    auto i = size_t(pos_);
    const rebase_row& row = model_.rows()[i];
    model_.set_state(i, false, true);
    hooks_.progress(total - i, total);

    std::string hash = row.commit.id.hex();
    bool squash_above = i > 0 && model_.rows()[i - 1].action == rebase_action::squash;

    if (row.action == rebase_action::squash || squash_above)
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

    if (row.action == rebase_action::edit)
    {
        run({"cherry-pick", hash},
            [this](int code, const std::string& out)
            {
                if (code != 0) return picked(code, out, false);
                hooks_.message(model_.rows()[size_t(pos_)].commit.message);
                stop(rebase_stage::edit);
            });
        return;
    }

    // --ff keeps the commit as is when it already sits on the new base.
    run({"cherry-pick", "--ff", hash},
        [this](int code, const std::string& out) { picked(code, out, false); });
}

// After a cherry-pick: on success carry on; on conflicts stop for the user;
// on other failures (an empty pick, say) ask whether to skip it.
void rebase_engine::picked(int code, const std::string& output, bool in_group)
{
    if (code == 0)
    {
        if (!in_group) return advance();
        // Inside a squash group: keep picking until the top of the group.
        auto i = size_t(pos_);
        if (i > 0 && model_.rows()[i - 1].action == rebase_action::squash) return advance();
        hooks_.message(squash_.message);
        return stop(rebase_stage::squash_edit);
    }

    run(
        {"diff", "--name-only", "--diff-filter=U"},
        [this, output, in_group](int, const std::string& u)
        {
            if (!u.empty())
            {
                hooks_.message(in_group ? squash_.message
                                        : model_.rows()[size_t(pos_)].commit.message);
                return stop(in_group ? rebase_stage::squash_conflict : rebase_stage::conflict);
            }
            hooks_.ask_skip(output,
                            [this](bool skip)
                            {
                                if (!skip) return stop(rebase_stage::error);
                                run({"reset", "--hard", "HEAD"},
                                    [this](int, auto&)
                                    {
                                        run({"cherry-pick", "--quit"},
                                            [this](int, auto&)
                                            {
                                                model_.row(size_t(pos_)).action =
                                                    rebase_action::skip;
                                                advance();
                                            });
                                    });
                            });
        },
        true);
}

void rebase_engine::advance()
{
    auto i = size_t(pos_);
    model_.set_state(i, true, false);
    --pos_;
    stage_ = rebase_stage::running;
    hooks_.stage_changed();
    step();
}

void rebase_engine::write_message(const std::string& message)
{
    if (std::FILE* f = std::fopen(msg_file_.c_str(), "w"))
    {
        std::fwrite(message.data(), 1, message.size(), f);
        std::fclose(f);
    }
}

std::string rebase_engine::author_arg(const commit& c) const
{
    return "--author=" + c.author + " <" + c.email + ">";
}

std::string rebase_engine::date_arg(const commit& c) const
{
    char tz[16];
    std::snprintf(tz, sizeof tz, "%+03d%02d", c.offset_minutes / 60,
                  std::abs(c.offset_minutes % 60));
    return "--date=" + std::to_string(c.time) + " " + tz;
}

// Squash groups take the first commit's author and date [spec 2.7.5].
void rebase_engine::commit_squash(const std::string& message)
{
    write_message(message);
    const commit& first = model_.rows()[squash_.first].commit;
    run({"commit", "-F", msg_file_, author_arg(first), date_arg(first)},
        [this](int code, auto&)
        {
            if (code != 0) return stop(rebase_stage::error);
            // Every row of the group is applied now.
            for (size_t i = size_t(pos_); i <= squash_.first; ++i) model_.set_state(i, true, false);
            squash_ = {};
            --pos_;
            stage_ = rebase_stage::running;
            hooks_.stage_changed();
            step();
        });
}

void rebase_engine::resume(const std::string& message)
{
    switch (stage_)
    {
    case rebase_stage::conflict:
    case rebase_stage::squash_conflict:
    {
        bool in_group = stage_ == rebase_stage::squash_conflict;
        // Unmerged files still holding conflict markers block the commit
        // [spec 2.7.3]; the rest count as resolved and are added.
        // TODO: per-file "Resolved" in the conflict list, as TortoiseGit has.
        run(
            {"diff", "--name-only", "--diff-filter=U"},
            [this, message, in_group](int, const std::string& unmerged)
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
                    if (has_conflict_markers(workdir_ + "/" + f)) marked.push_back(f);
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
                    [this, message, in_group](int code, auto&)
                    {
                        if (code != 0) return stop(rebase_stage::error);
                        if (in_group)
                        {
                            squash_.message = message;
                            stage_ = rebase_stage::running;
                            return picked(0, {}, true);
                        }
                        const commit& c = model_.rows()[size_t(pos_)].commit;
                        write_message(message);
                        run({"commit", "-F", msg_file_, author_arg(c), date_arg(c)},
                            [this](int code, auto&)
                            {
                                if (code != 0) return stop(rebase_stage::error);
                                advance();
                            });
                    });
            },
            true);
        break;
    }
    case rebase_stage::edit:
    {
        // Amend with the (possibly edited) message and any changes made.
        write_message(message);
        run({"commit", "--amend", "--all", "-F", msg_file_},
            [this](int code, auto&)
            {
                if (code != 0) return stop(rebase_stage::error);
                advance();
            });
        break;
    }
    case rebase_stage::squash_edit: commit_squash(message); break;
    default: break;
    }
}

// Move the branch to the rebased commits and check it out [spec 2.7.6].
void rebase_engine::finish()
{
    run({"checkout", "-B", start_.branch},
        [this](int code, auto&)
        {
            if (code != 0) return stop(rebase_stage::error);
            std::remove(msg_file_.c_str());
            std::remove(active_file_.c_str());
            hooks_.log("Finished rebasing.\n", true);
            stop(rebase_stage::done);
        });
}

// The branch only moves at the end, so aborting is: drop any pick in
// progress and check out what was checked out before [spec 2.7.8].
void rebase_engine::abort(std::function<void()> done)
{
    if (process_) process_->kill();
    if (!started() || stage_ == rebase_stage::done)
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
                    run({"checkout", "-f", start_.checked_out},
                        [this, done](int, auto&)
                        {
                            std::remove(msg_file_.c_str());
                            std::remove(active_file_.c_str());
                            hooks_.log("Rebase aborted.\n", true);
                            stage_ = rebase_stage::done;
                            hooks_.stage_changed();
                            if (done) done();
                        });
                });
        });
}

}  // namespace tortilla
