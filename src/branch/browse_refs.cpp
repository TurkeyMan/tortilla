#include "browse_refs.h"

#include "branch/create_ref_dialog.h"
#include "branch/merge_dialog.h"
#include "branch/switch_dialog.h"
#include "git/command.h"
#include "git/remotes.h"
#include "git/repo.h"
#include "i18n.h"
#include "log/log_window.h"
#include "remote/pull_fetch_dialog.h"
#include "settings.h"
#include "ui/menu_text.h"
#include "ui/progress_dialog.h"
#include "ui/windows.h"
#include "util/calendar.h"

#include <rapport/sys.h>

#include <algorithm>
#include <cctype>
#include <ctime>
#include <map>

namespace tortilla
{
namespace
{

// The list's columns [spec 10.5].
enum column : size_t
{
    name,
    tracked,
    author_date,
    subject,
    author,
    commit_date,
    committer,
    hash,
    description,
    column_count,
};

std::vector<rap::column> columns()
{
    using namespace rap::literals;
    std::vector<rap::column> c(column_count);
    c[name] = {.title = _("Branch Name"), .width = 24_ch, .sortable = true};
    c[tracked] = {.title = _("Tracked branch"), .width = 18_ch, .sortable = true};
    c[author_date] = {.title = _("Last Author Date"), .width = 19_ch, .sortable = true};
    c[subject] = {.title = _("Last Commit"), .width = 300_px, .stretch = true, .sortable = true};
    c[author] = {.title = _("Last Author"), .width = 16_ch, .sortable = true};
    c[commit_date] = {.title = _("Date Last Commit"), .width = 19_ch, .sortable = true};
    c[committer] = {.title = _("Last Committer"), .width = 16_ch, .sortable = true};
    c[hash] = {.title = _("SHA-1"), .width = 42_ch, .sortable = true};
    c[description] = {.title = _("Description"), .width = 80_px, .sortable = true};
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

int compare_text(const std::string& a, const std::string& b)
{
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i)
    {
        const int x = std::tolower(static_cast<unsigned char>(a[i])), y = std::tolower(static_cast<unsigned char>(b[i]));
        if (x != y) return x < y ? -1 : 1;
    }
    return a.size() == b.size() ? 0 : a.size() < b.size() ? -1 : 1;
}

std::string parent_of(const std::string& full)
{
    const size_t slash = full.rfind('/');
    return slash == std::string::npos ? std::string() : full.substr(0, slash);
}

// "origin" and "x" from refs/remotes/origin/x, for a configured remote.
std::optional<std::pair<std::string, std::string>> split_remote(const std::string& workdir, const std::string& full)
{
    if (!full.starts_with("refs/remotes/")) return std::nullopt;
    const std::string rest = full.substr(13);
    for (const std::string& r : remote_names(workdir))
        if (rest.starts_with(r + "/")) return std::pair{r, rest.substr(r.size() + 1)};
    return std::nullopt;
}

}  // namespace

// The refs of the selected folder, as the list shows them.
class browse_refs::rows : public rap::list_model
{
public:
    std::vector<const ref_entry*> items;
    std::string base;  // the folder, whose path the names are relative to
    size_t sort_column = name;
    bool descending = false;

    size_t row_count() const override { return items.size(); }
    std::string text(size_t row, size_t col) const override
    {
        if (row >= items.size()) return {};
        const ref_entry& e = *items[row];
        switch (col)
        {
        case name: return relative(e);
        case tracked: return e.upstream;
        case author_date: return date_text(e.author_date);
        case subject: return e.subject;
        case author: return e.author;
        case commit_date: return date_text(e.commit_date);
        case committer: return e.committer;
        case hash: return e.hash;
        case description: return e.description;
        default: return {};
        }
    }
    std::string relative(const ref_entry& e) const { return e.full.substr(base.size() + 1); }

    void set(std::vector<const ref_entry*> rows, std::string folder)
    {
        items = std::move(rows);
        base = std::move(folder);
        sort();
        notify_reset();
    }
    // By the sort column, the full name for names [spec 10.5].
    void sort()
    {
        auto key = [this](const ref_entry& e) -> std::string
        {
            switch (sort_column)
            {
            case tracked: return e.upstream;
            case subject: return e.subject;
            case author: return e.author;
            case committer: return e.committer;
            case hash: return e.hash;
            case description: return e.description;
            default: return e.full;
            }
        };
        std::ranges::stable_sort(items,
                                 [&](const ref_entry* a, const ref_entry* b)
                                 {
                                     int c = 0;
                                     if (sort_column == author_date)
                                         c = a->author_date < b->author_date ? -1 : a->author_date > b->author_date;
                                     else if (sort_column == commit_date)
                                         c = a->commit_date < b->commit_date ? -1 : a->commit_date > b->commit_date;
                                     else
                                         c = compare_text(key(*a), key(*b));
                                     return descending ? c > 0 : c < 0;
                                 });
    }
};

std::expected<std::unique_ptr<browse_refs>, std::string>
browse_refs::open(rap::ui& ui, const std::string& path, options o, rap::window* parent)
{
    auto repo = repo::open(path.empty() ? "." : path);
    if (!repo) return std::unexpected(repo.error());
    std::string dir = repo->workdir().empty() ? repo->git_dir() : repo->workdir();
    std::unique_ptr<browse_refs> d(new browse_refs(ui, std::move(dir), std::move(o), parent));
    d->bare_ = repo->bare();
    d->build();
    return d;
}

browse_refs::browse_refs(rap::ui& ui, std::string workdir, options o, rap::window* parent)
    : ui_(ui)
    , workdir_(std::move(workdir))
    , options_(std::move(o))
    , parent_(parent)
    , rows_(std::make_unique<rows>())
{
}

browse_refs::~browse_refs() = default;

void browse_refs::build()
{
    using namespace rap::layout;
    using namespace rap::literals;
    settings& st = settings::get();
    if (auto v = st.numbers("browseRefs.sort"); v.size() == 2)
    {
        rows_->sort_column = std::min(size_t(v[0]), size_t(column_count - 1));
        rows_->descending = v[1] != 0;
    }
    std::string root = workdir_;
    while (root.size() > 1 && (root.back() == '/' || root.back() == '\\')) root.pop_back();
    root = rap::sys::path_to_utf8(rap::sys::path_from_utf8(root).make_preferred());

    window_ = ui_.create(
        dialog{
            rap::i18n::format(_("{0} - Browse references - Tortilla"), root),
            {vbox{
                hbox{
                    label{_("Filter:")},
                    entry{}
                        .placeholder(std::string("   ") + _("Filter by Refname, Subject, Authors, SHA-1"))
                        .bind(filter_)
                        .on_changed(
                            [this](const std::string&)
                            {
                                // A second after the typing stops [spec 10.6].
                                filter_timer_ = ui_.timer(1000, false, [this] { fill(); });
                            })
                        .on_activate([this] { fill(); })
                        .stretch(),
                    drop_down{{_("All"), _("Only merged (to HEAD)"), _("Only unmerged (to HEAD)")}, 0}
                        .bind(merged_)
                        .on_changed([this](int) { reload(); }),
                },
                splitter{
                    horizontal,
                    tree{}
                        .bind(tree_)
                        .on_selection_changed([this] { fill(); })
                        .on_context_menu([this](std::optional<std::string> id) -> rap::menu
                                         { return folder_menu(id.value_or(std::string())); }),
                    list{columns(), rows_.get()}
                        .bind(list_)
                        .on_selection_changed([this] { update_info(); })
                        .on_activate(
                            [this](size_t row)
                            {
                                if (row >= rows_->items.size()) return;
                                if (options_.pick)
                                    ok();
                                else
                                    show_log(rows_->items[row]->full);
                            })
                        .on_sort_requested(
                            [this](size_t col, rap::sort_order order)
                            {
                                rows_->sort_column = col;
                                rows_->descending = order == rap::sort_order::descending;
                                rows_->sort();
                                list_->set_sort_indicator(col, order);
                                list_->select({});
                                rows_->set(rows_->items, rows_->base);
                            })
                        .on_context_menu([this](std::optional<size_t> row) { return list_menu(row); })
                        .selection(options_.pick && !options_.multiple ? rap::selection_mode::single
                                                                       : rap::selection_mode::multi)
                        .stretch(3),
                }
                    .bind(splitter_)
                    .stretch(),
                hbox{
                    check{_("Show _nested refs"), st.value("browseRefs.nested", "1") == "1"}
                        .bind(nested_)
                        .on_toggled(
                            [this](bool on)
                            {
                                settings::get().set("browseRefs.nested", on ? "1" : "0");
                                fill();
                            }),
                    label{}.bind(info_).stretch(),
                    button{_("Current Branch")}.on_click(
                        [this]
                        {
                            // The short name, or the hash when detached [spec 10.12].
                            std::string b = current_branch(workdir_);
                            finish(b.empty() || b == "HEAD" ? resolve(workdir_, "HEAD") : b);
                        }),
                    buttons{dialog_button{rap::dialog_role::ok, ""}.on_click([this] { ok(); }), cancel},
                },
            }}}
            .size(130_ch, 30_lines),
        parent_);
    st.restore_window("browseRefs.window", *window_);
    if (auto v = st.numbers("browseRefs.columns"); v.size() == column_count) list_->set_column_widths(v);
    // The folders get a fifth or so at first, as TortoiseGit's fixed tree has.
    if (auto v = st.numbers("browseRefs.panes"); v.size() == 2)
        splitter_->set_sizes(v);
    else
        splitter_->set_sizes({200, 0});
    list_->set_sort_indicator(rows_->sort_column,
                              rows_->descending ? rap::sort_order::descending : rap::sort_order::ascending);
    window_->on_key(
        [this](const rap::key_event& e)
        {
            if (!e.press) return false;
            if (e.key == rap::key::f5)
            {
                reload();
                return true;
            }
            if (e.key == rap::key::character && e.codepoint == U'e' && (e.modifiers & rap::mod_ctrl))
            {
                filter_->focus();
                return true;
            }
            return false;
        });
    window_->on_finished(
        [this](rap::dialog_role)
        {
            settings& st = settings::get();
            st.save_window("browseRefs.window", *window_);
            st.set_numbers("browseRefs.columns", list_->column_widths());
            st.set_numbers("browseRefs.panes", splitter_->sizes());
            st.set_numbers("browseRefs.sort", {float(rows_->sort_column), rows_->descending ? 1.f : 0.f});
            if (!done_ && picked_) std::exchange(picked_, {})(std::string());
            if (closed_) closed_();
        });
    reload(options_.initial.empty() ? std::string("HEAD") : options_.initial);
    list_->focus();  // as TortoiseGit has it [spec 10.1]
}

// Loads the refs again [spec 10.3], selecting `select` (a ref, a folder, or
// a short name), or keeping what was selected.
void browse_refs::reload(std::string select)
{
    if (select.empty())
    {
        const auto sel = selection();
        select = !sel.empty() ? sel.front()->full : folder();
    }
    const std::string head = current_branch(workdir_);
    current_branch_ = head.empty() || head == "HEAD" ? std::string() : "refs/heads/" + head;
    if (select == "HEAD") select = current_branch_.empty() ? std::string("refs/heads") : current_branch_;

    const int m = merged_ ? merged_->current() : 0;
    ref_list_result loaded = list_refs(workdir_, m == 1   ? merged_filter::merged
                                                 : m == 2 ? merged_filter::unmerged
                                                          : merged_filter::all);
    if (!loaded.error.empty())
        ui_.message(window_.get(), {.icon = rap::message_spec::icon::error,
                                    .title = "Tortilla",
                                    .text = _("Couldn't list the references."),
                                    .detail = loaded.error});
    // The kinds a picker offers [spec 10.3, 4].
    const unsigned k = options_.kinds;
    std::erase_if(loaded.refs,
                  [k](const ref_entry& e)
                  {
                      if (e.local_branch()) return !(k & heads);
                      if (e.tag()) return !(k & tags);
                      if (e.remote_branch()) return !(k & remotes);
                      return k == remotes;
                  });
    refs_ = std::move(loaded.refs);

    // Folders, each under its parent, labelled with its last segment.
    std::map<std::string, std::vector<std::string>> children;
    for (const std::string& f : ref_folders(refs_))
        if (f != "refs") children[parent_of(f)].push_back(f);
    std::function<rap::tree_item(const std::string&)> item = [&](const std::string& f)
    {
        rap::tree_item t{.label = f.substr(f.rfind('/') + 1), .id = f};
        for (const std::string& c : children[f]) t.children.push_back(item(c));
        return t;
    };
    rap::tree_item top = item("refs");
    top.expanded = true;
    tree_->set_items({std::move(top)});

    // A short name is the first ref ending in it at a '/' [spec 10.3, 8].
    if (!select.starts_with("refs"))
    {
        auto it = std::ranges::find_if(refs_, [&](const ref_entry& e) { return e.full.ends_with("/" + select); });
        select = it != refs_.end() ? it->full : std::string("refs");
    }
    const bool leaf = std::ranges::any_of(refs_, [&](const ref_entry& e) { return e.full == select; });
    std::string folder = leaf ? parent_of(select) : select;
    // An unknown folder: its deepest existing one.
    const auto folders = ref_folders(refs_);
    while (!folder.empty() && !std::ranges::contains(folders, folder)) folder = parent_of(folder);
    tree_->select(folder.empty() ? "refs" : folder);
    fill();
    if (leaf)
        for (size_t i = 0; i < rows_->items.size(); ++i)
            if (rows_->items[i]->full == select)
            {
                list_->select({i});
                list_->set_current(i);
                list_->scroll_to(i);
                break;
            }
    update_info();
}

std::string browse_refs::folder() const
{
    return tree_ ? tree_->selected().value_or(std::string()) : std::string();
}

// The selected folder's refs, and its folders' with nested refs on, that
// the filter lets through [spec 10.5, 10.6].
void browse_refs::fill()
{
    filter_timer_ = {};
    const std::string base = folder();
    // Tracked branch and Description are about local branches [spec 10.5].
    const bool heads_folder = base == "refs/heads" || base.starts_with("refs/heads/");
    list_->set_column_visible(tracked, heads_folder);
    list_->set_column_visible(description, heads_folder);
    if (base.empty())
    {
        rows_->set({}, {});
        update_info();
        return;
    }
    const bool nested = nested_->checked();
    const std::string filter = filter_->text();
    std::vector<const ref_entry*> rows;
    for (const ref_entry& e : refs_)
    {
        if (!e.full.starts_with(base + "/")) continue;
        const std::string rel = e.full.substr(base.size() + 1);
        if (!nested && rel.find('/') != std::string::npos) continue;
        if (!filter.empty() &&
            !ref_filter_matches(filter, rel + '\n' + e.subject + '\n' + e.author + '\n' + e.hash))
            continue;
        rows.push_back(&e);
    }
    rows_->set(std::move(rows), base);
    update_info();
}

void browse_refs::update_info()
{
    if (!info_ || !list_) return;
    info_->set_text(rap::i18n::format(_("Showing {0} ref(s), {1} ref(s) selected"),
                                      std::to_string(rows_->items.size()), std::to_string(list_->selection().size())));
}

std::vector<const ref_entry*> browse_refs::selection() const
{
    std::vector<const ref_entry*> out;
    if (!list_) return out;
    for (size_t row : list_->selection())
        if (row < rows_->items.size()) out.push_back(rows_->items[row]);
    return out;
}

// The full ref for one row, short names for several [spec 10.12].
std::string browse_refs::picked_text() const
{
    const auto sel = selection();
    if (sel.size() == 1) return sel.front()->full;
    std::string out;
    for (const ref_entry* e : sel) out += (out.empty() ? "" : " ") + strip_ref_name(e->full);
    return out;
}

void browse_refs::ok() { finish(options_.pick ? picked_text() : std::string()); }

void browse_refs::finish(std::string picked)
{
    done_ = true;
    auto callback = std::exchange(picked_, {});
    window_->close();
    if (callback) callback(picked);
}

// -- menus [spec 10.7] ---------------------------------------------------------

rap::menu browse_refs::list_menu(std::optional<size_t>)
{
    const auto sel = selection();
    if (sel.empty()) return folder_menu(folder());
    rap::menu m;
    const bool wc = !bare_;
    auto same_kind = [&](auto pred) { return std::ranges::all_of(sel, [&](const ref_entry* e) { return pred(*e); }); };
    auto copy_names = [this, sel]() -> rap::menu_item
    {
        std::string names;
        for (const ref_entry* e : sel) names += (names.empty() ? "" : "\n") + strip_ref_name(e->full);
        return {.label = _("Copy ref names"), .action = [this, names] { ui_.set_clipboard(names); }};
    };

    if (sel.size() == 1)
    {
        const ref_entry e = *sel.front();
        if (options_.pick)
        {
            m.push_back({.label = _("Select"), .action = [this] { ok(); }});
            m.push_back({});
        }
        if (e.commit()) m.push_back({.label = _("Show _log"), .action = [this, e] { show_log(e.full); }});
        if (e.remote_branch())
            if (auto rb = split_remote(workdir_, e.full))
                m.push_back({.label = rap::i18n::format(_("Fetch from \"{0}\""), menu_text(rb->first)),
                             .action = [this] { open_dialog(pull_fetch_dialog::open(ui_, workdir_, false)); }});
        if (wc && e.commit())
        {
            m.push_back({});
            if (e.full != current_branch_)
            {
                const std::string into =
                    current_branch_.empty() ? std::string("(no branch)") : strip_ref_name(current_branch_);
                m.push_back({.label = rap::i18n::format(_("_Merge to \"{0}\"..."), menu_text(into)),
                             .action = [this, e] { open_dialog(merge_dialog::open(ui_, workdir_, e.full)); }});
            }
            m.push_back({.label = _("Sw_itch/Checkout to this..."),
                         .action = [this, e] { open_dialog(switch_dialog::open(ui_, workdir_, e.full)); }});
        }
        rap::menu edit;
        // From a remote branch, with it as the base so tracking is offered.
        if (e.remote_branch())
            edit.push_back({.label = _("Create _Branch..."),
                            .action = [this, e]
                            { open_dialog(create_ref_dialog::open(ui_, workdir_, false, strip_ref_name(e.full))); }});
        if (e.local_branch() && e.commit())
            edit.push_back({.label = _("Edit description"), .action = [this, e] { edit_description(e); }});
        if (e.local_branch()) edit.push_back({.label = _("Rename"), .action = [this, e] { rename(e); }});
        if (!edit.empty())
        {
            m.push_back({});
            m.insert(m.end(), edit.begin(), edit.end());
        }
        if (wc && e.commit() && e.local_branch())
        {
            m.push_back({});
            if (!e.upstream.empty())
                m.push_back({.label = _("Unset tracked branch"),
                             .action = [this, e]
                             {
                                 run_then_reload(_("Unset tracked branch"),
                                                 {{"branch", "--unset-upstream", strip_ref_name(e.full)}});
                             }});
            m.push_back({.label = _("Select tracked branch"), .action = [this, e] { pick_upstream(e); }});
        }
        if (e.local_branch() || e.remote_branch() || e.tag())
        {
            m.push_back({});
            m.push_back({.label = e.remote_branch() ? _("Delete remote branch")
                                  : e.tag()         ? _("Delete tag")
                                                    : _("Delete branch"),
                         .action = [this, sel] { delete_refs(sel); }});
        }
    }
    else
    {
        const auto n = sel.size();
        if (same_kind([](const ref_entry& e) { return e.remote_branch(); }))
            m.push_back({.label = rap::i18n::format(N_("Delete {0} remote branch", "Delete {0} remote branches", n),
                                                    std::to_string(n)),
                         .action = [this, sel] { delete_refs(sel); }});
        else if (same_kind([](const ref_entry& e) { return e.local_branch(); }))
            m.push_back({.label = rap::i18n::format(N_("Delete {0} branch", "Delete {0} branches", n), std::to_string(n)),
                         .action = [this, sel] { delete_refs(sel); }});
        else if (same_kind([](const ref_entry& e) { return e.tag(); }))
            m.push_back({.label = rap::i18n::format(N_("Delete {0} tag", "Delete {0} tags", n), std::to_string(n)),
                         .action = [this, sel] { delete_refs(sel); }});
    }
    if (!m.empty()) m.push_back({});
    m.push_back(copy_names());
    return m;
}

rap::menu browse_refs::folder_menu(const std::string& f)
{
    rap::menu m;
    auto under = [&](const std::string& p) { return f == p || f.starts_with(p + "/"); };
    if (under("refs/remotes"))
    {
        const std::string rest = f.size() > 13 ? f.substr(13) : std::string();
        for (const std::string& r : remote_names(workdir_))
            if (rest == r || rest.starts_with(r + "/"))
                m.push_back({.label = rap::i18n::format(_("Fetch from \"{0}\""), menu_text(r)),
                             .action = [this] { open_dialog(pull_fetch_dialog::open(ui_, workdir_, false)); }});
    }
    if (under("refs/heads"))
        m.push_back({.label = _("Create _Branch..."),
                     .action = [this] { open_dialog(create_ref_dialog::open(ui_, workdir_, false)); }});
    if (under("refs/tags"))
    {
        m.push_back({.label = _("Create _Tag..."),
                     .action = [this] { open_dialog(create_ref_dialog::open(ui_, workdir_, true)); }});
        // Every tag the list shows [spec 10.9]; nothing to do with none.
        std::vector<const ref_entry*> shown(rows_->items.begin(), rows_->items.end());
        m.push_back({.label = _("Delete all tags"),
                     .action = [this, shown] { delete_refs(shown); },
                     .enabled = !shown.empty() && f == folder()});
    }
    if (!m.empty()) m.push_back({});
    m.push_back({.label = _("Copy ref names"),
                 .action = [this, f] { ui_.set_clipboard(f.empty() ? f : strip_ref_name(f)); }});
    return m;
}

// -- commands [spec 10.8] ------------------------------------------------------

void browse_refs::show_log(const std::string& rev)
{
    auto log = log_window::open(ui_, workdir_);
    if (!log)
    {
        ui_.message(window_.get(), {.icon = rap::message_spec::icon::error, .title = "Tortilla", .text = log.error()});
        return;
    }
    (*log)->start_at(rev);
    windows::keep(std::move(*log));
}

// A dialog opened from here: kept like any window, and the refs reloaded
// when it closes (Switch and Merge too, which TortoiseGit doesn't).
template <class D>
void browse_refs::open_dialog(std::expected<std::unique_ptr<D>, std::string> dialog)
{
    if (!dialog)
    {
        ui_.message(window_.get(), {.icon = rap::message_spec::icon::error, .title = "Tortilla", .text = dialog.error()});
        return;
    }
    std::weak_ptr<bool> alive = alive_;
    windows::keep(std::move(*dialog),
                  [this, alive]
                  {
                      if (alive.lock()) reload();
                  });
}

// Local commands one after another; the first failure is shown.
void browse_refs::run_then_reload(const std::string& title, std::vector<std::vector<std::string>> commands)
{
    for (const auto& args : commands)
        if (const git_result r = run_git(workdir_, args); !r.ok())
        {
            ui_.message(window_.get(), {.icon = rap::message_spec::icon::error, .title = title, .text = r.output});
            break;
        }
    reload();
}

// Confirmed with TortoiseGit's texts, then deleted: local branches forced
// (the question warns when one isn't merged into HEAD), tags, and remote
// branches on their remote, a push for each [spec 10.9].
void browse_refs::delete_refs(std::vector<const ref_entry*> sel)
{
    if (sel.empty()) return;
    std::vector<ref_entry> refs;
    for (const ref_entry* e : sel) refs.push_back(*e);
    const ref_entry& first = refs.front();
    std::string text;
    bool warn = false;
    if (refs.size() == 1)
    {
        text = rap::i18n::format(_("Do you really want to delete \"{0}\"?"),
                                 first.remote_branch() ? first.full.substr(13) : strip_ref_name(first.full));
        if (!first.tag() && !is_ancestor(workdir_, first.full, "HEAD"))
        {
            text += std::string("\n\n") + _("This branch is not fully merged into HEAD.");
            warn = true;
        }
    }
    else
    {
        text = rap::i18n::format(N_("Do you really want to permanently delete the selected ref? It can NOT be recovered!",
                                    "Do you really want to permanently delete the {0} selected refs? It can NOT be recovered!",
                                    refs.size()),
                                 std::to_string(refs.size()));
        if (!first.tag())
        {
            text += std::string("\n\n") + _("It has not been checked if these branches have been fully merged into HEAD.");
            warn = true;
        }
    }
    if (first.remote_branch())
    {
        text += std::string("\n\n") + _("This action will remove the branches on the remote.");
        warn = true;
    }
    ui_.message(
        window_.get(),
        {.icon = warn ? rap::message_spec::icon::warning : rap::message_spec::icon::question,
         .title = "Tortilla",
         .text = text,
         .buttons = {rap::dialog_role::yes, rap::dialog_role::no}},
        [this, refs, alive = std::weak_ptr<bool>(alive_)](rap::dialog_role role)
        {
            if (!alive.lock()) return;
            if (role != rap::dialog_role::yes) return reload();
            if (refs.front().remote_branch())
            {
                // One push per remote, deleting its branches together.
                std::map<std::string, std::vector<std::string>> by_remote;
                for (const ref_entry& e : refs)
                    if (auto rb = split_remote(workdir_, e.full)) by_remote[rb->first].push_back(":refs/heads/" + rb->second);
                std::vector<git_command> commands;
                for (auto& [remote, specs] : by_remote)
                {
                    std::vector<std::string> args{"push", "--", remote};
                    args.insert(args.end(), specs.begin(), specs.end());
                    commands.push_back(std::move(args));
                }
                run_progress_dialog(ui_, window_.get(), _("Deleting remote refs..."), workdir_, std::move(commands),
                                    [this, alive](bool)
                                    {
                                        if (alive.lock()) reload();
                                    });
                return;
            }
            std::vector<std::string> args = refs.front().tag() ? std::vector<std::string>{"tag", "-d", "--"}
                                                                : std::vector<std::string>{"branch", "-D", "--"};
            for (const ref_entry& e : refs) args.push_back(e.tag() ? e.full.substr(10) : strip_ref_name(e.full));
            run_then_reload(_("Delete"), {std::move(args)});
        });
}

// A small dialog asking for one line of text.
namespace
{

void ask_text(rap::ui& ui, rap::window* parent, const std::string& title, const std::string& prompt,
              const std::string& initial, std::function<void(const std::string&)> done)
{
    using namespace rap::layout;
    using namespace rap::literals;
    struct state
    {
        std::unique_ptr<rap::window> window;
        rap::entry* text = nullptr;
    };
    auto st = std::make_shared<state>();
    st->window = ui.create(dialog{title, {vbox{label{prompt}, entry{initial}.bind(st->text), buttons{ok, cancel}}}}
                               .size(60_ch, {}),
                           parent);
    st->window->on_finished(
        [st, done, &ui](rap::dialog_role role)
        {
            const std::string value = st->text->text();
            ui.post([st] {});  // the window goes after this returns
            if (role == rap::dialog_role::ok) done(value);
        });
    st->window->show();
    st->text->focus();
}

}  // namespace

// Renamed within its folder: the new name is relative to the selected
// folder, as TortoiseGit's inline edit has it [spec 10.10].
void browse_refs::rename(const ref_entry& e)
{
    const std::string base = folder();
    const std::string rel = base.empty() ? e.full : e.full.substr(base.size() + 1);
    ask_text(ui_, window_.get(), _("Rename"), _("New name:"), rel,
             [this, e, base, alive = std::weak_ptr<bool>(alive_)](const std::string& text)
             {
                 if (!alive.lock() || text.empty()) return;
                 const std::string to = base + "/" + text;
                 if (!to.starts_with("refs/heads/"))
                 {
                     ui_.message(window_.get(), {.icon = rap::message_spec::icon::error,
                                                 .title = "Tortilla",
                                                 .text = _("You cannot change the type of this ref with a rename.")});
                     return;
                 }
                 run_then_reload(_("Rename"), {{"branch", "-m", strip_ref_name(e.full), strip_ref_name(to)}});
             });
}

void browse_refs::edit_description(const ref_entry& e)
{
    ask_text(ui_, window_.get(), _("Edit description"), _("Edit description"), e.description,
             [this, e, alive = std::weak_ptr<bool>(alive_)](const std::string& text)
             {
                 if (!alive.lock()) return;
                 std::string t = text;
                 while (!t.empty() && std::isspace(static_cast<unsigned char>(t.back()))) t.pop_back();
                 const std::string key = "branch." + strip_ref_name(e.full) + ".description";
                 if (t.empty())
                     run_then_reload(_("Edit description"), {{"config", "--unset", key}});
                 else
                     run_then_reload(_("Edit description"), {{"config", key, t}});
             });
}

// A remote branch, picked here, becomes the branch's upstream [spec 10.8].
void browse_refs::pick_upstream(const ref_entry& e)
{
    pick_ref(ui_, window_.get(), workdir_, {.kinds = remotes, .pick = true},
             [this, e, alive = std::weak_ptr<bool>(alive_)](const std::string& picked)
             {
                 if (!alive.lock()) return;
                 const auto rb = split_remote(workdir_, picked);
                 if (!rb) return;
                 const std::string upstream = rb->first + "/" + rb->second;
                 const git_result r =
                     run_git(workdir_, {"branch", "--set-upstream-to=" + upstream, "--", strip_ref_name(e.full)});
                 if (!r.ok())
                     ui_.message(window_.get(),
                                 {.icon = rap::message_spec::icon::error,
                                  .title = "Tortilla",
                                  .text = r.output,
                                  .detail = rap::i18n::format(_("This is usually caused when the setting "
                                                                "\"remote.{0}.fetch\" does not include the desired branch."),
                                                              rb->first)});
                 reload();
             });
}

void pick_ref(rap::ui& ui, rap::window* parent, const std::string& workdir, browse_refs::options o,
              std::function<void(const std::string&)> done)
{
    o.pick = true;
    auto d = browse_refs::open(ui, workdir, std::move(o), parent);
    if (!d)
    {
        ui.message(parent, {.icon = rap::message_spec::icon::error, .title = "Tortilla", .text = d.error()});
        if (done) done({});
        return;
    }
    (*d)->on_picked(std::move(done));
    windows::keep(std::move(*d));
}

}  // namespace tortilla
