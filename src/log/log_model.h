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
#include <optional>
#include <regex>
#include <string_view>
#include <thread>

namespace tortilla
{

// Which commit fields the search box looks at [spec 1.9.2].
enum filter_field : unsigned
{
    filter_messages = 0x01,
    filter_paths = 0x02,  // TODO: needs per-commit file lists
    filter_authors = 0x04,
    filter_revs = 0x08,  // SHA-1
    filter_subject = 0x40,
    filter_refname = 0x80,
    filter_emails = 0x100,
    filter_all = 0xffff,
};

struct log_filter
{
    std::string text;  // empty = no filter
    unsigned fields = filter_all;
    bool regex = false;
    bool case_sensitive = false;
};

// Ref label kinds the View > Labels menu can hide.
enum label_kind : unsigned
{
    label_tags = 1,
    label_local = 2,
    label_remote = 4,
    label_other = 8,
    label_all = 15,
};

// The commits a selection in the log asks Format Patch for [spec 1.11]:
// everything from `oldest` to `newest` along history, or that one commit
// when they're the same.
struct patch_range
{
    std::string oldest, newest;
    bool oldest_is_root = false;  // no parent for the range to start after
};

// `git format-patch` writing `range` to its output, the patches one after
// another as git am takes them, for the unified diff window to show; or,
// given a folder, into it as a numbered file per commit.
std::vector<std::string> format_patch_args(const patch_range& range,
                                           const std::string& folder = {});

// Edit Notes' save [spec 1.11]: `text` (written to `file`) as the
// commit's note, or the note taken away when `text` is only blank.
std::vector<std::string> note_args(const std::string& hash, const std::string& text,
                                   const std::string& file);

// git bisect's steps: the log's and Explorer's Bisect entries [spec 1.11,
// 4.2 rows 28-32], and `tortilla bisect <step>`.
enum class bisect_step
{
    start,
    good,
    bad,
    skip,
    reset,
};

// "start", "good", ... as the command line names them; nullopt otherwise.
std::optional<bisect_step> parse_bisect_step(std::string_view);

// `git bisect` for `step`. start takes the bad revision then the good one,
// the order git wants; good, bad and skip the revisions they mark (none:
// HEAD, the one checked out); reset none.
std::vector<std::string> bisect_args(bisect_step, const std::vector<std::string>& revs = {});

class log_model : public rap::list_model
{
public:
    enum column : size_t
    {
        graph,
        actions,
        message,  // painted: ref labels, then the subject
        author,
        date,
        hash,  // the rest are hidden by default
        email,
        commit_name,
        commit_email,
        commit_date,
        column_count
    };

    explicit log_model(rap::ui& ui);
    ~log_model() override;

    static std::vector<rap::column> columns();

    // Starts (or restarts) loading; `done` runs on the UI thread when the
    // walk finishes or fails (empty string = success).
    void load(const std::string& path, const walk_options&,
              std::function<void(const std::string& error)> done);
    void cancel();
    bool loading() const { return loading_; }
    // Every ref by the commit it names, as last loaded (all of them, whatever
    // the walk); empty until they arrive.
    const ref_map& refs() const { return refs_; }
    // Re-reads the refs (branches and tags moved, made or deleted) without
    // walking the history again, repainting only rows whose labels changed.
    // `done(true)` when the commits shown could have changed too (all
    // branches, and a ref now names a commit not loaded, or a commit lost
    // its last label with nothing loaded above it): the caller reloads.
    void refresh_refs(std::function<void(bool reload)> done);
    bool apply_refs(ref_map next);  // true: reload needed

    // Returns an error message for an invalid regex, else empty.
    std::string set_filter(const log_filter&);
    bool filtering() const { return filtered_; }

    void set_action_icon_style(action_icon_style s)
    {
        icon_style_ = s;
        notify_changed(0, row_count());
    }
    action_icon_style icon_style() const { return icon_style_; }

    void set_labels(unsigned mask);
    unsigned labels() const { return labels_; }

    // Row 0 is the "Working tree changes" pseudo-row in non-bare repos.
    bool is_working_tree(size_t row) const { return working_tree_ && row == 0; }
    const tortilla::commit* commit(size_t row) const;
    std::optional<size_t> row_of(const oid&) const;
    size_t commit_rows() const { return row_count() - (working_tree_ ? 1 : 0); }
    const std::vector<file_change>& working_tree_files() const { return wt_files_; }
    // The working-tree status arrives after the history (it can take many
    // seconds on a big or slow tree); until then the row says so.
    bool working_tree_known() const { return wt_known_; }
    void on_working_tree(std::function<void()> f) { on_working_tree_ = std::move(f); }
    const std::vector<ref>* refs_of(size_t row) const;
    // The label painted at `x` (Message column, cell coordinates) in `row`,
    // as last painted.
    std::optional<ref> label_at(size_t row, float x) const;
    bool is_head(size_t row) const;
    const std::optional<oid>& head() const { return head_; }
    tortilla::repo* repo() { return repo_ ? &*repo_ : nullptr; }

    // rap::list_model
    size_t row_count() const override;
    std::string text(size_t row, size_t column) const override;
    rap::text_style style(size_t row, size_t column) const override;
    void paint(size_t row, size_t column, rap::painter&, const rap::cell_info&) const override;

private:
    struct batch
    {
        std::vector<tortilla::commit> commits;
        std::vector<graph_row> rows;
    };

    void append(batch&&);
    // Commit index for a non-working-tree row.
    size_t index(size_t row) const;
    std::optional<size_t> row_of_index(size_t index) const;  // the row showing commits_[index]
    bool matches(const tortilla::commit&) const;
    void refilter();
    uint8_t actions_for(size_t row) const;
    void start_actions(unsigned generation);
    void start_status();
    void stop_actions();
    bool show_label(const ref&) const;

    void paint_graph(const graph_row&, rap::painter&, const rap::cell_info&) const;
    void paint_actions(uint8_t mask, rap::painter&, const rap::cell_info&) const;
    void paint_message(size_t row, rap::painter&, const rap::cell_info&) const;

    rap::ui& ui_;
    std::optional<tortilla::repo> repo_;  // UI-thread repository, for lazy lookups
    std::vector<tortilla::commit> commits_;
    std::vector<graph_row> rows_;
    mutable std::vector<uint8_t> actions_;  // per commit; unknown until first painted
    // Computed actions by commit, kept across reloads: a commit's changes
    // never change, and recomputing them after every branch or tag change
    // filled the column in again row by row.
    std::unordered_map<oid, uint8_t, oid_hash> actions_cache_;
    ref_map refs_;
    mutable std::unordered_map<oid, std::vector<label_box>, oid_hash> label_boxes_;  // as last painted
    std::optional<oid> head_;
    bool working_tree_ = false;
    bool wt_known_ = false;
    std::vector<file_change> wt_files_;
    std::function<void()> on_working_tree_;
    std::array<std::shared_ptr<git_process>, 2> status_;  // status, numstat
    std::string path_;

    // Actions icons are computed on their own thread for the rows being
    // drawn, newest requests first [spec note 4].
    struct actions_work;
    std::shared_ptr<actions_work> actions_work_;
    unsigned labels_ = label_all;
    action_icon_style icon_style_ = action_icon_style::document;
    bool loading_ = false;
    walk_refs walk_refs_ = walk_refs::head;  // the loaded walk's
    std::string walk_rev_;
    std::vector<std::string> walk_paths_;  // the history's paths; the working tree row's too

    // Filtering: the matching commit indices, in order.
    struct term
    {
        std::string text;
        bool exclude = false;
    };
    log_filter filter_;
    bool filtered_ = false;
    bool invert_ = false;
    std::vector<term> terms_;
    std::optional<std::regex> regex_;
    std::vector<size_t> visible_;

    std::jthread worker_;
    // Bumped on every load/cancel; batches from an older generation are dropped.
    std::shared_ptr<std::atomic<unsigned>> generation_ = std::make_shared<std::atomic<unsigned>>(0);
};

}  // namespace tortilla
