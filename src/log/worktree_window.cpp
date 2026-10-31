#include "worktree_window.h"

#include "branch/ref_chooser.h"
#include "git/command.h"
#include "git/repo.h"
#include "i18n.h"
#include "log/log_window.h"
#include "settings.h"
#include "ui/repo_icon.h"
#include "ui/windows.h"

#include <rapport/sys.h>

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace tortilla
{
namespace
{

namespace fs = std::filesystem;

enum column : size_t
{
    path,
    branch,
    status,
    hash,
    column_count,
};

std::vector<rap::column> columns()
{
    using namespace rap::literals;
    std::vector<rap::column> c(column_count);
    c[path] = {.title = _("Path"), .width = 56_ch};
    c[branch] = {.title = _("Branch"), .width = 20_ch};
    c[hash] = {.title = _("SHA-1"), .width = 42_ch};
    c[status] = {.title = _("Status"), .width = 30_ch};
    return c;
}

// A path as the platform writes it: backslashes on Windows.
std::string shown_path(const std::string& p)
{
    return rap::sys::path_to_utf8(rap::sys::path_from_utf8(p).make_preferred());
}

std::string status_text(const worktree_info& w)
{
    std::vector<std::string> parts;
    if (w.main) parts.push_back(w.bare ? _("Bare repository") : _("Main worktree"));
    if (w.current) parts.push_back(_("Current"));
    if (w.locked)
        parts.push_back(w.lock_reason.empty() ? _("Locked")
                                              : rap::i18n::format(_("Locked: {0}"), w.lock_reason));
    if (w.prunable) parts.push_back(_("Prunable (folder missing)"));
    std::string out;
    for (const std::string& p : parts) out += (out.empty() ? "" : ", ") + p;
    return out;
}

std::string trimmed(std::string s)
{
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) ++start;
    return s.substr(start);
}

}  // namespace

class worktree_window::rows : public rap::list_model
{
public:
    using rap::list_model::notify_reset;
    std::vector<worktree_info> items;

    size_t row_count() const override { return items.size(); }
    std::string text(size_t row, size_t col) const override
    {
        if (row >= items.size()) return {};
        const worktree_info& w = items[row];
        switch (col)
        {
        case path: return shown_path(w.path);
        case branch:
            // A bare repository has nothing checked out; a missing worktree
            // nothing to read.
            if (!w.branch.empty() || w.bare || w.head.empty()) return w.branch;
            return _("(detached HEAD)");
        case hash: return w.head;
        case status: return status_text(w);
        default: return {};
        }
    }
};

// Add...: a folder, and what goes in it: a new branch, or an existing branch
// or commit as it is (`git worktree add [-b <new>] <path> [<commit-ish>]`).
class worktree_window::add_dialog
{
public:
    using go_function = std::function<void(git_command, std::string path)>;

    add_dialog(rap::ui& ui, std::string dir, rap::window* parent, go_function go)
        : ui_(ui)
        , dir_(std::move(dir))
        , base_(dir_)
        , go_(std::move(go))
    {
        build(parent);
    }

private:
    void build(rap::window* parent)
    {
        using namespace rap::layout;
        using namespace rap::literals;
        // Beside the repository: where worktrees usually go.
        const fs::path top = rap::sys::path_from_utf8(dir_);
        std::string folder = rap::sys::path_to_utf8(top.parent_path().make_preferred());
        if (!folder.empty() && folder.back() != fs::path::preferred_separator)
            folder += char(fs::path::preferred_separator);
        window_ = create_window(
            ui_,
            dialog{_("Add Worktree"),
                   {vbox{
                       group{_("Folder"),
                             hbox{
                                 entry{folder}.bind(folder_).stretch().width(60_ch).on_changed(
                                     [this](const std::string&) { folder_changed(); }),
                                 button{_("Bro_wse...")}.on_click([this] { browse(); }),
                             }},
                       base_.layout({.caption = _("Base On"),
                                     .head = true,
                                     .ui = &ui_,
                                     .window = [this] { return window_.get(); }}),
                       group{_("Option"),
                             hbox{
                                 check{_("Create _New Branch"), true}
                                     .bind(new_branch_)
                                     .on_toggled([this](bool) { update_state(); }),
                                 entry{}.bind(name_).stretch(),
                             }},
                       buttons{dialog_button{rap::dialog_role::ok, ""}.on_click([this] { ok(); }),
                               cancel},
                   }}},
            parent);
        base_.select({});
        update_state();
    }

    // The new branch is named after the folder, until it's given a name.
    void folder_changed()
    {
        if (name_->text() != auto_name_) return;
        std::string f = folder_->text();
        while (!f.empty() && (f.back() == '/' || f.back() == '\\')) f.pop_back();
        auto_name_ = rap::sys::path_to_utf8(rap::sys::path_from_utf8(f).filename());
        name_->set_text(auto_name_);
    }

    void browse()
    {
        ui_.choose_files(window_.get(),
                         {.mode = rap::file_dialog_spec::mode::folder,
                          .title = _("Add Worktree"),
                          .initial_path = folder_->text()},
                         [this](std::vector<std::string> chosen)
                         {
                             if (!chosen.empty()) folder_->set_text(chosen.front());
                         });
    }

    void update_state() { name_->set_enabled(new_branch_->checked()); }

    void ok()
    {
        auto refuse = [this](const std::string& text)
        {
            ui_.message(window_.get(), {.icon = rap::message_spec::icon::warning,
                                        .title = _("Add Worktree"),
                                        .text = text});
        };
        const std::string folder = trimmed(folder_->text());
        const bool make = new_branch_->checked();
        const std::string name = trimmed(name_->text());
        if (folder.empty()) return refuse(_("Choose a folder for the worktree."));
        if (make && name.empty())
            return refuse(_("Branch/Tag name must not be empty or is invalid."));
        if (base_.name().empty()) return refuse(_("Choose what to base it on."));
        // git runs in the main worktree: a relative folder is from there.
        const std::string where = rap::sys::path_to_utf8(
            (rap::sys::path_from_utf8(dir_) / rap::sys::path_from_utf8(folder)).lexically_normal());
        // A local branch by its short name, so it's checked out rather than
        // its commit detached (as Switch/Checkout does).
        std::string base = base_.revision();
        if (!make && base.starts_with("refs/heads/")) base.erase(0, 11);
        git_command c{"worktree", "add"};
        if (make) c.insert(c.end(), {"-b", name});
        c.insert(c.end(), {where, base});
        window_->close();
        go_(std::move(c), where);
    }

    rap::ui& ui_;
    std::string dir_;
    ref_chooser base_;
    go_function go_;
    std::string auto_name_;
    std::unique_ptr<rap::window> window_;
    rap::entry* folder_ = nullptr;
    rap::check* new_branch_ = nullptr;
    rap::entry* name_ = nullptr;
};

std::expected<std::unique_ptr<worktree_window>, std::string>
worktree_window::open(rap::ui& ui, const std::string& path, rap::window* parent)
{
    auto repo = repo::open(path.empty() ? "." : path);
    if (!repo) return std::unexpected(repo.error());
    // git runs in the main working tree (a bare repository's directory):
    // the one place that's there whichever worktree is removed.
    auto trees = list_worktrees(*repo);
    if (!trees) return std::unexpected(trees.error());
    std::unique_ptr<worktree_window> w(new worktree_window(ui, trees->front().path, parent));
    for (const worktree_info& t : *trees)
        if (t.current) w->here_ = t.path;
    w->build();
    return w;
}

worktree_window::worktree_window(rap::ui& ui, std::string dir, rap::window* parent)
    : ui_(ui)
    , dir_(std::move(dir))
    , parent_(parent)
    , rows_(std::make_unique<rows>())
{
}

worktree_window::~worktree_window() = default;

void worktree_window::build()
{
    using namespace rap::layout;
    using namespace rap::literals;
    window_ = create_window(
        ui_,
        dialog{rap::i18n::format(_("{0} - Worktrees - Tortilla"), shown_path(dir_)),
               {vbox{
                   list{columns(), rows_.get()}
                       .bind(list_)
                       .on_selection_changed([this] { update_state(); })
                       .on_activate(
                           [this](size_t row)
                           {
                               if (row < rows_->items.size() && !rows_->items[row].prunable)
                                   show_log(rows_->items[row]);
                           })
                       .on_context_menu([this](std::optional<size_t>) { return context_menu(); })
                       .stretch(),
                   hbox{
                       label{}.bind(info_).stretch(),
                       buttons{dialog_button{rap::dialog_role::none, _("_Add...")}.on_click(
                                   [this] { add(); }),
                               dialog_button{rap::dialog_role::none, _("Prune")}.on_click(
                                   [this] { prune(); }),
                               ok},
                   },
               }}}
            .size(120_ch, 20_lines),
        parent_);
    settings& st = settings::get();
    st.restore_window("worktrees.window", *window_);
    if (auto v = st.numbers("worktrees.columns"); v.size() == column_count)
        list_->set_column_widths(v);
    window_->on_key(
        [this](const rap::key_event& e)
        {
            if (!e.press || e.key != rap::key::f5) return false;
            const worktree_info* w = chosen();
            reload(w ? w->path : std::string());
            return true;
        });
    window_->on_finished(
        [this](rap::dialog_role)
        {
            settings& st = settings::get();
            st.save_window("worktrees.window", *window_);
            st.set_numbers("worktrees.columns", list_->column_widths());
            if (closed_) closed_();
        });
    reload();
    list_->focus();
}

// Read through libgit2: a few small files per worktree, no git process.
void worktree_window::reload(const std::string& select)
{
    rows_->items.clear();
    if (auto repo = repo::open(dir_))
    {
        if (auto trees = list_worktrees(*repo))
            rows_->items = std::move(*trees);
        else
            ui_.message(window_.get(), {.icon = rap::message_spec::icon::error,
                                        .title = "Tortilla",
                                        .text = _("Couldn't read the worktrees."),
                                        .detail = trees.error()});
    }
    // As paths, not text: libgit2 writes '/' where Windows has '\'.
    auto same = [](const std::string& a, const std::string& b)
    {
        return rap::sys::path_from_utf8(a).lexically_normal() ==
               rap::sys::path_from_utf8(b).lexically_normal();
    };
    // Read from the main worktree, but current is the one this was opened in.
    for (worktree_info& w : rows_->items) w.current = same(w.path, here_);
    rows_->notify_reset();
    if (!rows_->items.empty())
    {
        const std::string wanted = select.empty() ? here_ : select;
        const auto it = std::ranges::find_if(rows_->items, [&](const worktree_info& w)
                                             { return same(w.path, wanted); });
        const size_t row = it == rows_->items.end() ? 0 : size_t(it - rows_->items.begin());
        list_->select({row});
        list_->set_current(row);
        list_->scroll_to(row);
    }
    update_state();
}

void worktree_window::update_state()
{
    if (!info_ || !list_) return;
    const size_t prunable =
        size_t(std::ranges::count(rows_->items, true, &worktree_info::prunable));
    info_->set_text(rap::i18n::format(_("{0} worktrees, {1} prunable"),
                                      std::to_string(rows_->items.size()),
                                      std::to_string(prunable)));
}

const worktree_info* worktree_window::chosen() const
{
    const auto sel = list_->selection();
    if (sel.size() != 1 || *sel.begin() >= rows_->items.size()) return nullptr;
    return &rows_->items[*sel.begin()];
}

rap::menu worktree_window::context_menu()
{
    const worktree_info* chosen_one = chosen();
    if (!chosen_one) return {};
    const worktree_info w = *chosen_one;
    rap::menu m;
    // A missing folder has no log to show or place to go.
    if (!w.prunable && !w.bare)
    {
        m.push_back({.label = _("Show _log"), .action = [this, w] { show_log(w); }});
        m.push_back({.label = _("E_xplore to"), .action = [this, w] { explore(w); }});
        m.push_back({});
    }
    // git keeps the main worktree: it holds the repository.
    if (!w.main)
    {
        if (w.locked)
            m.push_back({.label = _("_Unlock"), .action = [this, w] { lock(w, false); }});
        else
            m.push_back({.label = _("Loc_k"), .action = [this, w] { lock(w, true); }});
        m.push_back({.label = _("_Remove..."), .action = [this, w] { remove(w); }});
        m.push_back({});
    }
    m.push_back(
        {.label = _("Copy _path"), .action = [this, w] { ui_.set_clipboard(shown_path(w.path)); }});
    if (!w.head.empty())
        m.push_back({.label = _("Copy SHA-1"), .action = [this, w] { ui_.set_clipboard(w.head); }});
    return m;
}

void worktree_window::show_log(const worktree_info& w)
{
    auto log = log_window::open(ui_, w.path);
    if (!log)
    {
        ui_.message(
            window_.get(),
            {.icon = rap::message_spec::icon::error, .title = "Tortilla", .text = log.error()});
        return;
    }
    windows::keep(std::move(*log));
}

void worktree_window::explore(const worktree_info& w)
{
    rap::sys::reveal(rap::sys::path_from_utf8(w.path));
}

void worktree_window::add()
{
    // The last one has closed (it's modal over this window): replaced.
    add_ = std::make_unique<add_dialog>(
        ui_, dir_, window_.get(),
        [this, alive = std::weak_ptr<bool>(alive_)](git_command c, std::string folder)
        {
            if (!alive.lock()) return;
            rap::ui& ui = ui_;
            // The log there once it's made, as after a clone.
            std::vector<progress_follow_up> follow_ups{{.label = _("Show _log"),
                                                        .after_close = [&ui, folder]
                                                        {
                                                            if (auto log =
                                                                    log_window::open(ui, folder))
                                                                windows::keep(std::move(*log));
                                                        }}};
            run(rap::i18n::format(_("{0} - Add Worktree"), shown_path(dir_)), {std::move(c)},
                folder, std::move(follow_ups));
        });
}

// Confirmed first: git deletes the folder. Changes in it, or a lock, stop
// it; the progress dialog then offers to force it.
void worktree_window::remove(const worktree_info& w)
{
    ui_.message(window_.get(),
                {.icon = rap::message_spec::icon::warning,
                 .title = "Tortilla",
                 .text = rap::i18n::format(
                     _("Do you really want to remove the worktree {0}? Its folder is deleted."),
                     shown_path(w.path)),
                 .buttons = {rap::dialog_role::yes, rap::dialog_role::no}},
                [this, w, alive = std::weak_ptr<bool>(alive_)](rap::dialog_role role)
                {
                    if (!alive.lock() || role != rap::dialog_role::yes) return;
                    // A locked worktree takes --force twice.
                    git_command force{"worktree", "remove", "--force"};
                    if (w.locked) force.push_back("--force");
                    force.push_back(w.path);
                    std::vector<progress_follow_up> follow_ups{
                        {.label = _("_Force remove"),
                         .commands = [force] { return std::vector<git_command>{force}; },
                         .offered = [](bool ok) { return !ok; }}};
                    run(rap::i18n::format(_("{0} - Remove Worktree"), shown_path(dir_)),
                        {{"worktree", "remove", w.path}}, {}, std::move(follow_ups));
                });
}

// Quick, so not in the progress dialog, but still off the UI thread; a
// failure is said in a message.
void worktree_window::lock(const worktree_info& w, bool lock)
{
    auto output = std::make_shared<std::string>();
    locking_ = git_process::start(
        ui_, dir_, {"worktree", lock ? "lock" : "unlock", w.path},
        [output](const std::string& chunk) { *output += chunk; },
        [this, output, path = w.path, alive = std::weak_ptr<bool>(alive_)](int exit_code)
        {
            if (!alive.lock()) return;
            if (exit_code != 0)
                ui_.message(
                    window_.get(),
                    {.icon = rap::message_spec::icon::error, .title = "Tortilla", .text = *output});
            reload(path);
        });
}

void worktree_window::prune()
{
    const worktree_info* w = chosen();
    run(rap::i18n::format(_("{0} - Prune Worktrees"), shown_path(dir_)),
        {{"worktree", "prune", "--verbose"}}, w ? w->path : std::string());
}

// In the progress dialog, in the main worktree, then the list again,
// `select` selected.
void worktree_window::run(const std::string& title, std::vector<git_command> commands,
                          const std::string& select, std::vector<progress_follow_up> follow_ups)
{
    run_progress_dialog(
        ui_, window_.get(), title, dir_, std::move(commands),
        [this, select, alive = std::weak_ptr<bool>(alive_)](bool)
        {
            if (alive.lock()) reload(select);
        },
        std::move(follow_ups));
}

}  // namespace tortilla
