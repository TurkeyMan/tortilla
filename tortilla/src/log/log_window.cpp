#include "log_window.h"

#include "settings.h"
#include "ui/menu_text.h"
#include "ui/settings_dialog.h"
#include "util/calendar.h"

#include <ctime>
#include <unordered_set>

namespace tortilla
{
namespace
{

// Jump combo entries [spec 1.9.6], in order.
enum JumpType
{
    JumpAuthorEmail,
    JumpCommitterEmail,
    JumpMergePoint,
    JumpParent1,
    JumpParent2,
    JumpTag,
    JumpTagFF,
    JumpBranch,
    JumpBranchFF,
    JumpHistory,
};

std::string formatTime(int64_t time, int offsetMinutes)
{
    std::time_t t = time + int64_t(offsetMinutes) * 60;
    std::tm tm = utcCalendar(t);
    char buf[48];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tm);
    char tz[16];
    std::snprintf(tz, sizeof tz, " %+03d%02d", offsetMinutes / 60, std::abs(offsetMinutes % 60));
    return std::string(buf) + tz;
}

rap::Date localDate(int64_t time)
{
    std::time_t t = time;
    std::tm tm = localCalendar(t);
    return {tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday};
}

// From means 00:00:00 of the day, To 23:59:59 [spec 1.9.3].
int64_t localTime(rap::Date d, bool endOfDay)
{
    std::tm tm{};
    tm.tm_year = d.year - 1900;
    tm.tm_mon = d.month - 1;
    tm.tm_mday = d.day;
    tm.tm_hour = endOfDay ? 23 : 0;
    tm.tm_min = endOfDay ? 59 : 0;
    tm.tm_sec = endOfDay ? 59 : 0;
    tm.tm_isdst = -1;
    return std::mktime(&tm);
}

// TortoiseGit's jump buttons are green arrows (IDI_JUMPUP/IDI_JUMPDOWN).
rap::Icon jumpArrow(bool up)
{
    return rap::Icon::drawn(
        [up](rap::Painter& p, rap::SizeF s)
        {
            float w = s.w, h = s.h, cx = w / 2;
            float tip = up ? h * 0.1f : h * 0.9f, base = up ? h * 0.55f : h * 0.45f;
            float tail = up ? h * 0.9f : h * 0.1f;
            rap::Color green{0x2e, 0xa0, 0x43}, edge{0x1d, 0x6b, 0x2b};
            p.polygon({{cx, tip},
                       {w * 0.9f, base},
                       {w * 0.65f, base},
                       {w * 0.65f, tail},
                       {w * 0.35f, tail},
                       {w * 0.35f, base},
                       {w * 0.1f, base}},
                      rap::Paint{green}, rap::Paint{edge});
        });
}

rap::MenuItem toggle(std::string label, bool on, std::function<void()> action,
                       bool enabled = true)
{
    return {
        .label = std::move(label), .action = std::move(action), .enabled = enabled, .checked = on};
}

}  // namespace

std::expected<std::unique_ptr<LogWindow>, std::string>
LogWindow::open(rap::Ui& ui, const std::string& path, bool allBranches)
{
    auto repo = Repo::open(path);
    if (!repo) return std::unexpected(repo.error());
    std::unique_ptr<LogWindow> w(new LogWindow(ui, std::move(*repo), path));
    w->build();
    if (allBranches)
    {
        w->walkOptions_.allBranches = true;
        w->allBranches_->setState(rap::CheckState::Checked);
        w->updateRefButton();
    }
    w->reload();
    return w;
}

LogWindow::LogWindow(rap::Ui& ui, Repo repo, std::string path)
    : ui_(ui)
    , repo_(std::move(repo))
    , path_(std::move(path))
    , log_(ui)
    , filesWorker_(std::make_unique<RepoWorker>(ui, path_))
{
}

void LogWindow::build()
{
    using namespace rap::layout;
    namespace a = rap;

    std::string title = repo_.workdir();
    if (title.size() > 1 && title.back() == '/') title.pop_back();

    window_ = ui_.create(Window{
        title + " - Log Messages - Tortilla",
        {VBox{
            // Filter bar [spec 1.2, row 1]
            HBox{
                MenuButton{"HEAD"}
                    .bind(refButton_)
                    .onMenu([this] { return refMenu(); })
                    .width(22_ch),
                Label{"From:"},
                DatePicker{}.bind(from_).onChanged([this](a::Date) { datesChanged(); }),
                Label{"To:"},
                DatePicker{}.bind(to_).onChanged([this](a::Date) { datesChanged(); }),
                Entry{}
                    .bind(search_)
                    .onChanged([this](const std::string&) { scheduleFilter(); })
                    .onActivate([this] { applyFilter(); })
                    .stretch(),
                // TODO: TortoiseGit puts this magnifier inside the search box.
                MenuButton{""}
                    .icon(rap::Icon::named("edit-find-symbolic"))
                    .onMenu([this] { return filterMenu(); })
                    .tooltip("Filter options"),
                Select{{"Author Email", "Committer Email", "Merge Point", "Parent 1", "Parent 2",
                        "Tag", "Tag (FF)", "Branch", "Branch (FF)", "Selection History"},
                       0}
                    .bind(jumpType_),
                Button{"Jump up (Alt+Up)"}
                    .icon(jumpArrow(true))
                    .iconOnly()
                    .onClick([this] { jump(true); }),
                Button{"Jump down (Alt+Down)"}
                    .icon(jumpArrow(false))
                    .iconOnly()
                    .onClick([this] { jump(false); }),
            },
            // Commit list, details, changed files [spec 1.2, main area]
            Splitter{
                Vertical,
                List{LogModel::columns(), &log_}
                    .compact()
                    .softSelection()
                    .autoScroll()
                    .bind(commitList_)
                    .onSelectionChanged([this] { selectionChanged(); })
                    .onContextMenu([this](std::optional<size_t> row) { return contextMenu(row); })
                    .onHeaderMenu([this](size_t) { return headerMenu(); })
                    .onCellDrag(labelDrag())
                    .onActivate([this](size_t row) { rowActivated(row); })
                    .onCellContextMenu([this](size_t row, size_t column, rap::PointF at)
                                       { return cellMenu(row, column, at); })
                    .stretch(3),
                // The details pane keeps its height: dragging the upper
                // divider moves the lower one with it, taking space from the
                // file list until that reaches its minimum. (TortoiseGit
                // shrinks the details first [spec 1.3]; this is deliberate.)
                TextView{}.readOnly().compact().bind(details_).height(9_lines),
                List{FileModel::columns(), &files_}
                    .compact()
                    .softSelection()
                    .bind(fileList_)
                    .onSelectionChanged([this] { updateStatus(); })
                    .onActivate([this](size_t row) { fileActivated(row); })
                    .onContextMenu([this](std::optional<size_t> row) { return fileMenu(row, std::nullopt); })
                    .onCellContextMenu([this](size_t row, size_t column, rap::PointF)
                                       { return fileMenu(row, column); })
                    .height(4_lines)
                    .stretch(2),
            }
                .bind(splitter_)
                .stretch(),
            // Bottom area [spec 1.2]
            Label{}.selectable().ellipsize().bind(status_),
            HBox{
                VBox{
                    // TODO: enabled once the log can be opened for a path.
                    Check{"Show _Whole Project"}.enabled(false),
                    Check{"_All Branches"}
                        .bind(allBranches_)
                        .onToggled(
                            [this](bool)
                            {
                                walkOptions_.allBranches = allBranches_->checked();
                                updateRefButton();
                                reload();
                            }),
                },
                // TODO: path filter.
                Entry{}.placeholder("   Filter paths").enabled(false).stretch(),
            },
            HBox{
                Button{"Refresh"}.tooltip("Reload the log (F5)").onClick([this] { reload(); }),
                Button{"S_tatistics"}.bind(statistics_).enabled(false),
                ProgressBar{}.bind(progress_).stretch(),
                MenuButton{"Walk Be_havior"}.bind(walk_).onMenu([this] { return walkMenu(); }),
                MenuButton{"_View"}.bind(view_).onMenu([this] { return viewMenu(); }),
                Spacer{},
                Button{"OK"}.onClick(
                    [this]
                    {
                        saveLayout();
                        window_->close();
                        if (closed_) closed_();
                    }),
            },
        }}}.size(140_ch, 50_lines));

    window_->onCloseRequest(
        [this]
        {
            saveLayout();
            if (closed_) closed_();
            return true;
        });
    // F5 reloads [TortoiseGit: Refresh], e.g. after git ran elsewhere.
    window_->onKey(
        [this](const rap::KeyEvent& e)
        {
            if (!e.press || e.key != rap::Key::F5 || e.modifiers) return false;
            reload();
            return true;
        });
    // When the working-tree status arrives, refresh it if it's on show.
    log_.onWorkingTree(
        [this]
        {
            auto sel = commitList_->selection();
            if (sel.size() == 1 && log_.isWorkingTree(sel.front())) showCommit(sel.front());
        });
    search_->setPlaceholder(filterCue());
    progress_->setVisible(false);
    // Today until the loaded range replaces it (not 1970 while loading).
    from_->setDate(localDate(std::time(nullptr)));
    to_->setDate(localDate(std::time(nullptr)));
    updateRefButton();
    if (auto saved = Settings::get().value("log.actionIcons"); !saved.empty())
        log_.setActionIconStyle(ActionIconStyle(std::clamp(std::atoi(saved.c_str()), 0, 3)));
    restoreLayout();
}

// Window size and position, splitter panes, column widths and visibility
// persist across runs, as TortoiseGit's do (LogDlgSizer1/2 and the column
// state, spec 1.3 and 1.4). Position is only restored where the platform
// lets apps place windows (not on Wayland).
void LogWindow::saveLayout()
{
    Settings& st = Settings::get();
    st.saveWindow("log.window", *window_);
    st.setNumbers("log.panes", splitter_->sizes());
    st.setNumbers("log.columns", commitList_->columnWidths());
    st.setNumbers("log.files.columns", fileList_->columnWidths());
    std::string visible;
    for (size_t i = 0; i < LogModel::ColumnCount; ++i)
        visible += commitList_->columnVisible(i) ? '1' : '0';
    st.set("log.columns.visible", visible);

    // Options. All Branches is per repository, as in TortoiseGit [spec
    // 1.9.7]; the rest apply everywhere.
    auto flag = [](bool b) { return std::string(b ? "1" : "0"); };
    st.set("log.allBranches." + repo_.workdir(), flag(walkOptions_.allBranches));
    st.set("log.walk.firstParent", flag(walkOptions_.firstParent));
    st.set("log.walk.noMerges", flag(walkOptions_.noMerges));
    st.set("log.view.labels", std::to_string(log_.labels()));
    st.set("log.view.unversioned", flag(showUnversioned_));
    st.set("log.filter.fields", std::to_string(filter_.fields));
    st.set("log.filter.regex", flag(filter_.regex));
    st.set("log.filter.caseSensitive", flag(filter_.caseSensitive));
    st.set("log.jumpType", std::to_string(jumpType_->current()));
}

void LogWindow::restoreLayout()
{
    Settings& st = Settings::get();
    st.restoreWindow("log.window", *window_);
    if (auto v = st.numbers("log.panes"); v.size() == 3) splitter_->setSizes(v);
    if (auto v = st.numbers("log.columns"); !v.empty()) commitList_->setColumnWidths(v);
    if (auto v = st.numbers("log.files.columns"); !v.empty()) fileList_->setColumnWidths(v);
    std::string visible = st.value("log.columns.visible");
    for (size_t i = 0; i < visible.size() && i < LogModel::ColumnCount; ++i)
        commitList_->setColumnVisible(i, visible[i] == '1');

    auto flag = [&](const std::string& key, bool fallback)
    {
        std::string v = st.value(key);
        return v.empty() ? fallback : v == "1";
    };
    auto number = [&](const std::string& key, long fallback)
    {
        std::string v = st.value(key);
        return v.empty() ? fallback : std::strtol(v.c_str(), nullptr, 10);
    };
    walkOptions_.allBranches = flag("log.allBranches." + repo_.workdir(), false);
    walkOptions_.firstParent = flag("log.walk.firstParent", false);
    walkOptions_.noMerges = flag("log.walk.noMerges", false);
    allBranches_->setState(walkOptions_.allBranches ? rap::CheckState::Checked
                                                    : rap::CheckState::Unchecked);
    walk_->setPushed(walkOptions_.firstParent || walkOptions_.noMerges);
    updateRefButton();
    log_.setLabels(unsigned(number("log.view.labels", LabelAll)) & LabelAll);
    showUnversioned_ = flag("log.view.unversioned", true);
    filter_.fields = unsigned(number("log.filter.fields", FilterAll)) & FilterAll;
    if (filter_.fields == 0) filter_.fields = FilterAll;
    filter_.regex = flag("log.filter.regex", false);
    filter_.caseSensitive = flag("log.filter.caseSensitive", false);
    search_->setPlaceholder(filterCue());
    jumpType_->setCurrent(int(std::clamp(number("log.jumpType", 0), 0L, 9L)));
}

// While loading, a progress bar replaces Statistics, Walk Behavior and View
// [spec 1.2].
void LogWindow::reload()
{
    if (log_.rowCount())
    {
        KeptSelection keep;
        for (size_t row : commitList_->selection())
            if (log_.isWorkingTree(row))
                keep.workingTree = true;
            else if (const Commit* c = log_.commit(row))
                keep.commits.push_back(c->id);
        if (auto cur = commitList_->current(); cur && !log_.isWorkingTree(*cur))
            if (const Commit* c = log_.commit(*cur)) keep.current = c->id;
        if (!keep.commits.empty() || keep.workingTree) kept_ = std::move(keep);
    }
    statistics_->setVisible(false);
    walk_->setVisible(false);
    view_->setVisible(false);
    progress_->setVisible(true);
    pulseTimer_ = ui_.timer(100, true, [this] { progress_->pulse(); });
    status_->setText("Loading…");

    WalkOptions opts = walkOptions_;
    opts.since = since_;
    opts.until = until_;
    log_.load(path_, opts, [this](const std::string& e) { loaded(e); });
    if (!filter_.text.empty()) log_.setFilter(filter_);
}

void LogWindow::loaded(const std::string& error)
{
    pulseTimer_.reset();
    progress_->setVisible(false);
    statistics_->setVisible(true);
    walk_->setVisible(true);
    view_->setVisible(true);
    if (!error.empty())
    {
        status_->setText("Error: " + error);
        return;
    }

    // Without explicit limits the pickers show the range actually loaded.
    size_t first = log_.rowCount() - log_.commitRows();
    if (log_.commitRows() && !since_ && !until_)
    {
        from_->setDate(localDate(log_.commit(log_.rowCount() - 1)->time));
        to_->setDate(localDate(log_.commit(first)->time));
    }
    restoreSelection();
    if (log_.rowCount() && !commitList_->current()) commitList_->setCurrent(0);
    updateStatus();
}

void LogWindow::restoreSelection()
{
    if (!kept_) return;
    KeptSelection keep = std::move(*kept_);
    kept_.reset();
    std::unordered_set<Oid, OidHash> wanted(keep.commits.begin(), keep.commits.end());
    std::vector<size_t> rows;
    std::optional<size_t> current;
    for (size_t row = 0; row < log_.rowCount(); ++row)
    {
        const Commit* c = log_.commit(row);
        if (log_.isWorkingTree(row) ? keep.workingTree : c && wanted.contains(c->id))
            rows.push_back(row);
        if (c && keep.current && c->id == *keep.current) current = row;
    }
    if (rows.empty()) return;  // gone (a deleted branch's commits): start at the top
    if (!current) current = rows.front();
    commitList_->setCurrent(*current);
    commitList_->select(rows);
    commitList_->scrollTo(*current);
}

void LogWindow::selectionChanged()
{
    auto sel = commitList_->selection();
    if (sel.size() == 1)
    {
        showCommit(sel.front());
        if (const Commit* c = log_.commit(sel.front()); c && !navigating_)
        {
            history_.resize(std::min(history_.size(), historyPos_ + 1));
            if (history_.empty() || history_.back() != c->id) history_.push_back(c->id);
            historyPos_ = history_.size() - 1;
        }
    }
    else
    {
        details_->setText("");
        files_.set({});
    }
    updateStatus();
}

void LogWindow::showCommit(size_t row)
{
    if (log_.isWorkingTree(row))
    {
        // [spec 1.8]: "Fetching Status..." until the status arrives.
        if (!log_.workingTreeKnown())
        {
            details_->setText("Working tree changes\n\nFetching status...");
            files_.set({});
            return;
        }
        details_->setText("Working tree changes\n\n" +
                          std::to_string(log_.workingTreeFiles().size()) + " files changed");
        files_.set(log_.workingTreeFiles(), showUnversioned_);
        return;
    }

    const Commit& c = *log_.commit(row);
    std::string text;
    std::vector<rap::TextView::Span> spans;
    auto line = [&](const std::string& label, const std::string& value)
    {
        spans.push_back({text.size(), text.size() + label.size(), {.bold = true}});
        text += label + value + "\n";
    };
    line("SHA-1: ", c.id.hex());
    line("Author: ", c.author + " <" + c.email + ">");
    line("Date: ", formatTime(c.time, c.offsetMinutes));
    if (c.committer != c.author || c.commitTime != c.time)
        line("Committer: ", c.committer + " <" + c.committerEmail + "> " +
                                formatTime(c.commitTime, c.commitOffsetMinutes));
    for (auto& p : c.parents) line("Parent: ", p.hex());
    text += "\n" + c.message;
    details_->setText(text, spans);

    // The file list comes from the worker, so the click (and its highlight)
    // never waits for git: the files as soon as the tree diff has them, then
    // their line counts, which read every file (a commit of thousands of
    // files on a slow disk takes many seconds). Clicking elsewhere drops
    // the counting.
    files_.set({});
    Oid id = c.id;
    filesWorker_->submit(
        [this, id](Repo& repo, const RepoWorker::Progress& progress) -> std::function<void()>
        {
            (void)repo.changes(
                id, {.listed =
                         [&](const std::vector<FileChange>& files)
                     {
                         progress.post(
                             [this, files]
                             {
                                 files_.set(files);
                                 updateStatus();
                             });
                     },
                     .counted =
                         [&](size_t first, const std::vector<FileChange>& files)
                     {
                         progress.post(
                             [this, first, files]
                             {
                                 files_.setCounts(first, files);
                                 updateStatus();
                             });
                     },
                     .stop = [&] { return progress.stale(); }});
            return {};
        });
}

// "Showing %1 revision(s), from revision %2 to revision %3 - %4
// revision(s) selected, %5 file(s) selected" plus, for one revision, the
// line and file statistics [spec 1.10].
void LogWindow::updateStatus()
{
    if (log_.loading()) return;
    size_t n = log_.commitRows();
    std::string s = "Showing " + std::to_string(n) + " revision(s)";
    if (n)
    {
        size_t first = log_.rowCount() - n;
        s += ", from revision " + log_.commit(log_.rowCount() - 1)->id.hex(8) + " to revision " +
             log_.commit(first)->id.hex(8);
    }
    auto sel = commitList_->selection();
    s += " - " + std::to_string(sel.size()) + " revision(s) selected, " +
         std::to_string(fileList_->selection().size()) + " file(s) selected";
    if (sel.size() == 1)
    {
        size_t added = 0, removed = 0, mod = 0, add = 0, del = 0, ren = 0;
        for (auto& f : files_.files())
        {
            added += f.added;
            removed += f.removed;
            switch (f.status)
            {
            case 'A':
            case '?': ++add; break;
            case 'D': ++del; break;
            case 'R': ++ren; break;
            default: ++mod;
            }
        }
        s += "; line: " + std::to_string(added) + "(+) " + std::to_string(removed) +
             "(-) files: modified = " + std::to_string(mod) + " added = " + std::to_string(add) +
             " deleted = " + std::to_string(del) + " replaced = " + std::to_string(ren);
    }
    status_->setText(s);
}

// The ref selector shows the current ref, or a mode [spec 1.9.1].
void LogWindow::updateRefButton()
{
    if (walkOptions_.allBranches)
        refButton_->setText("<All Branches>");
    else
    {
        std::string head = repo_.headName();
        refButton_->setText(head == "HEAD" ? "<No branch>" : head);
    }
}

// TODO: TortoiseGit's left click opens Browse References; this offers its
// right-click menu instead.
rap::Menu LogWindow::refMenu()
{
    std::string head = repo_.headName();
    return {
        {.label = "Browse References", .enabled = false},
        {.label = head == "HEAD" ? "HEAD" : "HEAD -> \"" + menuText(head) + "\"",
         .action =
             [this]
         {
             walkOptions_.allBranches = false;
             allBranches_->setState(rap::CheckState::Unchecked);
             updateRefButton();
             reload();
         }},
        {.label = "All",
         .action =
             [this]
         {
             walkOptions_.allBranches = true;
             allBranches_->setState(rap::CheckState::Checked);
             updateRefButton();
             reload();
         }},
        {.label = "All basic refs", .enabled = false},
        {.label = "All local branches", .enabled = false},
    };
}

// Cue text lists the selected fields [spec 1.9.2].
std::string LogWindow::filterCue() const
{
    static const std::pair<unsigned, const char*> fields[] = {
        {FilterSubject, "Subject"}, {FilterMessages, "Messages"}, {FilterPaths, "Paths"},
        {FilterAuthors, "Authors"}, {FilterEmails, "Emails"},     {FilterRevs, "SHA-1"},
        {FilterRefname, "Refname"}};
    std::string cue;
    for (auto [bit, name] : fields)
        if (filter_.fields & bit) cue += std::string(cue.empty() ? "" : ", ") + name;
    return "   Filter by " + cue;
}

rap::Menu LogWindow::filterMenu()
{
    auto field = [this](const char* label, unsigned bit, bool enabled = true)
    {
        return toggle(
            label, filter_.fields & bit,
            [this, bit]
            {
                filter_.fields ^= bit;
                search_->setPlaceholder(filterCue());
                applyFilter();
            },
            enabled);
    };
    return {
        field("Subject", FilterSubject),
        field("Messages", FilterMessages),
        field("Paths", FilterPaths, false),
        field("Authors", FilterAuthors),
        field("Emails", FilterEmails),
        field("SHA-1", FilterRevs),
        field("Refname", FilterRefname),
        rap::MenuItem::separator(),
        {.label = "Toggle filters",
         .action =
             [this]
         {
             filter_.fields = ~filter_.fields & FilterAll;
             search_->setPlaceholder(filterCue());
             applyFilter();
         }},
        {.label = "All",
         .action =
             [this]
         {
             filter_.fields = FilterAll;
             search_->setPlaceholder(filterCue());
             applyFilter();
         }},
        rap::MenuItem::separator(),
        toggle("Use regular expression", filter_.regex,
               [this]
               {
                   filter_.regex = !filter_.regex;
                   applyFilter();
               }),
        toggle("Case-sensitive", filter_.caseSensitive,
               [this]
               {
                   filter_.caseSensitive = !filter_.caseSensitive;
                   applyFilter();
               }),
    };
}

// Typing filters after a second; Enter filters at once [spec 1.9.2].
void LogWindow::scheduleFilter()
{
    filterTimer_ = ui_.timer(1000, false, [this] { applyFilter(); });
}

void LogWindow::applyFilter()
{
    filterTimer_.reset();
    filter_.text = search_->text();
    std::optional<Oid> keep;
    if (auto c = commitList_->current(); c && log_.commit(*c)) keep = log_.commit(*c)->id;

    std::string error = log_.setFilter(filter_);
    if (!error.empty())
    {
        status_->setText("Invalid regular expression: " + error);
        return;
    }
    if (keep)
        if (auto row = log_.rowOf(*keep))
        {
            commitList_->setCurrent(*row);
            commitList_->scrollTo(*row);
        }
    updateStatus();
}

// Walk Behavior [spec 1.9.4]; drawn pushed while any option is on.
rap::Menu LogWindow::walkMenu()
{
    auto flip = [this](bool WalkOptions::* field)
    {
        return [this, field]
        {
            walkOptions_.*field = !(walkOptions_.*field);
            walk_->setPushed(walkOptions_.firstParent || walkOptions_.noMerges);
            reload();
        };
    };
    return {
        toggle("First Parent", walkOptions_.firstParent, flip(&WalkOptions::firstParent)),
        toggle("No _merges", walkOptions_.noMerges, flip(&WalkOptions::noMerges)),
        // TODO: the rest of the walk options.
        toggle("_Follow renames", false, {}, false),
        toggle("Full history", false, {}, false),
        rap::MenuItem::separator(),
        toggle("_Compressed Graph", false, {}, false),
        toggle("Show la_beled commits only", false, {}, false),
    };
}

// View [spec 1.9.5].
rap::Menu LogWindow::viewMenu()
{
    auto iconStyle = [this](const char* text, ActionIconStyle style)
    {
        return toggle(text, log_.actionIconStyle() == style,
                      [this, style]
                      {
                          log_.setActionIconStyle(style);
                          Settings::get().set("log.actionIcons", std::to_string(int(style)));
                      });
    };
    auto label = [this](const char* text, unsigned bit)
    {
        return toggle(text, log_.labels() & bit,
                      [this, bit] { log_.setLabels(log_.labels() ^ bit); });
    };
    return {
        // Unrelated paths only exist once the log can be opened for a path.
        toggle("H_ide Unrelated Changed Paths", false, {}, false),
        toggle("_Gray Unrelated Changed Paths", true, {}, false),
        rap::MenuItem::separator(),
        toggle("Show _Unversioned Files", showUnversioned_,
               [this]
               {
                   showUnversioned_ = !showUnversioned_;
                   selectionChanged();
               }),
        rap::MenuItem::separator(),
        {.label = "_Action Icons",
         .children = {iconStyle("_Classic", ActionIconStyle::Document),
                      iconStyle("_Glyphs", ActionIconStyle::Glyph),
                      iconStyle("_Bevelled", ActionIconStyle::Bevel),
                      iconStyle("L_ine", ActionIconStyle::Line)}},
        {.label = "_Labels",
         .children = {label("_Tags", LabelTags), label("_Local branches", LabelLocal),
                      label("_Remote branches", LabelRemote), label("Other refs", LabelOther)}},
        rap::MenuItem::separator(),
        toggle("G_ravatar", false, {}, false),
        toggle("View Patch", false, {}, false),
        rap::MenuItem::separator(),
        {.label = "_Settings...", .action = [this] { openSettings(SettingsPage::General); }},
    };
}

void LogWindow::openSettings(SettingsPage page)
{
    showSettings(ui_, window_.get(), repo_.workdir(), page,
                 [this]
                 {
                     // What the pages change that's on show here.
                     const int style =
                         std::atoi(Settings::get().value("log.actionIcons", "1").c_str());
                     log_.setActionIconStyle(ActionIconStyle(std::clamp(style, 0, 3)));
                 });
}

// Header right-click: reset, then a check item per column [spec 1.4].
rap::Menu LogWindow::headerMenu()
{
    auto columns = LogModel::columns();
    rap::Menu menu{{.label = "Reset columns",
                      .action =
                          [this]
                      {
                          auto defaults = LogModel::columns();
                          for (size_t i = 0; i < defaults.size(); ++i)
                              commitList_->setColumnVisible(i, defaults[i].visible);
                      }},
                     rap::MenuItem::separator()};
    for (size_t i = 0; i < columns.size(); ++i)
        menu.push_back(
            toggle(columns[i].title, commitList_->columnVisible(i), [this, i]
                   { commitList_->setColumnVisible(i, !commitList_->columnVisible(i)); }));
    return menu;
}

// From is clamped to To and back; changes restart the log [spec 1.9.3].
void LogWindow::datesChanged()
{
    rap::Date from = from_->date(), to = to_->date();
    if (from > to)
    {
        to = from;
        to_->setDate(to);
    }
    since_ = localTime(from, false);
    until_ = localTime(to, true);
    reload();
}

// Up looks toward newer commits for the next row matching the selection
// by the chosen criterion; down the reverse [spec 1.9.6].
void LogWindow::jump(bool up)
{
    auto cur = commitList_->current();
    if (!cur) return;
    auto type = JumpType(jumpType_->current());

    if (type == JumpHistory)
    {
        if (history_.empty()) return;
        if (up ? historyPos_ == 0 : historyPos_ + 1 >= history_.size()) return;
        historyPos_ += up ? -1 : 1;
        if (auto row = log_.rowOf(history_[historyPos_]))
        {
            navigating_ = true;
            commitList_->setCurrent(*row);
            commitList_->scrollTo(*row);
            navigating_ = false;
        }
        return;
    }

    const Commit* sel = log_.commit(*cur);
    if (!sel) return;
    auto hasRef = [&](size_t row, bool tags)
    {
        if (auto* refs = log_.refsOf(row))
            for (auto& r : *refs)
                if (tags ? r.kind == RefKind::Tag
                         : r.kind == RefKind::LocalBranch || r.kind == RefKind::RemoteBranch)
                    return true;
        return false;
    };
    auto matches = [&](size_t row)
    {
        const Commit* c = log_.commit(row);
        if (!c) return false;
        auto parentIs = [](const Commit& child, const Commit& parent, size_t n)
        { return child.parents.size() > n && child.parents[n] == parent.id; };
        switch (type)
        {
        case JumpAuthorEmail: return c->email == sel->email;
        case JumpCommitterEmail: return c->committerEmail == sel->committerEmail;
        case JumpMergePoint: return c->parents.size() > 1;
        case JumpParent1: return up ? parentIs(*c, *sel, 0) : parentIs(*sel, *c, 0);
        case JumpParent2: return up ? parentIs(*c, *sel, 1) : parentIs(*sel, *c, 1);
        case JumpTag: return hasRef(row, true);
        case JumpBranch: return hasRef(row, false);
        case JumpTagFF:
        case JumpBranchFF:
            return hasRef(row, type == JumpTagFF) &&
                   (up ? repo_.isAncestor(sel->id, c->id) : repo_.isAncestor(c->id, sel->id));
        case JumpHistory: break;
        }
        return false;
    };

    if (up)
    {
        for (size_t row = *cur; row-- > 0;)
            if (matches(row))
            {
                commitList_->select({row});
                commitList_->setCurrent(row);
                commitList_->scrollTo(row);
                return;
            }
    }
    else
    {
        for (size_t row = *cur + 1; row < log_.rowCount(); ++row)
            if (matches(row))
            {
                commitList_->select({row});
                commitList_->setCurrent(row);
                commitList_->scrollTo(row);
                return;
            }
    }
    ui_.message(window_.get(), {.icon = rap::MessageSpec::Icon::Info,
                                .title = "Tortilla",
                                .text = "No more revisions found."});
}

}  // namespace tortilla
