// TortoiseGit's Changed Files dialog (CFileDiffDlg): the files that differ
// between two revisions, or between a revision and the working tree, in the
// shared file list. Double-click compares one; each has the list's menu
// (log, blame, open, save...). The log's comparisons open it when more than
// one file differs, rather than a diff tool's window for each in turn.
#pragma once

#include "changes/changes_view.h"
#include "git/repo_worker.h"

#include <rapport/rapport.h>

#include <expected>
#include <functional>
#include <memory>
#include <string>

namespace tortilla
{

class changed_files_window
{
public:
    // `from` against `to` (commits, refs or trees); `to` "" for the working
    // tree.
    static std::expected<std::unique_ptr<changed_files_window>, std::string>
    open(rap::ui&, const std::string& path, std::string from, std::string to);
    ~changed_files_window();
    void show() { window_->show(); }
    void on_closed(std::function<void()> f) { closed_ = std::move(f); }

private:
    changed_files_window(rap::ui&, std::string workdir, std::string from, std::string to);
    void build();
    void load();
    void update_status();

    rap::ui& ui_;
    std::string workdir_, from_, to_;
    changes_view files_;
    std::unique_ptr<rap::window> window_;
    rap::label* status_ = nullptr;
    std::function<void()> closed_;
    // Last: its results touch the list, so it goes first.
    std::unique_ptr<repo_worker> worker_;
};

}  // namespace tortilla
