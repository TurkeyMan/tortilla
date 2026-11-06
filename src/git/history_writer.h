// History edits' own worker [git/history_edit.h], apart from repo_worker's
// newest-only reads: an edit, once accepted, is never replaced or dropped,
// and its outcome reaches the journal exactly once, a partial failure's too.
//
// One edit at a time: begin() claims the writer, and it stays claimed from
// the replay, through the question about local changes, to the write; it's
// free again once the write's outcome is in the journal, or when the edit is
// given up before anything is written (cancel()). Meanwhile another edit, an
// Undo or a Redo is refused, and nothing is touched.
//
// The window closing during an edit: a write already accepted still runs to
// its end on the worker (the destructor waits for it), and its outcome, with
// no journal left to go in, is written to stderr instead; the window is
// never called. A replay not yet written is just dropped: nothing changed.
#pragma once

#include "git/history_edit.h"
#include "git/ref_journal.h"
#include "git/repo.h"

#include <rapport/rapport.h>

#include <condition_variable>
#include <deque>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace tortilla
{

// What an edit does to the journal once written: a new step, or the
// journal's own undoing or redoing of one.
enum class journal_use
{
    record,
    undo,
    redo,
};

// An accepted write and how it went: apply_edit's warning, or its failure
// (with the refs it left moved, if any).
struct history_outcome
{
    std::string title;  // "Reword"
    std::string label;  // its journal step's ("Reword 1a2b3c4d")
    history_edit edit;
    journal_use use = journal_use::record;
    std::expected<std::string, apply_failure> applied;
};

// The journal's side of an outcome: the step recorded, undone or redone; or,
// a failed write, the refs it left moved as a one-way step of their own
// ("Reword 1a2b3c4d (in part)"): Undo puts them back and nothing redoes them
// (their files never followed). True when the journal changed.
bool record_outcome(ref_journal&, const history_outcome&);

class history_writer
{
public:
    using write_fn = std::function<std::expected<std::string, apply_failure>(
        const repo&, const history_edit&, bool stash)>;

    // `journal` outlives the writer; it's touched on the UI thread only.
    history_writer(rap::ui&, std::string path, ref_journal& journal);
    ~history_writer();

    // An edit is outstanding: claimed and its outcome not yet handled.
    bool busy() const { return busy_; }
    // Claims the writer for a new edit; false (refused) while one is
    // outstanding.
    bool begin();
    // The claimed edit given up before writing: refused, failed to replay,
    // or the question about local changes declined.
    void cancel() { busy_ = false; }

    // The claimed edit's replay, on the worker: what it returns runs on the
    // UI thread, unless the writer has gone by then (nothing was written).
    void prepare(std::function<std::function<void()>(const repo&)> replay);
    // The claimed edit's write, on the worker, then on the UI thread: its
    // outcome recorded in the journal, the writer freed, and `done` called.
    void apply(std::string title, std::string label, history_edit, journal_use, bool stash,
               std::function<void(const history_outcome&)> done);

    // For tests: the write itself (apply_edit), and a hook run on the
    // worker after it, before its outcome is posted to the UI thread.
    write_fn write;
    std::function<void()> after_write;

    struct job
    {
        std::function<void(const repo*)> run;
        bool writes = false;  // runs even once the writer is going
    };
    struct state
    {
        std::mutex mutex;
        std::condition_variable wake;
        std::deque<job> jobs;  // in order, never replaced
        bool stop = false;
        // A write's outcome posted and not yet handled on the UI thread.
        std::optional<history_outcome> undelivered;
    };

private:
    void submit(job);

    rap::ui& ui_;
    ref_journal& journal_;
    bool busy_ = false;
    std::shared_ptr<state> state_ = std::make_shared<state>();
    std::thread thread_;
};

}  // namespace tortilla
