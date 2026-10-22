// TortoiseGit's Log dialog (IDD_LOGMESSAGE). See docs/tortoisegit-spec.md
// section 1.
#pragma once

#include "git/command.h"
#include "git/repo_worker.h"
#include "log/file_model.h"
#include "log/log_model.h"
#include "rebase/rebase_window.h"

#include "ui/settings_dialog.h"

#include <rapport/rapport.h>

#include <chrono>
#include <memory>

namespace tortilla
{

class LogWindow
{
public:
    // `path` is any path inside the repository.
    // `allBranches` starts with All Branches checked.
    static std::expected<std::unique_ptr<LogWindow>, std::string>
    open(rap::Ui&, const std::string& path, bool allBranches = false);

    void show() { window_->show(); }
    void onClosed(std::function<void()> f) { closed_ = std::move(f); }

private:
    LogWindow(rap::Ui&, Repo, std::string path);

    void build();
    void reload();
    void loaded(const std::string& error);
    void restoreSelection();
    void selectionChanged();
    void showCommit(size_t row);
    void updateStatus();
    void updateRefButton();
    void scheduleFilter();
    void applyFilter();
    void jump(bool up);
    void datesChanged();
    void saveLayout();
    void restoreLayout();

    // Commit-list context menu and its actions (log_actions.cpp).
    rap::Menu contextMenu(std::optional<size_t> row);
    void runGitCommands(const std::string& title, std::vector<std::vector<std::string>> commands);
    // Local branch/tag changes: no progress dialog, git's output only if
    // a command fails, and the labels refreshed instead of a full reload.
    void runRefCommands(const std::string& title, std::vector<std::vector<std::string>> commands);
    // git commands with no progress dialog; `after` gets whether all
    // succeeded and their output. A failure is shown unless `showFailure`
    // is false (the caller handles it).
    void runQuietly(const std::string& title, std::vector<std::vector<std::string>> commands,
                    std::function<void(bool ok, const std::string& output)> after,
                    bool showFailure = true);
    void refsChanged();
    void openSettings(SettingsPage);
    void difftool(std::vector<std::string> revisions, std::vector<std::string> paths = {});
    void confirmThenRun(const std::string& question, const std::string& title,
                        std::vector<std::vector<std::string>> commands);
    void createBranch(const Commit&, const std::string& name = {}, bool switchTo = false,
                      const std::string& startPoint = {});
    void createTag(const Commit&);
    void stashChanges();
    void resetTo(const Commit&);
    // A branch or tag label dropped on `target`: confirm, then move it.
    void moveRef(const Ref&, const Commit& target);
    // Dragging a branch or tag label onto another commit moves it there.
    rap::List::CellDrag labelDrag();
    // Double-clicking a label: a local branch is switched to (asking what to
    // do with uncommitted changes), a remote one opens Create Branch for a
    // local branch tracking it. The label comes from the last press, which
    // labelDrag's begin sees.
    struct LabelPress
    {
        size_t row;
        std::string fullName;
        std::chrono::steady_clock::time_point at;
    };
    std::optional<LabelPress> lastLabelPress_;
    void rowActivated(size_t row);
    enum class SwitchMode
    {
        Plain,
        Stash,
        BringAlong,
        Discard,
    };
    void switchTo(const std::string& target, bool detach);
    void runSwitch(const std::string& target, bool detach, SwitchMode);
    struct Choice
    {
        std::string label, description;
        std::function<void()> act;
    };
    void choose(const std::string& title, const std::string& question, std::vector<Choice> choices);
    void offerStashPop();
    rap::Menu cellMenu(size_t row, size_t column, rap::PointF at);
    std::optional<std::string> clickedRef_;  // while building a label's context menu
    void mergeFrom(const Commit&, const std::string& what);

    // Changed-files list context menu and its actions (log_file_actions.cpp).
    rap::Menu fileMenu(std::optional<size_t> row, std::optional<size_t> column);
    void fileActivated(size_t row);
    void copyFiles(int format, std::optional<size_t> column);
    void saveRevisions(const std::string& rev, std::vector<FileChange> files);
    void saveRevision(const std::string& rev, const FileChange& file);
    void copyToClipboard(const std::vector<size_t>& rows, int format);
    void showBranchesContaining(const Commit&);

    rap::Menu walkMenu();
    rap::Menu viewMenu();
    rap::Menu refMenu();
    rap::Menu filterMenu();
    rap::Menu headerMenu();
    std::string filterCue() const;

    rap::Ui& ui_;
    Repo repo_;  // UI-thread repository for details and file lists
    std::string path_;
    LogModel log_;
    std::unique_ptr<RepoWorker> filesWorker_;  // the selected commit's files
    FileModel files_;
    std::unique_ptr<rap::Window> window_;
    std::function<void()> closed_;

    // Widgets
    rap::MenuButton* refButton_ = nullptr;
    rap::DatePicker* from_ = nullptr;
    rap::DatePicker* to_ = nullptr;
    rap::Entry* search_ = nullptr;
    rap::Select* jumpType_ = nullptr;
    rap::List* commitList_ = nullptr;
    rap::TextView* details_ = nullptr;
    rap::List* fileList_ = nullptr;
    rap::Label* status_ = nullptr;
    rap::Check* allBranches_ = nullptr;
    rap::Button* statistics_ = nullptr;
    rap::MenuButton* walk_ = nullptr;
    rap::MenuButton* view_ = nullptr;
    rap::ProgressBar* progress_ = nullptr;
    rap::Splitter* splitter_ = nullptr;

    // Options
    WalkOptions walkOptions_;
    LogFilter filter_;
    bool showUnversioned_ = true;
    std::optional<int64_t> since_, until_;  // explicit date limits
    rap::Subscription filterTimer_, pulseTimer_;
    std::shared_ptr<GitProcess> difftool_;
    std::shared_ptr<GitProcess> refCommand_;
    std::vector<std::unique_ptr<RebaseWindow>> rebases_;
    void openRebase(const std::string& upstream);
    std::vector<Oid> history_;  // selection history, for the "Selection History" jump

    // The selection across a reload, by commit (rows move): put back, and
    // the current row scrolled into view, once loading finishes.
    struct KeptSelection
    {
        std::vector<Oid> commits;
        bool workingTree = false;
        std::optional<Oid> current;  // nullopt with workingTree: that row
    };
    std::optional<KeptSelection> kept_;
    size_t historyPos_ = 0;
    bool navigating_ = false;
};

}  // namespace tortilla
