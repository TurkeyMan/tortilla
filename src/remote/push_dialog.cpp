#include "push_dialog.h"

#include "git/command.h"
#include "git/remotes.h"
#include "git/repo.h"
#include "i18n.h"
#include "remote/pull_fetch_dialog.h"
#include "settings.h"
#include "ui/progress_dialog.h"
#include "ui/windows.h"

#include <rapport/sys.h>

namespace tortilla
{
namespace
{

// Shown translated; k_all is also what the settings remember for "all".
constexpr const char* k_url_item = T_("Arbitrary URL...");
constexpr const char* k_all = T_("- all -");

// `what` with the repository's folder for {0}.
std::string title(const std::string& workdir, const char* what)
{
    std::string root = workdir;
    while (root.size() > 1 && (root.back() == '/' || root.back() == '\\')) root.pop_back();
    return rap::i18n::format(what, rap::sys::path_to_utf8(rap::sys::path_from_utf8(root).make_preferred()));
}

}  // namespace

std::expected<std::unique_ptr<push_dialog>, std::string>
push_dialog::open(rap::ui& ui, const std::string& path, const std::string& local)
{
    auto repo = repo::open(path.empty() ? "." : path);
    if (!repo) return std::unexpected(repo.error());
    std::unique_ptr<push_dialog> d(
        new push_dialog(ui, repo->workdir().empty() ? repo->git_dir() : repo->workdir()));
    d->build(local);
    return d;
}

push_dialog::push_dialog(rap::ui& ui, std::string workdir)
    : ui_(ui)
    , workdir_(std::move(workdir))
    , remotes_(remote_names(workdir_))
{
}

// The drop-down: "- all -" (with several remotes), the remotes,
// "Arbitrary URL...".
int push_dialog::first_remote() const
{
    return remotes_.size() > 1 ? 1 : 0;
}

bool push_dialog::by_url() const
{
    return remote_->current() == first_remote() + int(remotes_.size());
}

bool push_dialog::all_remotes() const
{
    return first_remote() == 1 && remote_->current() == 0;
}

void push_dialog::build(const std::string& preselected)
{
    using namespace rap::layout;
    using namespace rap::literals;
    const std::string key = "push." + workdir_;
    std::vector<std::string> items;
    if (remotes_.size() > 1) items.push_back(_(k_all));
    items.insert(items.end(), remotes_.begin(), remotes_.end());
    items.push_back(_(k_url_item));
    // The last remote pushed to, until the branch's own says otherwise
    // [spec 7.3.2].
    // (TortoiseGit leaves it blank with several remotes and no say; a
    // drop-down can't be blank, so origin, else the first, never all.)
    const std::string last = settings::get().value(key + ".remote");
    int current = remotes_.empty() ? int(items.size()) - 1 : first_remote();
    for (size_t i = 0; i < remotes_.size(); ++i)
        if (remotes_[i] == "origin") current = first_remote() + int(i);
    for (size_t i = 0; i < remotes_.size(); ++i)
        if (remotes_[i] == last) current = first_remote() + int(i);
    if (last == k_all && first_remote() == 1) current = 0;
    const std::string branch = !preselected.empty() ? preselected : current_branch(workdir_);

    std::string recurse_config = config_value(workdir_, "push.recurseSubmodules");
    window_ = ui_.create(dialog{
        title(workdir_, _("{0} - Push - Tortilla")),
        {vbox{
            group{
                _("Ref"),
                vbox{
                    check{_("_Push all branches"), settings::get().value(key + ".all") == "1"}
                        .bind(all_branches_)
                        .on_toggled([this](bool) { update_state(); }),
                    form{
                        {_("Local"), combo{local_branches(workdir_), branch.empty() ? "HEAD" : branch}
                                      .bind(local_)
                                      .width(40_ch)
                                      .on_changed([this](const std::string&) { local_changed(); })},
                        {_("Remote branch"), combo{}.bind(remote_branch_)},
                    },
                }},
            group{_("Destination"),
                  form{
                      {_("Remote"), drop_down{items, current}.bind(remote_).on_changed(
                                     [this](int) { update_state(); })},
                      {_("URL"), combo{}.bind(url_)},
                  }},
            group{_("Options"),
                  vbox{
                      hbox{
                          check{_("Force _with lease")}
                              .bind(force_with_lease_)
                              .on_toggled([this](bool) { update_state(); }),
                          check{_("_Force")}.bind(force_).on_toggled([this](bool) { update_state(); }),
                          check{_("Include _Tags")}.bind(tags_).on_toggled([this](bool)
                                                                        { update_state(); }),
                          spacer{},
                      },
                      check{_("_Set upstream/track remote branch")}.bind(set_upstream_),
                      form{
                          {_("Recurse submodules"),
                           drop_down{{recurse_config.empty()
                                          ? std::string(_("Default"))
                                          : rap::i18n::format(_("Default ({0})"), recurse_config),
                                      _("None"), _("Check"), _("On demand")},
                                     0}
                               .bind(recurse_)},
                          {_("Push option"), entry{}.bind(push_option_)},
                      },
                  }},
            buttons{dialog_button{rap::dialog_role::ok, ""}.on_click([this] { ok(); }), cancel},
        }}});
    window_->on_finished(
        [this](rap::dialog_role)
        {
            if (closed_) closed_();
        });
    local_changed();
}

// A branch pushes where its config says, to its upstream branch
// [spec 7.3.2]; with no upstream, Set upstream starts ticked.
void push_dialog::local_changed()
{
    const std::string local = local_->text();
    if (const std::string r = push_remote_of(workdir_, local); !r.empty())
        for (size_t i = 0; i < remotes_.size(); ++i)
            if (remotes_[i] == r) remote_->set_current(first_remote() + int(i));
    if (remotes_.size() == 1 && !by_url()) remote_->set_current(0);
    const upstream up = upstream_of(workdir_, local);
    remote_branch_->set_text(up.branch);
    set_upstream_->set_checked(up.remote.empty() && local != "HEAD");
    update_state();
}

// Force and Include Tags exclude Force with lease (and back); all branches
// has no branch to name [spec 7.3.1].
void push_dialog::update_state()
{
    const bool all = all_branches_->checked();
    local_->set_enabled(!all);
    remote_branch_->set_enabled(!all);
    force_with_lease_->set_enabled(!force_->checked() && !tags_->checked());
    force_->set_enabled(!force_with_lease_->checked());
    tags_->set_enabled(!force_with_lease_->checked());
    url_->set_visible(by_url());
    set_upstream_->set_enabled(!by_url());
}

void push_dialog::ok()
{
    const bool url = by_url();
    if (url && url_->text().empty())
    {
        ui_.message(window_.get(), {.icon = rap::message_spec::icon::warning,
                                    .title = _("Push"),
                                    .text = _("Enter the URL to push to.")});
        return;
    }
    if (all_branches_->checked())
    {
        ui_.message(window_.get(),
                    {.icon = rap::message_spec::icon::question,
                     .title = _("Push"),
                     .text = _("Do you really want to push all local branches?"),
                     .buttons = {rap::dialog_role::yes, rap::dialog_role::no}},
                    [this](rap::dialog_role r)
                    {
                        if (r == rap::dialog_role::yes) push();
                    });
        return;
    }
    if (local_->text().empty())
    {
        ui_.message(window_.get(),
                    {.icon = rap::message_spec::icon::question,
                     .title = _("Push"),
                     .text = remote_branch_->text().empty()
                                 ? _("Local and remote branch are empty.\nContinue?")
                                 : _("This results in a remote removal.\nContinue?"),
                     .buttons = {rap::dialog_role::yes, rap::dialog_role::no}},
                    [this](rap::dialog_role r)
                    {
                        if (r == rap::dialog_role::yes) push();
                    });
        return;
    }
    push();
}

// git push, once per remote [spec 7.3.4].
void push_dialog::push()
{
    const bool all = all_branches_->checked(), url = by_url();
    std::vector<std::string> targets;
    if (url)
        targets.push_back(url_->text());
    else if (all_remotes())
        targets = remotes_;
    else
        targets.push_back(remotes_[size_t(remote_->current() - first_remote())]);

    const std::string key = "push." + workdir_;
    settings& s = settings::get();
    if (!url) s.set(key + ".remote", all_remotes() ? k_all : targets.front());
    s.set(key + ".all", all ? "1" : "0");

    std::vector<std::string> options;
    if (tags_->checked() && !all) options.push_back("--tags");
    if (force_->checked()) options.push_back("--force");
    if (force_with_lease_->checked()) options.push_back("--force-with-lease");
    if (set_upstream_->checked() && !url) options.push_back("--set-upstream");
    static const char* const recurse[] = {"", "no", "check", "on-demand"};
    if (recurse_->current() > 0)
        options.push_back(std::string("--recurse-submodules=") + recurse[recurse_->current()]);
    if (!push_option_->text().empty()) options.push_back("--push-option=" + push_option_->text());
    options.push_back("--progress");

    const std::string local = local_->text(), remote_branch = remote_branch_->text();
    std::vector<git_command> commands;
    for (const std::string& target : targets)
    {
        git_command c{"push"};
        if (all) c.push_back("--all");
        c.insert(c.end(), options.begin(), options.end());
        c.insert(c.end(), {"--", target});
        if (!all && !(local.empty() && remote_branch.empty()))
            c.push_back(remote_branch.empty() ? local : local + ":" + remote_branch);
        commands.push_back(c);
        if (all && tags_->checked())
            commands.push_back({"push", "--tags", "--progress", "--", target});
    }

    rap::ui& ui = ui_;
    const std::string workdir = workdir_;
    auto reopen = [&ui, workdir](int which)
    {
        return [&ui, workdir, which]
        {
            if (which == 0)
            {
                if (auto d = push_dialog::open(ui, workdir)) windows::keep(std::move(*d));
            }
            else if (auto d = pull_fetch_dialog::open(ui, workdir, which == 1))
                windows::keep(std::move(*d));
        };
    };
    // After a rejection, Pull or Fetch first; Push again always [spec 7.3.4].
    std::vector<progress_follow_up> follow_ups{
        {.label = _("_Pull..."), .after_close = reopen(1), .offered = [](bool ok) { return !ok; }},
        {.label = _("F_etch..."), .after_close = reopen(2), .offered = [](bool ok) { return !ok; }},
        {.label = _("Pus_h..."), .after_close = reopen(0), .offered = [](bool) { return true; }},
    };
    const size_t running = windows::hold({});
    run_progress_dialog(
        ui_, nullptr, title(workdir_, _("{0} - Push")), workdir_, std::move(commands),
        [running](bool) { windows::release(running); }, std::move(follow_ups));
    // close() only hides it: what closing means (its release) is ours.
    window_->close();
    if (closed_) closed_();
}

}  // namespace tortilla
