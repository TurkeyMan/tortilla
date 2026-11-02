#include "svn_dialogs.h"

#include "git/command.h"
#include "git/repo.h"
#include "git/svn.h"
#include "i18n.h"
#include "rebase/rebase_window.h"
#include "ui/progress_dialog.h"
#include "ui/repo_icon.h"
#include "ui/windows.h"

#include <rapport/sys.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <thread>
#include <utility>

namespace tortilla
{
namespace
{

namespace fs = std::filesystem;

std::string shown_path(const std::string& workdir)
{
    std::string root = workdir;
    while (root.size() > 1 && (root.back() == '/' || root.back() == '\\')) root.pop_back();
    return rap::sys::path_to_utf8(rap::sys::path_from_utf8(root).make_preferred());
}

std::string title_for(const std::string& what, const std::string& workdir)
{
    return rap::i18n::format(_("{0} - {1}"), shown_path(workdir), what);
}

// What's asked before a command starts, on a worker: git svn may not be
// there, and git status can take a while.
struct checks
{
    bool installed = false;
    std::string changes;   // git status of the tracked files: "" when clean
    std::string rmdir;     // svn.rmdir, "" if unset (DCommit)
    std::string upstream;  // the branch git svn fetches into (Rebase)
};

enum ask : unsigned
{
    ASK_CHANGES = 1u << 0,
    ASK_RMDIR = 1u << 1,
    ASK_UPSTREAM = 1u << 2,
};

void check_first(rap::ui& ui, const std::string& workdir, unsigned asked,
                 std::function<void(checks)> then)
{
    std::thread(
        [&ui, workdir, asked, then = std::move(then)]() mutable
        {
            checks c;
            c.installed = git_svn_installed(workdir);
            if (c.installed && (asked & ASK_CHANGES))
                c.changes = run_git(workdir, {"status", "--porcelain", "--untracked-files=no"},
                                    git_output::stdout_only)
                                .output;
            if (c.installed && (asked & ASK_RMDIR))
                if (git_result r =
                        run_git(workdir, {"config", "--get", "svn.rmdir"}, git_output::stdout_only);
                    r.ok())
                    c.rmdir = r.output;
            if (c.installed && (asked & ASK_UPSTREAM))
                if (git_result r = run_git(workdir, {"config", "--get", "svn-remote.svn.fetch"},
                                           git_output::stdout_only);
                    r.ok())
                    c.upstream = svn_upstream(r.output);
            ui.post([then = std::move(then), c = std::move(c)]() mutable { then(std::move(c)); });
        })
        .detach();
}

void fail(rap::ui& ui, const parent_window& parent, const std::string& title,
          const std::string& text, const std::string& detail, const std::function<void(bool)>& done)
{
    ui.message(
        parent(),
        {.icon = rap::message_spec::icon::error, .title = title, .text = text, .detail = detail},
        [done](rap::dialog_role)
        {
            if (done) done(false);
        });
}

void not_installed(rap::ui& ui, const parent_window& parent, const std::string& title,
                   const std::function<void(bool)>& done)
{
    fail(ui, parent, title, _("git svn isn't installed."),
         _("Your git doesn't have git svn, or the Perl modules it needs. Install git's svn "
           "support (on Linux, often a package of its own: git-svn), then try again."),
         done);
}

// Local changes in the way: stashed first, as TortoiseGit offers; no means
// it doesn't start. `then` gets whether they were stashed. Stashed before
// the progress dialog, so a failure after it knows there's a stash to pop.
void offer_stash(rap::ui& ui, const parent_window& parent, const std::string& workdir,
                 const std::string& title, const std::string& changes,
                 std::function<void(bool stashed)> then, const std::function<void(bool)>& done)
{
    if (changes.empty()) return then(false);
    ui.message(parent(),
               {.icon = rap::message_spec::icon::question,
                .title = title,
                .text = _("The working tree has local changes. Stash them first?"),
                .detail = changes,
                .buttons = {rap::dialog_role::yes, rap::dialog_role::cancel}},
               [&ui, parent, workdir, title, then = std::move(then), done](rap::dialog_role r)
               {
                   if (r != rap::dialog_role::yes)
                   {
                       if (done) done(false);
                       return;
                   }
                   std::thread(
                       [&ui, parent, workdir, title, then, done]
                       {
                           const git_result stashed = run_git(workdir, {"stash", "push"});
                           ui.post(
                               [&ui, parent, title, then, done, stashed]
                               {
                                   if (!stashed.ok())
                                       return fail(ui, parent, title,
                                                   _("The local changes couldn't be stashed."),
                                                   stashed.output, done);
                                   then(true);
                               });
                       })
                       .detach();
               });
}

// After it ran: the changes stashed first back, if the user wants them.
void offer_pop(rap::ui& ui, const parent_window& parent, const std::string& title,
               const std::string& workdir, bool stashed, bool ok, std::function<void(bool)> done)
{
    if (!stashed)
    {
        if (done) done(ok);
        return;
    }
    ui.message(parent(),
               {.icon = rap::message_spec::icon::question,
                .title = title,
                .text = _("Your changes were stashed first. Pop the stash now?"),
                .buttons = {rap::dialog_role::yes, rap::dialog_role::no}},
               [&ui, parent, title, workdir, ok, done = std::move(done)](rap::dialog_role r)
               {
                   if (r != rap::dialog_role::yes)
                   {
                       if (done) done(ok);
                       return;
                   }
                   run_progress_dialog(ui, parent(), title, workdir, {{"stash", "pop"}},
                                       [ok, done](bool popped)
                                       {
                                           if (done) done(ok && popped);
                                       });
               });
}

void run_dcommit(rap::ui& ui, const parent_window& parent, const std::string& workdir,
                 const std::string& title, bool rmdir, bool remember, const std::string& changes,
                 std::function<void(bool)> done)
{
    offer_stash(
        ui, parent, workdir, title, changes,
        [&ui, parent, workdir, title, rmdir, remember, done](bool stashed)
        {
            std::vector<git_command> commands;
            if (remember) commands.push_back({"config", "svn.rmdir", rmdir ? "true" : "false"});
            commands.push_back(svn_dcommit_args(rmdir));
            run_progress_dialog(ui, parent(), title, workdir, std::move(commands),
                                [&ui, parent, workdir, title, stashed, done](bool ok)
                                { offer_pop(ui, parent, title, workdir, stashed, ok, done); });
        },
        done);
}

}  // namespace

void svn_dcommit(rap::ui& ui, const parent_window& parent, const std::string& workdir,
                 std::function<void(bool ok)> done)
{
    const std::string title = title_for(_("SVN DCommit"), workdir);
    check_first(
        ui, workdir, ASK_CHANGES | ASK_RMDIR,
        [&ui, parent, workdir, title, done](checks c)
        {
            if (!c.installed) return not_installed(ui, parent, title, done);
            // svn.rmdir set: git svn follows it, and there's nothing to ask.
            if (!c.rmdir.empty())
                return run_dcommit(ui, parent, workdir, title, false, false, c.changes, done);

            struct state
            {
                std::unique_ptr<rap::window> window;
                rap::check* rmdir = nullptr;
                rap::check* remember = nullptr;
            };
            auto st = std::make_shared<state>();
            using namespace rap::layout;
            st->window = create_window(
                ui,
                dialog{
                    _("SVN DCommit"),
                    {vbox{
                        label{_("Commit the commits not yet in SVN to the SVN repository.")},
                        check{_("_Remove folders left empty from SVN (--rmdir)")}.bind(st->rmdir),
                        check{_("Re_member this choice (svn.rmdir)")}.bind(st->remember),
                        buttons{ok, cancel},
                    }}}
                    .resizable(false),
                parent());
            st->window->on_finished(
                [st, &ui, parent, workdir, title, done,
                 changes = c.changes](rap::dialog_role role) mutable
                {
                    auto keep = std::move(st);
                    if (role == rap::dialog_role::ok)
                        run_dcommit(ui, parent, workdir, title, keep->rmdir->checked(),
                                    keep->remember->checked(), changes, done);
                    else if (done)
                        done(false);
                    ui.post([keep] {});  // destroy after this callback returns
                });
            st->window->show();
        });
}

void svn_rebase(rap::ui& ui, const parent_window& parent, const std::string& workdir,
                std::function<void(bool ok)> done)
{
    const std::string title = title_for(_("SVN Rebase"), workdir);
    check_first(
        ui, workdir, ASK_CHANGES | ASK_UPSTREAM,
        [&ui, parent, workdir, title, done](checks c)
        {
            if (!c.installed) return not_installed(ui, parent, title, done);
            if (c.upstream.empty())
                return fail(ui, parent, title, _("There's no SVN branch to rebase onto."),
                            _("svn-remote.svn.fetch isn't set in this repository's config."), done);
            offer_stash(
                ui, parent, workdir, title, c.changes,
                [&ui, parent, workdir, title, done, upstream = c.upstream](bool stash)
                {
                    // Fetched: up to date, a fast-forward, or the Rebase
                    // dialog onto the SVN branch, as TortoiseGit's SVN Rebase.
                    auto fetched = [&ui, parent, workdir, title, done, upstream, stash](bool ok)
                    {
                        if (!ok) return offer_pop(ui, parent, title, workdir, stash, false, done);
                        std::thread(
                            [&ui, parent, workdir, title, done, upstream, stash]
                            {
                                const git_result head = run_git(workdir, {"rev-parse", "HEAD"},
                                                                git_output::stdout_only);
                                const git_result up = run_git(
                                    workdir, {"rev-parse", "--verify", upstream + "^{commit}"},
                                    git_output::stdout_only);
                                const bool same = head.ok() && up.ok() && head.output == up.output;
                                const bool forward =
                                    !same && run_git(workdir, {"merge-base", "--is-ancestor",
                                                               "HEAD", upstream})
                                                 .ok();
                                const bool known = up.ok();
                                ui.post(
                                    [&ui, parent, workdir, title, done, upstream, stash, same,
                                     forward, known]
                                    {
                                        auto pop =
                                            [&ui, parent, workdir, title, done, stash](bool ok)
                                        { offer_pop(ui, parent, title, workdir, stash, ok, done); };
                                        if (!known)
                                            return fail(
                                                ui, parent, title,
                                                rap::i18n::format(_("{0} isn't there."), upstream),
                                                {}, pop);
                                        if (same)
                                            return ui.message(
                                                parent(),
                                                {.title = title,
                                                 .text = _("Everything is up to date.")},
                                                [pop](rap::dialog_role) { pop(true); });
                                        if (forward)
                                            return run_progress_dialog(
                                                ui, parent(), title, workdir,
                                                {{"merge", "--ff-only", upstream}}, pop);
                                        auto w = rebase_window::open(ui, workdir, upstream);
                                        if (!w) return fail(ui, parent, title, w.error(), {}, pop);
                                        windows::keep(std::move(*w), [pop] { pop(true); });
                                    });
                            })
                            .detach();
                    };
                    run_progress_dialog(ui, parent(), title, workdir, {svn_fetch_args()},
                                        std::move(fetched));
                },
                done);
        });
}

void svn_fetch(rap::ui& ui, const parent_window& parent, const std::string& workdir,
               std::function<void(bool ok)> done)
{
    const std::string title = title_for(_("SVN Fetch"), workdir);
    check_first(ui, workdir, 0,
                [&ui, parent, workdir, title, done](checks c)
                {
                    if (!c.installed) return not_installed(ui, parent, title, done);
                    run_progress_dialog(ui, parent(), title, workdir, {svn_fetch_args()}, done);
                });
}

void svn_import_ignore(rap::ui& ui, const parent_window& parent, const std::string& workdir,
                       std::function<void(bool ok)> done)
{
    const std::string title = title_for(_("Import SVN Ignore"), workdir);
    check_first(
        ui, workdir, 0,
        [&ui, parent, workdir, title, done](checks c)
        {
            if (!c.installed) return not_installed(ui, parent, title, done);
            struct state
            {
                std::unique_ptr<rap::window> window;
                rap::drop_down* into = nullptr;
            };
            auto st = std::make_shared<state>();
            using namespace rap::layout;
            st->window = create_window(
                ui,
                dialog{_("Import SVN Ignore"),
                       {vbox{
                           form{
                               {_("Import svn:ignore"),
                                drop_down{
                                    {_("Into .git/info/exclude (git svn show-ignore)"),
                                     _("As a .gitignore in each folder (git svn create-ignore)")},
                                    0}
                                    .bind(st->into)},
                           },
                           buttons{ok, cancel},
                       }}}
                    .resizable(false),
                parent());
            st->window->on_finished(
                [st, &ui, parent, workdir, title, done](rap::dialog_role role) mutable
                {
                    auto keep = std::move(st);
                    ui.post([keep] {});  // destroy after this callback returns
                    if (role != rap::dialog_role::ok)
                    {
                        if (done) done(false);
                        return;
                    }
                    if (keep->into->current() == 1)
                        return run_progress_dialog(ui, parent(), title, workdir,
                                                   {svn_ignore_args(svn_ignore_into::gitignore)},
                                                   done);
                    // show-ignore's output goes into the file, so it isn't
                    // the progress dialog's to show.
                    std::thread(
                        [&ui, parent, workdir, title, done]
                        {
                            std::string error, detail;
                            const git_result r =
                                run_git(workdir, svn_ignore_args(svn_ignore_into::exclude),
                                        git_output::stdout_only);
                            auto rp = r.ok() ? repo::open(workdir)
                                             : std::expected<repo, std::string>(
                                                   std::unexpected(std::string()));
                            if (!r.ok())
                            {
                                error = _("git svn show-ignore failed.");
                                detail = run_git(workdir, svn_ignore_args(svn_ignore_into::exclude))
                                             .output;
                            }
                            else if (!rp)
                                error = rp.error();
                            else
                            {
                                const fs::path file =
                                    rap::sys::path_from_utf8(rp->common_dir()) / "info" / "exclude";
                                std::string existing;
                                {
                                    std::ifstream in(file, std::ios::binary);
                                    existing.assign(std::istreambuf_iterator<char>(in),
                                                    std::istreambuf_iterator<char>());
                                }
                                std::error_code ec;
                                fs::create_directories(file.parent_path(), ec);
                                std::ofstream out(file, std::ios::binary | std::ios::trunc);
                                out << merge_exclude(existing, r.output);
                                if (!out.flush())
                                    error = rap::i18n::format(_("Couldn't write {0}."),
                                                              rap::sys::path_to_utf8(file));
                            }
                            ui.post(
                                [&ui, parent, title, done, error, detail]
                                {
                                    if (!error.empty())
                                        return fail(ui, parent, title, error, detail, done);
                                    ui.message(parent(),
                                               {.title = title,
                                                .text = _("svn:ignore is imported into "
                                                          ".git/info/exclude.")},
                                               [done](rap::dialog_role)
                                               {
                                                   if (done) done(true);
                                               });
                                });
                        })
                        .detach();
                });
            st->window->show();
        });
}

}  // namespace tortilla
