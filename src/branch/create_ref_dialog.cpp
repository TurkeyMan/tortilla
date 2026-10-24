#include "create_ref_dialog.h"

#include "git/command.h"
#include "git/remotes.h"
#include "git/repo.h"
#include "remote/push_dialog.h"
#include "settings.h"
#include "ui/progress_dialog.h"
#include "ui/windows.h"

#include <rapport/sys.h>

#include <chrono>
#include <cstdio>
#include <filesystem>

namespace tortilla
{
namespace
{

namespace fs = std::filesystem;

std::string trimmed(std::string s)
{
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) ++start;
    return s.substr(start);
}

bool ref_exists(const std::string& workdir, const std::string& ref)
{
    return run_git(workdir, {"show-ref", "--verify", "--quiet", ref}, git_output::stdout_only).ok();
}

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
    const bool bare =
        run_git(workdir_, {"rev-parse", "--is-bare-repository"}, git_output::stdout_only)
            .output.starts_with("true");
    const bool can_sign = !config_value(workdir_, "user.signingkey").empty();
    window_ = ui_.create(dialog{
        tag_ ? "Create Tag" : "Create Branch",
        {vbox{
            group{"Name", entry{name}.bind(name_).width(40_ch).on_changed([this](const std::string&)
                                                                          { name_changed(); })},
            base_.layout({.caption = "Base On", .head = true}),
            group{"Options",
                  hbox{
                      check{"Trac_k"}.bind(track_).visible(!tag_),
                      check{"_Force"}.bind(force_),
                      check{"_Switch to new branch",
                            switch_to || settings::get().value("branch.switchTo") == "1"}
                          .bind(switch_)
                          .visible(!tag_ && !bare),
                      check{"Si_gn"}.bind(sign_).visible(tag_).enabled(can_sign),
                      check{"_Push", settings::get().value("tag.push") == "1"}.bind(push_).visible(
                          tag_),
                      spacer{},
                  }},
            group{tag_ ? "Message (makes an annotated tag)" : "Description",
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
    // (Tracking is offered, and on, for a remote branch.)
    track_->set_enabled(remote && !tag_);
    track_->set_checked(remote && !tag_);
    if (!remote) return;
    const size_t slash = base.find('/', 8);
    const std::string suggested = slash == std::string::npos ? "" : base.substr(slash + 1);
    if (name_->text().empty() || name_->text() == auto_name_)
    {
        auto_name_ = suggested;
        name_->set_text(suggested);
    }
}

void create_ref_dialog::name_changed()
{
    const std::string base = base_.name();
    if (base_.chosen() == ref_chooser::kind::branch && base.starts_with("remotes/") &&
        !base.ends_with("/" + name_->text()))
        track_->set_checked(false);
}

void create_ref_dialog::ok()
{
    const std::string name = trimmed(name_->text());
    const std::string message = trimmed(message_->text());
    auto refuse = [this](const std::string& text)
    {
        ui_.message(window_.get(), {.icon = rap::message_spec::icon::warning,
                                    .title = tag_ ? "Create Tag" : "Create Branch",
                                    .text = text});
    };
    if (tag_ && sign_->checked() && message.empty())
        return refuse("You must enter a log message for the commit");
    // [spec 8.2.3]
    const bool valid =
        !name.empty() && name.find_first_of("\"|<>") == std::string::npos &&
        run_git(workdir_, {"check-ref-format", tag_ ? "refs/tags/" + name : "refs/heads/" + name},
                git_output::stdout_only)
            .ok();
    if (!valid) return refuse("Branch/Tag name must not be empty or is invalid.");
    if (!force_->checked() && ref_exists(workdir_, (tag_ ? "refs/tags/" : "refs/heads/") + name))
    {
        if (!tag_ && name == base_.current_branch())
            return refuse(
                "Cannot force updating current branch. Use reset command if you want to do so.");
        return refuse(std::string(tag_ ? "A tag" : "A branch") +
                      " with this name already exists. Use a different name or use the \"Force\" "
                      "option to overwrite it.");
    }
    if (ref_exists(workdir_, (tag_ ? "refs/heads/" : "refs/tags/") + name))
    {
        ui_.message(
            window_.get(),
            {.icon = rap::message_spec::icon::warning,
             .title = tag_ ? "Create Tag" : "Create Branch",
             .text = tag_ ? "A branch with the same name as your entered tag name already exists "
                            "(so the short ref "
                            "is ambiguous).\nThis can cause problems so you should avoid it."
                          : "A tag with the same name as your entered branch name already exists "
                            "(so the short ref "
                            "is ambiguous).\nThis can cause problems so you should avoid it.",
             .detail = "Continue anyway?",
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
            const fs::path dir = fs::temp_directory_path(ec) / "tortilla";
            fs::create_directories(dir, ec);
            message_file =
                dir / ("tag-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                       ".txt");
            if (std::FILE* f = rap::sys::fopen(message_file, "wb"))
            {
                std::fwrite(message.data(), 1, message.size(), f);
                std::fclose(f);
            }
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
        ui_.message(window_.get(), {.icon = rap::message_spec::icon::error,
                                    .title = tag_ ? "Create Tag" : "Create Branch",
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
            run_progress_dialog(ui_, nullptr, "Switch/Checkout", workdir_,
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
