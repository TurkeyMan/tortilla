// TortoiseGit's Rebase dialog (IDD_REBASE) [spec 2].
#pragma once

#include "log/file_model.h"
#include "rebase/rebase_engine.h"
#include "rebase/rebase_model.h"

#include <rapport/rapport.h>

#include <memory>

namespace tortilla
{

class RebaseWindow
{
public:
    // `upstream` preselects the upstream (a ref or hash); empty = the
    // current branch's upstream.
    static std::expected<std::unique_ptr<RebaseWindow>, std::string>
    open(rap::Ui&, const std::string& path, const std::string& upstream = {});

    void show() { window_->show(); }
    void onClosed(std::function<void()> f) { closed_ = std::move(f); }
    // "Show log" from the post-action menu when the rebase is done.
    void onShowLog(std::function<void()> f) { showLog_ = std::move(f); }

private:
    RebaseWindow(rap::Ui&, Repo);

    void build(const std::string& upstream);
    void reloadList();
    void selectionChanged();
    void updateControls();
    void setStatus(const std::string&, bool warning = false);
    void continuePressed();
    void abortPressed();
    void close();
    void startRebase();
    void moveRows(bool up, bool toEnd);
    bool dropRows(const std::vector<size_t>& rows, size_t destination);
    void setAction(RebaseAction);
    bool key(const rap::KeyEvent&);
    rap::Menu contextMenu(std::optional<size_t> row);
    rap::Menu setAllMenu();
    void appendLog(const std::string& text, bool command);
    void refreshConflicts();

    rap::Ui& ui_;
    Repo repo_;
    RebaseModel model_;
    FileModel revisionFiles_, conflictFiles_;
    std::unique_ptr<RebaseEngine> engine_;
    std::unique_ptr<rap::Window> window_;
    std::function<void()> closed_, showLog_;

    std::vector<std::string> branches_, upstreams_;
    std::optional<Oid> branchHead_, upstreamHead_;
    bool fastForward_ = false;
    bool canStart_ = false;
    bool stashed_ = false;
    bool closing_ = false;

    // Widgets
    rap::Select* branch_ = nullptr;
    rap::Select* upstream_ = nullptr;
    rap::Button* swap_ = nullptr;
    rap::List* list_ = nullptr;
    rap::MenuButton* setAll_ = nullptr;
    rap::Button* up_ = nullptr;
    rap::Button* down_ = nullptr;
    rap::Check* preserveMerges_ = nullptr;
    rap::Check* force_ = nullptr;
    rap::Tabs* tabs_ = nullptr;
    rap::Widget* revisionPage_ = nullptr;
    rap::Widget* conflictPage_ = nullptr;
    rap::Widget* messagePage_ = nullptr;
    rap::Widget* logPage_ = nullptr;
    rap::TextView* message_ = nullptr;
    rap::TextView* log_ = nullptr;
    rap::ProgressBar* progress_ = nullptr;
    rap::Label* status_ = nullptr;
    rap::MenuButton* post_ = nullptr;
    rap::Button* continue_ = nullptr;
    rap::Button* abort_ = nullptr;
};

}  // namespace tortilla
