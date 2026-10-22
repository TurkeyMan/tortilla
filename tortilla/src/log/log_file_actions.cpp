// The Log dialog's changed-files list (bottom pane): its context menu,
// double-click and copying [spec 1.11.2; TortoiseGit's GitStatusListCtrl].
// The menu keeps TortoiseGit's order; what tortilla can't do yet is shown
// greyed out in its place.
#include "log_window.h"

#include "git/command.h"
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

using rap::Menu;
using rap::MenuItem;
using Args = std::vector<std::string>;

// git's empty tree: what a root commit is compared with.
constexpr const char* kEmptyTree = "4b825dc642cb6eb9a060e54bf8d69288fbee4904";

enum FileCopyFormat
{
    CopyFullPaths,
    CopyRelativePaths,
    CopyNames,
    CopyAll,
    CopyColumn,
};

// The paths, and for renames and copies the old ones too, so git diff can
// pair them up.
Args pathsOf(const std::vector<FileChange>& files, bool withOld = true)
{
    Args out;
    for (const FileChange& f : files)
    {
        out.push_back(f.path);
        if (withOld && !f.oldPath.empty()) out.push_back(f.oldPath);
    }
    return out;
}

std::string fileName(const std::string& path)
{
    const auto slash = path.find_last_of('/', path.size() > 1 ? path.size() - 2 : std::string::npos);
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

Args plus(Args a, const Args& b)
{
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

bool writeFile(const std::filesystem::path& path, const std::string& content)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::FILE* f = rap::sys::fopen(path, "wb");
    bool ok = f && std::fwrite(content.data(), 1, content.size(), f) == content.size();
    if (f) ok = std::fclose(f) == 0 && ok;
    return ok;
}

}  // namespace

Menu LogWindow::fileMenu(std::optional<size_t> row, std::optional<size_t> column)
{
    Menu m;
    const auto commits = commitList_->selection();
    if (commits.size() != 1) return m;
    std::vector<FileChange> files;
    for (size_t r : fileList_->selection())
        if (r < files_.files().size()) files.push_back(files_.files()[r]);
    if (files.empty() && row && *row < files_.files().size()) files.push_back(files_.files()[*row]);
    if (files.empty()) return m;

    const bool wc = log_.isWorkingTree(commits.front());
    const bool one = files.size() == 1;
    auto any = [&](auto pred) { return std::ranges::any_of(files, pred); };
    auto all = [&](auto pred) { return std::ranges::all_of(files, pred); };
    const bool conflicted = any([](const FileChange& f) { return f.status == 'U'; });
    const bool unversioned = all([](const FileChange& f) { return f.status == '?'; });
    const bool deleted = any([](const FileChange& f) { return f.status == 'D'; });
    const bool renamed = one && !files.front().oldPath.empty();
    const Args paths = pathsOf(files);
    const std::string dir = repo_.workdir();
    // Working-tree changes: git runs quietly, and the log reloads after.
    auto local = [this](const std::string& title, std::vector<Args> commands)
    { runQuietly(title, std::move(commands), [this](bool, const std::string&) { reload(); }); };
    auto confirmLocal = [this, local](const std::string& question, const std::string& title,
                                      std::vector<Args> commands)
    {
        ui_.message(window_.get(),
                    {.icon = rap::MessageSpec::Icon::Question,
                     .title = title,
                     .text = question,
                     .buttons = {rap::DialogRole::Yes, rap::DialogRole::No}},
                    [local, title, commands](rap::DialogRole r)
                    {
                        if (r == rap::DialogRole::Yes) local(title, commands);
                    });
    };
    const std::string count = one ? "“" + files.front().path + "”"
                                  : std::to_string(files.size()) + " files";

    if (wc)
    {
        if (conflicted && one)
        {
            const std::string& path = files.front().path;
            m.push_back(todo("_Edit conflicts"));
            m.push_back(item("Resolved", [local, path] { local("Resolved", {{"add", "--", path}}); }));
            m.push_back(item("Resolve conflict using “theirs”",
                             [local, path]
                             { local("Resolve", {{"checkout", "--theirs", "--", path}, {"add", "--", path}}); }));
            m.push_back(item("Resolve conflict using “mine”",
                             [local, path]
                             { local("Resolve", {{"checkout", "--ours", "--", path}, {"add", "--", path}}); }));
            separator(m);
        }
        if (unversioned)
        {
            m.push_back(item("Add", [local, paths] { local("Add", {plus({"add", "--"}, paths)}); }));
            m.push_back(todo("Commit..."));
        }
        else if (!conflicted)
        {
            m.push_back(item("Compare with b_ase", [this, paths] { difftool({"HEAD"}, paths); }));
            m.push_back(item("Show changes as _unified diff",
                             [this, paths, dir, count]
                             { showUnifiedDiff(ui_, count + " HEAD..working tree", dir, plus({"diff", "HEAD", "--"}, paths)); }));
            separator(m);
            m.push_back(todo("Commit..."));
            // New files leave the index (the file stays); the rest go
            // back to HEAD, in the index and the working tree.
            m.push_back(item("Re_vert...",
                             [confirmLocal, files, count]
                             {
                                 Args added, changed;
                                 for (const FileChange& f : files)
                                 {
                                     (f.status == 'A' ? added : changed).push_back(f.path);
                                     if (!f.oldPath.empty()) changed.push_back(f.oldPath);
                                 }
                                 std::vector<Args> commands;
                                 if (!added.empty()) commands.push_back(plus({"restore", "--staged", "--"}, added));
                                 if (!changed.empty())
                                     commands.push_back(plus({"restore", "--source=HEAD", "--staged", "--worktree", "--"}, changed));
                                 confirmLocal("Revert " + count + "? Their local changes will be lost.", "Revert", commands);
                             }));
            m.push_back(item("Skip worktree", [local, paths]
                             { local("Skip worktree", {plus({"update-index", "--skip-worktree", "--"}, paths)}); }));
            m.push_back(item("Assume Unchanged", [local, paths]
                             { local("Assume Unchanged", {plus({"update-index", "--assume-unchanged", "--"}, paths)}); }));
            m.push_back(item("Unflag as skip-worktree and assume-unchanged",
                             [local, paths]
                             {
                                 local("Unflag", {plus({"update-index", "--no-skip-worktree", "--"}, paths),
                                                  plus({"update-index", "--no-assume-unchanged", "--"}, paths)});
                             }));
        }
    }
    else
    {
        const Commit& c = *log_.commit(commits.front());
        const std::string hash = c.id.hex();
        const std::string parent = c.parents.empty() ? kEmptyTree : c.parents.front().hex();
        std::string parentTitle = parent.substr(0, 8);
        if (!c.parents.empty())
            if (auto r = log_.rowOf(c.parents.front()))
                if (const Commit* p = log_.commit(*r)) parentTitle += " " + p->summary;

        m.push_back(item("Compare with b_ase", [this, parent, hash, paths] { difftool({parent, hash}, paths); }));
        m.push_back(item("Show changes as _unified diff",
                         [this, parent, hash, paths, dir, count]
                         { showUnifiedDiff(ui_, count + " " + hash.substr(0, 8), dir, plus({"diff", parent, hash, "--"}, paths)); }));
        m.push_back(item("Compare with _working tree", [this, hash, paths] { difftool({hash}, paths); }));
        if (!c.parents.empty())
            m.push_back(item("Compare parent with working tree: " + menuText(parentTitle),
                             [this, parent, paths] { difftool({parent}, paths); }));
        separator(m);
        if (files.size() == 2) m.push_back(todo("Compare two files"));
        // The working tree's files replaced with this commit's (or its
        // parent's) versions, as TortoiseGit's Revert to revision.
        if (!deleted)
            m.push_back(item("R_evert to this revision",
                             [confirmLocal, hash, files, count]
                             {
                                 confirmLocal("Replace " + count + " in the working tree with the version in " +
                                                  hash.substr(0, 8) + "?",
                                              "Revert to this revision", {plus({"checkout", hash, "--"}, pathsOf(files, false))});
                             }));
        if (!c.parents.empty())
        {
            // A file the commit added has no parent version, and a renamed
            // one had another name there.
            const bool inParent = std::ranges::none_of(
                files, [](const FileChange& f) { return f.status == 'A' || !f.oldPath.empty(); });
            m.push_back(item("Revert to parent revision: " + menuText(parentTitle),
                             [confirmLocal, parent, files, count]
                             {
                                 confirmLocal("Replace " + count + " in the working tree with the version in " +
                                                  parent.substr(0, 8) + "?",
                                              "Revert to parent revision", {plus({"checkout", parent, "--"}, pathsOf(files, false))});
                             },
                             inParent));
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
        const std::string hash = log_.commit(commits.front())->id.hex();
        m.push_back(item("Export selection to...", [this, hash, files] { saveRevisions(hash, files); }));
    }
    if (one)
    {
        if (!wc && !deleted)
        {
            const std::string hash = log_.commit(commits.front())->id.hex();
            m.push_back(item("Save revision _to...", [this, hash, f = files.front()] { saveRevision(hash, f); }));
        }
        m.push_back(todo("View revision in alternative editor"));
        m.push_back(todo("_Open"));
        m.push_back(todo("Open with..."));
        m.push_back(todo("Explore to"));
        separator(m);
        m.push_back(todo("Mark for comparison"));
    }
    if (wc && unversioned)
    {
        m.push_back(item("_Delete",
                         [confirmLocal, paths, count]
                         { confirmLocal("Delete " + count + "? They aren't in git, so this can't be undone.", "Delete", {plus({"clean", "-f", "-d", "-q", "--"}, paths)}); }));
        // Patterns appended to the repository's top-level .gitignore.
        auto ignore = [this](std::vector<std::string> patterns)
        {
            const auto path = rap::sys::pathFromUtf8(repo_.workdir()) / ".gitignore";
            std::string add;
            for (const std::string& p : patterns) add += p + "\n";
            std::FILE* f = rap::sys::fopen(path, "ab");
            bool ok = f && std::fwrite(add.data(), 1, add.size(), f) == add.size();
            if (f) ok = std::fclose(f) == 0 && ok;
            if (!ok)
                ui_.message(window_.get(), {.icon = rap::MessageSpec::Icon::Error,
                                            .title = "Add to ignore list",
                                            .text = "Could not write " + rap::sys::pathToUtf8(path) + "."});
            reload();
        };
        std::vector<std::string> byName, byExtension;
        for (const FileChange& f : files)
        {
            byName.push_back("/" + f.path);
            const std::string name = fileName(f.path);
            const auto dot = name.rfind('.');
            if (dot != std::string::npos && dot > 0 && name.back() != '/')
                if (std::string ext = "*" + name.substr(dot); std::ranges::find(byExtension, ext) == byExtension.end())
                    byExtension.push_back(ext);
        }
        Menu sub;
        sub.push_back(item(one ? menuText(byName.front()) : "Ignore item(s) by name", [ignore, byName] { ignore(byName); }));
        if (!byExtension.empty())
        {
            std::string label;
            for (const std::string& e : byExtension) label += (label.empty() ? "" : ", ") + e;
            sub.push_back(item(menuText(label), [ignore, byExtension] { ignore(byExtension); }));
        }
        m.push_back({.label = "Add to _ignore list", .children = std::move(sub)});
    }

    separator(m);
    Menu copy{
        item("full paths", [this] { copyFiles(CopyFullPaths, std::nullopt); }),
        item("relative paths", [this] { copyFiles(CopyRelativePaths, std::nullopt); }),
        item("file/foldernames", [this] { copyFiles(CopyNames, std::nullopt); }),
        item("Copy all information to clipboard", [this] { copyFiles(CopyAll, std::nullopt); }),
    };
    if (column)
    {
        const auto columns = FileModel::columns();
        if (*column < columns.size())
            copy.push_back(item("column ‘" + menuText(columns[*column].title) + "’",
                                [this, column] { copyFiles(CopyColumn, column); }));
    }
    m.push_back({.label = "Copy to clipboard", .children = std::move(copy)});
    return m;
}

// Double-click: compare the file with the commit's parent (or, for the
// working tree, with HEAD).
void LogWindow::fileActivated(size_t row)
{
    const auto commits = commitList_->selection();
    if (commits.size() != 1 || row >= files_.files().size()) return;
    const FileChange& f = files_.files()[row];
    if (f.status == '?' || f.status == 'U') return;
    Args paths = pathsOf({f});
    if (log_.isWorkingTree(commits.front())) return difftool({"HEAD"}, paths);
    const Commit& c = *log_.commit(commits.front());
    difftool({c.parents.empty() ? kEmptyTree : c.parents.front().hex(), c.id.hex()}, paths);
}

void LogWindow::copyFiles(int format, std::optional<size_t> column)
{
    const auto& all = files_.files();
    const std::filesystem::path root = rap::sys::pathFromUtf8(repo_.workdir());
    std::string out;
    for (size_t r : fileList_->selection())
    {
        if (r >= all.size()) continue;
        const FileChange& f = all[r];
        std::string line;
        switch (format)
        {
        case CopyFullPaths: line = rap::sys::pathToUtf8((root / rap::sys::pathFromUtf8(f.path)).make_preferred()); break;
        case CopyRelativePaths: line = f.path; break;
        case CopyNames: line = fileName(f.path); break;
        case CopyAll:
            for (size_t c = 0; c < FileModel::columns().size(); ++c)
                line += (c ? "\t" : "") + files_.text(r, c);
            break;
        case CopyColumn: line = column ? files_.text(r, *column) : ""; break;
        }
        out += line + "\n";
    }
    if (!out.empty()) ui_.setClipboard(out);
}

// Export selection to: the files as they are in `rev`, written under a
// folder the user picks, keeping their paths.
void LogWindow::saveRevisions(const std::string& rev, std::vector<FileChange> files)
{
    rap::FileDialogSpec spec;
    spec.mode = rap::FileDialogSpec::Mode::Folder;
    spec.title = "Export selection to";
    ui_.chooseFiles(window_.get(), spec,
                    [this, rev, files](std::vector<std::string> picked)
                    {
                        if (picked.empty()) return;
                        const std::filesystem::path folder = rap::sys::pathFromUtf8(picked.front());
                        std::string failed;
                        for (const FileChange& f : files)
                        {
                            GitResult r = runGit(repo_.workdir(), {"show", rev + ":" + f.path}, GitOutput::Stdout);
                            if (!r.ok() || !writeFile(folder / rap::sys::pathFromUtf8(f.path), r.output))
                                failed += f.path + "\n";
                        }
                        if (!failed.empty())
                            ui_.message(window_.get(), {.icon = rap::MessageSpec::Icon::Error,
                                                        .title = "Export selection to",
                                                        .text = "Some files could not be exported.",
                                                        .detail = failed});
                    });
}

// Save revision to: one file as it is in `rev`, under a name the user picks.
void LogWindow::saveRevision(const std::string& rev, const FileChange& file)
{
    rap::FileDialogSpec spec;
    spec.mode = rap::FileDialogSpec::Mode::Save;
    spec.title = "Save revision to";
    // TortoiseGit's suggestion: the name with the revision before the extension.
    std::string name = fileName(file.path);
    const auto dot = name.rfind('.');
    spec.initialPath = dot == std::string::npos || dot == 0
                           ? name + "-" + rev.substr(0, 8)
                           : name.substr(0, dot) + "-" + rev.substr(0, 8) + name.substr(dot);
    ui_.chooseFiles(window_.get(), spec,
                    [this, rev, path = file.path](std::vector<std::string> picked)
                    {
                        if (picked.empty()) return;
                        GitResult r = runGit(repo_.workdir(), {"show", rev + ":" + path}, GitOutput::Stdout);
                        if (!r.ok() || !writeFile(rap::sys::pathFromUtf8(picked.front()), r.output))
                            ui_.message(window_.get(), {.icon = rap::MessageSpec::Icon::Error,
                                                        .title = "Save revision to",
                                                        .text = "Could not save " + picked.front() + ".",
                                                        .detail = r.ok() ? "" : r.output});
                    });
}

}  // namespace tortilla
