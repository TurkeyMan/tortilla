// TortoiseGitBlame: a file's lines, each with the commit that last changed
// it (shaded by age, the selected line's commit highlighted), the commit's
// details, and a slider through the file's history, blaming it as it was
// at each revision.
#pragma once

#include "git/blame.h"

#include <rapport/rapport.h>

#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace tortilla
{

class git_process;
class repo;

class blame_window
{
public:
    // `path` is a file in a repository; `revision` blames it as it was
    // there ("" for the working tree).
    static std::expected<std::unique_ptr<blame_window>, std::string>
    open(rap::ui&, const std::string& path, const std::string& revision = {});
    ~blame_window();
    void show() { window_->show(); }
    void on_closed(std::function<void()> f) { closed_ = std::move(f); }

private:
    class rows;

    // What a slider stop blames: a revision of the file (the working tree
    // first, when it was opened on that), by the file's name there.
    struct stop
    {
        std::string hash;  // "" for the working tree
        std::string path;
        std::string label;
    };

    blame_window(rap::ui&, std::string workdir, std::string path, std::string revision);
    void build();
    void load_history();
    void go_to(size_t stop);  // index into stops_, 0 the newest
    void blame(const stop&);
    void shown(const std::vector<blame_line>& lines);
    void update_slider();
    void update_details();
    std::optional<size_t> selected_line() const;
    std::string message(const std::string& hash);
    void find_next();

    rap::menu line_menu(std::optional<size_t> row);
    void blame_previous(const blame_line&);
    void show_changes(const blame_line&);
    void show_log(const blame_line&);

    rap::ui& ui_;
    std::string workdir_;
    // For commit messages, read in-process: starting git for one is slow on
    // Windows, worst over \\wsl.localhost (near a second a click).
    std::unique_ptr<repo> repo_;
    std::map<std::string, std::string> messages_;
    std::string path_;            // repository-relative, as opened
    std::string revision_;        // as opened
    std::unique_ptr<rows> rows_;  // before the window: its list goes first
    std::unique_ptr<rap::window> window_;
    std::vector<stop> stops_;
    size_t current_ = 0;
    std::map<std::string, std::vector<blame_line>> cache_;  // by stop hash and options
    std::shared_ptr<git_process> history_, running_, difftool_;
    rap::subscription slide_timer_;
    std::function<void()> closed_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);

    rap::slider* slider_ = nullptr;
    rap::button* older_ = nullptr;
    rap::button* newer_ = nullptr;
    rap::label* revision_label_ = nullptr;
    rap::entry* find_ = nullptr;
    rap::check* ignore_whitespace_ = nullptr;
    rap::check* detect_moves_ = nullptr;
    rap::splitter* splitter_ = nullptr;
    rap::list* list_ = nullptr;
    rap::text_view* details_ = nullptr;
    rap::label* status_ = nullptr;
};

}  // namespace tortilla
