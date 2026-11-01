#include "pull_request_dialog.h"

#include "git/command.h"
#include "git/remotes.h"
#include "i18n.h"
#include "settings.h"
#include "ui/progress_dialog.h"
#include "ui/repo_icon.h"
#include "ui/windows.h"

#include <rapport/sys.h>

#include <algorithm>
#include <cctype>
#include <thread>

namespace tortilla
{
namespace
{

std::string trimmed(std::string_view s)
{
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    return std::string(s);
}

// A branch's name as a title: "fix-login_page" -> "Fix login page".
std::string title_from(std::string_view branch)
{
    branch = branch.substr(branch.rfind('/') == std::string_view::npos ? 0 : branch.rfind('/') + 1);
    std::string out(branch);
    for (char& c : out)
        if (c == '-' || c == '_') c = ' ';
    if (!out.empty()) out[0] = char(std::toupper(static_cast<unsigned char>(out[0])));
    return out;
}

}  // namespace

std::expected<std::unique_ptr<pull_request_dialog>, std::string>
pull_request_dialog::open(rap::ui& ui, const std::string& workdir, const ref& branch,
                          std::vector<forge_remote> remotes)
{
    if (remotes.empty())
        return std::unexpected(std::string(
            _("None of this repository's remotes is on a forge you're signed in to: sign in on "
              "Settings > Accounts.")));
    std::unique_ptr<pull_request_dialog> d(new pull_request_dialog(ui, workdir));
    d->remotes_ = std::move(remotes);
    auto remote_index = [&](std::string_view name) -> std::optional<size_t>
    {
        for (size_t i = 0; i < d->remotes_.size(); ++i)
            if (d->remotes_[i].name == name) return i;
        return std::nullopt;
    };
    if (branch.kind == ref_kind::local_branch)
    {
        d->local_ = branch.name;
        d->tip_ = resolve(workdir, "refs/heads/" + branch.name);
        const upstream up = upstream_of(workdir, branch.name);
        if (auto i = up.remote.empty() ? std::nullopt : remote_index(up.remote))
        {
            d->head_remote_ = *i;
            d->head_branch_ = up.branch;
        }
        else
        {
            // Pushed for the first time, where it would push, and tracking it.
            d->head_remote_ = remote_index(push_remote_of(workdir, branch.name))
                                  .or_else([&] { return remote_index("origin"); })
                                  .value_or(0);
            d->head_branch_ = branch.name;
            d->sets_upstream_ = true;
        }
        const std::string pushed = resolve(
            workdir, "refs/remotes/" + d->remotes_[d->head_remote_].name + "/" + d->head_branch_);
        d->on_forge_ = !pushed.empty();
        d->needs_push_ = pushed != d->tip_;
    }
    else
    {
        // origin/feature: the remote whose name it starts with.
        bool found = false;
        for (size_t i = 0; i < d->remotes_.size() && !found; ++i)
            if (branch.name.starts_with(d->remotes_[i].name + "/"))
            {
                d->head_remote_ = i;
                d->head_branch_ = branch.name.substr(d->remotes_[i].name.size() + 1);
                found = true;
            }
        if (!found)
            return std::unexpected(
                rap::i18n::format(_("{0} isn't on a forge you're signed in to."), branch.name));
        d->tip_ = resolve(workdir, branch.full_name);
    }
    d->build();
    return d;
}

pull_request_dialog::pull_request_dialog(rap::ui& ui, std::string workdir)
    : ui_(ui)
    , workdir_(std::move(workdir))
{
}

const forge_remote& pull_request_dialog::into() const
{
    return remotes_[std::clamp<int>(into_->current(), 0, int(remotes_.size()) - 1)];
}

void pull_request_dialog::build()
{
    using namespace rap::layout;
    using namespace rap::literals;
    // Into the repository it was forked from, if there's a remote for it.
    size_t into = head_remote_;
    for (size_t i = 0; i < remotes_.size(); ++i)
        if (remotes_[i].name == "upstream") into = i;
    std::vector<std::string> names;
    for (const forge_remote& r : remotes_) names.push_back(r.name + " (" + r.repo.path() + ")");
    const forge_remote& head = remotes_[head_remote_];
    const std::string from = head.repo.path() + ": " + head_branch_ +
                             (tip_.empty() ? "" : "  (" + tip_.substr(0, 8) + ")");
    window_ = create_window(
        ui_,
        dialog{
            _("Create Pull Request"),
            {vbox{
                form{
                    {_("From"), label{from}},
                    {_("Into"), hbox{drop_down{names, int(into)}.bind(into_).on_changed(
                                         [this](int) { into_changed(); }),
                                     combo{}.bind(base_).width(24_ch).stretch().on_changed(
                                         [this](const std::string&) { base_changed(); })}},
                    {_("Title"), entry{}.bind(title_).width(60_ch)},
                },
                group{_("Description"), text_view{}.bind(body_).height(10_lines).stretch()}
                    .stretch(),
                label{}.bind(commits_),
                // A PR needs its branch on the forge: pushed first, if it isn't;
                // a newer copy here is the user's to push or hold back.
                label{rap::i18n::format(_("{0} will be pushed to {1} first."), local_, head.name)}
                    .visible(!on_forge_),
                hbox{
                    check{rap::i18n::format(_("_Push your newer commits to {0} first"), head.name),
                          true}
                        .bind(push_)
                        .visible(on_forge_ && needs_push_),
                    check{_("_Draft"), settings::get().value("pr.draft") == "1"}
                        .bind(draft_)
                        .visible(head.account.kind != forge_kind::gitea),
                    check{_("Open it in the _browser"),
                          settings::get().value("pr.browse", "1") == "1"}
                        .bind(browse_),
                    spacer{},
                },
                label{}.bind(status_).wrap(),
                buttons{dialog_button{rap::dialog_role::ok, _("_Create")}.bind(ok_).on_click(
                            [this] { create(); }),
                        cancel},
            }}});
    window_->on_finished(
        [this](rap::dialog_role)
        {
            if (closed_) closed_();
        });
    into_changed();
}

void pull_request_dialog::show()
{
    window_->show();
    title_->focus();
}

// The target's branches, and its default chosen: what the remote's HEAD names
// locally, else main or master.
void pull_request_dialog::into_changed()
{
    const std::string remote = into().name;
    const auto listed = run_git(
        workdir_, {"for-each-ref", "--format=%(refname:lstrip=3)", "refs/remotes/" + remote + "/"},
        git_output::stdout_only);
    std::vector<std::string> branches;
    for (std::string_view rest = listed.output; !rest.empty();)
    {
        const size_t nl = rest.find('\n');
        const std::string_view line = rest.substr(0, nl);
        rest.remove_prefix(nl == std::string_view::npos ? rest.size() : nl + 1);
        if (!line.empty() && line != "HEAD") branches.emplace_back(line);
    }
    std::string chosen =
        trimmed(run_git(workdir_, {"symbolic-ref", "--short", "refs/remotes/" + remote + "/HEAD"},
                        git_output::stdout_only)
                    .output);
    if (chosen.starts_with(remote + "/")) chosen = chosen.substr(remote.size() + 1);
    for (const char* usual : {"main", "master"})
        if (chosen.empty() && std::ranges::find(branches, usual) != branches.end()) chosen = usual;
    if (chosen.empty() && !branches.empty()) chosen = branches.front();
    base_->set_items(branches);
    base_->set_text(chosen);
    base_changed();
}

// What would merge: the commits on the branch the base doesn't have; the
// title and description from them, unless they've been written.
void pull_request_dialog::base_changed()
{
    const std::string base = trimmed(base_->text());
    const std::string base_ref = "refs/remotes/" + into().name + "/" + base;
    std::vector<std::pair<std::string, std::string>> commits;  // subject, body
    const bool fetched = !base.empty() && !resolve(workdir_, base_ref).empty();
    if (fetched)
    {
        const auto log = run_git(workdir_, {"log", "--format=%s%x1f%b%x1e", base_ref + ".." + tip_},
                                 git_output::stdout_only);
        for (std::string_view rest = log.output; !rest.empty();)
        {
            const size_t end = rest.find('\x1e');
            std::string_view one = rest.substr(0, end);
            rest.remove_prefix(end == std::string_view::npos ? rest.size() : end + 1);
            while (!one.empty() && one.front() == '\n') one.remove_prefix(1);
            if (one.empty()) continue;
            const size_t sep = one.find('\x1f');
            commits.push_back({trimmed(one.substr(0, sep)),
                               sep == std::string_view::npos ? "" : trimmed(one.substr(sep + 1))});
        }
        commits_->set_text(commits.empty()
                               ? rap::i18n::format(_("{0} already has everything on this branch."),
                                                   into().name + "/" + base)
                               : rap::i18n::format(N_("{0} commit to merge.",
                                                      "{0} commits to merge.", commits.size()),
                                                   commits.size()));
    }
    else
        commits_->set_text(base.empty() ? ""
                                        : rap::i18n::format(_("{0} isn't fetched here."),
                                                            into().name + "/" + base));
    ok_->set_enabled(!base.empty() && !(fetched && commits.empty()));

    // One commit speaks for itself; several are listed under the branch's name.
    std::string title = title_from(head_branch_), body;
    if (commits.size() == 1)
    {
        title = commits.front().first;
        body = commits.front().second;
    }
    else
        for (auto it = commits.rbegin(); it != commits.rend(); ++it)
            body += "- " + it->first + "\n";
    if (title_->text() == auto_title_) title_->set_text(title);
    if (body_->text() == auto_body_) body_->set_text(body);
    auto_title_ = title;
    auto_body_ = body;
}

void pull_request_dialog::create()
{
    if (trimmed(title_->text()).empty())
    {
        status_->set_text(_("A pull request needs a title."));
        title_->focus();
        return;
    }
    settings::get().set("pr.browse", browse_->checked() ? "1" : "0");
    settings::get().set("pr.draft", draft_->checked() ? "1" : "0");
    if (local_.empty() || !needs_push_ || (on_forge_ && !push_->checked())) return send();
    // The branch first, then (if that worked) the pull request.
    std::vector<std::string> push{"push"};
    if (sets_upstream_) push.push_back("--set-upstream");
    push.insert(push.end(), {remotes_[head_remote_].name,
                             "refs/heads/" + local_ + ":refs/heads/" + head_branch_});
    ok_->set_enabled(false);
    run_progress_dialog(ui_, window_.get(), _("Push"), workdir_, {push},
                        [this, alive = std::weak_ptr<bool>(alive_)](bool ok)
                        {
                            if (!alive.lock()) return;
                            if (ok)
                                send();
                            else
                                ok_->set_enabled(true);
                        });
}

void pull_request_dialog::send()
{
    ok_->set_enabled(false);
    const forge_remote target = into();
    new_pull_request n{.head = remotes_[head_remote_].repo,
                       .head_branch = head_branch_,
                       .base_branch = trimmed(base_->text()),
                       .title = trimmed(title_->text()),
                       .body = trimmed(body_->text()),
                       .draft = draft_->checked()};
    std::erase(n.body, '\r');
    status_->set_text(rap::i18n::format(_("Opening the pull request on {0}..."), target.repo.host));
    std::thread(
        [this, alive = std::weak_ptr<bool>(alive_), target, n, &ui = ui_]
        {
            std::expected<pull_request, std::string> made;
            std::string why;
            if (auto token = load_token(target.account, &why))
                made = create_pull_request(target.account.kind, target.repo, n, token->access);
            else
                made = std::unexpected(why.empty() ? std::string(_("No token for the account."))
                                                   : why);
            ui.post(
                [this, alive, made = std::move(made)]
                {
                    if (!alive.lock()) return;
                    if (!made)
                    {
                        status_->set_text(made.error());
                        ok_->set_enabled(true);
                        return;
                    }
                    if (browse_->checked() && !made->url.empty()) rap::sys::open_url(made->url);
                    if (done_) done_(*made);
                    window_->close();
                    if (closed_) closed_();
                });
        })
        .detach();
}

}  // namespace tortilla
