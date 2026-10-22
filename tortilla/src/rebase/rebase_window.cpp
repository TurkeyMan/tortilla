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
enum Page
{
    PageRevision,
    PageConflict,
    PageMessage,
    PageLog,
};

rap::MenuItem item(std::string label, std::function<void()> action, bool enabled = true)
{
    return {.label = std::move(label), .action = std::move(action), .enabled = enabled};
}

}  // namespace

std::expected<std::unique_ptr<RebaseWindow>, std::string>
RebaseWindow::open(rap::Ui& ui, const std::string& path, const std::string& upstream)
{
    auto repo = Repo::open(path);
    if (!repo) return std::unexpected(repo.error());
    if (repo->bare()) return std::unexpected("cannot rebase in a bare repository");
    std::unique_ptr<RebaseWindow> w(new RebaseWindow(ui, std::move(*repo)));
    w->build(upstream);
    return w;
}

RebaseWindow::RebaseWindow(rap::Ui& ui, Repo repo)
    : ui_(ui)
    , repo_(std::move(repo))
{
}

void RebaseWindow::build(const std::string& upstream)
{
    using namespace rap::layout;

    // Branch: local branches, the current one preselected. Upstream: every
    // branch and tag, preselecting the given one or the branch's upstream.
    branches_ = repo_.branchNames(true, false, false);
    upstreams_ = repo_.branchNames(true, true, true);
    std::string current = repo_.headName();
    std::string wanted = upstream.empty() ? repo_.upstreamOf(current) : upstream;
    if (!wanted.empty() && !std::ranges::contains(upstreams_, wanted))
        upstreams_.insert(upstreams_.begin(), wanted);
    auto indexOf = [](const std::vector<std::string>& v, const std::string& s)
    {
        auto it = std::ranges::find(v, s);
        return it == v.end() ? 0 : int(it - v.begin());
    };

    std::string title = repo_.workdir();
    if (title.size() > 1 && title.back() == '/') title.pop_back();

    auto swapIcon = rap::Icon::drawn(
        [](rap::Painter& p, rap::SizeF s)
        {
            rap::Paint c{rap::ThemeColor::WindowText};
            float w = s.w, h = s.h, t = std::max(1.f, h * 0.1f);
            p.line({w * 0.15f, h * 0.35f}, {w * 0.85f, h * 0.35f}, c, t);
            p.polygon({{w * 0.85f, h * 0.18f}, {w, h * 0.35f}, {w * 0.85f, h * 0.52f}}, c,
                      std::nullopt);
            p.line({w * 0.85f, h * 0.65f}, {w * 0.15f, h * 0.65f}, c, t);
            p.polygon({{w * 0.15f, h * 0.48f}, {0, h * 0.65f}, {w * 0.15f, h * 0.82f}}, c,
                      std::nullopt);
        });

    window_ = ui_.create(Window{
        title + " - Rebase - Tortilla",
        {VBox{
            // Row 1 [spec 2.2]
            HBox{
                Label{"Branch:"},
                Select{branches_, indexOf(branches_, current)}
                    .bind(branch_)
                    .onChanged([this](int) { reloadList(); })
                    .width(30_ch),
                Button{"Swap branch and upstream"}.icon(swapIcon).iconOnly().bind(swap_).onClick(
                    [this]
                    {
                        // Swap only when both names exist on both sides.
                        std::string b = branches_[size_t(std::max(0, branch_->current()))];
                        std::string u = upstreams_[size_t(std::max(0, upstream_->current()))];
                        auto bi = std::ranges::find(branches_, u);
                        if (bi == branches_.end()) return;
                        branch_->setCurrent(int(bi - branches_.begin()));
                        auto ui = std::ranges::find(upstreams_, b);
                        if (ui != upstreams_.end())
                            upstream_->setCurrent(int(ui - upstreams_.begin()));
                        reloadList();
                    }),
                Label{"Upstream:"},
                Select{upstreams_, indexOf(upstreams_, wanted)}
                    .bind(upstream_)
                    .onChanged([this](int) { reloadList(); })
                    .width(30_ch),
                // TODO: Browse References and --onto.
                Button{"..."}.enabled(false),
                Button{"_Onto"}.enabled(false),
                Spacer{},
            },
            Splitter{
                Vertical,
                List{RebaseModel::columns(), &model_}
                    .compact()
                    .softSelection()
                    .bind(list_)
                    .onSelectionChanged([this] { selectionChanged(); })
                    .onContextMenu([this](std::optional<size_t> row) { return contextMenu(row); })
                    .onReorder([this](const std::vector<size_t>& rows, size_t dest)
                               { return dropRows(rows, dest); })
                    .stretch(3),
                VBox{
                    // Row under the list [spec 2.2]
                    HBox{
                        MenuButton{"_Pick ALL"}.bind(setAll_).onMenu([this]
                                                                     { return setAllMenu(); }),
                        Button{"_Up"}.bind(up_).onClick([this] { moveRows(true, false); }),
                        Button{"_Down"}.bind(down_).onClick([this] { moveRows(false, false); }),
                        // TODO: Add (pick commits from a log).
                        Button{"_Add"}.enabled(false),
                        Check{"_Preserve merges"}
                            .bind(preserveMerges_)
                            .onToggled([this](bool) { reloadList(); })
                            .tooltip("Include merge commits. Reordering commits with merges "
                                     "does not work properly."),
                        Check{"_Force Rebase"}
                            .bind(force_)
                            .onToggled([this](bool) { reloadList(); })
                            .tooltip("Force the rebase even if the current branch is a "
                                     "descendant of the commit you are rebasing onto."),
                    },
                    Tabs{
                        {"Revision Files", VBox{List{FileModel::columns(), &revisionFiles_}
                                                    .compact()
                                                    .softSelection()
                                                    .stretch()}
                                               .handle(revisionPage_)},
                        {"Conflict Files", VBox{List{FileModel::columns(), &conflictFiles_}
                                                    .compact()
                                                    .softSelection()
                                                    .stretch()}
                                               .handle(conflictPage_)},
                        {"Commit Message",
                         VBox{TextView{}.compact().readOnly().bind(message_).stretch()}.handle(
                             messagePage_)},
                        {"Log", VBox{TextView{}
                                         .compact()
                                         .monospace()
                                         .readOnly()
                                         .wrap(false)
                                         .bind(log_)
                                         .stretch()}
                                    .handle(logPage_)},
                    }
                        .bind(tabs_)
                        .stretch(),
                }
                    .stretch(2),
            }
                .stretch(),
            // Bottom [spec 2.2]
            ProgressBar{}.bind(progress_),
            HBox{
                Label{}.ellipsize().bind(status_).stretch(),
                MenuButton{"Show log"}.bind(post_).onMenu(
                    [this]() -> rap::Menu
                    {
                        return {item("Show _log",
                                     [this]
                                     {
                                         if (showLog_) showLog_();
                                         close();
                                     })};
                    }),
                Button{"Start Rebase"}.bind(continue_).onClick([this] { continuePressed(); }),
                Button{"Abort"}
                    .bind(abort_)
                    .tooltip("Recover to the status before rebase")
                    .onClick([this] { abortPressed(); }),
                Button{"Help"}.enabled(false),
            },
        }}}.size(120_ch, 40_lines));

    window_->onCloseRequest(
        [this]
        {
            if (closing_) return true;
            abortPressed();
            return false;
        });
    window_->onKey([this](const rap::KeyEvent& e) { return key(e); });

    RebaseEngine::Hooks hooks{
        .log = [this](const std::string& t, bool cmd) { appendLog(t, cmd); },
        .stageChanged = [this] { updateControls(); },
        .progress =
            [this](size_t applied, size_t total)
        {
            progress_->setFraction(total ? float(applied) / float(total) : 0);
            setStatus("Rebasing... (" + std::to_string(applied) + "/" + std::to_string(total) +
                      ")");
            if (auto c = list_->current(); !c || *c != total - applied)
                list_->scrollTo(total - applied);
        },
        .message = [this](const std::string& m) { message_->setText(m); },
        .warn =
            [this](const std::string& m)
        {
            ui_.message(window_.get(),
                        {.icon = rap::MessageSpec::Icon::Warning, .title = "Rebase", .text = m});
        },
        .askSkip =
            [this](const std::string& output, std::function<void(bool)> answer)
        {
            ui_.message(window_.get(),
                        {.icon = rap::MessageSpec::Icon::Question,
                         .title = "Rebase",
                         .text = "Cherry-pick failed. Skip this commit?",
                         .detail = output,
                         .buttons = {rap::DialogRole::Yes, rap::DialogRole::No}},
                        [answer](rap::DialogRole r) { answer(r == rap::DialogRole::Yes); });
        },
    };
    engine_ = std::make_unique<RebaseEngine>(ui_, repo_.workdir(), repo_.gitDir(), model_, hooks);

    post_->setVisible(false);
    conflictPage_->setVisible(false);
    logPage_->setVisible(false);
    reloadList();
}

void RebaseWindow::setStatus(const std::string& text, bool warning)
{
    // TODO: TortoiseGit shows warnings as white text on red; rapport labels
    // have no colours yet.
    status_->setText(warning ? "⚠ " + text : text);
}

// Recomputes the commits to rebase from Branch and Upstream [spec 2.3].
void RebaseWindow::reloadList()
{
    if (engine_ && engine_->started()) return;
    fastForward_ = canStart_ = false;
    model_.set({}, {});
    revisionFiles_.set({});
    message_->setText("");

    std::string b = branches_.empty() ? "" : branches_[size_t(std::max(0, branch_->current()))];
    std::string u = upstreams_.empty() ? "" : upstreams_[size_t(std::max(0, upstream_->current()))];
    branchHead_ = b.empty() ? std::nullopt : repo_.resolve(b);
    upstreamHead_ = u.empty() ? std::nullopt : repo_.resolve(u);

    if (!branchHead_)
        setStatus("Please select branch");
    else if (!upstreamHead_)
        setStatus("Please select upstream");
    else if (*branchHead_ == *upstreamHead_)
        setStatus("No need to rebase: \"" + b + "\" equals \"" + u + "\"");
    else if (repo_.isAncestor(*branchHead_, *upstreamHead_))
    {
        setStatus("Branch \"" + b + "\" is behind \"" + u + "\": \"" + b +
                  "\" will fast-forward to \"" + u + "\"");
        fastForward_ = canStart_ = true;
    }
    else if (repo_.isAncestor(*upstreamHead_, *branchHead_) && !force_->checked())
        setStatus("Current branch \"" + b +
                  "\" is up to date. To rebase anyway, check \"Force Rebase\" below.");
    else
    {
        auto commits = repo_.range(*upstreamHead_, *branchHead_, preserveMerges_->checked());
        if (!commits || commits->empty())
            setStatus(commits ? "Nothing to Rebase" : commits.error());
        else
        {
            // Commits already upstream (git cherry "-") start as Skip.
            GitResult cherry = runGit(repo_.workdir(), {"cherry", u, b}, GitOutput::Stdout);
            std::vector<RebaseRow> rows;
            bool skipped = false;
            for (auto& c : *commits)
            {
                RebaseRow row{.commit = c};
                if (!preserveMerges_->checked() &&
                    cherry.output.find("- " + c.id.hex()) != std::string::npos)
                {
                    row.action = RebaseAction::Skip;
                    skipped = true;
                }
                rows.push_back(std::move(row));
            }
            model_.set(std::move(rows), repo_.refs());
            canStart_ = true;
            if (skipped)
                setStatus("Please take a look at the skipped commits", true);
            else
                setStatus(std::to_string(model_.size()) + " commit(s) to rebase onto " + u);
            list_->setCurrent(0);
        }
    }
    updateControls();
}

void RebaseWindow::selectionChanged()
{
    // Before the start, one selected row shows its files and message
    // [spec 2.9].
    if (engine_ && engine_->started()) return;
    auto sel = list_->selection();
    if (sel.size() != 1)
    {
        revisionFiles_.set({});
        message_->setText("");
        return;
    }
    const Commit& c = model_.rows()[sel.front()].commit;
    message_->setText(c.message);
    auto changes = repo_.changes(c.id);
    revisionFiles_.set(changes ? std::move(*changes) : std::vector<FileChange>{});
}

// Button texts and enabled states by stage [spec 2.7].
void RebaseWindow::updateControls()
{
    using Stage = RebaseEngine::Stage;
    Stage stage = engine_ ? engine_->stage() : Stage::Choose;
    bool before = stage == Stage::Choose;
    bool busy = engine_ && engine_->busy();

    for (rap::Widget* w : std::initializer_list<rap::Widget*>{
             branch_, upstream_, swap_, setAll_, up_, down_, preserveMerges_, force_})
        w->setEnabled(before);
    list_->setReorderable(before);

    const char* text = "Continue";
    switch (stage)
    {
    case Stage::Choose: text = fastForward_ ? "Start (FastFwd)" : "Start Rebase"; break;
    case Stage::Conflict:
    case Stage::SquashEdit: text = "Commit"; break;
    case Stage::Edit: text = "Amend"; break;
    case Stage::Done: text = "Done"; break;
    default: break;
    }
    continue_->setText(text);
    continue_->setEnabled(before ? canStart_ : !busy && stage != Stage::Error);

    bool editing = stage == Stage::Conflict || stage == Stage::SquashConflict ||
                   stage == Stage::Edit || stage == Stage::SquashEdit;
    message_->setReadOnly(!editing);

    if (!before)
    {
        revisionPage_->setVisible(false);
        bool done = stage == Stage::Done;
        conflictPage_->setVisible(!done);
        messagePage_->setVisible(!done);
        logPage_->setVisible(true);
        if (stage == Stage::Conflict || stage == Stage::SquashConflict)
        {
            refreshConflicts();
            tabs_->setCurrent(PageConflict);
            setStatus("Conflict: resolve the files, then press Commit", true);
        }
        else if (stage == Stage::Edit || stage == Stage::SquashEdit)
        {
            tabs_->setCurrent(PageMessage);
            setStatus(stage == Stage::Edit ? "Edit the commit, then press Amend"
                                           : "Edit the squashed message, then press Commit");
        }
        else if (stage == Stage::Error)
            setStatus("The rebase stopped with an error; see the Log tab, then Abort", true);
        else if (done)
        {
            tabs_->setCurrent(PageLog);
            progress_->setFraction(1);
            setStatus("Finished rebasing.");
            status_->setVisible(false);
            post_->setVisible(true);
        }
    }
}

void RebaseWindow::refreshConflicts()
{
    auto files = repo_.workingTreeChanges();
    conflictFiles_.set(files ? std::move(*files) : std::vector<FileChange>{}, false);
}

void RebaseWindow::appendLog(const std::string& text, bool command)
{
    rap::TextStyle s;
    s.bold = command;
    log_->append(command && !text.ends_with('\n') ? text + "\n" : text, s);
}

void RebaseWindow::continuePressed()
{
    using Stage = RebaseEngine::Stage;
    switch (engine_->stage())
    {
    case Stage::Choose: startRebase(); break;
    case Stage::Done:
        if (stashed_)
        {
            ui_.message(window_.get(),
                        {.icon = rap::MessageSpec::Icon::Question,
                         .title = "Rebase",
                         .text = "Your changes were stashed before the rebase. Pop the stash now?",
                         .buttons = {rap::DialogRole::Yes, rap::DialogRole::No}},
                        [this](rap::DialogRole r)
                        {
                            if (r == rap::DialogRole::Yes)
                                runGit(repo_.workdir(), {"stash", "pop"});
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
void RebaseWindow::startRebase()
{
    // The picks create commits, so an identity is required [spec 2.7.1].
    if (!runGit(repo_.workdir(), {"var", "GIT_COMMITTER_IDENT"}).ok())
    {
        ui_.message(window_.get(),
                    {.icon = rap::MessageSpec::Icon::Error,
                     .title = "Rebase",
                     .text = "Git doesn't know who you are, so it can't create commits.",
                     .detail = "Set your name and email, for example:\n"
                               "git config --global user.name \"Your Name\"\n"
                               "git config --global user.email you@example.com"});
        return;
    }
    GitResult dirty = runGit(repo_.workdir(), {"status", "--porcelain", "--untracked-files=no"},
                             GitOutput::Stdout);
    auto go = [this]
    {
        std::string branch = branches_[size_t(std::max(0, branch_->current()))];
        std::string head = repo_.headName();
        RebaseEngine::Start s{
            .branch = branch,
            .branchHead = *branchHead_,
            .onto = *upstreamHead_,
            .checkedOut = head == "HEAD" ? repo_.headId().value_or(Oid{}).hex() : head,
            .fastForward = fastForward_,
        };
        list_->select({});
        engine_->start(s);
    };
    if (dirty.ok() && dirty.output.empty()) return go();

    ui_.message(window_.get(),
                {.icon = rap::MessageSpec::Icon::Question,
                 .title = "Rebase",
                 .text = "The working tree has local changes. Stash them and start the rebase?",
                 .detail = dirty.output,
                 .buttons = {rap::DialogRole::Yes, rap::DialogRole::Cancel}},
                [this, go](rap::DialogRole r)
                {
                    if (r != rap::DialogRole::Yes) return;
                    if (!runGit(repo_.workdir(), {"stash", "push"}).ok()) return;
                    stashed_ = true;
                    go();
                });
}

void RebaseWindow::abortPressed()
{
    if (!engine_->started() || engine_->stage() == RebaseEngine::Stage::Done) return close();
    ui_.message(window_.get(),
                {.icon = rap::MessageSpec::Icon::Question,
                 .title = "Rebase",
                 .text = "Are you sure you want to abort the rebase process?",
                 .buttons = {rap::DialogRole::Yes, rap::DialogRole::No}},
                [this](rap::DialogRole r)
                {
                    if (r != rap::DialogRole::Yes) return;
                    engine_->abort(
                        [this]
                        {
                            if (stashed_) runGit(repo_.workdir(), {"stash", "pop"});
                            close();
                        });
                });
}

void RebaseWindow::close()
{
    closing_ = true;
    window_->close();
    if (closed_) closed_();
}

void RebaseWindow::moveRows(bool up, bool toEnd)
{
    if (engine_->started()) return;
    auto moved = model_.move(list_->selection(), up, toEnd);
    list_->select(moved);
    if (!moved.empty()) list_->scrollTo(up ? moved.front() : moved.back());
}

// Drag and drop, before the start only. TortoiseGit allows only contiguous
// selections [spec 2.6]; here a scattered selection is gathered at the drop
// point in its original order (useful for collecting fix-ups next to the
// commit they belong with).
bool RebaseWindow::dropRows(const std::vector<size_t>& rows, size_t dest)
{
    if (engine_->started() || rows.empty()) return false;
    auto moved = model_.moveTo(rows, dest);
    list_->select(moved);
    return true;
}

void RebaseWindow::setAction(RebaseAction a)
{
    for (size_t row : list_->selection()) model_.setAction(row, a);
}

// Space cycles; P/S/Q/E set; U/D move; Ctrl+Enter continues; F5 reloads;
// Esc aborts [spec 2.5].
bool RebaseWindow::key(const rap::KeyEvent& e)
{
    if (!e.press) return false;
    using rap::Key;
    bool ctrl = e.modifiers & rap::ModCtrl;
    if (e.key == Key::Escape)
    {
        abortPressed();
        return true;
    }
    if (e.key == Key::Enter && ctrl)
    {
        continuePressed();
        return true;
    }
    if (e.key == Key::F5)
    {
        if (engine_->started())
            refreshConflicts();
        else
            reloadList();
        return true;
    }
    if (engine_->started() || ctrl) return false;
    if (e.key == Key::Space)
    {
        model_.cycle(list_->selection());
        return true;
    }
    if (e.key != Key::Character) return false;
    switch (e.codepoint)
    {
    case 'p': setAction(RebaseAction::Pick); return true;
    case 's': setAction(RebaseAction::Skip); return true;
    case 'q': setAction(RebaseAction::Squash); return true;
    case 'e': setAction(RebaseAction::Edit); return true;
    case 'u': moveRows(true, e.modifiers & rap::ModShift); return true;
    case 'd': moveRows(false, e.modifiers & rap::ModShift); return true;
    }
    return false;
}

// Pick / Squash / Edit / Skip, then the usual commit items [spec 2.5].
rap::Menu RebaseWindow::contextMenu(std::optional<size_t> row)
{
    if (!row || engine_->started()) return {};
    auto sel = list_->selection();
    auto allowed = [&](RebaseAction a)
    { return std::ranges::any_of(sel, [&](size_t r) { return model_.canSet(r, a); }); };
    rap::Menu m{
        item(
            "_Pick", [this] { setAction(RebaseAction::Pick); }, allowed(RebaseAction::Pick)),
        item(
            "_Squash (with commit below)", [this] { setAction(RebaseAction::Squash); },
            allowed(RebaseAction::Squash)),
        item(
            "_Edit", [this] { setAction(RebaseAction::Edit); }, allowed(RebaseAction::Edit)),
        item(
            "S_kip", [this] { setAction(RebaseAction::Skip); }, allowed(RebaseAction::Skip)),
        rap::MenuItem::separator(),
    };
    if (sel.size() == 1)
    {
        std::string hash = model_.rows()[sel.front()].commit.id.hex();
        m.push_back(item("Copy SHA-1 to clipboard", [this, hash] { ui_.setClipboard(hash); }));
    }
    return m;
}

// The set-all button's entries [spec 2.2]. TortoiseGit's is a split button
// whose face repeats the last choice; here choosing runs it and relabels.
rap::Menu RebaseWindow::setAllMenu()
{
    auto entry = [this](const char* label, RebaseAction a, bool unselected)
    {
        return item(label,
                    [this, label, a, unselected]
                    {
                        if (unselected)
                            model_.setUnselected(list_->selection(), a);
                        else
                            model_.setAll(a);
                        setAll_->setText(label);
                    });
    };
    return {
        entry("_Pick ALL", RebaseAction::Pick, false),
        entry("_Squash ALL", RebaseAction::Squash, false),
        entry("_Edit ALL", RebaseAction::Edit, false),
        entry("S_kip unselected", RebaseAction::Skip, true),
        entry("S_quash unselected", RebaseAction::Squash, true),
        entry("E_dit unselected", RebaseAction::Edit, true),
    };
}

}  // namespace tortilla
