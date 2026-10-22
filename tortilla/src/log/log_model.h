// The log window's commit list: history loaded on a worker thread in
// batches, graph rows computed alongside, and TortoiseGit's drawing of the
// graph, the Actions icons and the ref labels. See docs/tortoisegit-spec.md
// sections 1.4-1.9.
#pragma once

#include "git/command.h"
#include "git/repo.h"
#include "graph/lanes.h"
#include "log/action_icons.h"
#include "log/ref_labels.h"

#include <rapport/rapport.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <regex>
#include <thread>

namespace tortilla
{

// Which commit fields the search box looks at [spec 1.9.2].
enum FilterField : unsigned
{
    FilterMessages = 0x01,
    FilterPaths = 0x02,  // TODO: needs per-commit file lists
    FilterAuthors = 0x04,
    FilterRevs = 0x08,  // SHA-1
    FilterSubject = 0x40,
    FilterRefname = 0x80,
    FilterEmails = 0x100,
    FilterAll = 0xffff,
};

struct LogFilter
{
    std::string text;  // empty = no filter
    unsigned fields = FilterAll;
    bool regex = false;
    bool caseSensitive = false;
};

// Ref label kinds the View > Labels menu can hide.
enum LabelKind : unsigned
{
    LabelTags = 1,
    LabelLocal = 2,
    LabelRemote = 4,
    LabelOther = 8,
    LabelAll = 15,
};

class LogModel : public rap::ListModel
{
public:
    enum Column : size_t
    {
        Graph,
        Actions,
        Message,  // painted: ref labels, then the subject
        Author,
        Date,
        Hash,  // the rest are hidden by default
        Email,
        CommitName,
        CommitEmail,
        CommitDate,
        ColumnCount
    };

    explicit LogModel(rap::Ui& ui);
    ~LogModel() override;

    static std::vector<rap::Column> columns();

    // Starts (or restarts) loading; `done` runs on the UI thread when the
    // walk finishes or fails (empty string = success).
    void load(const std::string& path, const WalkOptions&,
              std::function<void(const std::string& error)> done);
    void cancel();
    bool loading() const { return loading_; }
    // Re-reads the refs (branches and tags moved, made or deleted) without
    // walking the history again, repainting only rows whose labels changed.
    // `done(true)` when the commits shown could have changed too (all
    // branches, and a ref now names a commit not loaded, or a commit lost
    // its last label with nothing loaded above it): the caller reloads.
    void refreshRefs(std::function<void(bool reload)> done);
    bool applyRefs(RefMap next);  // true: reload needed

    // Returns an error message for an invalid regex, else empty.
    std::string setFilter(const LogFilter&);
    bool filtering() const { return filtered_; }

    void setActionIconStyle(ActionIconStyle s)
    {
        iconStyle_ = s;
        notifyChanged(0, rowCount());
    }
    ActionIconStyle actionIconStyle() const { return iconStyle_; }

    void setLabels(unsigned mask);
    unsigned labels() const { return labels_; }

    // Row 0 is the "Working tree changes" pseudo-row in non-bare repos.
    bool isWorkingTree(size_t row) const { return workingTree_ && row == 0; }
    const Commit* commit(size_t row) const;
    std::optional<size_t> rowOf(const Oid&) const;
    size_t commitRows() const { return rowCount() - (workingTree_ ? 1 : 0); }
    const std::vector<FileChange>& workingTreeFiles() const { return wtFiles_; }
    // The working-tree status arrives after the history (it can take many
    // seconds on a big or slow tree); until then the row says so.
    bool workingTreeKnown() const { return wtKnown_; }
    void onWorkingTree(std::function<void()> f) { onWorkingTree_ = std::move(f); }
    const std::vector<Ref>* refsOf(size_t row) const;
    // The label painted at `x` (Message column, cell coordinates) in `row`,
    // as last painted.
    std::optional<Ref> labelAt(size_t row, float x) const;
    bool isHead(size_t row) const;
    const std::optional<Oid>& head() const { return head_; }
    Repo* repo() { return repo_ ? &*repo_ : nullptr; }

    // rap::ListModel
    size_t rowCount() const override;
    std::string text(size_t row, size_t column) const override;
    rap::TextStyle style(size_t row, size_t column) const override;
    void paint(size_t row, size_t column, rap::Painter&, const rap::CellInfo&) const override;

private:
    struct Batch
    {
        std::vector<Commit> commits;
        std::vector<GraphRow> rows;
    };

    void append(Batch&&);
    // Commit index for a non-working-tree row.
    size_t index(size_t row) const;
    std::optional<size_t> rowOfIndex(size_t index) const;  // the row showing commits_[index]
    bool matches(const Commit&) const;
    void refilter();
    uint8_t actionsFor(size_t row) const;
    void startActions(unsigned generation);
    void startStatus();
    void stopActions();
    bool showLabel(const Ref&) const;

    void paintGraph(const GraphRow&, rap::Painter&, const rap::CellInfo&) const;
    void paintActions(uint8_t mask, rap::Painter&, const rap::CellInfo&) const;
    void paintMessage(size_t row, rap::Painter&, const rap::CellInfo&) const;

    rap::Ui& ui_;
    std::optional<Repo> repo_;  // UI-thread repository, for lazy lookups
    std::vector<Commit> commits_;
    std::vector<GraphRow> rows_;
    mutable std::vector<uint8_t> actions_;  // per commit; kUnknown until first painted
    // Computed actions by commit, kept across reloads: a commit's changes
    // never change, and recomputing them after every branch or tag change
    // filled the column in again row by row.
    std::unordered_map<Oid, uint8_t, OidHash> actionsCache_;
    RefMap refs_;
    mutable std::unordered_map<Oid, std::vector<LabelBox>, OidHash> labelBoxes_;  // as last painted
    std::optional<Oid> head_;
    bool workingTree_ = false;
    bool wtKnown_ = false;
    std::vector<FileChange> wtFiles_;
    std::function<void()> onWorkingTree_;
    std::array<std::shared_ptr<GitProcess>, 2> status_;  // status, numstat
    std::string path_;

    // Actions icons are computed on their own thread for the rows being
    // drawn, newest requests first [spec note 4].
    struct ActionsWork;
    std::shared_ptr<ActionsWork> actionsWork_;
    unsigned labels_ = LabelAll;
    ActionIconStyle iconStyle_ = ActionIconStyle::Document;
    bool loading_ = false;
    bool allBranches_ = false;  // the loaded walk's

    // Filtering: the matching commit indices, in order.
    struct Term
    {
        std::string text;
        bool exclude = false;
    };
    LogFilter filter_;
    bool filtered_ = false;
    bool invert_ = false;
    std::vector<Term> terms_;
    std::optional<std::regex> regex_;
    std::vector<size_t> visible_;

    std::jthread worker_;
    // Bumped on every load/cancel; batches from an older generation are dropped.
    std::shared_ptr<std::atomic<unsigned>> generation_ = std::make_shared<std::atomic<unsigned>>(0);
};

}  // namespace tortilla
