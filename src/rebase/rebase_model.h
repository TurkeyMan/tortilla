// The Rebase dialog's commit list: the commits to replay, newest at the top,
// each with an action. Commits are applied from the bottom up, and "squash"
// folds a commit into the one below it. Every setter enforces TortoiseGit's
// rules, so invalid states can't happen [spec 2.3-2.6].
#pragma once

#include "git/repo.h"

#include <rapport/rapport.h>

namespace tortilla
{

enum class rebase_action
{
    pick,
    squash,  // with the commit below
    edit,
    skip,
};

const char* action_name(rebase_action);

struct rebase_row
{
    tortilla::commit commit;
    rebase_action action = rebase_action::pick;
    bool done = false;     // already applied
    bool current = false;  // being applied
};

class rebase_model : public rap::list_model
{
public:
    enum column : size_t
    {
        action,
        id,
        hash,
        message,
        author,
        date,
        column_count
    };

    static std::vector<rap::column> columns();

    void set(std::vector<rebase_row> rows, ref_map refs);
    const std::vector<rebase_row>& rows() const { return rows_; }
    rebase_row& row(size_t i) { return rows_[i]; }
    size_t size() const { return rows_.size(); }

    // False when the rules forbid it: done/current rows never change,
    // squash isn't allowed on the bottom row or on merge commits.
    bool can_set(size_t row, rebase_action) const;
    bool set_action(size_t row, rebase_action);
    // Space: Pick -> Skip -> Edit -> Squash -> Pick, skipping disallowed ones.
    void cycle(const std::vector<size_t>& rows);
    void set_all(rebase_action);
    void set_unselected(const std::vector<size_t>& selected, rebase_action);

    // Moves the selected rows one place (or to the end) and returns the new
    // selection. Nothing moves when the block is already at that end.
    std::vector<size_t> move(const std::vector<size_t>& selected, bool up, bool to_end);
    // Moves the selected rows to stand in front of `destination` (an index
    // before the move; size() = the end). Returns the new selection.
    std::vector<size_t> move_to(const std::vector<size_t>& selected, size_t destination);

    void set_state(size_t row, bool done, bool current);
    void changed(size_t row) { notify_changed(row, 1); }

    // rap::list_model
    size_t row_count() const override { return rows_.size(); }
    std::string text(size_t row, size_t column) const override;
    rap::text_style style(size_t row, size_t column) const override;
    void paint(size_t row, size_t column, rap::painter&, const rap::cell_info&) const override;

private:
    std::vector<rebase_row> rows_;
    ref_map refs_;
};

}  // namespace tortilla
