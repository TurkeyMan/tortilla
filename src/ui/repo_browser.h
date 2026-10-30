// TortoiseGit's Repository Browser (Repo-browser): the repository's files
// as they are at a revision, nothing checked out. Its folders on the left,
// the selected folder's files on the right; a file opens, saves, blames or
// compares as it is there.
#pragma once

#include "git/tree_list.h"

#include <rapport/rapport.h>

#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace tortilla
{

class git_process;
class repo;

class repo_browser
{
public:
    // `path` is a repository, or a folder or file in one, which is where
    // browsing starts; `revision` "" for HEAD.
    static std::expected<std::unique_ptr<repo_browser>, std::string>
    open(rap::ui&, const std::string& path, const std::string& revision = {});
    ~repo_browser();
    void show() { window_->show(); }
    void on_closed(std::function<void()> f) { closed_ = std::move(f); }

private:
    class rows;  // the list's model

    repo_browser(rap::ui&, std::string workdir);
    void build();
    // Browses `revision`, in `folder` (or its deepest folder still there),
    // with the entry `select` selected; a revision that doesn't resolve is
    // said and the one shown stays.
    void load(const std::string& revision, std::string folder, const std::string& select = {});
    void show_folder(const std::string& folder, const std::string& select = {});
    void enter(const std::string& folder);
    std::string folder() const;
    std::vector<const tree_entry*> selection() const;
    std::string path_of(const tree_entry&) const;
    std::string absolute(const std::string& path) const;
    // What the actions name the revision by: its commit, else its tree.
    std::string revision_id() const;
    void error(const std::string& title, const std::string& text, const std::string& detail = {});

    rap::menu list_menu();
    rap::menu folder_menu(const std::string& folder);
    void activated(size_t row);
    void open_file(const std::string& path, bool choose);
    void save_as(const std::string& path);
    void save_to_folder(std::vector<std::string> paths);
    void show_log(const std::string& path);
    void blame(const std::string& path);
    void compare(const std::string& path);

    rap::ui& ui_;
    std::string workdir_;  // the working tree, or a bare repository's folder
    bool bare_ = false;
    // Read in process: starting git to list a folder is slow on Windows.
    std::unique_ptr<repo> repo_;
    std::string revision_;  // as typed or picked
    std::optional<tree_revision> tree_;
    std::vector<std::string> folders_;
    std::unique_ptr<rows> rows_;  // before the window: its list goes first
    std::unique_ptr<rap::window> window_;
    std::shared_ptr<git_process> difftool_;
    std::function<void()> closed_;
    // Callbacks that outlive the window (a picker, a file dialog) check
    // this is still there.
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);

    rap::entry* revision_entry_ = nullptr;
    rap::label* commit_ = nullptr;
    rap::splitter* splitter_ = nullptr;
    rap::tree* tree_view_ = nullptr;
    rap::list* list_ = nullptr;
};

}  // namespace tortilla
