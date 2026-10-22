#include "rebase_window.h"

#include "git/command.h"

#include <algorithm>

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
    if (repo->bare()) return std::unexpected("cannot rebase in a bare repository");
    std::unique_ptr<rebase_window> w(new rebase_window(ui, std::move(*repo)));
    w->build(upstream);
    return w;
}

rebase_window::rebase_window(rap::ui& ui, repo repo)
    : ui_(ui)
    , repo_(std::move(repo))
{
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

    window_ = ui_.create(window{
        title + " - Rebase - Tortilla",
        {vbox{
            // Row 1 [spec 2.2]
            hbox{
                label{"Branch:"},
                drop_down{branches_, index_of(branches_, current)}
                    .bind(branch_)
                    .on_changed([this](int) { reload_list(); })
                    .width(30_ch),
                button{"Swap branch and upstream"}.icon(swap_icon).icon_only().bind(swap_).on_click(
                    [this]
                    {
                        // Swap only when both names exist on both sides.
                        std::string b = branches_[size_t(std::max(0, branch_->current()))];
                        std::string u = upstreams_[size_t(std::max(0, upstream_->current()))];
                        auto bi = std::ranges::find(branches_, u);
                        if (bi == branches_.end()) return;
                        branch_->set_current(int(bi - branches_.begin()));
                        auto ui = std::ranges::find(upstreams_, b);
                        if (ui != upstreams_.end())
                            upstream_->set_current(int(ui - upstreams_.begin()));
                        reload_list();
                    }),
                label{"Upstream:"},
                drop_down{upstreams_, index_of(upstreams_, wanted)}
                    .bind(upstream_)
                    .on_changed([this](int) { reload_list(); })
                    .width(30_ch),
                // TODO: Browse References and --onto.
                button{"..."}.enabled(false),
                button{"_Onto"}.enabled(false),
                spacer{},
            },
            splitter{
                vertical,
                list{rebase_model::columns(), &model_}
                    .compact()
                    .soft_selection()
                    .bind(list_)
                    .on_selection_changed([this] { selection_changed(); })
                    .on_context_menu([this](std::optional<size_t> row) { return context_menu(row); })
                    .on_reorder([this](const std::vector<size_t>& rows, size_t dest)
                               { return drop_rows(rows, dest); })
                    .stretch(3),
                vbox{
                    // Row under the list [spec 2.2]
                    hbox{
                        menu_button{"_Pick ALL"}.bind(set_all_).on_menu([this]
                                                                     { return set_all_menu(); }),
                        button{"_Up"}.bind(up_).on_click([this] { move_rows(true, false); }),
                        button{"_Down"}.bind(down_).on_click([this] { move_rows(false, false); }),
                        // TODO: Add (pick commits from a log).
                        button{"_Add"}.enabled(false),
                        check{"_Preserve merges"}
                            .bind(preserve_merges_)
                            .on_toggled([this](bool) { reload_list(); })
                            .tooltip("Include merge commits. Reordering commits with merges "
                                     "does not work properly."),
                        check{"_Force Rebase"}
                            .bind(force_)
                            .on_toggled([this](bool) { reload_list(); })
                            .tooltip("Force the rebase even if the current branch is a "
                                     "descendant of the commit you are rebasing onto."),
                    },
                    tabs{
                        {"Revision Files", vbox{list{file_model::columns(), &revision_files_}
                                                    .compact()
                                                    .soft_selection()
                                                    .stretch()}
                                               .handle(revision_page_)},
                        {"Conflict Files", vbox{list{file_model::columns(), &conflict_files_}
                                                    .compact()
                                                    .soft_selection()
                                                    .stretch()}
                                               .handle(conflict_page_)},
                        {"Commit Message",
                         vbox{text_view{}.compact().read_only().bind(message_).stretch()}.handle(
                             message_page_)},
                        {"Log", vbox{text_view{}
                                         .compact()
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
                menu_button{"Show log"}.bind(post_).on_menu(
                    [this]() -> rap::menu
                    {
                        return {item("Show _log",
                                     [this]
                                     {
                                         if (show_log_) show_log_();
                                         close();
                                     })};
                    }),
                button{"Start Rebase"}.bind(continue_).on_click([this] { continue_pressed(); }),
                button{"Abort"}
                    .bind(abort_)
                    .tooltip("Recover to the status before rebase")
                    .on_click([this] { abort_pressed(); }),
                button{"Help"}.enabled(false),
            },
        }}}.size(120_ch, 40_lines));

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
            set_status("Rebasing... (" + std::to_string(applied) + "/" + std::to_string(total) +
                      ")");
            if (auto c = list_->current(); !c || *c != total - applied)
                list_->scroll_to(total - applied);
        },
        .message = [this](const std::string& m) { message_->set_text(m); },
        .warn =
            [this](const std::string& m)
        {
            ui_.message(window_.get(),
                        {.icon = rap::message_spec::icon::warning, .title = "Rebase", .text = m});
        },
        .ask_skip =
            [this](const std::string& output, std::function<void(bool)> answer)
        {
            ui_.message(window_.get(),
                        {.icon = rap::message_spec::icon::question,
                         .title = "Rebase",
                         .text = "Cherry-pick failed. Skip this commit?",
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
    revision_files_.set({});
    message_->set_text("");

    std::string b = branches_.empty() ? "" : branches_[size_t(std::max(0, branch_->current()))];
    std::string u = upstreams_.empty() ? "" : upstreams_[size_t(std::max(0, upstream_->current()))];
    branch_head_ = b.empty() ? std::nullopt : repo_.resolve(b);
    upstream_head_ = u.empty() ? std::nullopt : repo_.resolve(u);

    if (!branch_head_)
        set_status("Please select branch");
    else if (!upstream_head_)
        set_status("Please select upstream");
    else if (*branch_head_ == *upstream_head_)
        set_status("No need to rebase: \"" + b + "\" equals \"" + u + "\"");
    else if (repo_.is_ancestor(*branch_head_, *upstream_head_))
    {
        set_status("Branch \"" + b + "\" is behind \"" + u + "\": \"" + b +
                  "\" will fast-forward to \"" + u + "\"");
        fast_forward_ = can_start_ = true;
    }
    else if (repo_.is_ancestor(*upstream_head_, *branch_head_) && !force_->checked())
        set_status("Current branch \"" + b +
                  "\" is up to date. To rebase anyway, check \"Force Rebase\" below.");
    else
    {
        auto commits = repo_.range(*upstream_head_, *branch_head_, preserve_merges_->checked());
        if (!commits || commits->empty())
            set_status(commits ? "Nothing to Rebase" : commits.error());
        else
        {
            // Commits already upstream (git cherry "-") start as Skip.
            git_result cherry = run_git(repo_.workdir(), {"cherry", u, b}, git_output::stdout_only);
            std::vector<rebase_row> rows;
            bool skipped = false;
            for (auto& c : *commits)
            {
                rebase_row row{.commit = c};
                if (!preserve_merges_->checked() &&
                    cherry.output.find("- " + c.id.hex()) != std::string::npos)
                {
                    row.action = rebase_action::skip;
                    skipped = true;
                }
                rows.push_back(std::move(row));
            }
            model_.set(std::move(rows), repo_.refs());
            can_start_ = true;
            if (skipped)
                set_status("Please take a look at the skipped commits", true);
            else
                set_status(std::to_string(model_.size()) + " commit(s) to rebase onto " + u);
            list_->set_current(0);
        }
    }
    update_controls();
}

void rebase_window::selection_changed()
{
    // Before the start, one selected row shows its files and message
    // [spec 2.9].
    if (engine_ && engine_->started()) return;
    auto sel = list_->selection();
    if (sel.size() != 1)
    {
        revision_files_.set({});
        message_->set_text("");
        return;
    }
    const commit& c = model_.rows()[sel.front()].commit;
    message_->set_text(c.message);
    auto changes = repo_.changes(c.id);
    revision_files_.set(changes ? std::move(*changes) : std::vector<file_change>{});
}

// Button texts and enabled states by stage [spec 2.7].
void rebase_window::update_controls()
{
    using rebase_stage = rebase_engine::rebase_stage;
    rebase_stage stage = engine_ ? engine_->stage() : rebase_stage::choose;
    bool before = stage == rebase_stage::choose;
    bool busy = engine_ && engine_->busy();

    for (rap::widget* w : std::initializer_list<rap::widget*>{
             branch_, upstream_, swap_, set_all_, up_, down_, preserve_merges_, force_})
        w->set_enabled(before);
    list_->set_reorderable(before);

    const char* text = "Continue";
    switch (stage)
    {
    case rebase_stage::choose: text = fast_forward_ ? "Start (FastFwd)" : "Start Rebase"; break;
    case rebase_stage::conflict:
    case rebase_stage::squash_edit: text = "Commit"; break;
    case rebase_stage::edit: text = "Amend"; break;
    case rebase_stage::done: text = "Done"; break;
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
            set_status("Conflict: resolve the files, then press Commit", true);
        }
        else if (stage == rebase_stage::edit || stage == rebase_stage::squash_edit)
        {
            tabs_->set_current(page_message);
            set_status(stage == rebase_stage::edit ? "Edit the commit, then press Amend"
                                           : "Edit the squashed message, then press Commit");
        }
        else if (stage == rebase_stage::error)
            set_status("The rebase stopped with an error; see the Log tab, then Abort", true);
        else if (done)
        {
            tabs_->set_current(page_log);
            progress_->set_fraction(1);
            set_status("Finished rebasing.");
            status_->set_visible(false);
            post_->set_visible(true);
        }
    }
}

void rebase_window::refresh_conflicts()
{
    auto files = repo_.working_tree_changes();
    conflict_files_.set(files ? std::move(*files) : std::vector<file_change>{}, false);
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
                         .title = "Rebase",
                         .text = "Your changes were stashed before the rebase. Pop the stash now?",
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
                     .title = "Rebase",
                     .text = "Git doesn't know who you are, so it can't create commits.",
                     .detail = "Set your name and email, for example:\n"
                               "git config --global user.name \"Your Name\"\n"
                               "git config --global user.email you@example.com"});
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
            .onto = *upstream_head_,
            .checked_out = head == "HEAD" ? repo_.head_id().value_or(oid{}).hex() : head,
            .fast_forward = fast_forward_,
        };
        list_->select({});
        engine_->start(s);
    };
    if (dirty.ok() && dirty.output.empty()) return go();

    ui_.message(window_.get(),
                {.icon = rap::message_spec::icon::question,
                 .title = "Rebase",
                 .text = "The working tree has local changes. Stash them and start the rebase?",
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
    if (!engine_->started() || engine_->stage() == rebase_engine::rebase_stage::done) return close();
    ui_.message(window_.get(),
                {.icon = rap::message_spec::icon::question,
                 .title = "Rebase",
                 .text = "Are you sure you want to abort the rebase process?",
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
            "_Pick", [this] { set_action(rebase_action::pick); }, allowed(rebase_action::pick)),
        item(
            "_Squash (with commit below)", [this] { set_action(rebase_action::squash); },
            allowed(rebase_action::squash)),
        item(
            "_Edit", [this] { set_action(rebase_action::edit); }, allowed(rebase_action::edit)),
        item(
            "S_kip", [this] { set_action(rebase_action::skip); }, allowed(rebase_action::skip)),
        rap::menu_item::separator(),
    };
    if (sel.size() == 1)
    {
        std::string hash = model_.rows()[sel.front()].commit.id.hex();
        m.push_back(item("Copy SHA-1 to clipboard", [this, hash] { ui_.set_clipboard(hash); }));
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
        entry("_Pick ALL", rebase_action::pick, false),
        entry("_Squash ALL", rebase_action::squash, false),
        entry("_Edit ALL", rebase_action::edit, false),
        entry("S_kip unselected", rebase_action::skip, true),
        entry("S_quash unselected", rebase_action::squash, true),
        entry("E_dit unselected", rebase_action::edit, true),
    };
}

}  // namespace tortilla
