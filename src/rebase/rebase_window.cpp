#include "rebase_window.h"

#include "branch/browse_refs.h"
#include "ui/menu_text.h"

#include "changes/working_tree.h"
#include "git/command.h"
#include "i18n.h"
#include "log/log_window.h"
#include "ui/progress_dialog.h"
#include "ui/repo_icon.h"
#include "ui/settings_dialog.h"
#include "ui/welcome_window.h"
#include "ui/windows.h"

#include <rapport/sys.h>

#include <algorithm>
#include <numeric>

namespace tortilla
{
namespace
{

// Tab pages, in order. Before the start: Revision Files and Commit Message;
// after: Conflict Files, Commit Message and Log; when done: only Log
// [spec 2.2]. rapport can't rename or add tabs, so all four exist and the
// right ones are shown.
enum page
{
    page_revision,
    page_conflict,
    page_message,
    page_log,
};

rap::menu_item item(std::string label, std::function<void()> action, bool enabled = true)
{
    return {.label = std::move(label), .action = std::move(action), .enabled = enabled};
}

}  // namespace

std::expected<std::unique_ptr<rebase_window>, std::string>
rebase_window::open(rap::ui& ui, const std::string& path, const std::string& upstream)
{
    auto repo = repo::open(path);
    if (!repo) return std::unexpected(repo.error());
    if (repo->bare()) return std::unexpected(std::string(_("cannot rebase in a bare repository")));
    std::unique_ptr<rebase_window> w(new rebase_window(ui, std::move(*repo)));
    w->build(upstream);
    return w;
}

rebase_window::rebase_window(rap::ui& ui, repo repo)
    : ui_(ui)
    , repo_(std::move(repo))
    , revision_files_(files_host())
    , conflict_files_(files_host())
    , files_worker_(std::make_unique<repo_worker>(ui, repo_.workdir()))
    , list_worker_(std::make_unique<repo_worker>(ui, repo_.workdir()))
{
}

// The shared file lists' surroundings: their git commands (Resolved, using
// theirs/mine...) run in the progress dialog, then the conflicts reload.
changes_view::host rebase_window::files_host()
{
    return {.ui = ui_,
            .window = [this] { return window_.get(); },
            .workdir = repo_.workdir(),
            .run =
                [this](const std::string& title, std::vector<std::vector<std::string>> commands)
            {
                run_progress_dialog(ui_, window_.get(), title, repo_.workdir(), std::move(commands),
                                    [this](bool) { refresh_conflicts(); });
            },
            .refresh = [this] { refresh_conflicts(); },
            .open_settings = [this](settings_page page)
            { show_settings(ui_, window_.get(), repo_.workdir(), page); },
            .selection_changed = {}};
}

void rebase_window::build(const std::string& upstream)
{
    using namespace rap::layout;

    // Branch: local branches, the current one preselected. Upstream: every
    // branch and tag, preselecting the given one or the branch's upstream.
    branches_ = repo_.branch_names(true, false, false);
    upstreams_ = repo_.branch_names(true, true, true);
    std::string current = repo_.head_name();
    std::string wanted = upstream.empty() ? repo_.upstream_of(current) : upstream;
    if (!wanted.empty() && !std::ranges::contains(upstreams_, wanted))
        upstreams_.insert(upstreams_.begin(), wanted);
    auto index_of = [](const std::vector<std::string>& v, const std::string& s)
    {
        auto it = std::ranges::find(v, s);
        return it == v.end() ? 0 : int(it - v.begin());
    };

    std::string title = repo_.workdir();
    if (title.size() > 1 && title.back() == '/') title.pop_back();
    title = rap::sys::path_to_utf8(rap::sys::path_from_utf8(title).make_preferred());

    auto swap_icon = rap::icon::drawn(
        [](rap::painter& p, rap::size_f s)
        {
            rap::paint c{rap::theme_color::window_text};
            float w = s.w, h = s.h, t = std::max(1.f, h * 0.1f);
            p.line({w * 0.15f, h * 0.35f}, {w * 0.85f, h * 0.35f}, c, t);
            p.polygon({{w * 0.85f, h * 0.18f}, {w, h * 0.35f}, {w * 0.85f, h * 0.52f}}, c,
                      std::nullopt);
            p.line({w * 0.85f, h * 0.65f}, {w * 0.15f, h * 0.65f}, c, t);
            p.polygon({{w * 0.15f, h * 0.48f}, {0, h * 0.65f}, {w * 0.15f, h * 0.82f}}, c,
                      std::nullopt);
        });

    window_ = create_window(
        ui_,
        window{
            rap::i18n::format(_("{0} - Rebase - Tortilla"), title),
            {vbox{
                // Row 1 [spec 2.2]
                hbox{
                    label{_("Branch:")},
                    drop_down{branches_, index_of(branches_, current)}
                        .bind(branch_)
                        .on_changed([this](int) { reload_list(); })
                        .width(30_ch),
                    button{_("Swap branch and upstream")}
                        .icon(swap_icon)
                        .icon_only()
                        .bind(swap_)
                        .on_click(
                            [this]
                            {
                                // Swap only when both names exist on both sides.
                                std::string b = branches_[size_t(std::max(0, branch_->current()))];
                                std::string u =
                                    upstreams_[size_t(std::max(0, upstream_->current()))];
                                auto bi = std::ranges::find(branches_, u);
                                if (bi == branches_.end()) return;
                                branch_->set_current(int(bi - branches_.begin()));
                                auto ui = std::ranges::find(upstreams_, b);
                                if (ui != upstreams_.end())
                                    upstream_->set_current(int(ui - upstreams_.begin()));
                                reload_list();
                            }),
                    label{_("Upstream:")},
                    drop_down{upstreams_, index_of(upstreams_, wanted)}
                        .bind(upstream_)
                        .on_changed([this](int) { reload_list(); })
                        .width(30_ch),
                    // Browse References for the upstream, and for --onto [spec 2.8].
                    button{"..."}
                        .tooltip(_("Browse References"))
                        .on_click([this] { pick_upstream(); }),
                    button{_("_Onto")}.bind(onto_button_).on_click([this] { toggle_onto(); }),
                    spacer{},
                },
                splitter{
                    vertical,
                    list{rebase_model::columns(), &model_}
                        .soft_selection()
                        .bind(list_)
                        .on_selection_changed([this] { selection_changed(); })
                        .on_context_menu([this](std::optional<size_t> row)
                                         { return context_menu(row); })
                        .on_reorder([this](const std::vector<size_t>& rows, size_t dest)
                                    { return drop_rows(rows, dest); })
                        // Commits dragged in from a log: picks where they land.
                        .on_drop_in(
                            {.types = {commits_type},
                             .can_drop = [this](size_t, const std::string&) { return can_add(); },
                             .drop = [this](size_t at, const std::string&, const std::string& data)
                             { return add_commits_at(at, parse_commits_payload(data)); }})
                        .stretch(3),
                    vbox{
                        // Row under the list [spec 2.2]
                        hbox{
                            menu_button{_("_Pick ALL")}.bind(set_all_).on_menu(
                                [this] { return set_all_menu(); }),
                            button{_("_Up")}.bind(up_).on_click([this] { move_rows(true, false); }),
                            button{_("_Down")}.bind(down_).on_click([this]
                                                                    { move_rows(false, false); }),
                            button{_("_Add")}.bind(add_).on_click([this] { add_commits(); }),
                            check{_("_Preserve merges")}
                                .bind(preserve_merges_)
                                .on_toggled([this](bool) { reload_list(); })
                                .tooltip(_("Include merge commits. Reordering commits with merges "
                                           "does not work properly.")),
                            check{_("_Force Rebase")}
                                .bind(force_)
                                .on_toggled([this](bool) { reload_list(); })
                                .tooltip(_("Force the rebase even if the current branch is a "
                                           "descendant of the commit you are rebasing onto.")),
                        },
                        tabs{
                            {_("Revision Files"),
                             vbox{
                                 revision_files_.list("rebase.revision").soft_selection().stretch()}
                                 .handle(revision_page_)},
                            {_("Conflict Files"), vbox{conflict_files_.list("rebase.conflicts")
                                                           .soft_selection()
                                                           .stretch()}
                                                      .handle(conflict_page_)},
                            {_("Commit Message"),
                             vbox{text_view{}.read_only().bind(message_).stretch()}.handle(
                                 message_page_)},
                            {_("Log"), vbox{text_view{}
                                                .monospace()
                                                .read_only()
                                                .wrap(false)
                                                .bind(log_)
                                                .stretch()}
                                           .handle(log_page_)},
                        }
                            .bind(tabs_)
                            .stretch(),
                    }
                        .stretch(2),
                }
                    .stretch(),
                // Bottom [spec 2.2]
                progress_bar{}.bind(progress_),
                hbox{
                    label{}.ellipsize().bind(status_).stretch(),
                    menu_button{_("Show log")}.bind(post_).on_menu(
                        [this]() -> rap::menu
                        {
                            return {item(_("Show _log"),
                                         [this]
                                         {
                                             if (show_log_) show_log_();
                                             close();
                                         })};
                        }),
                    button{_("Start Rebase")}.bind(continue_).on_click([this]
                                                                       { continue_pressed(); }),
                    button{_("Abort")}
                        .bind(abort_)
                        .tooltip(_("Recover to the status before rebase"))
                        .on_click([this] { abort_pressed(); }),
                    button{_("Help")}.on_click([] { open_help(); }),
                },
            }}}
            .size(120_ch, 40_lines));

    window_->on_close_request(
        [this]
        {
            if (closing_) return true;
            abort_pressed();
            return false;
        });
    window_->on_key([this](const rap::key_event& e) { return key(e); });

    rebase_engine::hooks hooks{
        .log = [this](const std::string& t, bool cmd) { append_log(t, cmd); },
        .stage_changed = [this] { update_controls(); },
        .progress =
            [this](size_t applied, size_t total)
        {
            progress_->set_fraction(total ? float(applied) / float(total) : 0);
            set_status(rap::i18n::format(_("Rebasing... ({0}/{1})"), applied, total));
            if (auto c = list_->current(); !c || *c != total - applied)
                list_->scroll_to(total - applied);
        },
        .message = [this](const std::string& m) { message_->set_text(m); },
        .warn =
            [this](const std::string& m)
        {
            ui_.message(window_.get(),
                        {.icon = rap::message_spec::icon::warning, .title = _("Rebase"), .text = m});
        },
        .ask_skip =
            [this](const std::string& output, std::function<void(bool)> answer)
        {
            ui_.message(window_.get(),
                        {.icon = rap::message_spec::icon::question,
                         .title = _("Rebase"),
                         .text = _("Cherry-pick failed. Skip this commit?"),
                         .detail = output,
                         .buttons = {rap::dialog_role::yes, rap::dialog_role::no}},
                        [answer](rap::dialog_role r) { answer(r == rap::dialog_role::yes); });
        },
    };
    engine_ = std::make_unique<rebase_engine>(ui_, repo_.workdir(), repo_.git_dir(), model_, hooks);

    post_->set_visible(false);
    conflict_page_->set_visible(false);
    log_page_->set_visible(false);
    reload_list();
}

void rebase_window::set_status(const std::string& text, bool warning)
{
    // TODO: TortoiseGit shows warnings as white text on red; rapport labels
    // have no colours yet.
    status_->set_text(warning ? "⚠ " + text : text);
}

// Recomputes the commits to rebase from Branch and Upstream [spec 2.3].
void rebase_window::reload_list()
{
    if (engine_ && engine_->started()) return;
    fast_forward_ = can_start_ = false;
    model_.set({}, {});
    revision_files_.clear();
    message_->set_text("");

    std::string b = branches_.empty() ? "" : branches_[size_t(std::max(0, branch_->current()))];
    std::string u = upstreams_.empty() ? "" : upstreams_[size_t(std::max(0, upstream_->current()))];
    branch_head_ = b.empty() ? std::nullopt : repo_.resolve(b);
    upstream_head_ = u.empty() ? std::nullopt : repo_.resolve(u);

    if (!branch_head_)
        set_status(_("Please select branch"));
    else if (!upstream_head_)
        set_status(_("Please select upstream"));
    else if (*branch_head_ == *upstream_head_)
        set_status(rap::i18n::format(_("No need to rebase: \"{0}\" equals \"{1}\""), b, u));
    else if (onto_.empty() && repo_.is_ancestor(*branch_head_, *upstream_head_))
    {
        set_status(rap::i18n::format(
            _("Branch \"{0}\" is behind \"{1}\": \"{0}\" will fast-forward to \"{1}\""), b, u));
        fast_forward_ = can_start_ = true;
    }
    else if (onto_.empty() && repo_.is_ancestor(*upstream_head_, *branch_head_) && !force_->checked())
        set_status(rap::i18n::format(
            _("Current branch \"{0}\" is up to date. To rebase anyway, check \"Force Rebase\" below."),
            b));
    else
    {
        auto commits = repo_.range(*upstream_head_, *branch_head_, preserve_merges_->checked());
        if (!commits || commits->empty())
            set_status(commits ? std::string(_("Nothing to Rebase")) : commits.error());
        else
        {
            std::vector<rebase_row> rows;
            for (auto& c : *commits) rows.push_back({.commit = c});
            model_.set(std::move(rows), {});
            can_start_ = true;
            count_target_ = onto_.empty() ? u : onto_;
            show_count();
            list_->set_current(0);
            // Then, off the UI thread: the labels, and the commits already
            // upstream (git cherry "-"), which start as Skip. git cherry reads
            // every commit's patch: seconds over \\wsl.localhost. A row set
            // otherwise meanwhile keeps that.
            const bool merges = preserve_merges_->checked();
            list_worker_->submit(
                [this, u, b, merges](repo& r, const repo_worker::progress&) -> std::function<void()>
                {
                    ref_map refs = r.refs();
                    const std::string cherry =
                        merges ? std::string()
                               : run_git(r.workdir(), {"cherry", u, b}, git_output::stdout_only)
                                     .output;
                    return [this, refs = std::move(refs), cherry]() mutable
                    {
                        model_.set_refs(std::move(refs));
                        bool skipped = false;
                        for (size_t i = 0; i < model_.size(); ++i)
                            if (model_.rows()[i].action == rebase_action::pick &&
                                cherry.find("- " + model_.rows()[i].commit.id.hex()) !=
                                    std::string::npos &&
                                model_.set_action(i, rebase_action::skip))
                                skipped = true;
                        if (skipped)
                            set_status(_("Please take a look at the skipped commits"), true);
                    };
                });
        }
    }
    update_controls();
}

namespace
{

// A picked ref as the branch lists name them: main, origin/main, v1.
std::string short_ref(const std::string& full)
{
    for (std::string_view p : {"refs/heads/", "refs/remotes/", "refs/tags/"})
        if (full.starts_with(p)) return full.substr(p.size());
    return full;
}

}  // namespace

// The upstream from Browse References, added to the list if it isn't there.
void rebase_window::pick_upstream()
{
    const std::string current = upstreams_.empty() ? "" : upstreams_[size_t(std::max(0, upstream_->current()))];
    pick_ref(ui_, window_.get(), repo_.workdir(), {.initial = current},
             [this, alive = std::weak_ptr<bool>(alive_)](const std::string& picked)
             {
                 if (!alive.lock() || picked.empty() || picked.find(' ') != std::string::npos) return;
                 const std::string name = short_ref(picked);
                 auto it = std::ranges::find(upstreams_, name);
                 if (it == upstreams_.end())
                 {
                     upstreams_.push_back(name);
                     upstream_->set_items(upstreams_);
                     it = upstreams_.end() - 1;
                 }
                 upstream_->set_current(int(it - upstreams_.begin()));
                 reload_list();
             });
}

// Onto: picked from Browse References, or, when set, cleared. Not the
// upstream or the branch itself [spec 2.8].
void rebase_window::toggle_onto()
{
    if (!onto_.empty())
    {
        onto_.clear();
        onto_button_->set_text(_("_Onto"));
        reload_list();
        return;
    }
    pick_ref(ui_, window_.get(), repo_.workdir(), {},
             [this, alive = std::weak_ptr<bool>(alive_)](const std::string& picked)
             {
                 if (!alive.lock() || picked.empty() || picked.find(' ') != std::string::npos) return;
                 const std::string name = short_ref(picked);
                 const std::string b = branches_.empty() ? "" : branches_[size_t(std::max(0, branch_->current()))];
                 const std::string u = upstreams_.empty() ? "" : upstreams_[size_t(std::max(0, upstream_->current()))];
                 if (name == u || name == b) return;
                 onto_ = name;
                 onto_button_->set_text(rap::i18n::format(_("_Onto: {0}"), menu_text(name)));
                 reload_list();
             });
}

void rebase_window::selection_changed()
{
    // Before the start, one selected row shows its files and message
    // [spec 2.9].
    if (engine_ && engine_->started()) return;
    auto sel = list_->selection();
    if (sel.size() != 1)
    {
        revision_files_.clear();
        message_->set_text("");
        return;
    }
    const commit& c = model_.rows()[sel.front()].commit;
    message_->set_text(c.message);
    revision_files_.set_source(
        {.what = changes_source::revision,
         .hash = c.id.hex(),
         .parent = c.parents.empty() ? k_empty_tree : c.parents.front().hex()});
    // On the worker, as the log does: the file list, then its line counts.
    revision_files_.clear();
    const oid id = c.id;
    files_worker_->submit(
        [this, id](repo& r, const repo_worker::progress& progress) -> std::function<void()>
        {
            (void)r.changes(
                id, {.listed = [&](const std::vector<file_change>& files)
                     { progress.post([this, files] { revision_files_.set_files(files); }); },
                     .counted =
                         [&](size_t first, const std::vector<file_change>& files)
                     {
                         progress.post([this, first, files]
                                       { revision_files_.set_counts(first, files); });
                     },
                     .stop = [&] { return progress.stale(); }});
            return {};
        });
}

// Button texts and enabled states by stage [spec 2.7].
void rebase_window::update_controls()
{
    using rebase_stage = rebase_engine::rebase_stage;
    rebase_stage stage = engine_ ? engine_->stage() : rebase_stage::choose;
    bool before = stage == rebase_stage::choose;
    bool busy = engine_ && engine_->busy();

    for (rap::widget* w : std::initializer_list<rap::widget*>{branch_, upstream_, swap_, set_all_,
                                                              up_, down_, preserve_merges_, force_})
        w->set_enabled(before);
    // Added commits are picks: with merges recreated, the list isn't a
    // plain series to add to [spec 2.5].
    add_->set_enabled(before && !preserve_merges_->checked());
    list_->set_reorderable(before);

    const char* text = _("Continue");
    switch (stage)
    {
    case rebase_stage::choose: text = fast_forward_ ? _("Start (FastFwd)") : _("Start Rebase"); break;
    case rebase_stage::conflict:
    case rebase_stage::squash_edit: text = _("Commit"); break;
    case rebase_stage::edit: text = _("Amend"); break;
    case rebase_stage::done: text = _("Done"); break;
    default: break;
    }
    continue_->set_text(text);
    continue_->set_enabled(before ? can_start_ : !busy && stage != rebase_stage::error);

    bool editing = stage == rebase_stage::conflict || stage == rebase_stage::squash_conflict ||
                   stage == rebase_stage::edit || stage == rebase_stage::squash_edit;
    message_->set_read_only(!editing);

    if (!before)
    {
        revision_page_->set_visible(false);
        bool done = stage == rebase_stage::done;
        conflict_page_->set_visible(!done);
        message_page_->set_visible(!done);
        log_page_->set_visible(true);
        if (stage == rebase_stage::conflict || stage == rebase_stage::squash_conflict)
        {
            refresh_conflicts();
            tabs_->set_current(page_conflict);
            set_status(_("Conflict: resolve the files, then press Commit"), true);
        }
        else if (stage == rebase_stage::edit || stage == rebase_stage::squash_edit)
        {
            tabs_->set_current(page_message);
            set_status(stage == rebase_stage::edit
                           ? _("Edit the commit, then press Amend")
                           : _("Edit the squashed message, then press Commit"));
        }
        else if (stage == rebase_stage::error)
            set_status(_("The rebase stopped with an error; see the Log tab, then Abort"), true);
        else if (done)
        {
            tabs_->set_current(page_log);
            progress_->set_fraction(1);
            set_status(_("Finished rebasing."));
            status_->set_visible(false);
            post_->set_visible(true);
        }
    }
}

// The working tree while a step is stopped: the conflicted files (git
// status marks them) and what else the step changed.
void rebase_window::refresh_conflicts()
{
    working_tree_changes changes =
        load_working_tree(repo_.workdir(), {.unversioned = false, .all_staged = true});
    conflict_files_.set_files(std::move(changes.files), false);
}

void rebase_window::append_log(const std::string& text, bool command)
{
    rap::text_style s;
    s.bold = command;
    log_->append(command && !text.ends_with('\n') ? text + "\n" : text, s);
}

void rebase_window::continue_pressed()
{
    using rebase_stage = rebase_engine::rebase_stage;
    switch (engine_->stage())
    {
    case rebase_stage::choose: start_rebase(); break;
    case rebase_stage::done:
        if (stashed_)
        {
            ui_.message(window_.get(),
                        {.icon = rap::message_spec::icon::question,
                         .title = _("Rebase"),
                         .text = _("Your changes were stashed before the rebase. Pop the stash now?"),
                         .buttons = {rap::dialog_role::yes, rap::dialog_role::no}},
                        [this](rap::dialog_role r)
                        {
                            if (r == rap::dialog_role::yes)
                                run_git(repo_.workdir(), {"stash", "pop"});
                            close();
                        });
            return;
        }
        close();
        break;
    default: engine_->resume(message_->text()); break;
    }
}

// Before the start, checks for local changes and offers to stash them
// [spec 2.7.1].
void rebase_window::start_rebase()
{
    // The picks create commits, so an identity is required [spec 2.7.1].
    if (!run_git(repo_.workdir(), {"var", "GIT_COMMITTER_IDENT"}).ok())
    {
        ui_.message(window_.get(),
                    {.icon = rap::message_spec::icon::error,
                     .title = _("Rebase"),
                     .text = _("Git doesn't know who you are, so it can't create commits."),
                     .detail = _("Set your name and email, for example:\n"
                                 "git config --global user.name \"Your Name\"\n"
                                 "git config --global user.email you@example.com")});
        return;
    }
    git_result dirty = run_git(repo_.workdir(), {"status", "--porcelain", "--untracked-files=no"},
                               git_output::stdout_only);
    auto go = [this]
    {
        std::string branch = branches_[size_t(std::max(0, branch_->current()))];
        std::string head = repo_.head_name();
        rebase_engine::start_args s{
            .branch = branch,
            .branch_head = *branch_head_,
            // --onto: the upstream's commits replayed onto another [spec 2.8].
            .onto = onto_.empty() ? *upstream_head_ : repo_.resolve(onto_).value_or(*upstream_head_),
            .onto_name = !onto_.empty()      ? onto_
                         : upstreams_.empty() ? ""
                                              : upstreams_[size_t(std::max(0, upstream_->current()))],
            .checked_out = head == "HEAD" ? repo_.head_id().value_or(oid{}).hex() : head,
            .fast_forward = fast_forward_,
        };
        list_->select({});
        engine_->start(s);
    };
    if (dirty.ok() && dirty.output.empty()) return go();

    ui_.message(window_.get(),
                {.icon = rap::message_spec::icon::question,
                 .title = _("Rebase"),
                 .text = _("The working tree has local changes. Stash them and start the rebase?"),
                 .detail = dirty.output,
                 .buttons = {rap::dialog_role::yes, rap::dialog_role::cancel}},
                [this, go](rap::dialog_role r)
                {
                    if (r != rap::dialog_role::yes) return;
                    if (!run_git(repo_.workdir(), {"stash", "push"}).ok()) return;
                    stashed_ = true;
                    go();
                });
}

void rebase_window::abort_pressed()
{
    if (!engine_->started() || engine_->stage() == rebase_engine::rebase_stage::done)
        return close();
    ui_.message(window_.get(),
                {.icon = rap::message_spec::icon::question,
                 .title = _("Rebase"),
                 .text = _("Are you sure you want to abort the rebase process?"),
                 .buttons = {rap::dialog_role::yes, rap::dialog_role::no}},
                [this](rap::dialog_role r)
                {
                    if (r != rap::dialog_role::yes) return;
                    engine_->abort(
                        [this]
                        {
                            if (stashed_) run_git(repo_.workdir(), {"stash", "pop"});
                            close();
                        });
                });
}

void rebase_window::close()
{
    closing_ = true;
    window_->close();
    if (closed_) closed_();
}

void rebase_window::show_count()
{
    set_status(rap::i18n::format(
        N_("{0} commit to rebase onto {1}", "{0} commits to rebase onto {1}", model_.size()),
        model_.size(), count_target_));
}

bool rebase_window::can_add() const
{
    return !(engine_ && engine_->started()) && !preserve_merges_->checked();
}

bool rebase_window::add_commits_at(size_t at, const std::vector<std::string>& hashes)
{
    if (!can_add()) return false;
    // Merges can't be picked; commits from another repository aren't here.
    std::vector<commit> chosen;
    for (const std::string& h : hashes)
        if (auto id = repo_.resolve(h))
            if (auto c = repo_.lookup(*id); c && c->parents.size() <= 1) chosen.push_back(*c);
    const size_t n = model_.add(at, chosen);
    if (!n) return false;
    show_count();
    std::vector<size_t> rows(n);
    std::iota(rows.begin(), rows.end(), std::min(at, model_.size() - n));
    list_->select(rows);
    return true;
}

void rebase_window::add_commits()
{
    auto log = log_window::open(ui_, repo_.workdir(), true);
    if (!log)
        return ui_.message(
            window_.get(),
            {.icon = rap::message_spec::icon::error, .title = "Tortilla", .text = log.error()});
    (*log)->pick_commits(_("Add Commits to Rebase"), _("_Add"),
                         [this, alive = std::weak_ptr<bool>(alive_)](std::vector<commit> chosen)
                         {
                             if (!alive.lock()) return;
                             std::vector<std::string> hashes;
                             for (const commit& c : chosen) hashes.push_back(c.id.hex());
                             add_commits_at(0, hashes);
                         });
    windows::keep(std::move(*log));
}

void rebase_window::move_rows(bool up, bool to_end)
{
    if (engine_->started()) return;
    auto moved = model_.move(list_->selection(), up, to_end);
    list_->select(moved);
    if (!moved.empty()) list_->scroll_to(up ? moved.front() : moved.back());
}

// Drag and drop, before the start only. TortoiseGit allows only contiguous
// selections [spec 2.6]; here a scattered selection is gathered at the drop
// point in its original order (useful for collecting fix-ups next to the
// commit they belong with).
bool rebase_window::drop_rows(const std::vector<size_t>& rows, size_t dest)
{
    if (engine_->started() || rows.empty()) return false;
    auto moved = model_.move_to(rows, dest);
    list_->select(moved);
    return true;
}

void rebase_window::set_action(rebase_action a)
{
    for (size_t row : list_->selection()) model_.set_action(row, a);
}

// Space cycles; P/S/Q/E set; U/D move; Ctrl+Enter continues; F5 reloads;
// Esc aborts [spec 2.5].
bool rebase_window::key(const rap::key_event& e)
{
    if (!e.press) return false;
    using rap::key;
    bool ctrl = e.modifiers & rap::mod_ctrl;
    if (e.key == key::escape)
    {
        abort_pressed();
        return true;
    }
    if (e.key == key::enter && ctrl)
    {
        continue_pressed();
        return true;
    }
    if (e.key == key::f5)
    {
        if (engine_->started())
            refresh_conflicts();
        else
            reload_list();
        return true;
    }
    if (engine_->started() || ctrl) return false;
    if (e.key == key::space)
    {
        model_.cycle(list_->selection());
        return true;
    }
    if (e.key != key::character) return false;
    switch (e.codepoint)
    {
    case 'p': set_action(rebase_action::pick); return true;
    case 's': set_action(rebase_action::skip); return true;
    case 'q': set_action(rebase_action::squash); return true;
    case 'e': set_action(rebase_action::edit); return true;
    case 'u': move_rows(true, e.modifiers & rap::mod_shift); return true;
    case 'd': move_rows(false, e.modifiers & rap::mod_shift); return true;
    }
    return false;
}

// Pick / Squash / Edit / Skip, then the usual commit items [spec 2.5].
rap::menu rebase_window::context_menu(std::optional<size_t> row)
{
    if (!row || engine_->started()) return {};
    auto sel = list_->selection();
    auto allowed = [&](rebase_action a)
    { return std::ranges::any_of(sel, [&](size_t r) { return model_.can_set(r, a); }); };
    rap::menu m{
        item(
            _("_Pick"), [this] { set_action(rebase_action::pick); }, allowed(rebase_action::pick)),
        item(
            _("_Squash (into commit below)"), [this] { set_action(rebase_action::squash); },
            allowed(rebase_action::squash)),
        item(
            _("_Edit"), [this] { set_action(rebase_action::edit); }, allowed(rebase_action::edit)),
        item(
            _("S_kip"), [this] { set_action(rebase_action::skip); }, allowed(rebase_action::skip)),
        rap::menu_item::separator(),
    };
    if (sel.size() == 1)
    {
        std::string hash = model_.rows()[sel.front()].commit.id.hex();
        m.push_back(item(_("Copy SHA-1 to clipboard"), [this, hash] { ui_.set_clipboard(hash); }));
    }
    return m;
}

// The set-all button's entries [spec 2.2]. TortoiseGit's is a split button
// whose face repeats the last choice; here choosing runs it and relabels.
rap::menu rebase_window::set_all_menu()
{
    auto entry = [this](const char* label, rebase_action a, bool unselected)
    {
        return item(label,
                    [this, label, a, unselected]
                    {
                        if (unselected)
                            model_.set_unselected(list_->selection(), a);
                        else
                            model_.set_all(a);
                        set_all_->set_text(label);
                    });
    };
    return {
        entry(_("_Pick ALL"), rebase_action::pick, false),
        entry(_("_Squash ALL"), rebase_action::squash, false),
        entry(_("_Edit ALL"), rebase_action::edit, false),
        entry(_("S_kip unselected"), rebase_action::skip, true),
        entry(_("S_quash unselected"), rebase_action::squash, true),
        entry(_("E_dit unselected"), rebase_action::edit, true),
    };
}

}  // namespace tortilla
