#include "switch_dialog.h"

#include "branch/merge_dialog.h"
#include "changes/commit_window.h"
#include "git/command.h"
#include "git/remotes.h"
#include "git/repo.h"
#include "i18n.h"
#include "remote/pull_fetch_dialog.h"
#include "settings.h"
#include "ui/progress_dialog.h"
#include "ui/repo_icon.h"
#include "ui/windows.h"

#include <rapport/sys.h>

namespace tortilla
{
namespace
{

std::string trimmed(std::string s)
{
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) ++start;
    return s.substr(start);
}

}  // namespace

std::expected<std::unique_ptr<switch_dialog>, std::string>
switch_dialog::open(rap::ui& ui, const std::string& path, const std::string& target)
{
    auto repo = repo::open(path.empty() ? "." : path);
    if (!repo) return std::unexpected(repo.error());
    if (repo->bare()) return std::unexpected(std::string(_("Switch/Checkout needs a working tree.")));
    std::unique_ptr<switch_dialog> d(new switch_dialog(ui, repo->workdir()));
    d->build(target);
    return d;
}

switch_dialog::switch_dialog(rap::ui& ui, std::string workdir)
    : ui_(ui)
    , workdir_(std::move(workdir))
    , target_(workdir_)
{
}

void switch_dialog::build(const std::string& target)
{
    using namespace rap::layout;
    using namespace rap::literals;
    std::string root = workdir_;
    while (root.size() > 1 && (root.back() == '/' || root.back() == '\\')) root.pop_back();
    root = rap::sys::path_to_utf8(rap::sys::path_from_utf8(root).make_preferred());
    window_ = create_window(
        ui_,
        dialog{
            rap::i18n::format(_("{0} - Switch/Checkout - Tortilla"), root),
            {vbox{
                target_.layout({.caption = _("Switch To"),
                                .ui = &ui_,
                                .window = [this] { return window_.get(); }}),
                group{_("Option"),
                      vbox{
                          hbox{
                              check{_("Create _New Branch")}
                                  .bind(new_branch_)
                                  .on_toggled([this](bool) { update_state(); }),
                              entry{}.bind(name_).stretch().on_changed(
                                  [this](const std::string& n)
                                  {
                                      // Renamed away from the remote branch: no tracking [spec 8.3.2].
                                      if (target_.name().starts_with("remotes/") &&
                                          !target_.name().ends_with("/" + n))
                                          track_->set_checked(false);
                                  }),
                          },
                          hbox{
                              check{_("Overwrite working tree changes (_force)")}.bind(force_),
                              check{_("_Merge")}.bind(merge_),
                              spacer{},
                          },
                          check{_("T_rack")}.bind(track_),
                          check{_("_Override branch if exists")}.bind(override_),
                      }},
                buttons{dialog_button{rap::dialog_role::ok, ""}.on_click([this] { ok(); }), cancel},
            }}});
    window_->on_finished(
        [this](rap::dialog_role)
        {
            if (closed_) closed_();
        });
    target_.on_changed([this] { target_changed(); });
    target_.select(target);
}

// A remote branch becomes a new branch of its name, tracking it; anything
// else suggests Branch_<name>, new by default for tags and commits
// [spec 8.3.2].
void switch_dialog::target_changed()
{
    const std::string name = target_.name();
    const ref_chooser::kind kind = target_.chosen();
    if (kind == ref_chooser::kind::branch && name.starts_with("remotes/"))
    {
        const size_t slash = name.find('/', 8);
        name_->set_text(slash == std::string::npos ? name : name.substr(slash + 1));
        new_branch_->set_checked(true);
        track_->set_enabled(true);
        track_->set_checked(true);
    }
    else
    {
        std::string short_name = name;
        if (kind == ref_chooser::kind::commit) short_name = short_name.substr(0, 8);
        name_->set_text("Branch_" + short_name);
        const std::string key = kind == ref_chooser::kind::tag      ? "switch.tagNewBranch"
                                : kind == ref_chooser::kind::commit ? "switch.commitNewBranch"
                                                                    : "";
        new_branch_->set_checked(!key.empty() && settings::get().value(key, "1") == "1");
        track_->set_checked(false);
        track_->set_enabled(false);
    }
    update_state();
}

void switch_dialog::update_state()
{
    const bool make = new_branch_->checked();
    name_->set_enabled(make);
    override_->set_enabled(make);
    if (!make)
    {
        override_->set_checked(false);
        track_->set_checked(false);
        track_->set_enabled(false);
    }
}

void switch_dialog::ok()
{
    const bool make = new_branch_->checked();
    const std::string name = trimmed(name_->text());
    auto refuse = [this](const std::string& text)
    {
        ui_.message(
            window_.get(),
            {.icon = rap::message_spec::icon::warning, .title = _("Switch/Checkout"), .text = text});
    };
    if (make)
    {
        // [spec 8.3.3]
        if (name.empty() ||
            !run_git(workdir_, {"check-ref-format", "--branch", name}, git_output::stdout_only)
                 .ok())
            return refuse(_("Branch/Tag name must not be empty or is invalid."));
        if (!override_->checked() &&
            run_git(workdir_, {"show-ref", "--verify", "--quiet", "refs/heads/" + name},
                    git_output::stdout_only)
                .ok())
            return refuse(_("A branch with this name already exists. Use a different name or use the "
                            "\"Override branch\" "
                            "option."));
    }
    if (target_.name().empty()) return refuse(_("Choose what to switch to."));
    const ref_chooser::kind kind = target_.chosen();
    if (kind == ref_chooser::kind::tag || kind == ref_chooser::kind::commit)
        settings::get().set(kind == ref_chooser::kind::tag ? "switch.tagNewBranch"
                                                           : "switch.commitNewBranch",
                            make ? "1" : "0");

    // refs/heads/ goes unless overriding: checkout of the full name would
    // detach HEAD [spec 8.3.4].
    std::string target = target_.revision();
    if (target.starts_with("refs/heads/") && !override_->checked()) target.erase(0, 11);
    git_command c{"checkout"};
    if (force_->checked()) c.push_back("-f");
    if (make && target_.name().starts_with("remotes/"))
        c.push_back(track_->checked() ? "--track" : "--no-track");
    if (make) c.insert(c.end(), {override_->checked() ? "-B" : "-b", name});
    if (merge_->checked()) c.push_back("--merge");
    c.insert(c.end(), {"--end-of-options", target, "--"});

    rap::ui& ui = ui_;
    const std::string workdir = workdir_;
    const std::string before = current_branch(workdir_);
    const bool merging = merge_->checked();
    std::vector<progress_follow_up> follow_ups{
        {.label = _("_Merge..."),
         .after_close =
             [&ui, workdir, before]
         {
             if (auto d = merge_dialog::open(ui, workdir, before)) windows::keep(std::move(*d));
         },
         .offered = [before](bool ok) { return ok && !before.empty(); }},
        {.label = _("_Pull..."),
         .after_close =
             [&ui, workdir]
         {
             if (auto d = pull_fetch_dialog::open(ui, workdir, true)) windows::keep(std::move(*d));
         },
         .offered = [workdir](bool ok) { return ok && !current_branch(workdir).empty(); }},
        {.label = _("_Commit..."),
         .after_close =
             [&ui, workdir]
         {
             if (auto w = commit_window::open(ui, {workdir})) windows::keep(std::move(*w));
         },
         .offered = [merging, workdir](bool ok)
         { return ok || (merging && has_conflicts(workdir)); }},
        {.label = _("Checkout with merge"),
         .commands =
             [c]
         {
             git_command m = c;
             m.insert(m.end() - 3, "--merge");
             return std::vector<git_command>{m};
         },
         .offered = [merging](bool ok) { return !ok && !merging; }},
        {.label = _("_Retry"),
         .commands = [c] { return std::vector<git_command>{c}; },
         .offered = [](bool ok) { return !ok; }},
    };
    const size_t running = windows::hold({});
    std::string root = workdir_;
    while (root.size() > 1 && (root.back() == '/' || root.back() == '\\')) root.pop_back();
    root = rap::sys::path_to_utf8(rap::sys::path_from_utf8(root).make_preferred());
    run_progress_dialog(
        ui_, nullptr, rap::i18n::format(_("{0} - Switch/Checkout"), root), workdir_, {c},
        [done = done_, running](bool)
        {
            if (done) done();
            windows::release(running);
        },
        std::move(follow_ups));
    // close() only hides it: what closing means (its release) is ours.
    window_->close();
    if (closed_) closed_();
}

}  // namespace tortilla
