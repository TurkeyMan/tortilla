#include "history_writer.h"

#include "i18n.h"

#include <cstdio>

namespace tortilla
{
namespace
{

// An outcome with no window left to take it: on stderr, so a write is never
// silent, with the refs a failure left moved named for putting back by hand.
void log_orphan(const history_outcome& o)
{
    if (o.applied)
    {
        std::fprintf(stderr, "tortilla: %s: done after its window closed (not in Undo)\n",
                     o.label.c_str());
        return;
    }
    std::fprintf(stderr, "tortilla: %s: failed after its window closed: %s\n", o.label.c_str(),
                 o.applied.error().message.c_str());
    for (const ref_move& m : o.applied.error().moved.moves)
        std::fprintf(stderr, "tortilla:   %s left at %s (was %s)\n", m.ref.c_str(),
                     m.to ? m.to->hex().c_str() : "(deleted)",
                     m.from ? m.from->hex().c_str() : "(none)");
}

}  // namespace

bool record_outcome(ref_journal& journal, const history_outcome& o)
{
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

history_writer::history_writer(rap::ui& ui, std::string path, ref_journal& journal)
    : write([](const repo& r, const history_edit& e, bool stash)
            { return apply_edit(r, e, stash); })
    , ui_(ui)
    , journal_(journal)
{
    thread_ = std::thread(
        [state = state_, path = std::move(path)]
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
    if (state_->undelivered) log_orphan(*state_->undelivered);
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

void history_writer::apply(std::string title, std::string label, history_edit e, journal_use use,
                           bool stash, std::function<void(const history_outcome&)> done)
{
    history_outcome o{.title = std::move(title),
                      .label = std::move(label),
                      .edit = std::move(e),
                      .use = use,
                      .applied = {}};
    submit({.run =
                [this, &ui = ui_, state = state_, write = this->write, after = this->after_write,
                 o = std::move(o), stash, done = std::move(done)](const repo* r) mutable
            {
                if (r)
                    o.applied = write(*r, o.edit, stash);
                else
                    o.applied =
                        std::unexpected(apply_failure{_("The repository can't be opened.")});
                if (after) after();
                {
                    std::lock_guard lock(state->mutex);
                    state->undelivered = std::move(o);
                }
                ui.post(
                    [this, state, done]
                    {
                        std::optional<history_outcome> got;
                        {
                            std::lock_guard lock(state->mutex);
                            // Gone: its destructor logged the outcome.
                            if (state->stop) return;
                            got = std::move(state->undelivered);
                            state->undelivered.reset();
                        }
                        if (!got) return;
                        record_outcome(journal_, *got);
                        busy_ = false;
                        if (done) done(*got);
                    });
            },
            .writes = true});
}

}  // namespace tortilla
