#include "history_writer.h"

#include "git/command.h"
#include "i18n.h"
#include "util/notices.h"

#include <cstdio>

namespace tortilla
{
namespace
{

// An outcome with no window left to take it: on stderr, so a write is never
// silent; and a warning or failure saved for the next window on the
// repository too, as a GUI launch on Windows has no stderr.
void log_orphan(const history_outcome& o, const std::filesystem::path& notices,
                const std::string& repo, const std::function<void(const std::string&)>& unsaved)
{
    const std::string text = orphan_text(o);
    std::fprintf(stderr, "tortilla: %s\n", text.c_str());
    const bool plain = o.applied && o.applied->empty();
    if (!plain && !save_notice(notices, repo, text))
    {
        std::fprintf(stderr, "tortilla: (this couldn't be saved for the next window)\n");
        if (unsaved) unsaved(text);
    }
}

}  // namespace

bool record_outcome(ref_journal& journal, const history_outcome& o)
{
    // Ref commands: whatever moved is a step, done or failed part way.
    if (o.commands)
    {
        if (o.edit.moves.empty()) return false;
        journal.record(o.label, o.edit);
        return true;
    }
    if (!o.applied)
    {
        const history_edit& moved = o.applied.error().moved;
        if (moved.moves.empty()) return false;
        journal.record(rap::i18n::format(_("{0} (in part)"), o.label), moved, true);
        return true;
    }
    switch (o.use)
    {
    case journal_use::record: journal.record(o.label, o.edit); break;
    case journal_use::undo: journal.undone(); break;
    case journal_use::redo: journal.redone(); break;
    }
    return true;
}

std::string orphan_text(const history_outcome& o)
{
    if (o.applied)
    {
        std::string text = rap::i18n::format(_("{0}: done, after its window was closed."), o.label);
        if (!o.applied->empty()) text += "\n" + *o.applied;
        return text;
    }
    std::string text = rap::i18n::format(_("{0}: failed, after its window was closed."), o.label) +
                       "\n" + o.applied.error().message;
    const history_edit& moved = o.commands ? o.edit : o.applied.error().moved;
    if (!moved.moves.empty())
    {
        text += "\n" + std::string(_("These refs were left moved:"));
        for (const ref_move& m : moved.moves)
            text += "\n  " + rap::i18n::format(_("{0}: now {1}, was {2}"), m.ref,
                                               m.to ? m.to->hex() : std::string(_("deleted")),
                                               m.from ? m.from->hex() : std::string(_("none")));
    }
    return text;
}

history_writer::history_writer(rap::ui& ui, std::string path, ref_journal& journal)
    : write([](const repo& r, const history_edit& e, bool stash)
            { return apply_edit(r, e, stash); })
    , notices(notices_dir())
    , ui_(ui)
    , path_(std::move(path))
    , journal_(journal)
{
    thread_ = std::thread(
        [state = state_, path = path_]
        {
            auto opened = repo::open(path);
            const repo* r = opened ? &*opened : nullptr;
            for (;;)
            {
                job next;
                {
                    std::unique_lock lock(state->mutex);
                    state->wake.wait(lock, [&] { return state->stop || !state->jobs.empty(); });
                    if (state->jobs.empty()) return;
                    next = std::move(state->jobs.front());
                    state->jobs.pop_front();
                    // Going: a write accepted still runs; a replay is pointless.
                    if (state->stop && !next.writes) continue;
                }
                next.run(r);
            }
        });
}

history_writer::~history_writer()
{
    {
        std::lock_guard lock(state_->mutex);
        state_->stop = true;
    }
    state_->wake.notify_all();
    thread_.join();
    // Written, and its post not handled (it never will be now).
    if (state_->undelivered) log_orphan(*state_->undelivered, notices, path_, unsaved);
    state_->undelivered.reset();
}

bool history_writer::begin()
{
    if (busy_) return false;
    busy_ = true;
    return true;
}

void history_writer::submit(job j)
{
    {
        std::lock_guard lock(state_->mutex);
        state_->jobs.push_back(std::move(j));
    }
    state_->wake.notify_one();
}

void history_writer::prepare(std::function<std::function<void()>(const repo&)> replay)
{
    submit({.run = [this, &ui = ui_, state = state_, replay = std::move(replay)](const repo* r)
            {
                std::function<void()> then = r ? replay(*r) : nullptr;
                // `this` only while the writer stands: it's gone once stopped.
                ui.post(
                    [this, state, then = std::move(then)]
                    {
                        {
                            std::lock_guard lock(state->mutex);
                            if (state->stop) return;
                        }
                        if (then)
                            then();
                        else
                            busy_ = false;
                    });
            }});
}

void history_writer::deliver(history_outcome o, std::function<void(const history_outcome&)> done)
{
    // Held until the UI thread takes it, or, the writer gone first, its
    // destructor: one or the other, never both.
    {
        std::lock_guard lock(state_->mutex);
        state_->undelivered = std::move(o);
    }
    ui_.post(
        [this, state = state_, done = std::move(done)]
        {
            std::optional<history_outcome> got;
            {
                std::lock_guard lock(state->mutex);
                if (state->stop) return;
                got = std::move(state->undelivered);
                state->undelivered.reset();
            }
            if (!got) return;
            record_outcome(journal_, *got);
            busy_ = false;
            if (done) done(*got);
        });
}

void history_writer::apply(std::string title, std::string label, history_edit e, journal_use use,
                           bool stash, std::function<void(const history_outcome&)> done)
{
    history_outcome o{.title = std::move(title),
                      .label = std::move(label),
                      .edit = std::move(e),
                      .use = use,
                      .applied = {}};
    submit({.run =
                [this, write = this->write, after = this->after_write, o = std::move(o), stash,
                 done = std::move(done)](const repo* r) mutable
            {
                if (r)
                    o.applied = write(*r, o.edit, stash);
                else
                    o.applied =
                        std::unexpected(apply_failure{_("The repository can't be opened.")});
                if (after) after();
                deliver(std::move(o), std::move(done));
            },
            .writes = true});
}

void history_writer::run_commands(std::string title, std::string label,
                                  std::vector<std::vector<std::string>> commands,
                                  std::vector<std::string> refs,
                                  std::function<void(const history_outcome&)> done)
{
    history_outcome o{.title = std::move(title),
                      .label = std::move(label),
                      .edit = {},
                      .use = journal_use::record,
                      .applied = {},
                      .commands = true};
    submit({.run =
                [this, after = this->after_write, o = std::move(o), commands = std::move(commands),
                 refs = std::move(refs), done = std::move(done)](const repo* r) mutable
            {
                o.edit.reflog = "tortilla: " + o.label;
                if (!r)
                    o.applied =
                        std::unexpected(apply_failure{_("The repository can't be opened.")});
                else
                {
                    // The refs as they were, then each command until one fails.
                    for (const std::string& name : refs)
                        o.edit.moves.push_back({name, ref_target(*r, name), std::nullopt});
                    std::string output;
                    bool ok = true;
                    for (const auto& args : commands)
                    {
                        output += command_line(args) + "\n";
                        const git_result run =
                            run_git(path_, args, git_output::all, git_prompts::never);
                        output += run.output;
                        if (!(ok = run.ok())) break;
                    }
                    std::erase_if(o.edit.moves,
                                  [&](ref_move& m)
                                  {
                                      m.to = ref_target(*r, m.ref);
                                      return m.to == m.from;
                                  });
                    if (!ok) o.applied = std::unexpected(apply_failure{output, o.edit});
                }
                if (after) after();
                deliver(std::move(o), std::move(done));
            },
            .writes = true});
}

}  // namespace tortilla
