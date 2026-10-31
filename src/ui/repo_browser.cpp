#include "repo_browser.h"

#include "blame/blame_window.h"
#include "branch/browse_refs.h"
#include "git/command.h"
#include "git/repo.h"
#include "i18n.h"
#include "log/log_window.h"
#include "settings.h"
#include "ui/compare.h"
#include "ui/menu_items.h"
#include "ui/repo_icon.h"
#include "ui/settings_dialog.h"
#include "ui/windows.h"

#include <rapport/sys.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <map>

namespace tortilla
{
namespace
{

namespace fs = std::filesystem;

enum column : size_t
{
    name,
    extension,
    size,
    type,
    column_count,
};

std::vector<rap::column> columns()
{
    using namespace rap::literals;
    std::vector<rap::column> c(column_count);
    c[name] = {.title = _("Name"), .width = 40_ch, .sortable = true};
    c[extension] = {.title = _("Extension"), .width = 12_ch, .sortable = true};
    c[size] = {.title = _("Size"), .width = 12_ch, .sortable = true, .align = rap::align::end};
    c[type] = {.title = _("Type"), .width = 20_ch, .sortable = true};
    return c;
}

bool is_folder(const tree_entry& e)
{
    return e.type == tree_entry::kind::folder || e.type == tree_entry::kind::submodule;
}

bool is_blob(const tree_entry& e)
{
    return !is_folder(e);
}

std::string extension_of(const tree_entry& e)
{
    if (is_folder(e)) return {};
    const size_t dot = e.name.rfind('.');
    return dot == std::string::npos || dot == 0 ? std::string() : e.name.substr(dot + 1);
}

// As Explorer shows sizes: bytes, then KB, MB, GB to three figures.
std::string size_text(uint64_t n)
{
    if (n < 1024) return rap::i18n::format(_("{0} bytes"), std::to_string(n));
    const char* units[] = {"KB", "MB", "GB", "TB"};
    double v = double(n) / 1024;
    size_t u = 0;
    while (v >= 1024 && u + 1 < std::size(units))
    {
        v /= 1024;
        ++u;
    }
    char buf[32];
    std::snprintf(buf, sizeof buf,
                  v < 10    ? "%.2f %s"
                  : v < 100 ? "%.1f %s"
                            : "%.0f %s",
                  v, units[u]);
    return buf;
}

std::string type_text(const tree_entry& e)
{
    switch (e.type)
    {
    case tree_entry::kind::folder: return _("Folder");
    case tree_entry::kind::executable: return _("Executable file");
    case tree_entry::kind::symlink: return _("Symbolic link");
    case tree_entry::kind::submodule: return _("Submodule");
    default: return _("File");
    }
}

int compare_text(const std::string& a, const std::string& b)
{
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i)
    {
        const int x = std::tolower(static_cast<unsigned char>(a[i])),
                  y = std::tolower(static_cast<unsigned char>(b[i]));
        if (x != y) return x < y ? -1 : 1;
    }
    return a.size() == b.size() ? 0 : a.size() < b.size() ? -1 : 1;
}

std::string parent_of(const std::string& path)
{
    const size_t slash = path.rfind('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

std::string last_part(const std::string& path)
{
    return path.substr(path.rfind('/') + 1);
}

// The tree's ids: "/" the top, "/a/b" a folder, so none is empty.
std::string tree_id(const std::string& folder)
{
    return "/" + folder;
}

bool write_file(const fs::path& path, const std::string& content)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::FILE* f = rap::sys::fopen(path, "wb");
    bool ok = f && std::fwrite(content.data(), 1, content.size(), f) == content.size();
    if (f) ok = std::fclose(f) == 0 && ok;
    return ok;
}

}  // namespace

// The selected folder's entries: folders first, then files, each sorted.
class repo_browser::rows : public rap::list_model
{
public:
    std::vector<tree_entry> items;
    size_t sort_column = name;
    bool descending = false;

    size_t row_count() const override { return items.size(); }
    std::string text(size_t row, size_t col) const override
    {
        if (row >= items.size()) return {};
        const tree_entry& e = items[row];
        switch (col)
        {
        case name: return e.name;
        case extension: return extension_of(e);
        case size: return is_blob(e) ? size_text(e.size) : std::string();
        case type: return type_text(e);
        default: return {};
        }
    }
    rap::icon icon(size_t row, size_t col) const override
    {
        if (col != name || row >= items.size()) return {};
        return rap::icon::for_file(items[row].name + (is_folder(items[row]) ? "/" : ""));
    }

    void set(std::vector<tree_entry> entries)
    {
        items = std::move(entries);
        sort();
        notify_reset();
    }
    void sort()
    {
        std::ranges::stable_sort(items,
                                 [this](const tree_entry& a, const tree_entry& b)
                                 {
                                     if (is_folder(a) != is_folder(b)) return is_folder(a);
                                     int c = 0;
                                     if (sort_column == size)
                                         c = a.size < b.size ? -1 : a.size > b.size;
                                     else if (sort_column == extension)
                                         c = compare_text(extension_of(a), extension_of(b));
                                     else if (sort_column == type)
                                         c = compare_text(type_text(a), type_text(b));
                                     if (c == 0) c = compare_text(a.name, b.name);
                                     return descending ? c > 0 : c < 0;
                                 });
    }
};

std::expected<std::unique_ptr<repo_browser>, std::string>
repo_browser::open(rap::ui& ui, const std::string& path, const std::string& revision)
{
    // A path gone from the working tree (one only the revision has) is
    // looked for from the nearest folder that's there.
    std::error_code ec;
    const fs::path abs =
        fs::absolute(rap::sys::path_from_utf8(path.empty() ? "." : path), ec).lexically_normal();
    fs::path there = abs;
    while (!fs::exists(there, ec) && there.has_parent_path() && there.parent_path() != there)
        there = there.parent_path();
    auto repo = repo::open(rap::sys::path_to_utf8(there));
    if (!repo) return std::unexpected(repo.error());
    const bool bare = repo->bare();
    std::unique_ptr<repo_browser> b(new repo_browser(ui, bare ? repo->git_dir() : repo->workdir()));
    b->bare_ = bare;
    b->repo_ = std::make_unique<tortilla::repo>(std::move(*repo));

    // Where to start: the folder given, or a file's folder with it selected.
    std::string folder, select;
    if (!bare)
    {
        const fs::path rel = fs::relative(abs, rap::sys::path_from_utf8(b->workdir_), ec);
        std::string r = ec ? std::string() : rap::sys::path_to_utf8(rel);
        std::ranges::replace(r, '\\', '/');
        while (r.ends_with('/')) r.pop_back();
        if (r == "." || r == ".." || r.starts_with("../") || r == ".git" || r.starts_with(".git/"))
            r.clear();
        if (!r.empty() && fs::is_regular_file(abs, ec))
        {
            folder = parent_of(r);
            select = last_part(r);
        }
        else
            folder = r;
    }
    b->build();
    b->load(revision.empty() ? std::string("HEAD") : revision, folder, select);
    b->list_->focus();
    return b;
}

repo_browser::repo_browser(rap::ui& ui, std::string workdir)
    : ui_(ui)
    , workdir_(std::move(workdir))
    , rows_(std::make_unique<rows>())
{
}

repo_browser::~repo_browser() = default;

void repo_browser::build()
{
    using namespace rap::layout;
    using namespace rap::literals;
    settings& st = settings::get();
    if (auto v = st.numbers("repoBrowser.sort"); v.size() == 2)
    {
        rows_->sort_column = std::min(size_t(v[0]), size_t(column_count - 1));
        rows_->descending = v[1] != 0;
    }
    std::string root = workdir_;
    while (root.size() > 1 && (root.back() == '/' || root.back() == '\\')) root.pop_back();
    root = rap::sys::path_to_utf8(rap::sys::path_from_utf8(root).make_preferred());

    window_ = create_window(
        ui_,
        window{rap::i18n::format(_("{0} - Repository Browser - Tortilla"), root),
               {vbox{
                   hbox{
                       label{_("Revision:")},
                       entry{}
                           .bind(revision_entry_)
                           .on_activate([this] { load(revision_entry_->text(), folder()); })
                           .width(30_ch),
                       button{"..."}
                           .tooltip(_("Browse References"))
                           .on_click(
                               [this]
                               {
                                   pick_ref(ui_, window_.get(), workdir_, {.initial = revision_},
                                            [this, alive = std::weak_ptr<bool>(alive_)](
                                                const std::string& picked)
                                            {
                                                if (alive.lock() && !picked.empty())
                                                    load(picked, folder());
                                            });
                               }),
                       label{}.bind(commit_).ellipsize().stretch(),
                   },
                   splitter{
                       horizontal,
                       tree{}
                           .bind(tree_view_)
                           .on_selection_changed([this] { show_folder(folder()); })
                           .on_context_menu(
                               [this](std::optional<std::string> id) -> rap::menu
                               {
                                   if (!id) return {};
                                   return folder_menu(id->substr(1));
                               }),
                       list{columns(), rows_.get()}
                           .bind(list_)
                           .on_activate([this](size_t row) { activated(row); })
                           .on_sort_requested(
                               [this](size_t col, rap::sort_order order)
                               {
                                   rows_->sort_column = col;
                                   rows_->descending = order == rap::sort_order::descending;
                                   list_->set_sort_indicator(col, order);
                                   show_folder(folder());
                               })
                           .on_context_menu([this](std::optional<size_t>) { return list_menu(); })
                           .selection(rap::selection_mode::multi)
                           .stretch(3),
                   }
                       .bind(splitter_)
                       .stretch(),
               }}}
            .size(120_ch, 35_lines));
    st.restore_window("repoBrowser.window", *window_);
    if (auto v = st.numbers("repoBrowser.columns"); v.size() == column_count)
        list_->set_column_widths(v);
    if (auto v = st.numbers("repoBrowser.panes"); v.size() == 2)
        splitter_->set_sizes(v);
    else
        splitter_->set_sizes({250, 0});
    list_->set_sort_indicator(rows_->sort_column, rows_->descending ? rap::sort_order::descending
                                                                    : rap::sort_order::ascending);
    window_->on_close_request(
        [this]
        {
            settings& st = settings::get();
            st.save_window("repoBrowser.window", *window_);
            st.set_numbers("repoBrowser.columns", list_->column_widths());
            st.set_numbers("repoBrowser.panes", splitter_->sizes());
            st.set_numbers("repoBrowser.sort",
                           {float(rows_->sort_column), rows_->descending ? 1.f : 0.f});
            if (closed_) closed_();
            return true;
        });
    window_->on_key(
        [this](const rap::key_event& e)
        {
            if (!e.press) return false;
            // F5 reads the revision again: a branch may have moved.
            if (e.key == rap::key::f5)
            {
                const auto sel = selection();
                load(revision_, folder(), sel.empty() ? std::string() : sel.front()->name);
                return true;
            }
            // Backspace in the list goes up a folder, as in a file manager.
            if (e.key == rap::key::backspace && list_->has_focus() && !folder().empty())
            {
                const std::string from = folder();
                enter(parent_of(from));
                show_folder(parent_of(from), last_part(from));
                return true;
            }
            return false;
        });
}

void repo_browser::load(const std::string& revision, std::string folder, const std::string& select)
{
    auto resolved = resolve_tree(*repo_, revision);
    if (!resolved)
    {
        error(_("Repository Browser"),
              rap::i18n::format(_("Couldn't find the revision \"{0}\"."), revision),
              resolved.error());
        revision_entry_->set_text(revision_);
        return;
    }
    revision_ = revision;
    tree_ = *resolved;
    revision_entry_->set_text(revision_);
    commit_->set_text(tree_->commit ? tree_->commit->hex(8) + "  " + tree_->subject
                                    : tree_->tree.hex(8));

    // Only folders, so the whole tree is read at once; their files are read
    // as each is shown.
    folders_ = tree_folders(*repo_, tree_->tree);
    std::map<std::string, std::vector<std::string>> children;
    for (const std::string& f : folders_) children[parent_of(f)].push_back(f);
    for (auto& [_, c] : children)
        std::ranges::sort(c, [](const std::string& a, const std::string& b)
                          { return compare_text(last_part(a), last_part(b)) < 0; });
    std::function<rap::tree_item(const std::string&)> item = [&](const std::string& f)
    {
        rap::tree_item t{.label = last_part(f), .id = tree_id(f)};
        for (const std::string& c : children[f]) t.children.push_back(item(c));
        return t;
    };
    rap::tree_item top = item("");
    // The top is the repository's folder, by its name.
    std::string root = workdir_;
    while (root.size() > 1 && (root.back() == '/' || root.back() == '\\')) root.pop_back();
    top.label = rap::sys::path_to_utf8(rap::sys::path_from_utf8(root).filename());
    if (top.label.empty()) top.label = "/";
    top.expanded = true;
    tree_view_->set_items({std::move(top)});

    // A folder the revision hasn't: its deepest one that it has.
    while (!folder.empty() && !std::ranges::contains(folders_, folder)) folder = parent_of(folder);
    enter(folder);
    show_folder(folder, select);
}

void repo_browser::enter(const std::string& folder)
{
    tree_view_->select(tree_id(folder));
}

void repo_browser::show_folder(const std::string& folder, const std::string& select)
{
    std::vector<std::string> keep;
    if (select.empty())
        for (const tree_entry* e : selection()) keep.push_back(e->name);
    else
        keep.push_back(select);
    std::vector<tree_entry> entries;
    if (tree_)
        if (auto listed = tree_entries(*repo_, tree_->tree, folder)) entries = std::move(*listed);
    rows_->set(std::move(entries));
    std::vector<size_t> rows;
    for (size_t i = 0; i < rows_->items.size(); ++i)
        if (std::ranges::contains(keep, rows_->items[i].name)) rows.push_back(i);
    list_->select(rows);
    if (!rows.empty())
    {
        list_->set_current(rows.front());
        list_->scroll_to(rows.front());
    }
}

std::string repo_browser::folder() const
{
    const auto id = tree_view_ ? tree_view_->selected() : std::nullopt;
    return id && !id->empty() ? id->substr(1) : std::string();
}

std::vector<const tree_entry*> repo_browser::selection() const
{
    std::vector<const tree_entry*> out;
    if (!list_) return out;
    for (size_t row : list_->selection())
        if (row < rows_->items.size()) out.push_back(&rows_->items[row]);
    return out;
}

std::string repo_browser::path_of(const tree_entry& e) const
{
    const std::string f = folder();
    return f.empty() ? e.name : f + "/" + e.name;
}

std::string repo_browser::absolute(const std::string& path) const
{
    return rap::sys::path_to_utf8(rap::sys::path_from_utf8(workdir_) /
                                  rap::sys::path_from_utf8(path));
}

std::string repo_browser::revision_id() const
{
    if (!tree_) return {};
    return tree_->commit ? tree_->commit->hex() : tree_->tree.hex();
}

void repo_browser::error(const std::string& title, const std::string& text,
                         const std::string& detail)
{
    ui_.message(
        window_.get(),
        {.icon = rap::message_spec::icon::error, .title = title, .text = text, .detail = detail});
}

// Double-click: a folder is entered, a file opened.
void repo_browser::activated(size_t row)
{
    if (row >= rows_->items.size()) return;
    const tree_entry e = rows_->items[row];
    if (e.type == tree_entry::kind::folder)
    {
        const std::string f = path_of(e);
        enter(f);
        show_folder(f);
    }
    else if (is_blob(e))
        open_file(path_of(e), false);
}

// -- menus -------------------------------------------------------------------

rap::menu repo_browser::list_menu()
{
    const auto sel = selection();
    if (sel.empty()) return folder_menu(folder());
    if (!tree_) return {};
    // What needs a commit and a working tree: a bare repository has none,
    // and a tree browsed by its own id has no history.
    const bool history = tree_->commit && !bare_;
    rap::menu m;
    if (sel.size() == 1)
    {
        const tree_entry e = *sel.front();
        const std::string path = path_of(e);
        if (is_blob(e))
        {
            m.push_back(item(_("_Open"), [this, path] { open_file(path, false); }));
            m.push_back(item(_("Open with..."), [this, path] { open_file(path, true); }));
            m.push_back(item(_("Save revision _to..."), [this, path] { save_as(path); }));
            separator(m);
            if (history)
            {
                m.push_back(item(_("Show _log"), [this, path] { show_log(path); }));
                if (e.type != tree_entry::kind::symlink)
                    m.push_back(item(_("_Blame"), [this, path] { blame(path); }));
                m.push_back(item(_("Compare with _working tree"), [this, path] { compare(path); }));
            }
        }
        else if (e.type == tree_entry::kind::folder)
        {
            m.push_back(item(_("_Open"),
                             [this, path]
                             {
                                 enter(path);
                                 show_folder(path);
                             }));
            if (history) m.push_back(item(_("Show _log"), [this, path] { show_log(path); }));
        }
        separator(m);
        m.push_back(item(_("Copy _path"), [this, path] { ui_.set_clipboard(path); }));
        m.push_back(item(_("Copy SHA-1"), [this, id = e.id.hex()] { ui_.set_clipboard(id); }));
        return m;
    }
    std::vector<std::string> paths;
    for (const tree_entry* e : sel) paths.push_back(path_of(*e));
    if (std::ranges::all_of(sel, [](const tree_entry* e) { return is_blob(*e); }))
    {
        m.push_back(item(_("Export selection to..."), [this, paths] { save_to_folder(paths); }));
        separator(m);
    }
    std::string lines;
    for (const std::string& p : paths) lines += p + "\n";
    m.push_back(item(_("Copy _path"), [this, lines] { ui_.set_clipboard(lines); }));
    return m;
}

rap::menu repo_browser::folder_menu(const std::string& f)
{
    rap::menu m;
    if (tree_ && tree_->commit && !bare_)
        m.push_back(item(_("Show _log"), [this, f] { show_log(f); }));
    separator(m);
    m.push_back(item(_("Copy _path"), [this, f] { ui_.set_clipboard(f); }));
    return m;
}

// -- commands ------------------------------------------------------------------

// The revision's file written to a temporary folder, as the file lists'
// Open does: through git's filters, so an LFS file is its content.
void repo_browser::open_file(const std::string& path, bool choose)
{
    const std::string rev = revision_id();
    const git_result r = revision_file(workdir_, rev, path);
    std::error_code ec;
    const fs::path file = fs::temp_directory_path(ec) / "tortilla" / rev.substr(0, 8) /
                          rap::sys::path_from_utf8(last_part(path));
    if (!r.ok() || !write_file(file, r.output))
        return error(_("Open"),
                     rap::i18n::format(_("Could not get {0} at {1}."), path, rev.substr(0, 8)),
                     r.ok() ? "" : r.output);
    if (!(choose ? rap::sys::open_with(file) : rap::sys::open_path(file)))
        error(_("Open"), rap::i18n::format(_("Could not open {0}."), rap::sys::path_to_utf8(file)));
}

void repo_browser::save_as(const std::string& path)
{
    const std::string rev = revision_id();
    rap::file_dialog_spec spec;
    spec.mode = rap::file_dialog_spec::mode::save;
    spec.title = _("Save revision to");
    // As the file lists suggest it: the revision before the extension.
    const std::string name = last_part(path);
    const auto dot = name.rfind('.');
    spec.initial_path = dot == std::string::npos || dot == 0
                            ? name + "-" + rev.substr(0, 8)
                            : name.substr(0, dot) + "-" + rev.substr(0, 8) + name.substr(dot);
    ui_.choose_files(
        window_.get(), spec,
        [this, rev, path, alive = std::weak_ptr<bool>(alive_)](std::vector<std::string> picked)
        {
            if (!alive.lock() || picked.empty()) return;
            const git_result r = revision_file(workdir_, rev, path);
            if (!r.ok() || !write_file(rap::sys::path_from_utf8(picked.front()), r.output))
                error(_("Save revision to"),
                      rap::i18n::format(_("Could not save {0}."), picked.front()),
                      r.ok() ? "" : r.output);
        });
}

// Several files into a folder the user picks, by their names.
void repo_browser::save_to_folder(std::vector<std::string> paths)
{
    rap::file_dialog_spec spec;
    spec.mode = rap::file_dialog_spec::mode::folder;
    spec.title = _("Export selection to");
    ui_.choose_files(
        window_.get(), spec,
        [this, rev = revision_id(), paths,
         alive = std::weak_ptr<bool>(alive_)](std::vector<std::string> picked)
        {
            if (!alive.lock() || picked.empty()) return;
            const fs::path to = rap::sys::path_from_utf8(picked.front());
            std::string failed;
            for (const std::string& p : paths)
            {
                const git_result r = revision_file(workdir_, rev, p);
                if (!r.ok() || !write_file(to / rap::sys::path_from_utf8(last_part(p)), r.output))
                    failed += p + "\n";
            }
            if (!failed.empty())
                error(_("Export selection to"), _("Some files could not be exported."), failed);
        });
}

// The path's log, with the browsed commit selected.
void repo_browser::show_log(const std::string& path)
{
    auto log = log_window::open(ui_, absolute(path));
    if (!log) return error("Tortilla", log.error());
    if (tree_ && tree_->commit) (*log)->select_commit(tree_->commit->hex());
    windows::keep(std::move(*log));
}

void repo_browser::blame(const std::string& path)
{
    auto w = blame_window::open(ui_, absolute(path), revision_id());
    if (!w) return error("Tortilla", w.error());
    windows::keep(std::move(*w));
}

void repo_browser::compare(const std::string& path)
{
    difftool_ = compare_revisions(
        ui_, window_.get(), workdir_, {revision_id()}, {path},
        [this] { show_settings(ui_, window_.get(), workdir_, settings_page::diff_viewer); });
}

}  // namespace tortilla
