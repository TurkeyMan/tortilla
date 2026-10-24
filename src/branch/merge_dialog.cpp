#include "merge_dialog.h"

#include "changes/changes_window.h"
#include "changes/commit_window.h"
#include "changes/file_action_window.h"
#include "git/command.h"
#include "git/remotes.h"
#include "git/repo.h"
#include "i18n.h"
#include "remote/push_dialog.h"
#include "ui/progress_dialog.h"
#include "ui/windows.h"

#include <rapport/sys.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>

namespace tortilla
{
namespace
{

namespace fs = std::filesystem;

constexpr const char* k_strategies[] = {"", "resolve", "recursive", "ours", "subtree"};
constexpr const char* k_strategy_options[] = {"",
                                              "ours",
                                              "theirs",
                                              "patience",
                                              "ignore-space-change",
                                              "ignore-all-space",
                                              "ignore-space-at-eol",
                                              "renormalize",
                                              "no-renormalize",
                                              "rename-threshold",
                                              "subtree"};

// The repository's folder, as the system writes it, for titles.
std::string root_of(std::string workdir)
{
    while (workdir.size() > 1 && (workdir.back() == '/' || workdir.back() == '\\'))
        workdir.pop_back();
    return rap::sys::path_to_utf8(rap::sys::path_from_utf8(workdir).make_preferred());
}

bool has_conflicts(const std::string& workdir)
{
    const git_result r =
        run_git(workdir, {"diff", "--name-only", "--diff-filter=U"}, git_output::stdout_only);
    return r.ok() && r.output.find_first_not_of(" \r\n") != std::string::npos;
}

std::string trimmed(std::string s)
{
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) ++start;
    return s.substr(start);
}

}  // namespace

std::expected<std::unique_ptr<merge_dialog>, std::string>
merge_dialog::open(rap::ui& ui, const std::string& path, const std::string& from)
{
    auto repo = repo::open(path.empty() ? "." : path);
    if (!repo) return std::unexpected(repo.error());
    if (repo->bare()) return std::unexpected(std::string(_("Merge needs a working tree.")));
    const std::string workdir = repo->workdir();
    if (config_value(workdir, "user.name").empty() || config_value(workdir, "user.email").empty())
        return std::unexpected(
            std::string(_("User name and email must be set before commit (git config --global "
                          "user.name, user.email).")));
    std::unique_ptr<merge_dialog> d(new merge_dialog(ui, workdir));
    d->build(from);
    return d;
}

merge_dialog::merge_dialog(rap::ui& ui, std::string workdir)
    : ui_(ui)
    , workdir_(std::move(workdir))
    , from_(workdir_)
{
}

void merge_dialog::build(const std::string& from)
{
    using namespace rap::layout;
    using namespace rap::literals;
    // merge.log's count, else 20 [spec 8.4.3].
    int log_count = std::atoi(config_value(workdir_, "merge.log").c_str());
    if (log_count <= 0) log_count = 20;
    std::vector<std::string> strategies(std::begin(k_strategies), std::end(k_strategies));
    strategies.front() = _("(default)");
    std::vector<std::string> options(std::begin(k_strategy_options), std::end(k_strategy_options));
    options.front() = _("(none)");
    const std::string current = current_branch(workdir_);

    window_ = ui_.create(dialog{
        rap::i18n::format(_("{0} - Merge - Tortilla"), root_of(workdir_)),
        {vbox{
            hbox{label{_("Current branch:")}, label{current.empty() ? _("(no branch)") : current},
                 spacer{}},
            from_.layout({.caption = _("From"), .skip_current = true}),
            group{
                _("Option"),
                vbox{
                    hbox{
                        check{_("_Squash")}.bind(squash_).on_toggled([this](bool) { update_state(); }),
                        check{_("Messages")}.bind(log_).on_toggled([this](bool) { update_state(); }),
                        spin{1, 100000, double(log_count)}.bind(log_count_),
                        spacer{},
                    },
                    hbox{
                        check{_("No _Fast Forward")}.bind(no_ff_).on_toggled([this](bool)
                                                                          { update_state(); }),
                        check{_("Fast Forward O_nly")}.bind(ff_only_).on_toggled([this](bool)
                                                                              { update_state(); }),
                        spacer{},
                    },
                    check{_("No Co_mmit")}.bind(no_commit_),
                    hbox{
                        label{_("Strategy")},
                        drop_down{strategies, 0}.bind(strategy_).on_changed([this](int)
                                                                            { update_state(); }),
                        drop_down{options, 0}
                            .bind(strategy_option_)
                            .on_changed([this](int) { update_state(); }),
                        entry{}.bind(strategy_parameter_).width(10_ch),
                        spacer{},
                    },
                }},
            group{_("Merge Message"), text_view{}.bind(message_).height(6_lines).stretch()}.stretch(),
            buttons{dialog_button{rap::dialog_role::ok, ""}.on_click([this] { ok(); }), cancel},
        }}}.size(70_ch, 30_lines));
    window_->on_finished(
        [this](rap::dialog_role)
        {
            if (closed_) closed_();
        });
    // With nothing chosen, the branch's push target [spec 8.4.3].
    std::string initial = from;
    if (initial.empty() && !current.empty())
    {
        const std::string remote = push_remote_of(workdir_, current);
        const std::string branch = upstream_of(workdir_, current).branch;
        if (!remote.empty() && !branch.empty()) initial = "refs/remotes/" + remote + "/" + branch;
    }
    from_.select(initial);
    update_state();
}

// Squash has no message and excludes No Fast Forward; the two fast-forward
// options exclude each other; a strategy option is recursive's, and a
// parameter only some options' [spec 8.4.3].
void merge_dialog::update_state()
{
    message_->set_enabled(!squash_->checked());
    no_ff_->set_enabled(!squash_->checked() && !ff_only_->checked());
    squash_->set_enabled(!no_ff_->checked());
    ff_only_->set_enabled(!no_ff_->checked());
    log_count_->set_enabled(log_->checked());
    const bool recursive =
        std::string(k_strategies[std::max(strategy_->current(), 0)]) == "recursive";
    strategy_option_->set_enabled(recursive);
    const std::string option = k_strategy_options[std::max(strategy_option_->current(), 0)];
    strategy_parameter_->set_enabled(recursive &&
                                     (option == "rename-threshold" || option == "subtree"));
}

void merge_dialog::ok()
{
    const std::string rev = from_.revision();
    if (rev.empty())
    {
        ui_.message(window_.get(), {.icon = rap::message_spec::icon::warning,
                                    .title = _("Merge"),
                                    .text = _("Branch/Tag name must not be empty or is invalid.")});
        return;
    }
    // [spec 8.4.4]
    git_command c{"merge"};
    if (no_ff_->checked()) c.push_back("--no-ff");
    if (ff_only_->checked()) c.push_back("--ff-only");
    if (squash_->checked()) c.push_back("--squash");
    if (no_commit_->checked()) c.push_back("--no-commit");
    if (log_->checked())
        c.push_back("--log=" + std::to_string(int(std::lround(log_count_->value()))));
    const std::string strategy = k_strategies[std::max(strategy_->current(), 0)];
    if (!strategy.empty())
    {
        c.push_back("--strategy=" + strategy);
        const std::string option = k_strategy_options[std::max(strategy_option_->current(), 0)];
        if (strategy == "recursive" && !option.empty())
        {
            const std::string parameter = trimmed(strategy_parameter_->text());
            c.push_back("--strategy-option=" + option +
                        ((option == "rename-threshold" || option == "subtree") && !parameter.empty()
                             ? "=" + parameter
                             : ""));
        }
    }
    std::error_code ec;
    auto message_file = std::make_shared<fs::path>();
    const std::string message = trimmed(message_->text());
    if (!message.empty() && !squash_->checked())
    {
        const fs::path dir = fs::temp_directory_path(ec) / "tortilla";
        fs::create_directories(dir, ec);
        *message_file =
            dir /
            ("merge-" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".txt");
        std::FILE* f = rap::sys::fopen(*message_file, "wb");
        if (!f || std::fwrite(message.data(), 1, message.size(), f) != message.size())
        {
            if (f) std::fclose(f);
            ui_.message(window_.get(), {.icon = rap::message_spec::icon::error,
                                        .title = _("Merge"),
                                        .text = _("Could not save merge message")});
            return;
        }
        std::fclose(f);
        c.insert(c.end(), {"-F", rap::sys::path_to_utf8(*message_file)});
    }
    c.insert(c.end(), {"--", rev});

    rap::ui& ui = ui_;
    const std::string workdir = workdir_;
    const bool branch = from_.chosen() == ref_chooser::kind::branch;
    const bool local_branch = branch && !from_.name().starts_with("remotes/");
    const bool no_commit = no_commit_->checked() || squash_->checked();
    auto conflicted = [workdir](bool ok) { return !ok && has_conflicts(workdir); };
    std::vector<progress_follow_up> follow_ups{
        {.label = _("_Resolve..."),
         .after_close =
             [&ui, workdir]
         {
             if (auto w = file_action_window::open(ui, file_action::resolve, {workdir}))
                 windows::keep(std::move(*w));
         },
         .offered = conflicted},
        {.label = _("_Commit..."),
         .after_close =
             [&ui, workdir]
         {
             if (auto w = commit_window::open(ui, {workdir})) windows::keep(std::move(*w));
         },
         .offered = [conflicted, no_commit](bool ok)
         { return conflicted(ok) || (ok && no_commit); }},
        {.label = _("_Abort Merge..."),
         .after_close =
             [&ui, workdir]
         {
             if (auto d = abort_merge_dialog::open(ui, workdir)) windows::keep(std::move(*d));
         },
         .offered = conflicted},
        // Unrelated histories: the same merge, allowed [spec 8.4.4].
        {.label = _("_Merge unrelated history"),
         .commands =
             [c]
         {
             git_command m = c;
             m.insert(m.end() - 2, "--allow-unrelated-histories");
             return std::vector<git_command>{m};
         },
         .offered =
             [workdir, rev](bool ok)
         {
             return !ok &&
                    !run_git(workdir, {"merge-base", "HEAD", rev}, git_output::stdout_only).ok();
         }},
        {.label = _("Remove _branch"),
         .after_close =
             [&ui, workdir, rev]
         {
             const size_t asking = windows::hold({});
             ui.message(nullptr,
                        {.icon = rap::message_spec::icon::question,
                         .title = _("Merge"),
                         .text = rap::i18n::format(_("Do you really want to delete \"{0}\"?"), rev),
                         .buttons = {rap::dialog_role::yes, rap::dialog_role::no}},
                        [&ui, workdir, rev, asking](rap::dialog_role r)
                        {
                            if (r == rap::dialog_role::yes)
                                if (git_result d = run_git(workdir, {"branch", "-D", "--", rev});
                                    !d.ok())
                                    ui.message(nullptr, {.icon = rap::message_spec::icon::error,
                                                         .title = _("Merge"),
                                                         .text = d.output});
                            windows::release(asking);
                        });
         },
         .offered = [local_branch, no_commit](bool ok)
         { return ok && !no_commit && local_branch; }},
        {.label = _("Pus_h..."),
         .after_close =
             [&ui, workdir]
         {
             if (auto d = push_dialog::open(ui, workdir)) windows::keep(std::move(*d));
         },
         .offered = [branch, no_commit](bool ok) { return ok && !no_commit && branch; }},
    };
    const size_t running = windows::hold({});
    run_progress_dialog(
        ui_, nullptr, rap::i18n::format(_("{0} - Merge"), root_of(workdir_)), workdir_, {c},
        [&ui, workdir, done = done_, message_file, running](bool ok)
        {
            std::error_code ec;
            if (!message_file->empty()) fs::remove(*message_file, ec);
            if (!ok && has_conflicts(workdir))
                ui.message(nullptr, {.icon = rap::message_spec::icon::info,
                                     .title = _("Merge"),
                                     .text = _("While merging, a conflict in at least one file "
                                               "occurred. After resolving all "
                                               "files, you need to perform a commit in order to "
                                               "complete the merge. If you want "
                                               "to abort the merge, use Abort Merge.")});
            if (done) done();
            windows::release(running);
        },
        std::move(follow_ups));
    // close() only hides it: what closing means (its release) is ours.
    window_->close();
    if (closed_) closed_();
}

std::expected<std::unique_ptr<abort_merge_dialog>, std::string>
abort_merge_dialog::open(rap::ui& ui, const std::string& path)
{
    auto repo = repo::open(path.empty() ? "." : path);
    if (!repo) return std::unexpected(repo.error());
    std::unique_ptr<abort_merge_dialog> d(new abort_merge_dialog(ui, repo->workdir()));
    d->build();
    return d;
}

abort_merge_dialog::abort_merge_dialog(rap::ui& ui, std::string workdir)
    : ui_(ui)
    , workdir_(std::move(workdir))
{
}

// A reset to HEAD of the kind chosen, Merge by default [spec 8.5].
void abort_merge_dialog::build()
{
    using namespace rap::layout;
    window_ = ui_.create(dialog{
        rap::i18n::format(_("{0} - Abort Merge - Tortilla"), root_of(workdir_)),
        {vbox{
            label{_("In order to abort a merge progress a reset (to HEAD) is needed.")},
            group{_("Reset Type"),
                  drop_down{{_("Merge: Resets the index and try to reconstruct the pre-merge state"),
                             _("Mixed: Leave working tree untouched, reset index"),
                             _("Hard: Reset working tree and index (discard all local changes)")},
                            0}
                      .bind(type_)},
            button{_("Show modified files in _working tree")}.on_click(
                [this]
                {
                    if (auto w = changes_window::open(ui_, {workdir_}))
                        windows::keep(std::move(*w));
                }),
            buttons{dialog_button{rap::dialog_role::ok, ""}.on_click([this] { ok(); }), cancel},
        }}}.resizable(false));
    window_->on_finished(
        [this](rap::dialog_role)
        {
            if (closed_) closed_();
        });
}

void abort_merge_dialog::ok()
{
    const int type = type_->current();
    git_command c = type == 0 ? git_command{"reset", "--merge"}
                              : git_command{"reset", type == 1 ? "--mixed" : "--hard",
                                            "--end-of-options", "HEAD", "--"};
    const size_t running = windows::hold({});
    run_progress_dialog(ui_, nullptr, rap::i18n::format(_("{0} - Abort Merge"), root_of(workdir_)),
                        workdir_, {c},
                        [done = done_, running](bool)
                        {
                            if (done) done();
                            windows::release(running);
                        },
                        {{.label = _("_Retry"),
                          .commands = [c] { return std::vector<git_command>{c}; },
                          .offered = [](bool ok) { return !ok; }}});
    // close() only hides it: what closing means (its release) is ours.
    window_->close();
    if (closed_) closed_();
}

}  // namespace tortilla
