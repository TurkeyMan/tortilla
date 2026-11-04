// What Tortilla did to a repository's refs from a log window, for its Undo
// and Redo: each step the refs it moved, from what to what, and a label
// ("Reword 1a2b3c4d"). Undoing puts the refs back only if every one is still
// where the step left it, and redoing only if each is back where it was, so
// nothing anyone did since is overwritten (apply_edit's compare-and-swap).
// Kept in memory, the newest steps only.
#pragma once

#include "git/history_edit.h"

#include <deque>
#include <optional>
#include <string>
#include <vector>

namespace tortilla
{

struct journal_step
{
    std::string label;  // "Reword 1a2b3c4d", for "Undo Reword 1a2b3c4d"
    history_edit edit;  // as it was applied
    // Undone, it's gone, never redone: refs a failed edit left moved, whose
    // files never followed them (apply_failure::moved). Redoing it would
    // move them again without their files.
    bool one_way = false;
};

class ref_journal
{
public:
    explicit ref_journal(size_t limit = 100)
        : limit_(limit)
    {
    }

    // A step done: on top of the undo list; what was undone can't be redone
    // after it. The oldest go past the limit.
    void record(std::string label, history_edit, bool one_way = false);

    // The step Undo or Redo would take, if any.
    const journal_step* next_undo() const { return done_.empty() ? nullptr : &done_.back(); }
    const journal_step* next_redo() const { return undone_.empty() ? nullptr : &undone_.back(); }
    // The edit that undoes next_undo(), and the one that redoes next_redo().
    std::optional<history_edit> undo_edit() const;
    std::optional<history_edit> redo_edit() const;
    // After the edit applied: the step goes to the other list (a one-way
    // step undone goes altogether).
    void undone();
    void redone();
    // A step that can't be taken (its refs moved since): gone.
    void forget_undo();
    void forget_redo();

    size_t undo_count() const { return done_.size(); }
    size_t redo_count() const { return undone_.size(); }

private:
    size_t limit_;
    std::deque<journal_step> done_;     // oldest first
    std::vector<journal_step> undone_;  // the next to redo last
};

// The ref's own value, unpeeled (an annotated tag's object, not its commit);
// nullopt when it doesn't exist.
std::optional<oid> ref_target(const repo&, const std::string& full_name);

// Why `e` can't be applied as it stands: a ref it moves isn't where it moves
// from ("main has moved since"), or one it makes exists; nullopt when every
// one is.
std::optional<std::string> why_stale(const repo&, const history_edit& e);

}  // namespace tortilla
