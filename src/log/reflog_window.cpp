#include "reflog_window.h"

#include "branch/create_ref_dialog.h"
#include "branch/switch_dialog.h"
#include "git/command.h"
#include "git/ref_list.h"
#include "git/remotes.h"
#include "git/repo.h"
#include "i18n.h"
#include "log/log_window.h"
#include "settings.h"
#include "ui/compare.h"
#include "ui/menu_text.h"
#include "ui/progress_dialog.h"
#include "ui/repo_icon.h"
#include "ui/reset_dialog.h"
#include "ui/settings_dialog.h"
#include "ui/windows.h"
#include "util/calendar.h"

#include <rapport/sys.h>

#include <algorithm>
#include <ctime>

namespace tortilla
{
namespace
{

enum column : size_t
{
    selector,
    action,
    message,
    date,
    hash,
    column_count,
};

std::vector<rap::column> columns()
{
    using namespace rap::literals;
    std::vector<rap::column> c(column_count);
    c[selector] = {.title = _("Ref"), .width = 18_ch};
    c[action] = {.title = _("Action"), .width = 16_ch};
    c[message] = {.title = _("Message"), .width = 60_ch};
    c[date] = {.title = _("Date"), .width = 23_ch};
    c[hash] = {.title = _("SHA-1"), .width = 42_ch};
    return c;
}

std::string date_text(int64_t t)
{
    if (!t) return {};
    std::tm tm = local_calendar(std::time_t(t));
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tm);
    return buf;
}

// The menu's name for a ref: "HEAD", "main", "origin/main", "stash".
std::string ref_label(const std::string& ref)
{
    if (ref == "refs/stash") return "stash";
    if (ref.starts_with("refs/remotes/")) return ref.substr(13);
    return strip_ref_name(ref);
}

}  // namespace

class reflog_window::rows : public rap::list_model
{
public:
    using rap::list_model::notify_reset;
    std::string ref;
    std::vector<reflog_entry> items;

    size_t row_count() const override { return items.size(); }
    std::string text(size_t row, size_t col) const override
    {
        if (row >= items.size()) return {};
        const reflog_entry& e = items[row];
        switch (col)
        {
        case selector: return short_selector(ref, row);
        case action: return e.action;
        case message: return e.message;
        case date: return date_text(e.time);
        case hash: return e.id;
        default: return {};
        }
    }
};

std::expected<std::unique_ptr<reflog_window>, std::string>
reflog_window::open(rap::ui& ui, const std::string& path, std::string ref, rap::window* parent)
{
    auto repo = repo::open(path.empty() ? "." : path);
    if (!repo) return std::unexpected(repo.error());
    std::string dir = repo->workdir().empty() ? repo->git_dir() : repo->workdir();
    std::unique_ptr<reflog_window> w(new reflog_window(ui, std::move(dir), std::move(ref), parent));
    w->bare_ = repo->bare();
    w->build();
    return w;
}

reflog_window::reflog_window(rap::ui& ui, std::string workdir, std::string ref, rap::window* parent)
    : ui_(ui)
    , workdir_(std::move(workdir))
    , initial_ref_(ref.empty() ? std::string("HEAD") : std::move(ref))
    , parent_(parent)
    , rows_(std::make_unique<rows>())
{
}

reflog_window::~reflog_window() = default;

void reflog_window::build()
{
    using namespace rap::layout;
    using namespace rap::literals;
    std::string root = workdir_;
    while (root.size() > 1 && (root.back() == '/' || root.back() == '\\')) root.pop_back();
    root = rap::sys::path_to_utf8(rap::sys::path_from_utf8(root).make_preferred());

    window_ = create_window(
        ui_,
        dialog{rap::i18n::format(_("{0} - RefLog - Tortilla"), root),
               {vbox{
                   hbox{
                       label{_("Ref:")},
                       drop_down{std::vector<std::string>{}, -1}
                           .bind(ref_box_)
                           .on_changed([this](int) { load(); })
                           .width(40_ch),
                       spacer{},
                   },
                   list{columns(), rows_.get()}
                       .bind(list_)
                       .selection(rap::selection_mode::multi)
                       .on_selection_changed([this] { update_info(); })
                       .on_activate([this](size_t row) { activate(row); })
                       .on_context_menu([this](std::optional<size_t>) { return context_menu(); })
                       .stretch(),
                   hbox{
                       label{}.bind(info_).stretch(),
                       buttons{dialog_button{rap::dialog_role::none, _("Clear stash")}
                                   .on_click([this] { clear_stash(); })
                                   .bind(clear_),
                               ok},
                   },
               }}}
            .size(120_ch, 30_lines),
        parent_);
    settings& st = settings::get();
    st.restore_window("refLog.window", *window_);
    if (auto v = st.numbers("refLog.columns"); v.size() == column_count)
        list_->set_column_widths(v);
    window_->on_key(
        [this](const rap::key_event& e)
        {
            if (!e.press || e.key != rap::key::f5) return false;
            reload(list_->current().value_or(0));
            return true;
        });
    window_->on_finished(
        [this](rap::dialog_role)
        {
            settings& st = settings::get();
            st.save_window("refLog.window", *window_);
            st.set_numbers("refLog.columns", list_->column_widths());
            if (closed_) closed_();
        });
    reload();
    list_->focus();
}

void reflog_window::reload(size_t select)
{
    const std::string chosen = refs_.empty() ? initial_ref_ : ref();
    if (auto repo = repo::open(workdir_)) refs_ = reflog_refs(*repo);
    // A ref without a reflog (an emptied stash) stays chosen, with no entries.
    if (!std::ranges::contains(refs_, chosen)) refs_.push_back(chosen);
    std::vector<std::string> labels;
    for (const std::string& r : refs_) labels.push_back(ref_label(r));
    ref_box_->set_items(std::move(labels));
    ref_box_->set_current(int(std::ranges::find(refs_, chosen) - refs_.begin()));
    load();
    if (rows_->items.empty()) return;
    select = std::min(select, rows_->items.size() - 1);
    list_->select({select});
    list_->set_current(select);
    list_->scroll_to(select);
}

std::string reflog_window::ref() const
{
    const int i = ref_box_ ? ref_box_->current() : -1;
    return i >= 0 && size_t(i) < refs_.size() ? refs_[size_t(i)] : initial_ref_;
}

void reflog_window::load()
{
    rows_->ref = ref();
    rows_->items.clear();
    // No reflog yet (a ref git hasn't logged, an emptied stash) is no entries.
    if (auto repo = repo::open(workdir_))
    {
        if (auto log = read_reflog(*repo, rows_->ref))
            rows_->items = std::move(*log);
        else
            ui_.message(window_.get(), {.icon = rap::message_spec::icon::error,
                                        .title = "Tortilla",
                                        .text = _("Couldn't read the reflog."),
                                        .detail = log.error()});
    }
    rows_->notify_reset();
    clear_->set_visible(stash());
    clear_->set_enabled(!rows_->items.empty());
    update_info();
}

void reflog_window::update_info()
{
    if (!info_ || !list_) return;
    info_->set_text(rap::i18n::format(_("Showing {0} entries, {1} selected"),
                                      std::to_string(rows_->items.size()),
                                      std::to_string(list_->selection().size())));
}

std::vector<size_t> reflog_window::selection() const
{
    std::vector<size_t> out;
    for (size_t row : list_->selection())
        if (row < rows_->items.size()) out.push_back(row);
    std::ranges::sort(out);
    return out;
}

// -- commands --------------------------------------------------------------------

// A stash's changes; any other entry's commit in the log.
void reflog_window::activate(size_t row)
{
    if (row >= rows_->items.size()) return;
    const reflog_entry& e = rows_->items[row];
    if (stash())
        difftool({e.id + "^", e.id});
    else
        show_log(e.id);
}

rap::menu reflog_window::context_menu()
{
    const auto sel = selection();
    if (sel.empty()) return {};
    rap::menu m;
    const bool wc = !bare_;
    const bool is_stash = stash();

    if (sel.size() == 1)
    {
        const reflog_entry e = rows_->items[sel.front()];
        const size_t index = sel.front();
        if (is_stash)
        {
            m.push_back({.label = _("Show changes"),
                         .action = [this, e] { difftool({e.id + "^", e.id}); }});
            m.push_back({.label = _("Show _log"), .action = [this, e] { show_log(e.id); }});
        }
        else
        {
            m.push_back({.label = _("Show _log"), .action = [this, e] { show_log(e.id); }});
            // What this entry did to the ref: where it was, against where it went.
            if (!e.old_id.empty() && e.old_id != e.id)
                m.push_back({.label = _("Compare with previous position"),
                             .action = [this, e] { difftool({e.old_id, e.id}); }});
        }
        if (wc)
            m.push_back({.label = _("Compare with _working tree"),
                         .action = [this, e] { difftool({e.id}); }});
        m.push_back({});
        if (wc && !is_stash)
        {
            m.push_back(
                {.label = _("Sw_itch/Checkout to this..."),
                 .action = [this, e] { open_dialog(switch_dialog::open(ui_, workdir_, e.id)); }});
            m.push_back(
                {.label = rap::i18n::format(_("R_eset {0} to this..."),
                                            "\"" + menu_text(current_branch(workdir_)) + "\""),
                 .action = [this, e] { reset(e); }});
        }
        m.push_back({.label = _("Create Br_anch at this version..."),
                     .action = [this, e]
                     { open_dialog(create_ref_dialog::open(ui_, workdir_, false, e.id)); }});
        m.push_back({});
        if (is_stash && wc)
        {
            const std::string name = "stash@{" + std::to_string(index) + "}";
            m.push_back(
                {.label = _("Stash Apply"),
                 .action = [this, name] { run(_("Stash Apply"), {{"stash", "apply", name}}); }});
            m.push_back(
                {.label = _("Stash Pop"),
                 .action = [this, name] { run(_("Stash Pop"), {{"stash", "pop", name}}); }});
        }
        m.push_back({.label = is_stash ? _("Drop stash") : _("Delete entry"),
                     .action = [this, sel] { delete_entries(sel); }});
    }
    else
    {
        if (sel.size() == 2)
        {
            // The older entry first.
            const reflog_entry newer = rows_->items[sel[0]], older = rows_->items[sel[1]];
            m.push_back({.label = _("_Compare revisions"),
                         .action = [this, older, newer] { difftool({older.id, newer.id}); }});
            m.push_back({});
        }
        const size_t n = sel.size();
        m.push_back(
            {.label = rap::i18n::format(is_stash ? N_("Drop {0} stash", "Drop {0} stashes", n)
                                                 : N_("Delete {0} entry", "Delete {0} entries", n),
                                        std::to_string(n)),
             .action = [this, sel] { delete_entries(sel); }});
    }
    m.push_back({});
    std::string hashes, messages;
    for (size_t i : sel)
    {
        hashes += (hashes.empty() ? "" : "\n") + rows_->items[i].id;
        const reflog_entry& e = rows_->items[i];
        messages += (messages.empty() ? "" : "\n") +
                    (e.action.empty() ? e.message : e.action + ": " + e.message);
    }
    m.push_back({.label = _("Copy to clipboard"),
                 .children = {
                     {.label = _("SHA-1"), .action = [this, hashes] { ui_.set_clipboard(hashes); }},
                     {.label = _("Messages"),
                      .action = [this, messages] { ui_.set_clipboard(messages); }}}});
    return m;
}

void reflog_window::show_log(const std::string& rev)
{
    auto log = log_window::open(ui_, workdir_);
    if (!log)
    {
        ui_.message(
            window_.get(),
            {.icon = rap::message_spec::icon::error, .title = "Tortilla", .text = log.error()});
        return;
    }
    (*log)->start_at(rev);
    windows::keep(std::move(*log));
}

void reflog_window::difftool(std::vector<std::string> revisions)
{
    difftool_ = compare_revisions(
        ui_, window_.get(), workdir_, std::move(revisions), {},
        [this] { show_settings(ui_, window_.get(), workdir_, settings_page::diff_viewer, {}); });
}

void reflog_window::reset(const reflog_entry& e)
{
    const git_result subject =
        run_git(workdir_, {"log", "-1", "--format=%s", e.id}, git_output::stdout_only);
    std::string summary = subject.ok() ? subject.output : std::string();
    while (!summary.empty() && (summary.back() == '\n' || summary.back() == '\r'))
        summary.pop_back();
    tortilla::reset_to(ui_, window_.get(), workdir_, current_branch(workdir_), e.id, summary,
                       [this, alive = std::weak_ptr<bool>(alive_)](bool)
                       {
                           if (alive.lock()) reload();
                       });
}

// Confirmed, then deleted newest-last, so the indexes of those still to go
// don't move: git reflog delete, or git stash drop for the stash.
void reflog_window::delete_entries(std::vector<size_t> sel)
{
    if (sel.empty()) return;
    const bool is_stash = stash();
    std::vector<std::string> names;
    for (size_t i : sel)
        names.push_back(is_stash ? "stash@{" + std::to_string(i) + "}" : rows_->items[i].selector);
    const size_t n = names.size();
    const std::string text =
        is_stash
            ? rap::i18n::format(
                  N_("Do you really want to drop {0}? It can NOT be recovered!",
                     "Do you really want to drop these {1} stashes? They can NOT be recovered!", n),
                  short_selector(rows_->ref, sel.front()), std::to_string(n))
            : rap::i18n::format(
                  N_("Do you really want to delete {0} from the reflog?",
                     "Do you really want to delete these {1} entries from the reflog?", n),
                  short_selector(rows_->ref, sel.front()), std::to_string(n));
    ui_.message(window_.get(),
                {.icon = rap::message_spec::icon::warning,
                 .title = "Tortilla",
                 .text = text,
                 .buttons = {rap::dialog_role::yes, rap::dialog_role::no}},
                [this, names, is_stash, first = sel.front(),
                 alive = std::weak_ptr<bool>(alive_)](rap::dialog_role role)
                {
                    if (!alive.lock() || role != rap::dialog_role::yes) return;
                    std::vector<std::vector<std::string>> commands;
                    for (auto it = names.rbegin(); it != names.rend(); ++it)
                        commands.push_back(is_stash
                                               ? std::vector<std::string>{"stash", "drop", *it}
                                               : std::vector<std::string>{"reflog", "delete", *it});
                    for (const auto& args : commands)
                        if (const git_result r = run_git(workdir_, args); !r.ok())
                        {
                            ui_.message(window_.get(), {.icon = rap::message_spec::icon::error,
                                                        .title = "Tortilla",
                                                        .text = r.output});
                            break;
                        }
                    reload(first);
                });
}

void reflog_window::clear_stash()
{
    ui_.message(
        window_.get(),
        {.icon = rap::message_spec::icon::warning,
         .title = "Tortilla",
         .text = _("Do you really want to clear all stashed changes? They can NOT be recovered!"),
         .buttons = {rap::dialog_role::yes, rap::dialog_role::no}},
        [this, alive = std::weak_ptr<bool>(alive_)](rap::dialog_role role)
        {
            if (!alive.lock() || role != rap::dialog_role::yes) return;
            if (const git_result r = run_git(workdir_, {"stash", "clear"}); !r.ok())
                ui_.message(window_.get(), {.icon = rap::message_spec::icon::error,
                                            .title = "Tortilla",
                                            .text = r.output});
            reload();
        });
}

// In the progress dialog (stash apply and pop touch the working tree), then
// the entries again.
void reflog_window::run(const std::string& title, std::vector<std::vector<std::string>> commands)
{
    std::vector<git_command> c(commands.begin(), commands.end());
    run_progress_dialog(ui_, window_.get(), title, workdir_, std::move(c),
                        [this, alive = std::weak_ptr<bool>(alive_)](bool)
                        {
                            if (alive.lock()) reload(list_->current().value_or(0));
                        });
}

// A dialog opened from here: kept like any window, and the entries reloaded
// once what it started has run.
template <class D>
void reflog_window::open_dialog(std::expected<std::unique_ptr<D>, std::string> dialog)
{
    if (!dialog)
    {
        ui_.message(
            window_.get(),
            {.icon = rap::message_spec::icon::error, .title = "Tortilla", .text = dialog.error()});
        return;
    }
    (*dialog)->on_done(
        [this, alive = std::weak_ptr<bool>(alive_)]
        {
            if (alive.lock()) reload(list_->current().value_or(0));
        });
    windows::keep(std::move(*dialog));
}

}  // namespace tortilla
