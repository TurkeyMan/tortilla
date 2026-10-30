// A list of changed files, as TortoiseGit's GitStatusListCtrl: the one
// behind the Log dialog's file pane, Check for modifications, Commit and the
// rest. Its columns, status colours, double-click diff, context menu and
// copying live here, so every window that lists changes gets them alike;
// a window only says which files to show (a revision's, or the working
// tree's) and what to run and refresh around them.
#pragma once

#include "changes/file_model.h"
#include "git/command.h"
#include "ui/settings_dialog.h"

#include <rapport/rapport.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace tortilla
{

// git's empty tree: what a root commit is compared with.
inline constexpr const char* k_empty_tree = "4b825dc642cb6eb9a060e54bf8d69288fbee4904";

// Whose files the list shows: what its menu offers and what double-click
// compares.
struct changes_source
{
    enum kind
    {
        working_tree,  // the working tree against HEAD
        revision,      // a commit against its (first) parent, or any two revisions
        // The working tree against `parent`, a revision (Compare with
        // working tree): its files are the working tree's, but none of the
        // working tree's own actions (Revert, Add...) apply.
        revision_to_working_tree,
    };
    kind what = working_tree;
    std::string hash;          // revision: the commit
    std::string parent;        // revision: its first parent, or git's empty tree
    std::string parent_title;  // revision: "abc12345 summary", for the menu; empty for a root
};

class changes_view
{
public:
    // What belongs to the window around the list.
    struct host
    {
        rap::ui& ui;
        std::function<rap::window*()> window;  // parent for dialogs and messages
        std::string workdir;
        // git commands that change the working tree, run without a progress
        // dialog; the window refreshes afterwards.
        std::function<void(const std::string& title,
                           std::vector<std::vector<std::string>> commands)>
            run;
        // Re-read the files (after an ignore list changed, say).
        std::function<void()> refresh;
        std::function<void(settings_page)> open_settings;
        // The selection changed (the window's status line counts it).
        std::function<void()> selection_changed;
        // With checkboxes: which files are checked changed.
        std::function<void()> checks_changed = {};
    };

    explicit changes_view(host);

    // The list, for the window's layout. `settings_key` names where its
    // column widths and order are kept ("log.files", ...).
    // With `checkboxes`, a box per row: clicking one in a selection sets
    // them all [TortoiseGit, spec 5.2.6].
    rap::layout::list list(std::string settings_key, bool checkboxes = false);
    void save_layout() const;
    void restore_layout();

    void set_source(changes_source);
    const changes_source& source() const { return source_; }
    void set_files(std::vector<file_change>, bool show_unversioned = true);
    void set_counts(size_t first, const std::vector<file_change>&);
    void clear() { set_files({}); }

    const std::vector<file_change>& files() const { return model_.files(); }
    std::vector<size_t> selection() const;

    // Checkboxes.
    std::vector<file_change> checked_files() const;
    size_t checked_count() const { return model_.checked_count(); }
    // Checks (or unchecks) every row `which` accepts (the Commit dialog's
    // "Check:" links).
    void check_where(const std::function<bool(const file_change&)>& which, bool on);

private:
    rap::menu context_menu(std::optional<size_t> row, std::optional<size_t> column);
    void activated(size_t row);
    void edit_conflict(const std::string& path);
    void open_file(const file_change&, bool choose);
    void difftool(std::vector<std::string> revisions, std::vector<std::string> paths);
    void show_log(const std::string& path, const std::string& rev);
    void blame(const std::string& path, const std::string& rev);
    void copy_files(int format, std::optional<size_t> column);
    void save_revisions(const std::string& rev, std::vector<file_change> files);
    void save_revision(const std::string& rev, const file_change& file);

    host host_;
    file_model model_;
    rap::list* list_ = nullptr;
    std::string settings_key_;
    changes_source source_;
    std::shared_ptr<git_process> difftool_;
};

}  // namespace tortilla
