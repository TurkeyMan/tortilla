// TortoiseGit's Browse References (CBrowseRefsDlg) [spec 10]: the refs as
// folders on the left and a list of each folder's refs on the right, with
// their commits; a ref manager (log, switch, merge, create, rename, delete,
// upstreams), and the ref picker other dialogs use.
#pragma once

#include "git/ref_list.h"

#include <rapport/rapport.h>

#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace tortilla
{

class browse_refs
{
public:
    // Which refs a picker offers [spec 10.12]; other namespaces always show.
    enum kinds : unsigned
    {
        heads = 1,
        tags = 2,
        remotes = 4,
        all = 7,
    };
    struct options
    {
        std::string initial;  // a ref, a short name, or "" for HEAD's branch
        unsigned kinds = all;
        bool pick = false;      // a picker: OK returns what's selected
        bool multiple = false;  // several rows (space-separated short names)
    };

    static std::expected<std::unique_ptr<browse_refs>, std::string>
    open(rap::ui&, const std::string& path, options, rap::window* parent = nullptr);
    ~browse_refs();
    void show() { window_->show(); }
    void on_closed(std::function<void()> f) { closed_ = std::move(f); }
    // A picker's choice, once, as it closes [spec 10.12]: the full ref for
    // one row, the short names for several, the current branch's short
    // name (its hash when detached), or "" when cancelled.
    void on_picked(std::function<void(const std::string&)> f) { picked_ = std::move(f); }

private:
    class rows;  // the list's model

    browse_refs(rap::ui&, std::string workdir, options, rap::window* parent);
    void build();
    void reload(std::string select = {});
    void fill();
    void update_info();
    std::string folder() const;
    std::vector<const ref_entry*> selection() const;
    std::string picked_text() const;
    void finish(std::string picked);
    void ok();

    rap::menu list_menu(std::optional<size_t> row);
    rap::menu folder_menu(const std::string& folder);
    void show_log(const std::string& rev);
    void delete_refs(std::vector<const ref_entry*>);
    void rename(const ref_entry&);
    void edit_description(const ref_entry&);
    void pick_upstream(const ref_entry&);
    void run_then_reload(const std::string& title, std::vector<std::vector<std::string>> commands);
    template <class D>
    void open_dialog(std::expected<std::unique_ptr<D>, std::string> dialog);

    rap::ui& ui_;
    std::string workdir_;
    options options_;
    rap::window* parent_;
    std::unique_ptr<rows> rows_;  // before the window: its list goes first
    std::unique_ptr<rap::window> window_;
    std::vector<ref_entry> refs_;
    std::string current_branch_;  // refs/heads/<b>, or "" when detached
    std::function<void()> closed_;
    std::function<void(const std::string&)> picked_;
    bool done_ = false;
    bool bare_ = false;
    rap::subscription filter_timer_;
    // Callbacks that outlive the dialog (a child dialog, a confirmation)
    // check this is still there.
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);

    rap::splitter* splitter_ = nullptr;
    rap::tree* tree_ = nullptr;
    rap::list* list_ = nullptr;
    rap::entry* filter_ = nullptr;
    rap::drop_down* merged_ = nullptr;
    rap::check* nested_ = nullptr;
    rap::label* info_ = nullptr;
};

// Opens Browse References as a picker over `parent` [spec 10.12]; `done`
// gets the choice, or "" when cancelled.
void pick_ref(rap::ui&, rap::window* parent, const std::string& workdir, browse_refs::options,
              std::function<void(const std::string&)> done);

}  // namespace tortilla
