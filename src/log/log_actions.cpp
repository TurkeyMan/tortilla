// The Log dialog's commit-list context menu and the actions behind it
// [spec 1.11]. Actions shell out to git and show their output in the
// progress dialog, as TortoiseGit does, then reload the log.
#include "log_window.h"

#include "branch/create_ref_dialog.h"
#include "branch/merge_dialog.h"
#include "branch/switch_dialog.h"
#include "changes/commit_window.h"
#include "remote/pull_fetch_dialog.h"
#include "remote/push_dialog.h"
#include "ui/windows.h"

#include "git/command.h"
#include "git/diff_tool.h"
#include "ui/compare.h"
#include "ui/diff_window.h"
#include "ui/menu_items.h"
#include "ui/menu_text.h"
#include "ui/progress_dialog.h"
#include "ui/stash_dialog.h"
#include "util/calendar.h"

#include <ctime>

namespace tortilla
{
namespace
{

using rap::menu;
using rap::menu_item;
using arg_list = std::vector<std::string>;

enum copy_format
{
    copy_full,
    copy_full_no_paths,
    copy_hash,
    copy_authors,
    copy_author_names,
    copy_author_emails,
    copy_subjects,
    copy_messages,
    copy_ref_names,
};

std::string short_subject(const commit& c)
{
    return c.summary.size() > 20 ? c.summary.substr(0, 20) + "..." : c.summary;
}

std::string format_time(int64_t time, int offset_minutes)
{
    std::time_t t = time + int64_t(offset_minutes) * 60;
    std::tm tm = utc_calendar(t);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tm);
    return buf;
}

// A modal dialog that owns its widgets' state until it closes.
template <class state>
void run_dialog(rap::ui& ui, rap::window* parent, std::shared_ptr<state> st, rap::window_spec spec,
                std::function<void(state&)> on_ok)
{
    st->window = ui.create(spec, parent);
    st->window->on_finished(
        [st, on_ok, &ui](rap::dialog_role role) mutable
        {
            auto keep = std::move(st);
            if (role == rap::dialog_role::ok) on_ok(*keep);
            ui.post([keep] {});  // destroy after this callback returns
        });
    st->window->show();
}

}  // namespace

void log_window::run_git_commands(const std::string& title, std::vector<arg_list> commands)
{
    run_progress_dialog(ui_, window_.get(), title, repo_.workdir(), std::move(commands),
                        [this](bool) { reload(); });
}

void log_window::run_ref_commands(const std::string& title, std::vector<arg_list> commands)
{
    run_quietly(title, std::move(commands), [this](bool, const std::string&) { refs_changed(); });
}

void log_window::run_quietly(const std::string& title, std::vector<arg_list> commands,
                             std::function<void(bool ok, const std::string& output)> after,
                             bool show_failure)
{
    // One after another; the first failure stops the rest.
    struct command_run
    {
        std::vector<arg_list> commands;
        size_t next = 0;
        std::string output;
    };
    auto run = std::make_shared<command_run>(command_run{std::move(commands)});
    auto step = std::make_shared<std::function<void()>>();
    *step = [this, title, run, after, show_failure,
             weak_step = std::weak_ptr<std::function<void()>>(step)]
    {
        if (run->next == run->commands.size())
        {
            ref_command_.reset();
            if (after) after(true, run->output);
            return;
        }
        const arg_list& args = run->commands[run->next++];
        run->output += command_line(args) + "\n";
        auto self = weak_step.lock();
        ref_command_ = git_process::start(
            ui_, repo_.workdir(), args, [run](const std::string& chunk) { run->output += chunk; },
            [this, title, run, self, after, show_failure](int code)
            {
                if (code == 0) return (*self)();
                ref_command_.reset();
                if (show_failure)
                    ui_.message(window_.get(), {.icon = rap::message_spec::icon::error,
                                                .title = title,
                                                .text = title + " failed.",
                                                .detail = run->output});
                if (after) after(false, run->output);  // an earlier command may have succeeded
            });
    };
    (*step)();
}

void log_window::refs_changed()
{
    log_.refresh_refs(
        [this](bool reload_needed)
        {
            if (reload_needed) return reload();
            update_ref_button();
            if (auto sel = commit_list_->selection(); sel.size() == 1) show_commit(sel.front());
        });
}

void log_window::confirm_then_run(const std::string& question, const std::string& title,
                                  std::vector<arg_list> commands)
{
    ui_.message(window_.get(),
                {.icon = rap::message_spec::icon::question,
                 .title = title,
                 .text = question,
                 .buttons = {rap::dialog_role::yes, rap::dialog_role::no}},
                [this, title, commands](rap::dialog_role r)
                {
                    if (r != rap::dialog_role::yes) return;
                    // Deleting local branches and tags only touches refs.
                    const bool refs_only = std::ranges::all_of(
                        commands,
                        [](const arg_list& a)
                        {
                            return !a.empty() &&
                                   (a[0] == "branch" || a[0] == "tag" || a[0] == "update-ref");
                        });
                    if (refs_only)
                        run_ref_commands(title, commands);
                    else
                        run_git_commands(title, commands);
                });
}

// Comparing revisions with the tool the Diff Viewer page chose (compare.h).
void log_window::difftool(std::vector<std::string> revisions, std::vector<std::string> paths)
{
    difftool_ =
        compare_revisions(ui_, window_.get(), repo_.workdir(), std::move(revisions),
                          std::move(paths), [this] { open_settings(settings_page::diff_viewer); });
}

menu log_window::context_menu(std::optional<size_t> clicked)
{
    auto sel = commit_list_->selection();
    if (sel.empty()) return {};

    const bool has_wc = !repo_.bare();
    const repo::repo_state state = repo_.state();
    const bool merging = state == repo::repo_state::merge;
    const std::string current = repo_.head_name();
    const std::string cur = "\"" + menu_text(current) + "\"";
    bool has_stash = false;
    for (size_t row = 0; row < log_.row_count() && !has_stash; ++row)
        if (auto* refs = log_.refs_of(row))
            for (auto& r : *refs) has_stash = has_stash || r.kind == ref_kind::stash;
    (void)clicked;

    menu m;

    if (sel.size() == 1)
    {
        size_t row = sel.front();
        const commit* c = log_.commit(row);
        bool wc = !c;
        bool head = log_.is_head(row);
        const std::vector<ref>* refs = log_.refs_of(row);
        // Right-clicked on a label: the menu is about that one ref.
        std::vector<ref> clicked_only;
        if (refs && clicked_ref_)
            for (const ref& r : *refs)
                if (r.full_name == *clicked_ref_)
                {
                    clicked_only = {r};
                    refs = &clicked_only;
                    break;
                }
        bool is_stash = false;
        if (refs)
            for (auto& r : *refs) is_stash = is_stash || r.kind == ref_kind::stash;
        std::string hash = c ? c->id.hex() : "";

        // Group A: diffs
        if (wc)
        {
            m.push_back(item("C_ommit...",
                             [this] { keep_dialog(commit_window::open(ui_, {repo_.workdir()})); }));
            if (merging)
                m.push_back(item("Abort _Merge", [this]
                                 { keep_dialog(abort_merge_dialog::open(ui_, repo_.workdir())); }));
            m.push_back(item("Show changes as _unified diff",
                             [this]
                             {
                                 show_unified_diff(ui_, "Working tree changes", repo_.workdir(),
                                                   {"diff", "HEAD"});
                             }));
            m.push_back(item("_Compare with previous revision", [this] { difftool({"HEAD"}); }));
        }
        else
        {
            if (has_wc)
                m.push_back(item("Compare with _working tree", [this, hash] { difftool({hash}); }));
            if (c->parents.size() > 1)
            {
                menu_item udiff{.label = "_Unified diff with"};
                menu_item compare{.label = "_Compare with previous revision"};
                for (size_t i = 0; i < c->parents.size(); ++i)
                {
                    std::string p = c->parents[i].hex();
                    std::string label = "Parent " + std::to_string(i + 1) + ": ";
                    if (auto prow = log_.row_of(c->parents[i]))
                        label += "\"" + menu_text(short_subject(*log_.commit(*prow))) + "\" ";
                    label += "(" + c->parents[i].hex(8) + ")";
                    udiff.children.push_back(item(label,
                                                  [this, p, hash]
                                                  {
                                                      show_unified_diff(ui_, hash.substr(0, 8),
                                                                        repo_.workdir(),
                                                                        {"diff", p, hash});
                                                  }));
                    compare.children.push_back(
                        item(label, [this, p, hash] { difftool({p, hash}); }));
                }
                if (has_wc) m.push_back(std::move(udiff));
                m.push_back(std::move(compare));
            }
            else
            {
                if (has_wc)
                    m.push_back(item("Show changes as _unified diff",
                                     [this, hash]
                                     {
                                         show_unified_diff(ui_, hash.substr(0, 8), repo_.workdir(),
                                                           {"show", "--format=fuller", hash});
                                     }));
                if (!c->parents.empty())
                    m.push_back(item("_Compare with previous revision",
                                     [this, hash] { difftool({hash + "^", hash}); }));
            }
        }
        separator(m);

        // Group B: stash
        if (wc && !merging) m.push_back(item("Stash changes...", [this] { stash_changes(); }));
        if ((wc || is_stash) && has_stash)
        {
            m.push_back(
                item("Stash Pop", [this] { run_git_commands("Stash Pop", {{"stash", "pop"}}); }));
            m.push_back(todo("Stash List"));
        }
        separator(m);

        // Group D: working tree row
        if (wc)
        {
            if (!merging)
                m.push_back(
                    item("_Pull...", [this]
                         { keep_dialog(pull_fetch_dialog::open(ui_, repo_.workdir(), true)); }));
            m.push_back(
                item("F_etch...", [this]
                     { keep_dialog(pull_fetch_dialog::open(ui_, repo_.workdir(), false)); }));
            separator(m);
            m.push_back(todo("_Clean up..."));
            separator(m);
        }

        // Group E: commit rows
        if (!wc)
        {
            if (refs && !is_stash) m.push_back(todo("Show log..."));
            m.push_back(todo("_Browse repository"));
            if (!head && has_wc && !merging && !is_stash)
            {
                // Merge a branch at this commit by name, else the commit.
                std::string what = hash;
                if (refs)
                    for (auto& r : *refs)
                        if (r.kind == ref_kind::local_branch || r.kind == ref_kind::remote_branch ||
                            r.kind == ref_kind::tag)
                        {
                            what = r.full_name;
                            break;
                        }
                m.push_back(
                    item("_Merge to " + cur + "...", [this, c, what] { merge_from(*c, what); }));
            }
            if (has_wc && !is_stash)
                m.push_back(item("R_eset " + cur + " to this...", [this, c] { reset_to(*c); }));

            std::vector<std::string> branches;
            if (refs)
                for (auto& r : *refs)
                    if (r.kind == ref_kind::local_branch && !r.current) branches.push_back(r.name);
            if (!branches.empty() && has_wc && !is_stash)
            {
                auto checkout = [this](const std::string& b)
                { run_git_commands("Switch/Checkout", {{"checkout", b}}); };
                if (branches.size() == 1)
                    m.push_back(item("_Switch/Checkout to \"" + menu_text(branches[0]) + "\"",
                                     [checkout, b = branches[0]] { checkout(b); }));
                else
                {
                    menu_item sub{.label = "_Switch/Checkout to"};
                    for (auto& b : branches)
                        sub.children.push_back(item(menu_text(b), [checkout, b] { checkout(b); }));
                    m.push_back(std::move(sub));
                }
            }
            if (!head && has_wc && !is_stash)
                m.push_back(item("Sw_itch/Checkout to this...",
                                 [this, c]
                                 {
                                     keep_dialog(switch_dialog::open(ui_, repo_.workdir(),
                                                                     remote_or_hash(*c)));
                                 }));
            if (!is_stash)
            {
                m.push_back(
                    item("Create Br_anch at this version...", [this, c] { create_branch(*c); }));
                m.push_back(item("Create _Tag at this version...", [this, c] { create_tag(*c); }));
            }
            if (!head && has_wc && !merging && !is_stash)
                m.push_back(item("Rebase " + cur + " onto this...(_G)",
                                 [this, hash] { open_rebase(hash); }));
            m.push_back(todo("E_xport this version..."));
            if (has_wc && !merging && !is_stash)
            {
                auto revert = [this, hash](std::optional<size_t> parent)
                {
                    arg_list args{"revert", "--no-edit"};
                    if (parent) args.insert(args.end(), {"-m", std::to_string(*parent + 1)});
                    args.push_back(hash);
                    confirm_then_run("Revert the changes made by " + hash.substr(0, 8) + "?",
                                     "Revert", {args});
                };
                if (c->parents.size() > 1)
                {
                    menu_item sub{.label = "_Revert change by this commit"};
                    for (size_t i = 0; i < c->parents.size(); ++i)
                        sub.children.push_back(item("Parent " + std::to_string(i + 1) + " (" +
                                                        c->parents[i].hex(8) + ")",
                                                    [revert, i] { revert(i); }));
                    m.push_back(std::move(sub));
                }
                else
                    m.push_back(item("_Revert change by this commit", [revert] { revert({}); }));
            }
            if (!is_stash) m.push_back(todo("Edit _Notes"));
            separator(m);
        }
    }
    else
    {
        // Two or more selected [spec 1.11]. sel is ascending: front() is the
        // newest row, back() the oldest.
        const commit* newest = log_.commit(sel.front());
        const commit* oldest = log_.commit(sel.back());
        bool contiguous = sel.back() - sel.front() + 1 == sel.size();
        if (newest && oldest)
        {
            std::string a = oldest->id.hex(), b = newest->id.hex();
            if (sel.size() == 2 || contiguous)
                m.push_back(item("_Compare revisions", [this, a, b] { difftool({a, b}); }));
            if (sel.size() == 2)
            {
                if (has_wc)
                    m.push_back(item("Show changes as _unified diff",
                                     [this, a, b]
                                     {
                                         show_unified_diff(ui_,
                                                           a.substr(0, 8) + ".." + b.substr(0, 8),
                                                           repo_.workdir(), {"diff", a, b});
                                     }));
                m.push_back(todo("Show log of " + a.substr(0, 8) + ".." + b.substr(0, 8)));
                m.push_back(todo("Show log of " + b.substr(0, 8) + ".." + a.substr(0, 8)));
                m.push_back(todo("Show log of " + a.substr(0, 8) + "..." + b.substr(0, 8)));
                m.push_back(todo("Comp_are change sets"));
            }
            separator(m);
            if (has_wc && !merging)
            {
                // Newest first, so later changes are undone before earlier ones.
                arg_list args{"revert", "--no-edit"};
                for (size_t r : sel)
                    if (const commit* c = log_.commit(r)) args.push_back(c->id.hex());
                m.push_back(item("_Revert changes by these commits",
                                 [this, args]
                                 {
                                     confirm_then_run("Revert the changes made by " +
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
        bool any_head = false;
        for (auto it = sel.rbegin(); it != sel.rend(); ++it)
            if (const commit* c = log_.commit(*it))
            {
                picks.push_back(c->id.hex());
                any_head = any_head || log_.is_head(*it);
            }
        if (!picks.empty() && picks.size() == sel.size())
        {
            if (sel.size() > 1) m.push_back(todo("Com_bine to one commit"));
            if (!any_head && has_wc && !merging)
            {
                arg_list args{"cherry-pick"};
                args.insert(args.end(), picks.begin(), picks.end());
                std::string label = picks.size() == 1 ? "Cherr_y Pick this commit..."
                                                      : "Cherr_y Pick selected commits...";
                m.push_back(item(label,
                                 [this, args, n = picks.size()]
                                 {
                                     confirm_then_run("Cherry-pick " + std::to_string(n) +
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
        const std::vector<ref>* refs = log_.refs_of(row);
        // Right-clicked on a label: the menu is about that one ref.
        std::vector<ref> clicked_only;
        if (refs && clicked_ref_)
            for (const ref& r : *refs)
                if (r.full_name == *clicked_ref_)
                {
                    clicked_only = {r};
                    refs = &clicked_only;
                    break;
                }
        if (refs)
        {
            // The row's local branch, if it has one, is what's pushed.
            std::string local;
            for (const auto& r : *refs)
                if (r.kind == ref_kind::local_branch && local.empty()) local = r.name;
            m.push_back(item("Pus_h...", [this, local]
                             { keep_dialog(push_dialog::open(ui_, repo_.workdir(), local)); }));
        }
        if (log_.is_head(row) && has_wc && !merging)
            m.push_back(
                item("_Pull...",
                     [this] { keep_dialog(pull_fetch_dialog::open(ui_, repo_.workdir(), true)); }));

        std::vector<const ref*> deletable;
        if (refs)
            for (auto& r : *refs)
                if (!r.current) deletable.push_back(&r);
        auto delete_args = [](const ref& r) -> arg_list
        {
            switch (r.kind)
            {
            case ref_kind::local_branch: return {"branch", "-D", r.name};
            case ref_kind::tag: return {"tag", "-d", r.name};
            case ref_kind::remote_branch:
            {
                auto slash = r.name.find('/');
                return {"push", r.name.substr(0, slash), "--delete", r.name.substr(slash + 1)};
            }
            case ref_kind::stash: return {"stash", "drop"};
            default: return {"update-ref", "-d", r.full_name};
            }
        };
        if (deletable.size() == 1)
        {
            const ref r = *deletable.front();  // a copy: `refs` may be this call's own
            m.push_back(item(
                "_Delete " + menu_text(r.full_name), [this, r, delete_args]
                { confirm_then_run("Delete " + r.full_name + "?", "Delete", {delete_args(r)}); }));
        }
        else if (deletable.size() > 1)
        {
            menu_item sub{.label = "_Delete branch/tag"};
            std::vector<arg_list> all;
            for (const ref* r : deletable)
            {
                arg_list args = delete_args(*r);
                all.push_back(args);
                sub.children.push_back(
                    item(menu_text(r->full_name), [this, r, args]
                         { confirm_then_run("Delete " + r->full_name + "?", "Delete", {args}); }));
            }
            sub.children.push_back(item("All",
                                        [this, all]
                                        {
                                            confirm_then_run(
                                                "Delete " + std::to_string(all.size()) + " refs?",
                                                "Delete", all);
                                        }));
            m.push_back(std::move(sub));
        }
        separator(m);
        if (!log_.filtering() && !log_.is_working_tree(row))
        {
            m.push_back(todo("Collapse revisions"));
            separator(m);
        }
    }

    // Always present
    menu_item copy{.label = "Copy to clipboard"};
    auto copy_item = [this, sel](const char* label, int format)
    { return item(label, [this, sel, format] { copy_to_clipboard(sel, format); }); };
    copy.children = {copy_item("Full data", copy_full),
                     copy_item("Full commit data without changed paths", copy_full_no_paths),
                     copy_item("SHA-1", copy_hash),
                     copy_item("Authors (full)", copy_authors),
                     copy_item("Author names", copy_author_names),
                     copy_item("Author email addresses", copy_author_emails),
                     copy_item("Subjects", copy_subjects),
                     copy_item("Messages", copy_messages)};
    if (sel.size() == 1 && log_.refs_of(sel.front()))
        copy.children.push_back(copy_item("Tag/branch names", copy_ref_names));
    m.push_back(std::move(copy));
    m.push_back(item("Search _log messages...", [this] { search_->focus(); }));
    if (sel.size() == 1)
        if (const commit* c = log_.commit(sel.front()))
            m.push_back(item("Show branches this commit is _on",
                             [this, c] { show_branches_containing(*c); }));
    return m;
}

// Clipboard formats [GLLB:2378-2492].
void log_window::copy_to_clipboard(const std::vector<size_t>& rows, int format)
{
    std::string out;
    for (size_t row : rows)
    {
        const commit* c = log_.commit(row);
        if (!c) continue;
        switch (format)
        {
        case copy_full:
        case copy_full_no_paths:
        {
            out += "Revision: " + c->id.hex() + "\n";
            out += "Author: " + c->author + " <" + c->email + ">\n";
            out += "Date: " + format_time(c->time, c->offset_minutes) + "\n";
            out += "Message:\n" + c->message;
            if (!out.ends_with('\n')) out += '\n';
            out += "----\n";
            if (format == copy_full)
                if (auto files = repo_.changes(c->id))
                    for (auto& f : *files)
                    {
                        out += file_model::status_name(f.status) + ": " + f.path;
                        if (!f.old_path.empty()) out += " (from " + f.old_path + ")";
                        out += "\n";
                    }
            out += "\n";
            break;
        }
        case copy_hash: out += c->id.hex() + "\n"; break;
        case copy_authors: out += c->author + " <" + c->email + ">\n"; break;
        case copy_author_names: out += c->author + "\n"; break;
        case copy_author_emails: out += c->email + "\n"; break;
        case copy_subjects: out += "* " + c->summary + "\n\n"; break;
        case copy_messages: out += "* " + c->message + "\n\n"; break;
        case copy_ref_names:
            if (auto* refs = log_.refs_of(row))
                for (auto& r : *refs) out += r.name + "\n";
            break;
        }
    }
    ui_.set_clipboard(out);
}

void log_window::show_branches_containing(const commit& c)
{
    git_result r = run_git(repo_.workdir(), {"branch", "-a", "--contains", c.id.hex()},
                           git_output::stdout_only);
    ui_.message(window_.get(), {.icon = rap::message_spec::icon::info,
                                .title = "Branches",
                                .text = "Branches containing " + c.id.hex(8) + ":",
                                .detail = r.output});
}

// TortoiseGit's Create Branch dialog, reduced to the common options.
// `name`, `switch_to` and `start_point` prefill it (a remote branch
// double-clicked: its local name, switch ticked, the remote branch as the
// start point so the new branch tracks it).
void log_window::create_branch(const commit& c, const std::string& name, bool switch_to,
                               const std::string& start_point)
{
    keep_dialog(create_ref_dialog::open(ui_, repo_.workdir(), false,
                                        start_point.empty() ? remote_or_hash(c) : start_point, name,
                                        switch_to));
}

// TortoiseGit's Stash Save: a message, and whether untracked files go too
// (off, as git's own default: they may be build output or downloads, and
// a stash of thousands of files is slow to show and to apply).
void log_window::stash_changes()
{
    tortilla::stash_changes(ui_, window_.get(), repo_.workdir(), [this](bool) { reload(); });
}

void log_window::create_tag(const commit& c)
{
    keep_dialog(create_ref_dialog::open(ui_, repo_.workdir(), true, remote_or_hash(c)));
}

// The base the log's Create Branch, Create Tag and Switch/Checkout to this
// start from: the row's first remote branch, else the commit [spec 8.6].
std::string log_window::remote_or_hash(const commit& c)
{
    if (auto row = log_.row_of(c.id))
        if (const auto* refs = log_.refs_of(*row))
            for (const auto& r : *refs)
                if (r.kind == ref_kind::remote_branch) return r.full_name;
    return c.id.hex();
}

// A shared dialog opened from the log: the log reloads after it acts, if
// it's still open.
template <class Dialog>
void log_window::keep_dialog(std::expected<std::unique_ptr<Dialog>, std::string> d)
{
    if (!d)
    {
        ui_.message(
            window_.get(),
            {.icon = rap::message_spec::icon::error, .title = "Tortilla", .text = d.error()});
        return;
    }
    auto reload_if_open = [this, alive = std::weak_ptr<bool>(alive_)]
    {
        if (alive.lock()) reload();
    };
    // After the dialog acts, or (one that doesn't say) when it closes.
    if constexpr (requires { (*d)->on_done(reload_if_open); })
    {
        (*d)->on_done(reload_if_open);
        windows::keep(std::move(*d));
    }
    else
        windows::keep(std::move(*d), reload_if_open);
}

void log_window::move_ref(const ref& ref, const commit& target)
{
    if (ref.current) return reset_to(target);
    auto from = repo_.resolve(ref.full_name + "^{commit}");
    if (!from || *from == target.id) return;
    const std::string to = target.id.hex();
    auto describe = [&](const oid& id)
    {
        const commit* c = nullptr;
        if (auto row = log_.row_of(id)) c = log_.commit(*row);
        return id.hex(8) + (c ? " \u201c" + c->summary + "\u201d" : "");
    };
    const bool tag = ref.kind == ref_kind::tag;
    std::string text = std::string(tag ? "Move tag " : "Move branch ") + ref.name + "\nfrom " +
                       describe(*from) + "\nto " + describe(target.id) + "?";
    std::string detail;
    if (tag)
        detail = "Anyone who already fetched the tag keeps the old one.";
    else if (repo_.is_ancestor(*from, target.id))
        detail = "A fast-forward: " + ref.name + " gains commits and loses none.";
    else
    {
        git_result lost = run_git(repo_.workdir(), {"rev-list", "--count", to + ".." + from->hex()},
                                  git_output::stdout_only);
        git_result some = run_git(
            repo_.workdir(), {"log", "--format=%h %s", "--max-count=6", to + ".." + from->hex()},
            git_output::stdout_only);
        detail = ref.name + " will no longer contain " +
                 (lost.ok() ? lost.output.substr(0, lost.output.find('\n')) : "some") +
                 " commit(s):\n" + some.output +
                 "(They stay reachable from the reflog for a while.)";
    }

    arg_list args;
    if (!tag)
        args = {"branch", "--force", ref.name, to};
    else if (ref.annotated)
    {
        // Keeping it annotated: same message on the new commit.
        git_result msg = run_git(repo_.workdir(), {"tag", "-l", "--format=%(contents)", ref.name},
                                 git_output::stdout_only);
        args = {"tag", "--force", "-a", ref.name, to, "-m", msg.ok() ? msg.output : ref.name};
    }
    else
        args = {"tag", "--force", ref.name, to};

    const std::string title = tag ? "Move Tag" : "Move Branch";
    ui_.message(window_.get(),
                {.icon = rap::message_spec::icon::question,
                 .title = title,
                 .text = text,
                 .detail = detail,
                 .buttons = {rap::dialog_role::ok, rap::dialog_role::cancel}},
                [this, title, args](rap::dialog_role r)
                {
                    if (r == rap::dialog_role::ok) run_ref_commands(title, {args});
                });
}

// A right-click on a label makes the menu about that label (checkout,
// delete) rather than offering every label on the commit.
menu log_window::cell_menu(size_t row, size_t column, rap::point_f at)
{
    if (column == log_model::message)
        if (auto ref = log_.label_at(row, at.x)) clicked_ref_ = ref->full_name;
    menu m = context_menu(row);
    clicked_ref_.reset();
    return m;
}

void log_window::row_activated(size_t row)
{
    // A double-click on a label (its press was just now, on this row).
    auto press = std::exchange(last_label_press_, std::nullopt);
    if (!press || press->row != row ||
        std::chrono::steady_clock::now() - press->at > std::chrono::seconds(1))
        return;
    const std::vector<ref>* refs = log_.refs_of(row);
    const commit* c = log_.commit(row);
    if (!refs || !c) return;
    for (const ref& r : *refs)
    {
        if (r.full_name != press->full_name) continue;
        if (r.kind == ref_kind::local_branch && !r.current) switch_to(r.name, false);
        if (r.kind == ref_kind::tag)
        {
            // A tag isn't a branch: checking it out detaches HEAD, so ask.
            const std::string t = "\u201c" + r.name + "\u201d";
            choose(
                "Switch to " + r.name, "What would you like to do with the tag " + t + "?",
                {{"_Check out " + menu_text(r.name) + " (detached)",
                  "To look at or build that version; new commits there aren't on any branch.",
                  [this, name = r.name] { switch_to(name, true); }},
                 {"Create a _branch there and switch to it...",
                  "For working from that version: the new branch starts at the tag.",
                  [this, commit = *c, name = r.name] { create_branch(commit, "", true, name); }}});
        }
        if (r.kind == ref_kind::remote_branch)
        {
            // origin/feature -> a local "feature" tracking it, switched to;
            // with a local "feature" already there, that's switched to (as
            // git switch feature does).
            const size_t slash = r.name.find('/');
            const std::string local =
                slash == std::string::npos ? r.name : r.name.substr(slash + 1);
            if (repo_.resolve("refs/heads/" + local))
                switch_to(local, false);
            else
                create_branch(*c, local, true, r.name);
        }
        return;
    }
}

// A dialog of choices, each a button with a line saying what it does:
// choosing one closes the dialog, then acts.
void log_window::choose(const std::string& title, const std::string& question,
                        std::vector<choice> choices)
{
    struct state
    {
        std::unique_ptr<rap::window> window;
    };
    auto st = std::make_shared<state>();
    std::weak_ptr<state> weak = st;
    using namespace rap::layout;
    std::vector<rap::node> rows{label{question}};
    for (choice& c : choices)
    {
        rows.push_back(button{c.label}.on_click(
            [this, weak, act = std::move(c.act)]
            {
                auto s = weak.lock();
                if (!s) return;
                s->window->close();
                ui_.post([s] {});  // destroyed after this click returns
                act();
            }));
        rows.push_back(label{"   " + c.description});
    }
    rows.push_back(buttons{cancel});
    // A box from a list built at run time (VBox{...} takes a fixed list).
    rap::node box{rap::spec::box{rap::orientation::vertical, std::move(rows)}, {}};
    st->window = ui_.create(dialog{title, {std::move(box)}}.resizable(false), window_.get());
    st->window->on_finished(
        [this, st](rap::dialog_role) mutable
        {
            auto keep = std::move(st);
            ui_.post([keep] {});
        });
    st->window->show();
}

// Switching to a branch, or to a tag or commit (`detach`). A clean working
// tree just switches; uncommitted changes (tracked files: untracked ones
// stay put either way) ask what to do with them.
void log_window::switch_to(const std::string& target, bool detach)
{
    git_result dirty = run_git(repo_.workdir(), {"status", "--porcelain", "--untracked-files=no"},
                               git_output::stdout_only);
    if (dirty.ok() && dirty.output.empty()) return run_switch(target, detach, switch_mode::plain);
    const std::string t = "\u201c" + target + "\u201d";
    choose(
        "Switch to " + target,
        "You have uncommitted changes. What should happen to them when switching to " + t + "?",
        {{"_Stash them, then switch", "The stash stays with this commit, ready when you come back.",
          [this, target, detach] { run_switch(target, detach, switch_mode::stash); }},
         {"_Bring them along",
          "Merged into " + t +
              " where it differs; conflicts, if any, are left marked in the files.",
          [this, target, detach] { run_switch(target, detach, switch_mode::bring_along); }},
         {"_Discard them and switch", "Your uncommitted changes are lost. This can't be undone.",
          [this, target, detach] { run_switch(target, detach, switch_mode::discard); }}});
}

// Runs a switch, then reloads and offers back a stash made at the new
// commit. Bringing changes along tries a plain checkout first (git carries
// changes over when both sides agree on those files) and only falls back to
// --merge when git refuses.
void log_window::run_switch(const std::string& target, bool detach, switch_mode mode)
{
    auto checkout = [&](const char* flag)
    {
        arg_list a{"checkout"};
        if (flag) a.push_back(flag);
        if (detach) a.push_back("--detach");
        a.push_back(target);
        return a;
    };
    std::vector<arg_list> commands;
    if (mode == switch_mode::stash)
        commands.push_back({"stash", "push", "-m", "Tortilla: before switching to " + target});
    commands.push_back(checkout(mode == switch_mode::discard ? "--force" : nullptr));
    const bool bring_along = mode == switch_mode::bring_along;
    const arg_list merge = checkout("--merge");
    run_quietly(
        "Switch/Checkout", std::move(commands),
        [this, bring_along, merge](bool ok, const std::string& output)
        {
            if (!ok && bring_along && output.find("would be overwritten") != std::string::npos)
                return run_quietly("Switch/Checkout", {merge},
                                   [this](bool ok2, const std::string&)
                                   {
                                       reload();
                                       if (ok2) offer_stash_pop();
                                   });
            if (!ok && bring_along)
                ui_.message(window_.get(), {.icon = rap::message_spec::icon::error,
                                            .title = "Switch/Checkout",
                                            .text = "Switch/Checkout failed.",
                                            .detail = output});
            reload();
            if (ok) offer_stash_pop();
        },
        !bring_along);
}

// The newest stash made on the commit now checked out, if any, offered back.
void log_window::offer_stash_pop()
{
    auto head = repo_.resolve("HEAD");
    if (!head) return;
    git_result list = run_git(repo_.workdir(), {"stash", "list", "--format=%gd%x09%P%x09%gs"},
                              git_output::stdout_only);
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
        git_result dirty =
            run_git(repo_.workdir(), {"status", "--porcelain", "--untracked-files=no"},
                    git_output::stdout_only);
        const bool clean = dirty.ok() && dirty.output.empty();
        ui_.message(
            window_.get(),
            {.icon = rap::message_spec::icon::question,
             .title = "Stash",
             .text = "A stash was made here:\n\u201c" + subject + "\u201d\n\nApply it now?",
             .detail = clean ? ""
                             : "You have uncommitted changes. If the stash changes the same files, "
                               "git won't apply it and it stays in the stash list.",
             .buttons = {rap::dialog_role::yes, rap::dialog_role::no}},
            [this, ref](rap::dialog_role r)
            {
                if (r == rap::dialog_role::yes)
                    run_quietly("Stash Pop", {{"stash", "pop", ref}},
                                [this](bool, const std::string&) { reload(); });
            });
        return;  // the newest only
    }
}

rap::list::cell_drag log_window::label_drag()
{
    return {
        .begin = [this](size_t row, size_t column,
                        rap::point_f at) -> std::optional<rap::list::cell_drag_start>
        {
            last_label_press_.reset();
            if (column != log_model::message) return std::nullopt;
            auto ref = log_.label_at(row, at.x);
            if (ref)
                last_label_press_ =
                    label_press{row, ref->full_name, std::chrono::steady_clock::now()};
            // Remote branches would need a force push: not by dragging.
            if (!ref || (ref->kind != ref_kind::local_branch && ref->kind != ref_kind::tag))
                return std::nullopt;
            return rap::list::cell_drag_start{ref->name, ref->full_name};
        },
        .can_drop = [this](size_t from, size_t to, const std::string&)
        { return to != from && log_.commit(to) != nullptr; },
        .drop =
            [this](size_t from, size_t to, const std::string& full_name)
        {
            const std::vector<ref>* refs = log_.refs_of(from);
            const commit* target = log_.commit(to);
            if (!refs || !target) return;
            for (const ref& r : *refs)
                if (r.full_name == full_name) return move_ref(r, *target);
        },
    };
}

// TortoiseGit's Reset dialog: soft / mixed / hard.
void log_window::reset_to(const commit& c)
{
    struct state
    {
        std::unique_ptr<rap::window> window;
        rap::drop_down* type = nullptr;
    };
    auto st = std::make_shared<state>();
    std::string hash = c.id.hex();

    using namespace rap::layout;
    run_dialog<state>(
        ui_, window_.get(), st,
        dialog{
            "Reset",
            {vbox{
                label{"Reset \"" + repo_.head_name() + "\" to " + hash.substr(0, 8) + " " +
                      c.summary},
                form{
                    {"Reset Type",
                     drop_down{{"Soft: Leave working tree and index untouched",
                                "Mixed: Leave working tree untouched, reset index",
                                "Hard: Reset working tree and index (discard all local changes)"},
                               1}
                         .bind(st->type)},
                },
                buttons{ok, cancel},
            }}}
            .resizable(false),
        [this, hash](state& s)
        {
            static const char* modes[] = {"--soft", "--mixed", "--hard"};
            int i = std::clamp(s.type->current(), 0, 2);
            run_git_commands("Reset", {{"reset", modes[i], hash}});
        });
}

// TortoiseGit's Merge dialog, reduced to its main options.
void log_window::merge_from(const commit&, const std::string& what)
{
    keep_dialog(merge_dialog::open(ui_, repo_.workdir(), what));
}

void log_window::open_rebase(const std::string& upstream)
{
    auto w = rebase_window::open(ui_, repo_.workdir(), upstream);
    if (!w)
    {
        ui_.message(window_.get(),
                    {.icon = rap::message_spec::icon::error, .title = "Rebase", .text = w.error()});
        return;
    }
    rebase_window* raw = w->get();
    raw->on_closed(
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
