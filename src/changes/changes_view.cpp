// The changed-files list shared by the log and the working-tree dialogs: its
// context menu, double-click and copying [spec 1.11.2; TortoiseGit's
// GitStatusListCtrl]. The menu keeps TortoiseGit's order; what tortilla
// can't do yet is shown greyed out in its place.
#include "changes_view.h"

#include "settings.h"
#include "ui/compare.h"
#include "ui/diff_window.h"
#include "ui/menu_items.h"
#include "ui/menu_text.h"

#include <rapport/sys.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>

namespace tortilla
{
namespace
{

using rap::menu;
using rap::menu_item;
using arg_list = std::vector<std::string>;

enum file_copy_format
{
    copy_full_paths,
    copy_relative_paths,
    copy_names,
    copy_all,
    copy_column,
};

// The paths, and for renames and copies the old ones too, so git diff can
// pair them up.
arg_list paths_of(const std::vector<file_change>& files, bool with_old = true)
{
    arg_list out;
    for (const file_change& f : files)
    {
        out.push_back(f.path);
        if (with_old && !f.old_path.empty()) out.push_back(f.old_path);
    }
    return out;
}

std::string file_name(const std::string& path)
{
    const auto slash =
        path.find_last_of('/', path.size() > 1 ? path.size() - 2 : std::string::npos);
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

arg_list plus(arg_list a, const arg_list& b)
{
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

bool write_file(const std::filesystem::path& path, const std::string& content)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::FILE* f = rap::sys::fopen(path, "wb");
    bool ok = f && std::fwrite(content.data(), 1, content.size(), f) == content.size();
    if (f) ok = std::fclose(f) == 0 && ok;
    return ok;
}

}  // namespace

changes_view::changes_view(host h)
    : host_(std::move(h))
{
}

rap::layout::list changes_view::list(std::string settings_key, bool checkboxes)
{
    settings_key_ = std::move(settings_key);
    if (checkboxes)
        model_.on_toggle = [this](size_t row)
        {
            // A box in the selection sets the whole selection to its new state.
            std::vector<size_t> rows = selection();
            if (std::ranges::find(rows, row) == rows.end()) rows = {row};
            model_.set_checked(rows, !model_.is_checked(row));
            if (host_.checks_changed) host_.checks_changed();
        };
    return rap::layout::list{file_model::columns(), &model_}
        .checkboxes(checkboxes)
        .compact()
        .soft_selection()
        .bind(list_)
        .on_selection_changed(
            [this]
            {
                if (host_.selection_changed) host_.selection_changed();
            })
        .on_activate([this](size_t row) { activated(row); })
        .on_context_menu([this](std::optional<size_t> row)
                         { return context_menu(row, std::nullopt); })
        .on_cell_context_menu([this](size_t row, size_t column, rap::point_f)
                              { return context_menu(row, column); });
}

// Column widths and order, as the user left them.
void changes_view::save_layout() const
{
    if (!list_ || settings_key_.empty()) return;
    settings& st = settings::get();
    st.set_numbers(settings_key_ + ".columns", list_->column_widths());
    std::vector<float> order;
    for (size_t i : list_->column_order()) order.push_back(float(i));
    st.set_numbers(settings_key_ + ".columns.order", order);
}

void changes_view::restore_layout()
{
    if (!list_ || settings_key_.empty()) return;
    settings& st = settings::get();
    if (auto v = st.numbers(settings_key_ + ".columns"); !v.empty()) list_->set_column_widths(v);
    // The list ignores an order that doesn't fit its columns.
    std::vector<size_t> order;
    for (float f : st.numbers(settings_key_ + ".columns.order")) order.push_back(size_t(f));
    list_->set_column_order(order);
}

void changes_view::set_source(changes_source s)
{
    source_ = std::move(s);
}

void changes_view::set_files(std::vector<file_change> files, bool show_unversioned)
{
    model_.set(std::move(files), show_unversioned);
}

void changes_view::set_counts(size_t first, const std::vector<file_change>& files)
{
    model_.set_counts(first, files);
}

std::vector<size_t> changes_view::selection() const
{
    return list_ ? list_->selection() : std::vector<size_t>{};
}

std::vector<file_change> changes_view::checked_files() const
{
    std::vector<file_change> out;
    for (size_t r = 0; r < model_.files().size(); ++r)
        if (model_.is_checked(r)) out.push_back(model_.files()[r]);
    return out;
}

void changes_view::check_where(const std::function<bool(const file_change&)>& which, bool on)
{
    std::vector<size_t> rows;
    for (size_t r = 0; r < model_.files().size(); ++r)
        if (which(model_.files()[r])) rows.push_back(r);
    model_.set_checked(rows, on);
    if (host_.checks_changed) host_.checks_changed();
}

menu changes_view::context_menu(std::optional<size_t> row, std::optional<size_t> column)
{
    menu m;
    std::vector<file_change> files;
    for (size_t r : selection())
        if (r < model_.files().size()) files.push_back(model_.files()[r]);
    if (files.empty() && row && *row < model_.files().size()) files.push_back(model_.files()[*row]);
    if (files.empty()) return m;

    const bool wc = source_.what == changes_source::working_tree;
    const bool one = files.size() == 1;
    auto any = [&](auto pred) { return std::ranges::any_of(files, pred); };
    auto all = [&](auto pred) { return std::ranges::all_of(files, pred); };
    const bool conflicted = any([](const file_change& f) { return f.status == 'U'; });
    const bool unversioned = all([](const file_change& f) { return f.status == '?'; });
    const bool deleted = any([](const file_change& f) { return f.status == 'D'; });
    const bool renamed = one && !files.front().old_path.empty();
    const arg_list paths = paths_of(files);
    const std::string dir = host_.workdir;
    // Working-tree changes: git runs quietly, and the log reloads after.
    auto local = [this](const std::string& title, std::vector<arg_list> commands)
    { host_.run(title, std::move(commands)); };
    auto confirm_local = [this, local](const std::string& question, const std::string& title,
                                       std::vector<arg_list> commands)
    {
        host_.ui.message(host_.window(),
                         {.icon = rap::message_spec::icon::question,
                          .title = title,
                          .text = question,
                          .buttons = {rap::dialog_role::yes, rap::dialog_role::no}},
                         [local, title, commands](rap::dialog_role r)
                         {
                             if (r == rap::dialog_role::yes) local(title, commands);
                         });
    };
    const std::string count =
        one ? "“" + files.front().path + "”" : std::to_string(files.size()) + " files";

    if (wc)
    {
        if (conflicted && one)
        {
            const std::string& path = files.front().path;
            m.push_back(todo("_Edit conflicts"));
            m.push_back(
                item("Resolved", [local, path] { local("Resolved", {{"add", "--", path}}); }));
            m.push_back(item("Resolve conflict using “theirs”",
                             [local, path]
                             {
                                 local("Resolve",
                                       {{"checkout", "--theirs", "--", path}, {"add", "--", path}});
                             }));
            m.push_back(item(
                "Resolve conflict using “mine”", [local, path]
                { local("Resolve", {{"checkout", "--ours", "--", path}, {"add", "--", path}}); }));
            separator(m);
        }
        if (unversioned)
        {
            m.push_back(
                item("Add", [local, paths] { local("Add", {plus({"add", "--"}, paths)}); }));
            m.push_back(todo("Commit..."));
        }
        else if (!conflicted)
        {
            m.push_back(item("Compare with b_ase", [this, paths] { difftool({"HEAD"}, paths); }));
            m.push_back(item("Show changes as _unified diff",
                             [this, paths, dir, count]
                             {
                                 show_unified_diff(host_.ui, count + " HEAD..working tree", dir,
                                                   plus({"diff", "HEAD", "--"}, paths));
                             }));
            separator(m);
            m.push_back(todo("Commit..."));
            // New files leave the index (the file stays); the rest go
            // back to HEAD, in the index and the working tree.
            m.push_back(item(
                "Re_vert...",
                [confirm_local, files, count]
                {
                    arg_list added, changed;
                    for (const file_change& f : files)
                    {
                        (f.status == 'A' ? added : changed).push_back(f.path);
                        if (!f.old_path.empty()) changed.push_back(f.old_path);
                    }
                    std::vector<arg_list> commands;
                    if (!added.empty())
                        commands.push_back(plus({"restore", "--staged", "--"}, added));
                    if (!changed.empty())
                        commands.push_back(plus(
                            {"restore", "--source=HEAD", "--staged", "--worktree", "--"}, changed));
                    confirm_local("Revert " + count + "? Their local changes will be lost.",
                                  "Revert", commands);
                }));
            m.push_back(item("Skip worktree",
                             [local, paths]
                             {
                                 local("Skip worktree",
                                       {plus({"update-index", "--skip-worktree", "--"}, paths)});
                             }));
            m.push_back(item("Assume Unchanged",
                             [local, paths]
                             {
                                 local("Assume Unchanged",
                                       {plus({"update-index", "--assume-unchanged", "--"}, paths)});
                             }));
            m.push_back(item(
                "Unflag as skip-worktree and assume-unchanged",
                [local, paths]
                {
                    local("Unflag", {plus({"update-index", "--no-skip-worktree", "--"}, paths),
                                     plus({"update-index", "--no-assume-unchanged", "--"}, paths)});
                }));
        }
    }
    else
    {
        const std::string& hash = source_.hash;
        const std::string& parent = source_.parent;
        const std::string& parent_title = source_.parent_title;

        m.push_back(item("Compare with b_ase",
                         [this, parent, hash, paths] { difftool({parent, hash}, paths); }));
        m.push_back(item("Show changes as _unified diff",
                         [this, parent, hash, paths, dir, count]
                         {
                             show_unified_diff(host_.ui, count + " " + hash.substr(0, 8), dir,
                                               plus({"diff", parent, hash, "--"}, paths));
                         }));
        m.push_back(
            item("Compare with _working tree", [this, hash, paths] { difftool({hash}, paths); }));
        if (!parent_title.empty())
            m.push_back(item("Compare parent with working tree: " + menu_text(parent_title),
                             [this, parent, paths] { difftool({parent}, paths); }));
        separator(m);
        if (files.size() == 2) m.push_back(todo("Compare two files"));
        // The working tree's files replaced with this commit's (or its
        // parent's) versions, as TortoiseGit's Revert to revision.
        if (!deleted)
            m.push_back(item(
                "R_evert to this revision",
                [confirm_local, hash, files, count]
                {
                    confirm_local("Replace " + count + " in the working tree with the version in " +
                                      hash.substr(0, 8) + "?",
                                  "Revert to this revision",
                                  {plus({"checkout", hash, "--"}, paths_of(files, false))});
                }));
        if (!parent_title.empty())
        {
            // A file the commit added has no parent version, and a renamed
            // one had another name there.
            const bool in_parent = std::ranges::none_of(
                files, [](const file_change& f) { return f.status == 'A' || !f.old_path.empty(); });
            m.push_back(item(
                "Revert to parent revision: " + menu_text(parent_title),
                [confirm_local, parent, files, count]
                {
                    confirm_local("Replace " + count + " in the working tree with the version in " +
                                      parent.substr(0, 8) + "?",
                                  "Revert to parent revision",
                                  {plus({"checkout", parent, "--"}, paths_of(files, false))});
                },
                in_parent));
        }
    }

    separator(m);
    if (one)
    {
        m.push_back(todo("Show _log"));
        if (renamed) m.push_back(todo("Show log _before rename/copy"));
        m.push_back(todo("_Blame"));
    }
    if (!wc && !deleted)
    {
        m.push_back(item("Export selection to...",
                         [this, hash = source_.hash, files] { save_revisions(hash, files); }));
    }
    if (one)
    {
        if (!wc && !deleted)
        {
            m.push_back(item("Save revision _to...", [this, hash = source_.hash, f = files.front()]
                             { save_revision(hash, f); }));
        }
        m.push_back(todo("View revision in alternative editor"));
        const file_change& f = files.front();
        // A deleted file has nothing to open in the working tree, nor in
        // the revision that deleted it.
        const bool openable = f.status != 'D';
        m.push_back(item("_Open", [this, f] { open_file(f, false); }, openable));
#ifdef _WIN32
        m.push_back(item("Open with...", [this, f] { open_file(f, true); }, openable));
#else
        m.push_back(todo("Open with..."));  // no common chooser on Linux desktops
#endif
        m.push_back(item("Explore to",
                         [this, f]
                         {
                             const auto path = rap::sys::path_from_utf8(host_.workdir) /
                                               rap::sys::path_from_utf8(f.path);
                             std::error_code ec;
                             rap::sys::reveal(
                                 std::filesystem::exists(path, ec) ? path : path.parent_path());
                         }));
        separator(m);
        m.push_back(todo("Mark for comparison"));
    }
    if (wc && unversioned)
    {
        m.push_back(item(
            "_Delete",
            [confirm_local, paths, count]
            {
                confirm_local("Delete " + count + "? They aren't in git, so this can't be undone.",
                              "Delete", {plus({"clean", "-f", "-d", "-q", "--"}, paths)});
            }));
        // Patterns appended to the repository's top-level .gitignore.
        auto ignore = [this](std::vector<std::string> patterns)
        {
            const auto path = rap::sys::path_from_utf8(host_.workdir) / ".gitignore";
            std::string add;
            for (const std::string& p : patterns) add += p + "\n";
            std::FILE* f = rap::sys::fopen(path, "ab");
            bool ok = f && std::fwrite(add.data(), 1, add.size(), f) == add.size();
            if (f) ok = std::fclose(f) == 0 && ok;
            if (!ok)
                host_.ui.message(host_.window(),
                                 {.icon = rap::message_spec::icon::error,
                                  .title = "Add to ignore list",
                                  .text = "Could not write " + rap::sys::path_to_utf8(path) + "."});
            host_.refresh();
        };
        std::vector<std::string> by_name, by_extension;
        for (const file_change& f : files)
        {
            by_name.push_back("/" + f.path);
            const std::string name = file_name(f.path);
            const auto dot = name.rfind('.');
            if (dot != std::string::npos && dot > 0 && name.back() != '/')
                if (std::string ext = "*" + name.substr(dot);
                    std::ranges::find(by_extension, ext) == by_extension.end())
                    by_extension.push_back(ext);
        }
        menu sub;
        sub.push_back(item(one ? menu_text(by_name.front()) : "Ignore item(s) by name",
                           [ignore, by_name] { ignore(by_name); }));
        if (!by_extension.empty())
        {
            std::string label;
            for (const std::string& e : by_extension) label += (label.empty() ? "" : ", ") + e;
            sub.push_back(item(menu_text(label), [ignore, by_extension] { ignore(by_extension); }));
        }
        m.push_back({.label = "Add to _ignore list", .children = std::move(sub)});
    }

    separator(m);
    menu copy{
        item("full paths", [this] { copy_files(copy_full_paths, std::nullopt); }),
        item("relative paths", [this] { copy_files(copy_relative_paths, std::nullopt); }),
        item("file/foldernames", [this] { copy_files(copy_names, std::nullopt); }),
        item("Copy all information to clipboard", [this] { copy_files(copy_all, std::nullopt); }),
    };
    if (column)
    {
        const auto columns = file_model::columns();
        if (*column < columns.size())
            copy.push_back(item("column ‘" + menu_text(columns[*column].title) + "’",
                                [this, column] { copy_files(copy_column, column); }));
    }
    m.push_back({.label = "Copy to clipboard", .children = std::move(copy)});
    return m;
}

// Double-click: compare the file with the commit's parent (or, for the
// working tree, with HEAD).
void changes_view::activated(size_t row)
{
    if (row >= model_.files().size()) return;
    const file_change& f = model_.files()[row];
    // Nothing to compare an unversioned file with: it opens instead.
    if (f.status == '?' || f.status == '!') return open_file(f, false);
    if (f.status == 'U') return;
    arg_list paths = paths_of({f});
    if (source_.what == changes_source::working_tree) return difftool({"HEAD"}, paths);
    difftool({source_.parent, source_.hash}, paths);
}

// Open: the working file, or a revision's version of it written to a
// temporary file named after it [TortoiseGit: Open / Open with].
void changes_view::open_file(const file_change& f, bool choose)
{
    std::filesystem::path path =
        rap::sys::path_from_utf8(host_.workdir) / rap::sys::path_from_utf8(f.path);
    if (source_.what == changes_source::revision)
    {
        git_result r =
            run_git(host_.workdir, {"show", source_.hash + ":" + f.path}, git_output::stdout_only);
        std::error_code ec;
        const std::filesystem::path dir =
            std::filesystem::temp_directory_path(ec) / "tortilla" / source_.hash.substr(0, 8);
        path = dir / rap::sys::path_from_utf8(file_name(f.path));
        if (!r.ok() || !write_file(path, r.output))
        {
            host_.ui.message(host_.window(), {.icon = rap::message_spec::icon::error,
                                              .title = "Open",
                                              .text = "Could not get " + f.path + " at " +
                                                      source_.hash.substr(0, 8) + ".",
                                              .detail = r.ok() ? "" : r.output});
            return;
        }
    }
    if (!(choose ? rap::sys::open_with(path) : rap::sys::open_path(path)))
        host_.ui.message(host_.window(),
                         {.icon = rap::message_spec::icon::error,
                          .title = "Open",
                          .text = "Could not open " + rap::sys::path_to_utf8(path) + "."});
}

// With the tool the Diff Viewer page chose (compare.h).
void changes_view::difftool(std::vector<std::string> revisions, std::vector<std::string> paths)
{
    difftool_ = compare_revisions(host_.ui, host_.window(), host_.workdir, std::move(revisions),
                                  std::move(paths),
                                  [this]
                                  {
                                      if (host_.open_settings)
                                          host_.open_settings(settings_page::diff_viewer);
                                  });
}

void changes_view::copy_files(int format, std::optional<size_t> column)
{
    const auto& all = model_.files();
    const std::filesystem::path root = rap::sys::path_from_utf8(host_.workdir);
    std::string out;
    for (size_t r : list_->selection())
    {
        if (r >= all.size()) continue;
        const file_change& f = all[r];
        std::string line;
        switch (format)
        {
        case copy_full_paths:
            line =
                rap::sys::path_to_utf8((root / rap::sys::path_from_utf8(f.path)).make_preferred());
            break;
        case copy_relative_paths: line = f.path; break;
        case copy_names: line = file_name(f.path); break;
        case copy_all:
            for (size_t c = 0; c < file_model::columns().size(); ++c)
                line += (c ? "\t" : "") + model_.text(r, c);
            break;
        case copy_column: line = column ? model_.text(r, *column) : ""; break;
        }
        out += line + "\n";
    }
    if (!out.empty()) host_.ui.set_clipboard(out);
}

// Export selection to: the files as they are in `rev`, written under a
// folder the user picks, keeping their paths.
void changes_view::save_revisions(const std::string& rev, std::vector<file_change> files)
{
    rap::file_dialog_spec spec;
    spec.mode = rap::file_dialog_spec::mode::folder;
    spec.title = "Export selection to";
    host_.ui.choose_files(
        host_.window(), spec,
        [this, rev, files](std::vector<std::string> picked)
        {
            if (picked.empty()) return;
            const std::filesystem::path folder = rap::sys::path_from_utf8(picked.front());
            std::string failed;
            for (const file_change& f : files)
            {
                git_result r =
                    run_git(host_.workdir, {"show", rev + ":" + f.path}, git_output::stdout_only);
                if (!r.ok() || !write_file(folder / rap::sys::path_from_utf8(f.path), r.output))
                    failed += f.path + "\n";
            }
            if (!failed.empty())
                host_.ui.message(host_.window(), {.icon = rap::message_spec::icon::error,
                                                  .title = "Export selection to",
                                                  .text = "Some files could not be exported.",
                                                  .detail = failed});
        });
}

// Save revision to: one file as it is in `rev`, under a name the user picks.
void changes_view::save_revision(const std::string& rev, const file_change& file)
{
    rap::file_dialog_spec spec;
    spec.mode = rap::file_dialog_spec::mode::save;
    spec.title = "Save revision to";
    // TortoiseGit's suggestion: the name with the revision before the extension.
    std::string name = file_name(file.path);
    const auto dot = name.rfind('.');
    spec.initial_path = dot == std::string::npos || dot == 0
                            ? name + "-" + rev.substr(0, 8)
                            : name.substr(0, dot) + "-" + rev.substr(0, 8) + name.substr(dot);
    host_.ui.choose_files(
        host_.window(), spec,
        [this, rev, path = file.path](std::vector<std::string> picked)
        {
            if (picked.empty()) return;
            git_result r =
                run_git(host_.workdir, {"show", rev + ":" + path}, git_output::stdout_only);
            if (!r.ok() || !write_file(rap::sys::path_from_utf8(picked.front()), r.output))
                host_.ui.message(host_.window(), {.icon = rap::message_spec::icon::error,
                                                  .title = "Save revision to",
                                                  .text = "Could not save " + picked.front() + ".",
                                                  .detail = r.ok() ? "" : r.output});
        });
}

}  // namespace tortilla
