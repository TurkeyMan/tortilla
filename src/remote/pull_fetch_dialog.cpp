#include "pull_fetch_dialog.h"

#include "changes/commit_window.h"
#include "changes/file_action_window.h"
#include "git/command.h"
#include "git/remotes.h"
#include "git/repo.h"
#include "i18n.h"
#include "log/log_window.h"
#include "rebase/rebase_window.h"
#include "settings.h"
#include "ui/compare.h"
#include "ui/progress_dialog.h"
#include "ui/settings_dialog.h"
#include "ui/windows.h"

#include <rapport/sys.h>

#include <cmath>
#include <filesystem>

namespace tortilla
{
namespace
{

namespace fs = std::filesystem;

// Shown translated; k_all is also what the settings remember for "all".
constexpr const char* k_url_item = T_("Arbitrary URL...");
constexpr const char* k_all = T_("- all -");

bool has_conflicts(const std::string& workdir)
{
    const git_result r =
        run_git(workdir, {"diff", "--name-only", "--diff-filter=U"}, git_output::stdout_only);
    return r.ok() && r.output.find_first_not_of(" \r\n") != std::string::npos;
}

// "<path> - Pull - Tortilla": `what` with the path for {0}.
std::string title(const std::string& workdir, const char* what)
{
    std::string root = workdir;
    while (root.size() > 1 && (root.back() == '/' || root.back() == '\\')) root.pop_back();
    return rap::i18n::format(what, rap::sys::path_to_utf8(rap::sys::path_from_utf8(root).make_preferred()));
}

// The URLs typed before, newest first (TortoiseGit's History\PullURLS).
std::vector<std::string> url_history()
{
    std::vector<std::string> out;
    const std::string s = settings::get().value("pull.urls");
    for (size_t at = 0; at < s.size();)
    {
        size_t end = s.find('\t', at);
        if (end == std::string::npos) end = s.size();
        if (end > at) out.push_back(s.substr(at, end - at));
        at = end + 1;
    }
    return out;
}

void remember_url(const std::string& url)
{
    std::vector<std::string> urls = url_history();
    std::erase(urls, url);
    urls.insert(urls.begin(), url);
    if (urls.size() > 25) urls.resize(25);
    std::string s;
    for (const std::string& u : urls) s += (s.empty() ? "" : "\t") + u;
    settings::get().set("pull.urls", s);
}

}  // namespace

std::expected<std::unique_ptr<pull_fetch_dialog>, std::string>
pull_fetch_dialog::open(rap::ui& ui, const std::string& path, bool pull, const std::string& remote)
{
    auto repo = repo::open(path.empty() ? "." : path);
    if (!repo) return std::unexpected(repo.error());
    if (pull && repo->bare()) return std::unexpected(std::string(_("Pull needs a working tree.")));
    std::unique_ptr<pull_fetch_dialog> d(new pull_fetch_dialog(
        ui, repo->workdir().empty() ? repo->git_dir() : repo->workdir(), pull, remote));
    d->build();
    return d;
}

pull_fetch_dialog::pull_fetch_dialog(rap::ui& ui, std::string workdir, bool pull, std::string remote)
    : ui_(ui)
    , workdir_(std::move(workdir))
    , pull_(pull)
    , branch_(current_branch(workdir_))
    , preselect_(std::move(remote))
    , remotes_(remote_names(workdir_))
{
}

// The drop-down: "- all -" (Fetch, with several remotes), the remotes,
// "Arbitrary URL...".
int pull_fetch_dialog::first_remote() const
{
    return !pull_ && remotes_.size() > 1 ? 1 : 0;
}

bool pull_fetch_dialog::by_url() const
{
    return remote_->current() == first_remote() + int(remotes_.size());
}

bool pull_fetch_dialog::all_remotes() const
{
    return first_remote() == 1 && remote_->current() == 0;
}

std::string pull_fetch_dialog::remote() const
{
    if (by_url()) return url_->text();
    const int i = remote_->current() - first_remote();
    return i >= 0 && i < int(remotes_.size()) ? remotes_[size_t(i)] : "";
}

void pull_fetch_dialog::build()
{
    using namespace rap::layout;
    using namespace rap::literals;
    const std::string key = (pull_ ? "pull." : "fetch.") + workdir_;

    // The remote: the caller's, else the branch's own, else the last used here [spec 7.2.2];
    // Fetch with several remotes offers (and defaults to) all of them.
    std::vector<std::string> items;
    if (!pull_ && remotes_.size() > 1) items.push_back(_(k_all));
    items.insert(items.end(), remotes_.begin(), remotes_.end());
    items.push_back(_(k_url_item));
    const upstream up = upstream_of(workdir_, branch_);
    std::string chosen = !preselect_.empty() ? preselect_
                         : up.remote.empty() ? settings::get().value(key + ".remote")
                                             : up.remote;
    int current = 0;
    for (size_t i = 0; i < remotes_.size(); ++i)
        if (remotes_[i] == chosen) current = first_remote() + int(i);
    if (chosen == k_all && first_remote() == 1) current = 0;
    if (remotes_.empty()) current = int(items.size()) - 1;  // only a URL makes sense

    // The branch: the upstream's, else this branch's own name.
    const std::string remote_branch = !up.branch.empty() ? up.branch : branch_;

    // Pull rebases when the config says git pull would [spec 7.2.3].
    std::string rebase_config =
        branch_.empty() ? "" : config_value(workdir_, "branch." + branch_ + ".rebase");
    if (rebase_config.empty()) rebase_config = config_value(workdir_, "pull.rebase");
    rebase_from_config_ =
        pull_ && !branch_.empty() && (rebase_config == "true" || rebase_config == "merges");
    const bool rebase = rebase_from_config_ || settings::get().value(key + ".rebase") == "1";
    const bool shallow = is_shallow(workdir_);

    window_ = ui_.create(dialog{
        title(workdir_, pull_ ? _("{0} - Pull - Tortilla") : _("{0} - Fetch - Tortilla")),
        {vbox{
            group{_("Remote"),
                  form{
                      {_("Remote"),
                       hbox{drop_down{items, current}.bind(remote_).stretch().on_changed(
                                [this](int) { remote_changed(); }),
                            button{_("_Manage")}.on_click([this] { manage_remotes(); })}},
                      {_("URL"), combo{url_history()}.bind(url_)},
                      {_("Remote branch"),
                       combo{{}, remote_branch}.bind(remote_branch_).width(40_ch)},
                  }},
            group{_("Options"),
                  vbox{
                      hbox{
                          check{_("_Squash")}.bind(squash_).enabled(pull_),
                          check{_("No _Fast Forward")}.bind(no_ff_).enabled(pull_).on_toggled(
                              [this](bool on)
                              {
                                  ff_only_->set_enabled(!on);
                                  if (on) ff_only_->set_checked(false);
                              }),
                          check{_("No Co_mmit")}.bind(no_commit_).enabled(pull_),
                          check{_("Fast Forward O_nly"),
                                pull_ && settings::get().value(key + ".ffonly") == "1"}
                              .bind(ff_only_)
                              .enabled(pull_)
                              .on_toggled(
                                  [this](bool on)
                                  {
                                      no_ff_->set_enabled(!on);
                                      if (on) no_ff_->set_checked(false);
                                  }),
                          spacer{},
                      },
                      hbox{
                          check{_("Depth"), shallow}.bind(depth_).visible(shallow),
                          spin{1, 1e9, 1}.bind(depth_value_).visible(shallow),
                          spacer{},
                      },
                      form{
                          {_("Tags"), drop_down{{_("Default")}, 0}.bind(tags_)},
                          {_("Prune"), drop_down{{_("Default")}, 0}.bind(prune_)},
                      },
                      check{_("_Launch Rebase After Fetch"), rebase}.bind(rebase_),
                  }},
            // OK checks before the dialog goes.
            buttons{dialog_button{rap::dialog_role::ok, ""}.on_click([this] { ok(); }), cancel},
        }}});
    window_->on_finished([this](rap::dialog_role) { finished(); });
    remote_changed();
}

// The URL field for "Arbitrary URL...", the branch field (a named remote's
// fetch updates all its branches), and the config's say on tags and prune
// [spec 7.2.2].
void pull_fetch_dialog::remote_changed()
{
    const bool url = by_url();
    url_->set_visible(url);
    remote_branch_->set_enabled(pull_ || url);
    rebase_->set_enabled(!url && !all_remotes());
    if (url || all_remotes()) rebase_->set_checked(false);

    const std::string r = url || all_remotes() ? "" : remote();
    const std::string tagopt = r.empty() ? "" : config_value(workdir_, "remote." + r + ".tagopt");
    tags_->set_items({tagopt == "--no-tags" ? _("Default (none)")
                      : tagopt == "--tags"  ? _("Default (all)")
                                            : _("Default (reachable)"),
                      _("All (--tags)"), _("None (--no-tags)")});
    std::string prune = r.empty() ? "" : config_value(workdir_, "remote." + r + ".prune");
    if (prune.empty()) prune = config_value(workdir_, "fetch.prune");
    prune_->set_items({prune.empty() ? std::string(_("Default"))
                                     : rap::i18n::format(_("Default ({0})"), prune),
                       _("Prune (--prune)"), _("Don't prune (--no-prune)")});
}

void pull_fetch_dialog::show()
{
    window_->show();
    // With no remote to choose (or a URL chosen), the URL is what's typed.
    if (by_url()) url_->focus();
}

void pull_fetch_dialog::ok()
{
    const bool url = by_url(), all = all_remotes();
    const std::string where = remote();
    if (!all && where.empty())
    {
        ui_.message(window_.get(),
                    {.icon = rap::message_spec::icon::warning,
                     .title = pull_ ? _("Pull") : _("Fetch"),
                     .text = url ? _("Enter the URL to pull from.") : _("Choose a remote.")});
        return;
    }
    const bool rebase = rebase_->checked() && !url && !all;
    std::string branch = remote_branch_->text();
    if (pull_ && rebase && branch.empty())
    {
        ui_.message(window_.get(),
                    {.icon = rap::message_spec::icon::warning,
                     .title = _("Pull"),
                     .text = _("You asked to pull, but did not specify a remote branch.")});
        return;
    }

    // Remembered: the remote, rebase and fast-forward-only [spec 7.2.4].
    const std::string key = (pull_ ? "pull." : "fetch.") + workdir_;
    settings& s = settings::get();
    if (!url) s.set(key + ".remote", all ? k_all : where);
    if (url) remember_url(where);
    s.set(key + ".rebase", rebase_->checked() && !rebase_from_config_ ? "1" : "0");
    if (pull_) s.set(key + ".ffonly", ff_only_->checked() ? "1" : "0");

    std::vector<std::string> options;
    if (depth_->checked())
        options.push_back("--depth=" + std::to_string(int(std::lround(depth_value_->value()))));
    if (prune_->current() == 1) options.push_back("--prune");
    if (prune_->current() == 2) options.push_back("--no-prune");
    if (tags_->current() == 1) options.push_back("--tags");
    if (tags_->current() == 2) options.push_back("--no-tags");

    const upstream up = upstream_of(workdir_, branch_);
    git_command command;
    std::string rebase_onto;  // after the fetch, when rebasing
    if (pull_ && !rebase)
    {
        // A pull of the upstream names no branch, so git updates the
        // remote-tracking ones [spec 7.2.4].
        if (!url && where == up.remote && branch == up.branch) branch.clear();
        command = {"pull", "--progress", "-v", "--no-rebase"};
        command.insert(command.end(), options.begin(), options.end());
        if (no_ff_->checked()) command.push_back("--no-ff");
        if (ff_only_->checked()) command.push_back("--ff-only");
        if (squash_->checked()) command.push_back("--squash");
        if (no_commit_->checked()) command.push_back("--no-commit");
        command.insert(command.end(), {"--", where});
        if (!branch.empty()) command.push_back(branch);
    }
    else
    {
        // Fetch (a named remote fetches all its branches), or Pull's fetch
        // before a rebase [spec 7.2.5, 7.2.6].
        command = {"fetch", "--progress", "-v"};
        if (all) command.insert(command.begin() + 1, "--all");
        command.insert(command.end(), options.begin(), options.end());
        if (!all)
        {
            command.insert(command.end(), {"--", where});
            if (url && !branch.empty()) command.push_back(branch);
        }
        if (rebase)
            rebase_onto = url ? "FETCH_HEAD"
                              : "remotes/" + where + "/" + (!branch.empty() ? branch : up.branch);
    }

    std::vector<progress_follow_up> follow_ups;
    rap::ui& ui = ui_;
    const std::string workdir = workdir_;
    auto keep_log = [&ui, workdir]
    {
        if (auto w = log_window::open(ui, workdir)) windows::keep(std::move(*w));
    };
    const std::string before = resolve(workdir_, "HEAD");
    const bool pulling = pull_ && !rebase;
    if (pulling)
    {
        follow_ups.push_back(
            {.label = _("Pulled _Diff"),
             .after_close =
                 [&ui, workdir, before]
             {
                 const std::string after = resolve(workdir, "HEAD");
                 auto token = std::make_shared<size_t>(0);
                 auto tool = compare_revisions(ui, nullptr, workdir, {before, after}, {}, {},
                                               [token] { windows::release(*token); });
                 *token = windows::hold(tool);
             },
             .offered = [workdir, before](bool ok)
             { return ok && !before.empty() && resolve(workdir, "HEAD") != before; }});
        follow_ups.push_back({.label = _("Pulled _Log"), .after_close = keep_log});
        follow_ups.push_back(
            {.label = _("_Resolve..."),
             .after_close =
                 [&ui, workdir]
             {
                 if (auto w = file_action_window::open(ui, file_action::resolve, {workdir}))
                     windows::keep(std::move(*w));
             },
             .offered = [workdir](bool ok) { return !ok && has_conflicts(workdir); }});
        follow_ups.push_back(
            {.label = _("_Commit..."),
             .after_close =
                 [&ui, workdir]
             {
                 if (auto w = commit_window::open(ui, {workdir})) windows::keep(std::move(*w));
             },
             .offered = [workdir](bool ok) { return !ok && has_conflicts(workdir); }});
    }
    else
    {
        follow_ups.push_back(
            {.label = _("Show _log"), .after_close = keep_log, .offered = [](bool) { return true; }});
        follow_ups.push_back({.label = _("_Rebase..."),
                              .after_close =
                                  [&ui, workdir]
                              {
                                  if (auto w = rebase_window::open(ui, workdir, ""))
                                      windows::keep(std::move(*w));
                              },
                              .offered = [rebase](bool ok) { return ok && !rebase; }});
    }
    follow_ups.push_back({.label = pulling ? _("_Pull...") : _("F_etch..."),
                          .after_close =
                              [&ui, workdir, pull = pull_]
                          {
                              if (auto d = pull_fetch_dialog::open(ui, workdir, pull))
                                  windows::keep(std::move(*d));
                          },
                          .offered = [](bool) { return true; }});

    // The progress dialog keeps the program going while it runs.
    const size_t running = windows::hold({});
    run_progress_dialog(
        ui_, nullptr, title(workdir_, pull_ ? _("{0} - Pull") : _("{0} - Fetch")), workdir_, {command},
        [&ui, workdir, rebase_onto, before, forced = rebase_from_config_, running, done = done_](bool ok)
        {
            if (done) done();
            if (ok && !rebase_onto.empty())
            {
                // Then the rebase onto what came, unless there's nothing to
                // do or a fast-forward does it [spec 7.2.6].
                const std::string onto = resolve(workdir, rebase_onto);
                if (!forced && !onto.empty() && is_ancestor(workdir, onto, "HEAD"))
                {
                    const size_t asking = windows::hold({});
                    ui.message(nullptr,
                               {.icon = rap::message_spec::icon::question,
                                .title = _("Rebase"),
                                .text = _("Current branch is up to date or newer than the fetched "
                                          "branch. Open rebase anyway?"),
                                .buttons = {rap::dialog_role::yes, rap::dialog_role::no}},
                               [&ui, workdir, rebase_onto, asking](rap::dialog_role r)
                               {
                                   if (r == rap::dialog_role::yes)
                                       if (auto w = rebase_window::open(ui, workdir, rebase_onto))
                                           windows::keep(std::move(*w));
                                   windows::release(asking);
                               });
                }
                else if (auto w = rebase_window::open(ui, workdir, rebase_onto))
                    windows::keep(std::move(*w));
            }
            windows::release(running);
        },
        std::move(follow_ups));
    // close() only hides it: what closing means (its release) is ours.
    window_->close();
    finished();
}

void pull_fetch_dialog::finished()
{
    if (closed_) closed_();
}

// Settings' Remotes page [spec 7, "Manage"]; the drop-down after, keeping
// what was chosen when it's still there.
void pull_fetch_dialog::manage_remotes()
{
    show_settings(ui_, window_.get(), workdir_, settings_page::remotes, {},
                  [this] { refresh_remotes(); });
}

void pull_fetch_dialog::refresh_remotes()
{
    const bool url = by_url(), all = all_remotes();
    const int was = remote_->current() - first_remote();
    const std::string name = was >= 0 && was < int(remotes_.size()) ? remotes_[size_t(was)] : "";
    remotes_ = remote_names(workdir_);
    std::vector<std::string> items;
    if (first_remote() == 1) items.push_back(_(k_all));
    items.insert(items.end(), remotes_.begin(), remotes_.end());
    items.push_back(_(k_url_item));
    remote_->set_items(items);
    int current = remotes_.empty() || url ? int(items.size()) - 1 : first_remote();
    if (all && first_remote() == 1) current = 0;
    for (size_t i = 0; i < remotes_.size(); ++i)
        if (remotes_[i] == name) current = first_remote() + int(i);
    remote_->set_current(current);
    remote_changed();
}

}  // namespace tortilla
