// TortoiseGit's RefLog dialog (CRefLogDlg): a ref's reflog, newest first,
// with commands on its entries: the log at one, comparing, switching,
// branching, resetting to one (getting back what a reset or rebase left
// behind), and deleting entries. Opened on refs/stash it is Explorer's
// Stash List, with Apply, Pop and Drop.
#pragma once

#include "git/reflog.h"

#include <rapport/rapport.h>

#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tortilla
{

class git_process;

class reflog_window
{
public:
    // `ref`: a full name, or HEAD (the default).
    static std::expected<std::unique_ptr<reflog_window>, std::string>
    open(rap::ui&, const std::string& path, std::string ref = {}, rap::window* parent = nullptr);
    ~reflog_window();
    void show() { window_->show(); }
    void on_closed(std::function<void()> f) { closed_ = std::move(f); }

private:
    class rows;  // the list's model

    reflog_window(rap::ui&, std::string workdir, std::string ref, rap::window* parent);
    void build();
    // The refs again, then the chosen one's entries, selecting `select`.
    void reload(size_t select = 0);
    void load();
    void update_info();
    std::string ref() const;
    bool stash() const { return ref() == "refs/stash"; }
    std::vector<size_t> selection() const;

    rap::menu context_menu();
    void activate(size_t row);
    void show_log(const std::string& rev);
    void difftool(std::vector<std::string> revisions);
    void reset(const reflog_entry&);
    void delete_entries(std::vector<size_t> rows);
    void clear_stash();
    void run(const std::string& title, std::vector<std::vector<std::string>> commands);
    template <class D>
    void open_dialog(std::expected<std::unique_ptr<D>, std::string> dialog);

    rap::ui& ui_;
    std::string workdir_;
    std::string initial_ref_;
    rap::window* parent_;
    bool bare_ = false;
    std::vector<std::string> refs_;
    std::unique_ptr<rows> rows_;  // before the window: its list goes first
    std::unique_ptr<rap::window> window_;
    std::shared_ptr<git_process> difftool_;
    std::function<void()> closed_;
    // Callbacks that outlive the window (a child dialog, a confirmation)
    // check this is still there.
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);

    rap::drop_down* ref_box_ = nullptr;
    rap::list* list_ = nullptr;
    rap::label* info_ = nullptr;
    rap::button* clear_ = nullptr;
};

}  // namespace tortilla
