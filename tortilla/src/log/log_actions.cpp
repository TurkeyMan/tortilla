// The Log dialog's commit-list context menu and the actions behind it
// [spec 1.11]. Actions shell out to git and show their output in the
// progress dialog, as TortoiseGit does, then reload the log.
#include "log_window.h"

#include "git/command.h"
#include "git/diff_tool.h"
#include "ui/diff_window.h"
#include "ui/menu_items.h"
#include "ui/menu_text.h"
#include "ui/progress_dialog.h"
#include "util/calendar.h"

#include <ctime>

namespace tortilla
{
namespace
{

using rap::Menu;
using rap::MenuItem;
using Args = std::vector<std::string>;

enum CopyFormat
{
    CopyFull,
    CopyFullNoPaths,
    CopyHash,
    CopyAuthors,
    CopyAuthorNames,
    CopyAuthorEmails,
    CopySubjects,
    CopyMessages,
    CopyRefNames,
};

std::string shortSubject(const Commit& c)
{
    return c.summary.size() > 20 ? c.summary.substr(0, 20) + "..." : c.summary;
}

std::string formatTime(int64_t time, int offsetMinutes)
{
    std::time_t t = time + int64_t(offsetMinutes) * 60;
    std::tm tm = utcCalendar(t);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tm);
    return buf;
}

// A modal dialog that owns its widgets' state until it closes.
template <class State>
void runDialog(rap::Ui& ui, rap::Window* parent, std::shared_ptr<State> st,
               rap::WindowSpec spec, std::function<void(State&)> onOk)
{
    st->window = ui.create(spec, parent);
    st->window->onFinished(
        [st, onOk, &ui](rap::DialogRole role) mutable
        {
            auto keep = std::move(st);
            if (role == rap::DialogRole::Ok) onOk(*keep);
            ui.post([keep] {});  // destroy after this callback returns
        });
    st->window->show();
}

}  // namespace

void LogWindow::runGitCommands(const std::string& title, std::vector<Args> commands)
{
    runProgressDialog(ui_, window_.get(), title, repo_.workdir(), std::move(commands),
                      [this](bool) { reload(); });
}

void LogWindow::runRefCommands(const std::string& title, std::vector<Args> commands)
{
    runQuietly(title, std::move(commands), [this](bool, const std::string&) { refsChanged(); });
}

void LogWindow::runQuietly(const std::string& title, std::vector<Args> commands,
                           std::function<void(bool ok, const std::string& output)> after,
                           bool showFailure)
{
    // One after another; the first failure stops the rest.
    struct Run
    {
        std::vector<Args> commands;
        size_t next = 0;
        std::string output;
    };
    auto run = std::make_shared<Run>(Run{std::move(commands)});
    auto step = std::make_shared<std::function<void()>>();
    *step = [this, title, run, after, showFailure,
             weakStep = std::weak_ptr<std::function<void()>>(step)]
    {
        if (run->next == run->commands.size())
        {
            refCommand_.reset();
            if (after) after(true, run->output);
            return;
        }
        const Args& args = run->commands[run->next++];
        run->output += commandLine(args) + "\n";
        auto self = weakStep.lock();
        refCommand_ = GitProcess::start(
            ui_, repo_.workdir(), args, [run](const std::string& chunk) { run->output += chunk; },
            [this, title, run, self, after, showFailure](int code)
            {
                if (code == 0) return (*self)();
                refCommand_.reset();
                if (showFailure)
                    ui_.message(window_.get(), {.icon = rap::MessageSpec::Icon::Error,
                                                .title = title,
                                                .text = title + " failed.",
                                                .detail = run->output});
                if (after) after(false, run->output);  // an earlier command may have succeeded
            });
    };
    (*step)();
}

void LogWindow::refsChanged()
{
    log_.refreshRefs(
        [this](bool reloadNeeded)
        {
            if (reloadNeeded) return reload();
            updateRefButton();
            if (auto sel = commitList_->selection(); sel.size() == 1) showCommit(sel.front());
        });
}

void LogWindow::confirmThenRun(const std::string& question, const std::string& title,
                               std::vector<Args> commands)
{
    ui_.message(window_.get(),
                {.icon = rap::MessageSpec::Icon::Question,
                 .title = title,
                 .text = question,
                 .buttons = {rap::DialogRole::Yes, rap::DialogRole::No}},
                [this, title, commands](rap::DialogRole r)
                {
                    if (r != rap::DialogRole::Yes) return;
                    // Deleting local branches and tags only touches refs.
                    const bool refsOnly = std::ranges::all_of(
                        commands,
                        [](const Args& a)
                        {
                            return !a.empty() &&
                                   (a[0] == "branch" || a[0] == "tag" || a[0] == "update-ref");
                        });
                    if (refsOnly)
                        runRefCommands(title, commands);
                    else
                        runGitCommands(title, commands);
                });
}

// Comparing revisions, with the tool the Settings dialog's Diff Viewer page
// chose: tortilla's unified diff window, git difftool with diff.tool, or a
// command of the user's. `revisions` are as for git diff: one means against
// the working tree.
void LogWindow::difftool(std::vector<std::string> revisions, std::vector<std::string> paths)
{
    auto name = [](const std::string& rev)
    {
        // Full hashes as TortoiseGit shows them: 8 characters.
        const bool hex = rev.size() >= 40 && rev.find_first_not_of("0123456789abcdef") >= 40;
        return hex ? rev.substr(0, 8) + rev.substr(40) : rev;
    };
    // One file: its name too, as TortoiseGit titles a file comparison.
    const std::string file = paths.size() == 1 ? paths.front() + " : " : "";
    const std::string base = file + name(revisions.front());
    const std::string mine = file + (revisions.size() > 1 ? name(revisions[1]) : "working tree");
    // Renames are only found with both paths given (rename detection is
    // on by default in git diff).
    std::vector<std::string> limit;
    if (!paths.empty())
    {
        limit.push_back("--");
        limit.insert(limit.end(), paths.begin(), paths.end());
    }
    const std::string dir = repo_.workdir();
    auto offerSettings = [this](const std::string& text)
    {
        ui_.message(window_.get(),
                    {.icon = rap::MessageSpec::Icon::Question,
                     .title = "Tortilla",
                     .text = text,
                     .detail = "Choose one on the Diff Viewer page of the Settings.",
                     .buttons = {rap::DialogRole::Ok, rap::DialogRole::Cancel}},
                    [this](rap::DialogRole r)
                    {
                        if (r == rap::DialogRole::Ok) openSettings(SettingsPage::DiffViewer);
                    });
    };

    Args args;
    switch (diffMode(dir))
    {
    case DiffMode::Builtin:
    {
        Args diff{"diff"};
        diff.insert(diff.end(), revisions.begin(), revisions.end());
        diff.insert(diff.end(), limit.begin(), limit.end());
        showUnifiedDiff(ui_, base + ".." + mine, dir, diff);
        return;
    }
    case DiffMode::Git:
        if (gitDiffTool(dir).empty())
            return offerSettings("git has no diff tool configured (diff.tool).");
        args = {"difftool", "--no-prompt"};
        if (paths.size() != 1) args.insert(args.begin() + 1, "--dir-diff");
        args.insert(args.end(), revisions.begin(), revisions.end());
        args.insert(args.end(), limit.begin(), limit.end());
        break;
    case DiffMode::Custom:
        if (diffCommand().empty()) return offerSettings("No diff program is set.");
        args = customDifftoolArgs(diffCommand(), revisions, base, mine, paths);
        break;
    }
    auto output = std::make_shared<std::string>();
    difftool_ = GitProcess::start(
        ui_, dir, args, [output](const std::string& chunk) { *output += chunk; },
        [this, output](int code)
        {
            if (code != 0)
                ui_.message(window_.get(), {.icon = rap::MessageSpec::Icon::Error,
                                            .title = "Tortilla",
                                            .text = "The diff tool failed.",
                                            .detail = *output});
        });
}

Menu LogWindow::contextMenu(std::optional<size_t> clicked)
{
    auto sel = commitList_->selection();
    if (sel.empty()) return {};

    const bool hasWC = !repo_.bare();
    const Repo::State state = repo_.state();
    const bool merging = state == Repo::State::Merge;
    const std::string current = repo_.headName();
    const std::string cur = "\"" + menuText(current) + "\"";
    bool hasStash = false;
    for (size_t row = 0; row < log_.rowCount() && !hasStash; ++row)
        if (auto* refs = log_.refsOf(row))
            for (auto& r : *refs) hasStash = hasStash || r.kind == RefKind::Stash;
    (void)clicked;

    Menu m;

    if (sel.size() == 1)
    {
        size_t row = sel.front();
        const Commit* c = log_.commit(row);
        bool wc = !c;
        bool head = log_.isHead(row);
        const std::vector<Ref>* refs = log_.refsOf(row);
        // Right-clicked on a label: the menu is about that one ref.
        std::vector<Ref> clickedOnly;
        if (refs && clickedRef_)
            for (const Ref& r : *refs)
                if (r.fullName == *clickedRef_)
                {
                    clickedOnly = {r};
                    refs = &clickedOnly;
                    break;
                }
        bool isStash = false;
        if (refs)
            for (auto& r : *refs) isStash = isStash || r.kind == RefKind::Stash;
        std::string hash = c ? c->id.hex() : "";

        // Group A: diffs
        if (wc)
        {
            m.push_back(todo("C_ommit..."));
            if (merging)
                m.push_back(item("Abort _Merge",
                                 [this]
                                 {
                                     confirmThenRun("Abort the merge and discard its changes?",
                                                    "Abort Merge", {{"merge", "--abort"}});
                                 }));
            m.push_back(item("Show changes as _unified diff",
                             [this]
                             {
                                 showUnifiedDiff(ui_, "Working tree changes", repo_.workdir(),
                                                 {"diff", "HEAD"});
                             }));
            m.push_back(item("_Compare with previous revision", [this] { difftool({"HEAD"}); }));
        }
        else
        {
            if (hasWC)
                m.push_back(item("Compare with _working tree", [this, hash] { difftool({hash}); }));
            if (c->parents.size() > 1)
            {
                MenuItem udiff{.label = "_Unified diff with"};
                MenuItem compare{.label = "_Compare with previous revision"};
                for (size_t i = 0; i < c->parents.size(); ++i)
                {
                    std::string p = c->parents[i].hex();
                    std::string label = "Parent " + std::to_string(i + 1) + ": ";
                    if (auto prow = log_.rowOf(c->parents[i]))
                        label += "\"" + menuText(shortSubject(*log_.commit(*prow))) + "\" ";
                    label += "(" + c->parents[i].hex(8) + ")";
                    udiff.children.push_back(item(label,
                                                  [this, p, hash]
                                                  {
                                                      showUnifiedDiff(ui_, hash.substr(0, 8),
                                                                      repo_.workdir(),
                                                                      {"diff", p, hash});
                                                  }));
                    compare.children.push_back(
                        item(label, [this, p, hash] { difftool({p, hash}); }));
                }
                if (hasWC) m.push_back(std::move(udiff));
                m.push_back(std::move(compare));
            }
            else
            {
                if (hasWC)
                    m.push_back(item("Show changes as _unified diff",
                                     [this, hash]
                                     {
                                         showUnifiedDiff(ui_, hash.substr(0, 8), repo_.workdir(),
                                                         {"show", "--format=fuller", hash});
                                     }));
                if (!c->parents.empty())
                    m.push_back(item("_Compare with previous revision",
                                     [this, hash] { difftool({hash + "^", hash}); }));
            }
        }
        separator(m);

        // Group B: stash
        if (wc && !merging)
            m.push_back(
                item("Stash changes...", [this] { stashChanges(); }));
        if ((wc || isStash) && hasStash)
        {
            m.push_back(
                item("Stash Pop", [this] { runGitCommands("Stash Pop", {{"stash", "pop"}}); }));
            m.push_back(todo("Stash List"));
        }
        separator(m);

        // Group D: working tree row
        if (wc)
        {
            if (!merging) m.push_back(todo("_Pull..."));
            m.push_back(todo("F_etch..."));
            separator(m);
            m.push_back(todo("_Clean up..."));
            separator(m);
        }

        // Group E: commit rows
        if (!wc)
        {
            if (refs && !isStash) m.push_back(todo("Show log..."));
            m.push_back(todo("_Browse repository"));
            if (!head && hasWC && !merging && !isStash)
            {
                // Merge a branch at this commit by name, else the commit.
                std::string what = hash;
                if (refs)
                    for (auto& r : *refs)
                        if (r.kind == RefKind::LocalBranch || r.kind == RefKind::RemoteBranch)
                        {
                            what = r.name;
                            break;
                        }
                m.push_back(
                    item("_Merge to " + cur + "...", [this, c, what] { mergeFrom(*c, what); }));
            }
            if (hasWC && !isStash)
                m.push_back(item("R_eset " + cur + " to this...", [this, c] { resetTo(*c); }));

            std::vector<std::string> branches;
            if (refs)
                for (auto& r : *refs)
                    if (r.kind == RefKind::LocalBranch && !r.current) branches.push_back(r.name);
            if (!branches.empty() && hasWC && !isStash)
            {
                auto checkout = [this](const std::string& b)
                { runGitCommands("Switch/Checkout", {{"checkout", b}}); };
                if (branches.size() == 1)
                    m.push_back(item("_Switch/Checkout to \"" + menuText(branches[0]) + "\"",
                                     [checkout, b = branches[0]] { checkout(b); }));
                else
                {
                    MenuItem sub{.label = "_Switch/Checkout to"};
                    for (auto& b : branches)
                        sub.children.push_back(item(menuText(b), [checkout, b] { checkout(b); }));
                    m.push_back(std::move(sub));
                }
            }
            if (!head && hasWC && !isStash)
                m.push_back(item("Sw_itch/Checkout to this...",
                                 [this, hash]
                                 {
                                     confirmThenRun(
                                         "Check out " + hash.substr(0, 8) +
                                             "?\n\nThis leaves HEAD detached from any branch.",
                                         "Switch/Checkout", {{"checkout", "--detach", hash}});
                                 }));
            if (!isStash)
            {
                m.push_back(
                    item("Create Br_anch at this version...", [this, c] { createBranch(*c); }));
                m.push_back(item("Create _Tag at this version...", [this, c] { createTag(*c); }));
            }
            if (!head && hasWC && !merging && !isStash)
                m.push_back(item("Rebase " + cur + " onto this...(_G)",
                                 [this, hash] { openRebase(hash); }));
            m.push_back(todo("E_xport this version..."));
            if (hasWC && !merging && !isStash)
            {
                auto revert = [this, hash](std::optional<size_t> parent)
                {
                    Args args{"revert", "--no-edit"};
                    if (parent) args.insert(args.end(), {"-m", std::to_string(*parent + 1)});
                    args.push_back(hash);
                    confirmThenRun("Revert the changes made by " + hash.substr(0, 8) + "?",
                                   "Revert", {args});
                };
                if (c->parents.size() > 1)
                {
                    MenuItem sub{.label = "_Revert change by this commit"};
                    for (size_t i = 0; i < c->parents.size(); ++i)
                        sub.children.push_back(item("Parent " + std::to_string(i + 1) + " (" +
                                                        c->parents[i].hex(8) + ")",
                                                    [revert, i] { revert(i); }));
                    m.push_back(std::move(sub));
                }
                else
                    m.push_back(item("_Revert change by this commit", [revert] { revert({}); }));
            }
            if (!isStash) m.push_back(todo("Edit _Notes"));
            separator(m);
        }
    }
    else
    {
        // Two or more selected [spec 1.11]. sel is ascending: front() is the
        // newest row, back() the oldest.
        const Commit* newest = log_.commit(sel.front());
        const Commit* oldest = log_.commit(sel.back());
        bool contiguous = sel.back() - sel.front() + 1 == sel.size();
        if (newest && oldest)
        {
            std::string a = oldest->id.hex(), b = newest->id.hex();
            if (sel.size() == 2 || contiguous)
                m.push_back(item("_Compare revisions", [this, a, b] { difftool({a, b}); }));
            if (sel.size() == 2)
            {
                if (hasWC)
                    m.push_back(item("Show changes as _unified diff",
                                     [this, a, b]
                                     {
                                         showUnifiedDiff(ui_,
                                                         a.substr(0, 8) + ".." + b.substr(0, 8),
                                                         repo_.workdir(), {"diff", a, b});
                                     }));
                m.push_back(todo("Show log of " + a.substr(0, 8) + ".." + b.substr(0, 8)));
                m.push_back(todo("Show log of " + b.substr(0, 8) + ".." + a.substr(0, 8)));
                m.push_back(todo("Show log of " + a.substr(0, 8) + "..." + b.substr(0, 8)));
                m.push_back(todo("Comp_are change sets"));
            }
            separator(m);
            if (hasWC && !merging)
            {
                // Newest first, so later changes are undone before earlier ones.
                Args args{"revert", "--no-edit"};
                for (size_t r : sel)
                    if (const Commit* c = log_.commit(r)) args.push_back(c->id.hex());
                m.push_back(item("_Revert changes by these commits",
                                 [this, args]
                                 {
                                     confirmThenRun("Revert the changes made by " +
                                                        std::to_string(args.size() - 2) +
                                                        " commits?",
                                                    "Revert", {args});
                                 }));
            }
            separator(m);
        }
    }

    // Commit rows, single or multi
    {
        std::vector<std::string> picks;  // oldest first
        bool anyHead = false;
        for (auto it = sel.rbegin(); it != sel.rend(); ++it)
            if (const Commit* c = log_.commit(*it))
            {
                picks.push_back(c->id.hex());
                anyHead = anyHead || log_.isHead(*it);
            }
        if (!picks.empty() && picks.size() == sel.size())
        {
            if (sel.size() > 1) m.push_back(todo("Com_bine to one commit"));
            if (!anyHead && hasWC && !merging)
            {
                Args args{"cherry-pick"};
                args.insert(args.end(), picks.begin(), picks.end());
                std::string label = picks.size() == 1 ? "Cherr_y Pick this commit..."
                                                      : "Cherr_y Pick selected commits...";
                m.push_back(item(label,
                                 [this, args, n = picks.size()]
                                 {
                                     confirmThenRun("Cherry-pick " + std::to_string(n) +
                                                        " commit(s) onto the current branch?",
                                                    "Cherry Pick", {args});
                                 }));
            }
            m.push_back(todo("_Format Patch..."));
            separator(m);
        }
    }

    // Single selection, part 2
    if (sel.size() == 1)
    {
        size_t row = sel.front();
        const std::vector<Ref>* refs = log_.refsOf(row);
        // Right-clicked on a label: the menu is about that one ref.
        std::vector<Ref> clickedOnly;
        if (refs && clickedRef_)
            for (const Ref& r : *refs)
                if (r.fullName == *clickedRef_)
                {
                    clickedOnly = {r};
                    refs = &clickedOnly;
                    break;
                }
        if (refs) m.push_back(todo("Pus_h..."));
        if (log_.isHead(row) && hasWC && !merging) m.push_back(todo("_Pull..."));

        std::vector<const Ref*> deletable;
        if (refs)
            for (auto& r : *refs)
                if (!r.current) deletable.push_back(&r);
        auto deleteArgs = [](const Ref& r) -> Args
        {
            switch (r.kind)
            {
            case RefKind::LocalBranch: return {"branch", "-D", r.name};
            case RefKind::Tag: return {"tag", "-d", r.name};
            case RefKind::RemoteBranch:
            {
                auto slash = r.name.find('/');
                return {"push", r.name.substr(0, slash), "--delete", r.name.substr(slash + 1)};
            }
            case RefKind::Stash: return {"stash", "drop"};
            default: return {"update-ref", "-d", r.fullName};
            }
        };
        if (deletable.size() == 1)
        {
            const Ref r = *deletable.front();  // a copy: `refs` may be this call's own
            m.push_back(
                item("_Delete " + menuText(r.fullName), [this, r, deleteArgs]
                     { confirmThenRun("Delete " + r.fullName + "?", "Delete", {deleteArgs(r)}); }));
        }
        else if (deletable.size() > 1)
        {
            MenuItem sub{.label = "_Delete branch/tag"};
            std::vector<Args> all;
            for (const Ref* r : deletable)
            {
                Args args = deleteArgs(*r);
                all.push_back(args);
                sub.children.push_back(
                    item(menuText(r->fullName), [this, r, args]
                         { confirmThenRun("Delete " + r->fullName + "?", "Delete", {args}); }));
            }
            sub.children.push_back(item("All",
                                        [this, all]
                                        {
                                            confirmThenRun("Delete " + std::to_string(all.size()) +
                                                               " refs?",
                                                           "Delete", all);
                                        }));
            m.push_back(std::move(sub));
        }
        separator(m);
        if (!log_.filtering() && !log_.isWorkingTree(row))
        {
            m.push_back(todo("Collapse revisions"));
            separator(m);
        }
    }

    // Always present
    MenuItem copy{.label = "Copy to clipboard"};
    auto copyItem = [this, sel](const char* label, int format)
    { return item(label, [this, sel, format] { copyToClipboard(sel, format); }); };
    copy.children = {copyItem("Full data", CopyFull),
                     copyItem("Full commit data without changed paths", CopyFullNoPaths),
                     copyItem("SHA-1", CopyHash),
                     copyItem("Authors (full)", CopyAuthors),
                     copyItem("Author names", CopyAuthorNames),
                     copyItem("Author email addresses", CopyAuthorEmails),
                     copyItem("Subjects", CopySubjects),
                     copyItem("Messages", CopyMessages)};
    if (sel.size() == 1 && log_.refsOf(sel.front()))
        copy.children.push_back(copyItem("Tag/branch names", CopyRefNames));
    m.push_back(std::move(copy));
    m.push_back(item("Search _log messages...", [this] { search_->focus(); }));
    if (sel.size() == 1)
        if (const Commit* c = log_.commit(sel.front()))
            m.push_back(item("Show branches this commit is _on",
                             [this, c] { showBranchesContaining(*c); }));
    return m;
}

// Clipboard formats [GLLB:2378-2492].
void LogWindow::copyToClipboard(const std::vector<size_t>& rows, int format)
{
    std::string out;
    for (size_t row : rows)
    {
        const Commit* c = log_.commit(row);
        if (!c) continue;
        switch (format)
        {
        case CopyFull:
        case CopyFullNoPaths:
        {
            out += "Revision: " + c->id.hex() + "\n";
            out += "Author: " + c->author + " <" + c->email + ">\n";
            out += "Date: " + formatTime(c->time, c->offsetMinutes) + "\n";
            out += "Message:\n" + c->message;
            if (!out.ends_with('\n')) out += '\n';
            out += "----\n";
            if (format == CopyFull)
                if (auto files = repo_.changes(c->id))
                    for (auto& f : *files)
                    {
                        out += FileModel::statusName(f.status) + ": " + f.path;
                        if (!f.oldPath.empty()) out += " (from " + f.oldPath + ")";
                        out += "\n";
                    }
            out += "\n";
            break;
        }
        case CopyHash: out += c->id.hex() + "\n"; break;
        case CopyAuthors: out += c->author + " <" + c->email + ">\n"; break;
        case CopyAuthorNames: out += c->author + "\n"; break;
        case CopyAuthorEmails: out += c->email + "\n"; break;
        case CopySubjects: out += "* " + c->summary + "\n\n"; break;
        case CopyMessages: out += "* " + c->message + "\n\n"; break;
        case CopyRefNames:
            if (auto* refs = log_.refsOf(row))
                for (auto& r : *refs) out += r.name + "\n";
            break;
        }
    }
    ui_.setClipboard(out);
}

void LogWindow::showBranchesContaining(const Commit& c)
{
    GitResult r =
        runGit(repo_.workdir(), {"branch", "-a", "--contains", c.id.hex()}, GitOutput::Stdout);
    ui_.message(window_.get(), {.icon = rap::MessageSpec::Icon::Info,
                                .title = "Branches",
                                .text = "Branches containing " + c.id.hex(8) + ":",
                                .detail = r.output});
}

// TortoiseGit's Create Branch dialog, reduced to the common options.
// `name`, `switchTo` and `startPoint` prefill it (a remote branch
// double-clicked: its local name, switch ticked, the remote branch as the
// start point so the new branch tracks it).
void LogWindow::createBranch(const Commit& c, const std::string& name, bool switchTo,
                             const std::string& startPoint)
{
    struct State
    {
        std::unique_ptr<rap::Window> window;
        rap::Entry* name = nullptr;
        rap::Check* force = nullptr;
        rap::Check* switchTo = nullptr;
    };
    auto st = std::make_shared<State>();
    const std::string hash = c.id.hex();
    const std::string base = startPoint.empty() ? hash : startPoint;
    const std::string shown =
        (startPoint.empty() ? "" : startPoint + " ") + hash.substr(0, 8) + " " + c.summary;

    using namespace rap::layout;
    runDialog<State>(
        ui_, window_.get(), st,
        Dialog{"Create Branch",
               {VBox{
                   Form{
                       {"Branch", Entry{name}.bind(st->name).width(40_ch)},
                       {"Base On", Label{shown}.selectable()},
                       {"", Check{"_Force"}.bind(st->force)},
                       {"", Check{"Switch to new branch", switchTo}.bind(st->switchTo)},
                   },
                   Buttons{Ok, Cancel},
               }}}
            .resizable(false),
        [this, base](State& s)
        {
            std::string name = s.name->text();
            if (name.empty()) return;
            std::vector<Args> cmds;
            Args args{"branch"};
            if (s.force->checked()) args.push_back("--force");
            args.insert(args.end(), {name, base});
            cmds.push_back(args);
            // Switching moves HEAD (and can fail on local changes): that
            // one shows its progress and reloads.
            if (s.switchTo->checked())
            {
                cmds.push_back({"checkout", name});
                runGitCommands("Create Branch", cmds);
            }
            else
                runRefCommands("Create Branch", cmds);
        });
}

// TortoiseGit's Stash Save: a message, and whether untracked files go too
// (off, as git's own default: they may be build output or downloads, and
// a stash of thousands of files is slow to show and to apply).
void LogWindow::stashChanges()
{
    struct State
    {
        std::unique_ptr<rap::Window> window;
        rap::Entry* message = nullptr;
        rap::Check* untracked = nullptr;
    };
    auto st = std::make_shared<State>();

    using namespace rap::layout;
    runDialog<State>(
        ui_, window_.get(), st,
        Dialog{"Stash",
               {VBox{
                   Form{
                       {"Message", Entry{}.bind(st->message).placeholder("optional").width(40_ch)},
                       {"", Check{"Include _untracked files"}.bind(st->untracked)},
                   },
                   Buttons{Ok, Cancel},
               }}}
            .resizable(false),
        [this](State& s)
        {
            Args args{"stash", "push"};
            if (s.untracked->checked()) args.push_back("--include-untracked");
            if (std::string msg = s.message->text(); !msg.empty()) args.insert(args.end(), {"-m", msg});
            runGitCommands("Stash", {args});
        });
}

void LogWindow::createTag(const Commit& c)
{
    struct State
    {
        std::unique_ptr<rap::Window> window;
        rap::Entry* name = nullptr;
        rap::TextView* message = nullptr;
        rap::Check* force = nullptr;
    };
    auto st = std::make_shared<State>();
    std::string hash = c.id.hex();

    using namespace rap::layout;
    runDialog<State>(
        ui_, window_.get(), st,
        Dialog{"Create Tag",
               {VBox{
                   Form{
                       {"Tag", Entry{}.bind(st->name).width(40_ch)},
                       {"Base On", Label{hash.substr(0, 8) + " " + c.summary}.selectable()},
                       {"", Check{"_Force"}.bind(st->force)},
                   },
                   Label{"Message (makes an annotated tag):"},
                   TextView{}.bind(st->message).height(5_lines).stretch(),
                   Buttons{Ok, Cancel},
               }}}
            .size(60_ch, 18_lines),
        [this, hash](State& s)
        {
            std::string name = s.name->text();
            if (name.empty()) return;
            Args args{"tag"};
            if (s.force->checked()) args.push_back("--force");
            std::string msg = s.message->text();
            if (!msg.empty()) args.insert(args.end(), {"-a", "-m", msg});
            args.insert(args.end(), {name, hash});
            runRefCommands("Create Tag", {args});
        });
}

// Dragging a label onto another commit. The confirmation says what will
// happen: a fast-forward, or which commits the branch stops containing. The
// checked-out branch can't be moved under the working tree, so for it the
// drop opens Reset instead.
void LogWindow::moveRef(const Ref& ref, const Commit& target)
{
    if (ref.current) return resetTo(target);
    auto from = repo_.resolve(ref.fullName + "^{commit}");
    if (!from || *from == target.id) return;
    const std::string to = target.id.hex();
    auto describe = [&](const Oid& id)
    {
        const Commit* c = nullptr;
        if (auto row = log_.rowOf(id)) c = log_.commit(*row);
        return id.hex(8) + (c ? " \u201c" + c->summary + "\u201d" : "");
    };
    const bool tag = ref.kind == RefKind::Tag;
    std::string text = std::string(tag ? "Move tag " : "Move branch ") + ref.name + "\nfrom " +
                       describe(*from) + "\nto " + describe(target.id) + "?";
    std::string detail;
    if (tag)
        detail = "Anyone who already fetched the tag keeps the old one.";
    else if (repo_.isAncestor(*from, target.id))
        detail = "A fast-forward: " + ref.name + " gains commits and loses none.";
    else
    {
        GitResult lost = runGit(repo_.workdir(), {"rev-list", "--count", to + ".." + from->hex()},
                                GitOutput::Stdout);
        GitResult some = runGit(repo_.workdir(),
                                {"log", "--format=%h %s", "--max-count=6", to + ".." + from->hex()},
                                GitOutput::Stdout);
        detail = ref.name + " will no longer contain " +
                 (lost.ok() ? lost.output.substr(0, lost.output.find('\n')) : "some") +
                 " commit(s):\n" + some.output +
                 "(They stay reachable from the reflog for a while.)";
    }

    Args args;
    if (!tag)
        args = {"branch", "--force", ref.name, to};
    else if (ref.annotated)
    {
        // Keeping it annotated: same message on the new commit.
        GitResult msg = runGit(repo_.workdir(), {"tag", "-l", "--format=%(contents)", ref.name},
                               GitOutput::Stdout);
        args = {"tag", "--force", "-a", ref.name, to, "-m", msg.ok() ? msg.output : ref.name};
    }
    else
        args = {"tag", "--force", ref.name, to};

    const std::string title = tag ? "Move Tag" : "Move Branch";
    ui_.message(window_.get(),
                {.icon = rap::MessageSpec::Icon::Question,
                 .title = title,
                 .text = text,
                 .detail = detail,
                 .buttons = {rap::DialogRole::Ok, rap::DialogRole::Cancel}},
                [this, title, args](rap::DialogRole r)
                {
                    if (r == rap::DialogRole::Ok) runRefCommands(title, {args});
                });
}

// A right-click on a label makes the menu about that label (checkout,
// delete) rather than offering every label on the commit.
Menu LogWindow::cellMenu(size_t row, size_t column, rap::PointF at)
{
    if (column == LogModel::Message)
        if (auto ref = log_.labelAt(row, at.x)) clickedRef_ = ref->fullName;
    Menu m = contextMenu(row);
    clickedRef_.reset();
    return m;
}

void LogWindow::rowActivated(size_t row)
{
    // A double-click on a label (its press was just now, on this row).
    auto press = std::exchange(lastLabelPress_, std::nullopt);
    if (!press || press->row != row ||
        std::chrono::steady_clock::now() - press->at > std::chrono::seconds(1))
        return;
    const std::vector<Ref>* refs = log_.refsOf(row);
    const Commit* c = log_.commit(row);
    if (!refs || !c) return;
    for (const Ref& r : *refs)
    {
        if (r.fullName != press->fullName) continue;
        if (r.kind == RefKind::LocalBranch && !r.current) switchTo(r.name, false);
        if (r.kind == RefKind::Tag)
        {
            // A tag isn't a branch: checking it out detaches HEAD, so ask.
            const std::string t = "\u201c" + r.name + "\u201d";
            choose(
                "Switch to " + r.name, "What would you like to do with the tag " + t + "?",
                {{"_Check out " + menuText(r.name) + " (detached)",
                  "To look at or build that version; new commits there aren't on any branch.",
                  [this, name = r.name] { switchTo(name, true); }},
                 {"Create a _branch there and switch to it...",
                  "For working from that version: the new branch starts at the tag.",
                  [this, commit = *c, name = r.name] { createBranch(commit, "", true, name); }}});
        }
        if (r.kind == RefKind::RemoteBranch)
        {
            // origin/feature -> a local "feature" tracking it, switched to;
            // with a local "feature" already there, that's switched to (as
            // git switch feature does).
            const size_t slash = r.name.find('/');
            const std::string local =
                slash == std::string::npos ? r.name : r.name.substr(slash + 1);
            if (repo_.resolve("refs/heads/" + local))
                switchTo(local, false);
            else
                createBranch(*c, local, true, r.name);
        }
        return;
    }
}

// A dialog of choices, each a button with a line saying what it does:
// choosing one closes the dialog, then acts.
void LogWindow::choose(const std::string& title, const std::string& question,
                       std::vector<Choice> choices)
{
    struct State
    {
        std::unique_ptr<rap::Window> window;
    };
    auto st = std::make_shared<State>();
    std::weak_ptr<State> weak = st;
    using namespace rap::layout;
    std::vector<rap::Node> rows{Label{question}};
    for (Choice& c : choices)
    {
        rows.push_back(Button{c.label}.onClick(
            [this, weak, act = std::move(c.act)]
            {
                auto s = weak.lock();
                if (!s) return;
                s->window->close();
                ui_.post([s] {});  // destroyed after this click returns
                act();
            }));
        rows.push_back(Label{"   " + c.description});
    }
    rows.push_back(Buttons{Cancel});
    // A box from a list built at run time (VBox{...} takes a fixed list).
    rap::Node box{rap::spec::Box{rap::Orientation::Vertical, std::move(rows)}, {}};
    st->window = ui_.create(Dialog{title, {std::move(box)}}.resizable(false), window_.get());
    st->window->onFinished(
        [this, st](rap::DialogRole) mutable
        {
            auto keep = std::move(st);
            ui_.post([keep] {});
        });
    st->window->show();
}

// Switching to a branch, or to a tag or commit (`detach`). A clean working
// tree just switches; uncommitted changes (tracked files: untracked ones
// stay put either way) ask what to do with them.
void LogWindow::switchTo(const std::string& target, bool detach)
{
    GitResult dirty = runGit(repo_.workdir(), {"status", "--porcelain", "--untracked-files=no"},
                             GitOutput::Stdout);
    if (dirty.ok() && dirty.output.empty()) return runSwitch(target, detach, SwitchMode::Plain);
    const std::string t = "\u201c" + target + "\u201d";
    choose(
        "Switch to " + target,
        "You have uncommitted changes. What should happen to them when switching to " + t + "?",
        {{"_Stash them, then switch", "The stash stays with this commit, ready when you come back.",
          [this, target, detach] { runSwitch(target, detach, SwitchMode::Stash); }},
         {"_Bring them along",
          "Merged into " + t +
              " where it differs; conflicts, if any, are left marked in the files.",
          [this, target, detach] { runSwitch(target, detach, SwitchMode::BringAlong); }},
         {"_Discard them and switch", "Your uncommitted changes are lost. This can't be undone.",
          [this, target, detach] { runSwitch(target, detach, SwitchMode::Discard); }}});
}

// Runs a switch, then reloads and offers back a stash made at the new
// commit. Bringing changes along tries a plain checkout first (git carries
// changes over when both sides agree on those files) and only falls back to
// --merge when git refuses.
void LogWindow::runSwitch(const std::string& target, bool detach, SwitchMode mode)
{
    auto checkout = [&](const char* flag)
    {
        Args a{"checkout"};
        if (flag) a.push_back(flag);
        if (detach) a.push_back("--detach");
        a.push_back(target);
        return a;
    };
    std::vector<Args> commands;
    if (mode == SwitchMode::Stash)
        commands.push_back({"stash", "push", "-m", "Tortilla: before switching to " + target});
    commands.push_back(checkout(mode == SwitchMode::Discard ? "--force" : nullptr));
    const bool bringAlong = mode == SwitchMode::BringAlong;
    const Args merge = checkout("--merge");
    runQuietly(
        "Switch/Checkout", std::move(commands),
        [this, bringAlong, merge](bool ok, const std::string& output)
        {
            if (!ok && bringAlong && output.find("would be overwritten") != std::string::npos)
                return runQuietly("Switch/Checkout", {merge},
                                  [this](bool ok2, const std::string&)
                                  {
                                      reload();
                                      if (ok2) offerStashPop();
                                  });
            if (!ok && bringAlong)
                ui_.message(window_.get(), {.icon = rap::MessageSpec::Icon::Error,
                                            .title = "Switch/Checkout",
                                            .text = "Switch/Checkout failed.",
                                            .detail = output});
            reload();
            if (ok) offerStashPop();
        },
        !bringAlong);
}

// The newest stash made on the commit now checked out, if any, offered back.
void LogWindow::offerStashPop()
{
    auto head = repo_.resolve("HEAD");
    if (!head) return;
    GitResult list =
        runGit(repo_.workdir(), {"stash", "list", "--format=%gd%x09%P%x09%gs"}, GitOutput::Stdout);
    if (!list.ok()) return;
    for (size_t at = 0; at < list.output.size();)
    {
        size_t nl = list.output.find('\n', at);
        if (nl == std::string::npos) nl = list.output.size();
        const std::string line = list.output.substr(at, nl - at);
        at = nl + 1;
        const size_t t1 = line.find('\t'),
                     t2 = t1 == std::string::npos ? t1 : line.find('\t', t1 + 1);
        if (t2 == std::string::npos) continue;
        const std::string ref = line.substr(0, t1), parent = line.substr(t1 + 1, 40);
        if (parent != head->hex()) continue;
        const std::string subject = line.substr(t2 + 1);
        // On the commit it was made on, with nothing else changed, a stash
        // applies cleanly by construction. Only changes already here (ones
        // brought along by the switch) can stand in its way.
        GitResult dirty = runGit(repo_.workdir(), {"status", "--porcelain", "--untracked-files=no"},
                                 GitOutput::Stdout);
        const bool clean = dirty.ok() && dirty.output.empty();
        ui_.message(
            window_.get(),
            {.icon = rap::MessageSpec::Icon::Question,
             .title = "Stash",
             .text = "A stash was made here:\n\u201c" + subject + "\u201d\n\nApply it now?",
             .detail = clean ? ""
                             : "You have uncommitted changes. If the stash changes the same files, "
                               "git won't apply it and it stays in the stash list.",
             .buttons = {rap::DialogRole::Yes, rap::DialogRole::No}},
            [this, ref](rap::DialogRole r)
            {
                if (r == rap::DialogRole::Yes)
                    runQuietly("Stash Pop", {{"stash", "pop", ref}},
                               [this](bool, const std::string&) { reload(); });
            });
        return;  // the newest only
    }
}

rap::List::CellDrag LogWindow::labelDrag()
{
    return {
        .begin = [this](size_t row, size_t column,
                        rap::PointF at) -> std::optional<rap::List::CellDragStart>
        {
            lastLabelPress_.reset();
            if (column != LogModel::Message) return std::nullopt;
            auto ref = log_.labelAt(row, at.x);
            if (ref)
                lastLabelPress_ = LabelPress{row, ref->fullName, std::chrono::steady_clock::now()};
            // Remote branches would need a force push: not by dragging.
            if (!ref || (ref->kind != RefKind::LocalBranch && ref->kind != RefKind::Tag))
                return std::nullopt;
            return rap::List::CellDragStart{ref->name, ref->fullName};
        },
        .canDrop = [this](size_t from, size_t to, const std::string&)
        { return to != from && log_.commit(to) != nullptr; },
        .drop =
            [this](size_t from, size_t to, const std::string& fullName)
        {
            const std::vector<Ref>* refs = log_.refsOf(from);
            const Commit* target = log_.commit(to);
            if (!refs || !target) return;
            for (const Ref& r : *refs)
                if (r.fullName == fullName) return moveRef(r, *target);
        },
    };
}

// TortoiseGit's Reset dialog: soft / mixed / hard.
void LogWindow::resetTo(const Commit& c)
{
    struct State
    {
        std::unique_ptr<rap::Window> window;
        rap::Select* type = nullptr;
    };
    auto st = std::make_shared<State>();
    std::string hash = c.id.hex();

    using namespace rap::layout;
    runDialog<State>(
        ui_, window_.get(), st,
        Dialog{"Reset",
               {VBox{
                   Label{"Reset \"" + repo_.headName() + "\" to " + hash.substr(0, 8) + " " +
                         c.summary},
                   Form{
                       {"Reset Type",
                        Select{{"Soft: Leave working tree and index untouched",
                                "Mixed: Leave working tree untouched, reset index",
                                "Hard: Reset working tree and index (discard all local changes)"},
                               1}
                            .bind(st->type)},
                   },
                   Buttons{Ok, Cancel},
               }}}
            .resizable(false),
        [this, hash](State& s)
        {
            static const char* modes[] = {"--soft", "--mixed", "--hard"};
            int i = std::clamp(s.type->current(), 0, 2);
            runGitCommands("Reset", {{"reset", modes[i], hash}});
        });
}

// TortoiseGit's Merge dialog, reduced to its main options.
void LogWindow::mergeFrom(const Commit&, const std::string& what)
{
    struct State
    {
        std::unique_ptr<rap::Window> window;
        rap::Check* noFF = nullptr;
        rap::Check* squash = nullptr;
        rap::Check* noCommit = nullptr;
    };
    auto st = std::make_shared<State>();

    using namespace rap::layout;
    runDialog<State>(ui_, window_.get(), st,
                     Dialog{"Merge",
                            {VBox{
                                Label{"Merge " + what + " into \"" + repo_.headName() + "\""},
                                Group{"Options",
                                      VBox{
                                          Check{"No _Fast Forward"}.bind(st->noFF),
                                          Check{"S_quash"}.bind(st->squash),
                                          Check{"No _Commit"}.bind(st->noCommit),
                                      }},
                                Buttons{Ok, Cancel},
                            }}}
                         .resizable(false),
                     [this, what](State& s)
                     {
                         Args args{"merge"};
                         if (s.noFF->checked()) args.push_back("--no-ff");
                         if (s.squash->checked()) args.push_back("--squash");
                         if (s.noCommit->checked()) args.push_back("--no-commit");
                         args.insert(args.end(), {"--no-edit", what});
                         runGitCommands("Merge", {args});
                     });
}

// Opens the Rebase dialog onto `upstream`; the log reloads when it closes.
void LogWindow::openRebase(const std::string& upstream)
{
    auto w = RebaseWindow::open(ui_, repo_.workdir(), upstream);
    if (!w)
    {
        ui_.message(
            window_.get(),
            {.icon = rap::MessageSpec::Icon::Error, .title = "Rebase", .text = w.error()});
        return;
    }
    RebaseWindow* raw = w->get();
    raw->onClosed(
        [this, raw]
        {
            reload();
            // Destroyed after the close callback has returned.
            ui_.post([this, raw]
                     { std::erase_if(rebases_, [raw](auto& r) { return r.get() == raw; }); });
        });
    raw->show();
    rebases_.push_back(std::move(*w));
}

}  // namespace tortilla
