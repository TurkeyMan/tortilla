#include "create_ref_dialog.h"

#include "git/command.h"
#include "git/remotes.h"
#include "git/repo.h"
#include "git/temp_files.h"
#include "i18n.h"
#include "remote/push_dialog.h"
#include "settings.h"
#include "ui/modal_dialogs.h"
#include "ui/progress_dialog.h"
#include "ui/repo_icon.h"
#include "ui/windows.h"
#include "util/strings.h"

#include <rapport/sys.h>

#include <cstdio>
#include <filesystem>

namespace tortilla
{
namespace
{

namespace fs = std::filesystem;

}  // namespace

std::expected<std::unique_ptr<create_ref_dialog>, std::string>
create_ref_dialog::open(rap::ui& ui, const std::string& path, bool tag, const std::string& base,
                        const std::string& name, bool switch_to)
{
    auto repo = repo::open(path.empty() ? "." : path);
    if (!repo) return std::unexpected(repo.error());
    std::unique_ptr<create_ref_dialog> d(new create_ref_dialog(
        ui, repo->workdir().empty() ? repo->git_dir() : repo->workdir(), tag));
    d->build(base, name, switch_to);
    return d;
}

create_ref_dialog::create_ref_dialog(rap::ui& ui, std::string workdir, bool tag)
    : ui_(ui)
    , workdir_(std::move(workdir))
    , tag_(tag)
    , base_(workdir_)
{
}

void create_ref_dialog::build(const std::string& base, const std::string& name, bool switch_to)
{
    using namespace rap::layout;
    using namespace rap::literals;
    const auto opened = repo::open(workdir_);
    const bool bare =
        opened ? opened->bare()
               : run_git(workdir_, {"rev-parse", "--is-bare-repository"}, git_output::stdout_only)
                     .output.starts_with("true");
    const bool can_sign = !config_value(workdir_, "user.signingkey").empty();
    window_ = create_window(
        ui_,
        dialog{
            tag_ ? _("Create Tag") : _("Create Branch"),
            {vbox{
                group{_("Name"), entry{name}.bind(name_).width(40_ch).on_changed(
                                     [this](const std::string&) { name_changed(); })},
                base_.layout({.caption = _("Base On"),
                              .head = true,
                              .ui = &ui_,
                              .window = [this] { return window_.get(); }}),
                group{_("Options"),
                      hbox{
                          check{_("Trac_k")}.bind(track_).visible(!tag_),
                          check{_("_Force")}.bind(force_),
                          check{_("_Switch to new branch"),
                                switch_to || settings::get().value("branch.switchTo") == "1"}
                              .bind(switch_)
                              .visible(!tag_ && !bare),
                          check{_("Si_gn")}.bind(sign_).visible(tag_).enabled(can_sign),
                          check{_("_Push"), settings::get().value("tag.push") == "1"}
                              .bind(push_)
                              .visible(tag_),
                          spacer{},
                      }},
                group{tag_ ? _("Message (makes an annotated tag)") : _("Description"),
                      text_view{}.bind(message_).height(4_lines).stretch()},
                buttons{dialog_button{rap::dialog_role::ok, ""}.on_click([this] { ok(); }), cancel},
            }}});
    window_->on_finished(
        [this](rap::dialog_role)
        {
            if (closed_) closed_();
        });
    base_.on_changed([this] { base_changed(); });
    base_.select(base);
}

void create_ref_dialog::show()
{
    window_->show();
    name_->focus();
}

// A remote branch names the new one, unless a name was typed; tracking is
// for remote branches [spec 8.2.2].
void create_ref_dialog::base_changed()
{
    const std::string base = base_.name();
    const bool remote = base_.chosen() == ref_chooser::kind::branch && base.starts_with("remotes/");
    // (Tracking is offered for a remote branch, and on only for its namesake:
    // a feature branch started from origin/main isn't main's.)
    track_->set_enabled(remote && !tag_);
    if (!remote)
    {
        track_->set_checked(false);
        return;
    }
    const size_t slash = base.find('/', 8);
    const std::string suggested = slash == std::string::npos ? "" : base.substr(slash + 1);
    if (name_->text().empty() || name_->text() == auto_name_)
    {
        auto_name_ = suggested;
        name_->set_text(suggested);
    }
    track_->set_checked(!tag_ && trimmed(name_->text()) == suggested);
}

void create_ref_dialog::name_changed()
{
    const std::string base = base_.name();
    if (base_.chosen() == ref_chooser::kind::branch && base.starts_with("remotes/") && !tag_)
        track_->set_checked(base.ends_with("/" + trimmed(name_->text())));
}

void create_ref_dialog::ok()
{
    const std::string name = trimmed(name_->text());
    const std::string message = trimmed(message_->text());
    auto refuse = [this](const std::string& text)
    {
        show_message(ui_, window_.get(),
                     {.icon = rap::message_spec::icon::warning,
                      .title = tag_ ? _("Create Tag") : _("Create Branch"),
                      .text = text});
    };
    if (tag_ && sign_->checked() && message.empty())
        return refuse(_("You must enter a log message for the commit"));
    // [spec 8.2.3]
    const bool valid = !name.empty() && name.find_first_of("\"|<>") == std::string::npos &&
                       valid_ref_name(tag_ ? "refs/tags/" + name : "refs/heads/" + name);
    if (!valid) return refuse(_("Branch/Tag name must not be empty or is invalid."));
    if (!force_->checked() && ref_exists(workdir_, (tag_ ? "refs/tags/" : "refs/heads/") + name))
    {
        if (!tag_ && name == base_.current_branch())
            return refuse(
                _("Cannot force updating current branch. Use reset command if you want to do so."));
        return refuse(tag_ ? _("A tag with this name already exists. Use a different name or use the "
                               "\"Force\" option to overwrite it.")
                           : _("A branch with this name already exists. Use a different name or use "
                               "the \"Force\" option to overwrite it."));
    }
    if (ref_exists(workdir_, (tag_ ? "refs/heads/" : "refs/tags/") + name))
    {
        show_message(
            ui_, window_.get(),
            {.icon = rap::message_spec::icon::warning,
             .title = tag_ ? _("Create Tag") : _("Create Branch"),
             .text = tag_ ? _("A branch with the same name as your entered tag name already exists "
                              "(so the short ref "
                              "is ambiguous).\nThis can cause problems so you should avoid it.")
                          : _("A tag with the same name as your entered branch name already exists "
                              "(so the short ref "
                              "is ambiguous).\nThis can cause problems so you should avoid it."),
             .detail = _("Continue anyway?"),
             .buttons = {rap::dialog_role::yes, rap::dialog_role::no}},
            [this](rap::dialog_role r)
            {
                if (r == rap::dialog_role::yes) create();
            });
        return;
    }
    create();
}

// git branch or git tag at once; then the switch to the new branch in the
// progress dialog, the description, or the push [spec 8.2.4].
void create_ref_dialog::create()
{
    const std::string name = trimmed(name_->text()), base = base_.revision();
    std::string message = message_->text();
    std::erase(message, '\r');
    message = trimmed(message);
    git_result r;
    std::error_code ec;
    fs::path message_file;
    if (tag_)
    {
        std::vector<std::string> args{"tag"};
        if (force_->checked()) args.push_back("-f");
        if (sign_->checked()) args.push_back("-s");
        if (!message.empty())
        {
            message_file = unique_temp_path("tag", ".txt");
            write_file(message_file, message);  // if not, git says it can't read it
            args.insert(args.end(), {"-F", rap::sys::path_to_utf8(message_file)});
        }
        args.insert(args.end(), {"--", name, base});
        r = run_git(workdir_, args);
        if (!message_file.empty()) fs::remove(message_file, ec);
    }
    else
    {
        std::vector<std::string> args{"branch"};
        if (force_->checked()) args.push_back("-f");
        if (base_.chosen() == ref_chooser::kind::branch && base_.name().starts_with("remotes/"))
            args.push_back(track_->checked() ? "--track" : "--no-track");
        args.insert(args.end(), {"--", name, base});
        r = run_git(workdir_, args);
    }
    if (!r.ok())
    {
        show_message(ui_, window_.get(),
                     {.icon = rap::message_spec::icon::error,
                      .title = tag_ ? _("Create Tag") : _("Create Branch"),
                      .text = r.output});
        return;
    }
    settings::get().set(tag_ ? "tag.push" : "branch.switchTo",
                        (tag_ ? push_ : switch_)->checked() ? "1" : "0");
    if (!tag_)
    {
        if (!message.empty())
            run_git(workdir_, {"config", "branch." + name + ".description", message});
        if (switch_->checked())
        {
            const size_t running = windows::hold({});
            run_progress_dialog(ui_, nullptr, _("Switch/Checkout"), workdir_,
                                {{"checkout", "--end-of-options", name, "--"}},
                                [done = done_, running](bool)
                                {
                                    if (done) done();
                                    windows::release(running);
                                });
        }
        else if (done_)
            done_();
    }
    else
    {
        if (done_) done_();
        if (push_->checked())
            if (auto d = push_dialog::open(ui_, workdir_, "refs/tags/" + name))
                windows::keep(std::move(*d));
    }
    // close() only hides it: what closing means (its release) is ours.
    window_->close();
    if (closed_) closed_();
}

}  // namespace tortilla
