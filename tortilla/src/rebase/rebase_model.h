// The Rebase dialog's commit list: the commits to replay, newest at the top,
// each with an action. Commits are applied from the bottom up, and "squash"
// folds a commit into the one below it. Every setter enforces TortoiseGit's
// rules, so invalid states can't happen [spec 2.3-2.6].
#pragma once

#include "git/repo.h"

#include <rapport/rapport.h>

namespace tortilla
{

enum class RebaseAction
{
    Pick,
    Squash,  // with the commit below
    Edit,
    Skip,
};

const char* actionName(RebaseAction);

struct RebaseRow
{
    Commit commit;
    RebaseAction action = RebaseAction::Pick;
    bool done = false;     // already applied
    bool current = false;  // being applied
};

class RebaseModel : public rap::ListModel
{
public:
    enum Column : size_t
    {
        Action,
        Id,
        Hash,
        Message,
        Author,
        Date,
        ColumnCount
    };

    static std::vector<rap::Column> columns();

    void set(std::vector<RebaseRow> rows, RefMap refs);
    const std::vector<RebaseRow>& rows() const { return rows_; }
    RebaseRow& row(size_t i) { return rows_[i]; }
    size_t size() const { return rows_.size(); }

    // False when the rules forbid it: done/current rows never change,
    // squash isn't allowed on the bottom row or on merge commits.
    bool canSet(size_t row, RebaseAction) const;
    bool setAction(size_t row, RebaseAction);
    // Space: Pick -> Skip -> Edit -> Squash -> Pick, skipping disallowed ones.
    void cycle(const std::vector<size_t>& rows);
    void setAll(RebaseAction);
    void setUnselected(const std::vector<size_t>& selected, RebaseAction);

    // Moves the selected rows one place (or to the end) and returns the new
    // selection. Nothing moves when the block is already at that end.
    std::vector<size_t> move(const std::vector<size_t>& selected, bool up, bool toEnd);
    // Moves the selected rows to stand in front of `destination` (an index
    // before the move; size() = the end). Returns the new selection.
    std::vector<size_t> moveTo(const std::vector<size_t>& selected, size_t destination);

    void setState(size_t row, bool done, bool current);
    void changed(size_t row) { notifyChanged(row, 1); }

    // rap::ListModel
    size_t rowCount() const override { return rows_.size(); }
    std::string text(size_t row, size_t column) const override;
    rap::TextStyle style(size_t row, size_t column) const override;
    void paint(size_t row, size_t column, rap::Painter&, const rap::CellInfo&) const override;

private:
    std::vector<RebaseRow> rows_;
    RefMap refs_;
};

}  // namespace tortilla
