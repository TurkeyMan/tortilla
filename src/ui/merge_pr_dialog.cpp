#include "merge_pr_dialog.h"

#include "i18n.h"
#include "settings.h"
#include "ui/modal_dialogs.h"
#include "ui/repo_icon.h"
#include "util/strings.h"

#include <rapport/sys.h>

#include <algorithm>
#include <thread>

namespace tortilla
{
namespace
{

std::string method_label(merge_method m)
{
    switch (m)
    {
    case merge_method::squash: return _("Squash and merge");
    case merge_method::rebase: return _("Rebase and merge");
    default: return _("Create a merge commit");
    }
}


// The branch is in the repository the PR is on, so it's there to delete
// (a fork's branch is its owner's).
bool own_branch(const open_pr& p)
{
    return p.pr.head == p.into.repo;
}

// On a worker: the account's token, then `work` with it; `done` on the UI
// thread with its result, if `alive` still is.
template <class Work, class Done>
void with_token(rap::ui& ui, std::weak_ptr<bool> alive, const forge_account& account, Work work,
                Done done)
{
    std::thread(
        [&ui, alive, account, work = std::move(work), done = std::move(done)]() mutable
        {
            std::string why;
            auto token = load_token(account, &why);
            auto result =
                token ? work(token->access)
                      : decltype(work(std::string()))(std::unexpected(
                            why.empty() ? std::string(_("No token for the account.")) : why));
            ui.post(
                [alive, done = std::move(done), result = std::move(result)]() mutable
                {
                    if (alive.lock()) done(std::move(result));
                });
        })
        .detach();
}

}  // namespace

// -- Merge --------------------------------------------------------------------

std::unique_ptr<merge_pr_dialog> merge_pr_dialog::open(rap::ui& ui, const open_pr& p)
{
    std::unique_ptr<merge_pr_dialog> d(new merge_pr_dialog(ui, p));
    d->build();
    d->ask();
    return d;
}

merge_pr_dialog::merge_pr_dialog(rap::ui& ui, open_pr p)
    : ui_(ui)
    , pr_(std::move(p))
{
}

void merge_pr_dialog::build()
{
    using namespace rap::layout;
    using namespace rap::literals;
    const pull_request& pr = pr_.pr;
    window_ = create_window(
        ui_,
        dialog{rap::i18n::format(_("Merge Pull Request #{0}"), pr.number),
               {vbox{
                   label{pr.title}.wrap(),
                   // The repositories only when they differ: a fork's.
                   label{own_branch(pr_)
                             ? rap::i18n::format(_("{0} into {1}"), pr.head_branch, pr.base_branch)
                             : rap::i18n::format(_("{0} into {1}"),
                                                 pr.head.path() + ":" + pr.head_branch,
                                                 pr_.into.repo.path() + ":" + pr.base_branch)},
                   label{_("Asking the forge...")}.bind(status_).wrap(),
                   form{
                       {_("Method"),
                        drop_down{std::vector<std::string>{}, -1}.bind(method_).on_changed(
                            [this](int) { method_changed(); })},
                       {_("Commit title"), entry{}.bind(title_).placeholder(_("The forge's own"))},
                   },
                   group{_("Commit message"), text_view{}.bind(message_).height(8_lines).stretch()}
                       .stretch(),
                   check{_("_Delete the branch after"),
                         settings::get().value("pr.deleteBranch", "1") == "1"}
                       .bind(delete_)
                       .visible(false),
                   check{_("Merge _anyway, bypassing the branch's rules (as an administrator)")}
                       .bind(bypass_)
                       .visible(false)
                       .on_toggled([this](bool) { update(); }),
                   buttons{dialog_button{rap::dialog_role::none, _("Merge when _ready")}
                               .bind(when_ready_)
                               .on_click([this] { merge(true); }),
                           dialog_button{rap::dialog_role::ok, _("_Merge")}.bind(merge_).on_click(
                               [this] { merge(false); }),
                           cancel},
               }}}
            .size(640_px, 480_px));
    window_->on_finished(
        [this](rap::dialog_role)
        {
            if (closed_) closed_();
        });
    update();
}

void merge_pr_dialog::show()
{
    window_->show();
}

void merge_pr_dialog::ask()
{
    with_token(
        ui_, alive_, pr_.into.account,
        [kind = pr_.into.account.kind, repo = pr_.into.repo, n = pr_.pr.number](
            const std::string& token) { return pull_request_merge_state(kind, repo, n, token); },
        [this](std::expected<merge_state, std::string> s) { answered(std::move(s)); });
}

void merge_pr_dialog::answered(std::expected<merge_state, std::string> s)
{
    if (!s)
    {
        status_->set_text(s.error());
        return;
    }
    // The forge works mergeability out lazily: ask again a few times.
    if (s->state == merge_state::verdict::checking && ++asked_ < 6)
    {
        status_->set_text(s->why);
        again_ = ui_.timer(2000, false, [this] { ask(); });
        return;
    }
    const bool first = !state_;
    state_ = std::move(*s);
    if (first)
    {
        std::vector<std::string> labels;
        for (merge_method m : state_->methods) labels.push_back(method_label(m));
        method_->set_items(labels);
        const auto at = std::ranges::find(state_->methods, state_->preferred);
        method_->set_current(at == state_->methods.end() ? 0 : int(at - state_->methods.begin()));
        can_delete_ = own_branch(pr_) && !state_->forge_deletes_branch && !state_->methods.empty();
        delete_->set_visible(can_delete_);
        method_changed();
    }
    using v = merge_state::verdict;
    std::string said =
        state_->state == v::ready ? std::string(_("It can be merged.")) : state_->why;
    if (state_->state == v::waiting && state_->can_merge_when_ready)
        said += std::string(" ") + _("It can be set to merge once they pass.");
    if (state_->merge_when_ready_set)
        said += std::string(" ") + _("It's already set to merge when ready.");
    status_->set_text(said);
    bypass_->set_visible(state_->state != v::ready && state_->can_bypass);
    update();
}

// The commit's title and message follow the method: a rebase makes none.
void merge_pr_dialog::method_changed()
{
    if (!state_ || state_->methods.empty()) return;
    const merge_method m = state_->methods[size_t(std::max(0, method_->current()))];
    const pull_request& pr = pr_.pr;
    std::string title;
    if (m == merge_method::squash)
        title = pr.title + " (#" + std::to_string(pr.number) + ")";
    else if (m == merge_method::merge && pr_.into.account.kind == forge_kind::github)
        title = "Merge pull request #" + std::to_string(pr.number) + " from " + pr.head.owner +
                "/" + pr.head_branch;
    if (title_->text() == auto_title_) title_->set_text(title);
    auto_title_ = title;
    title_->set_enabled(m != merge_method::rebase);
    message_->set_read_only(m == merge_method::rebase);
}

void merge_pr_dialog::update()
{
    using v = merge_state::verdict;
    const bool known = state_ && !state_->methods.empty();
    const bool ready = known && (state_->state == v::ready || bypass_->checked());
    merge_->set_enabled(!busy_ && ready);
    when_ready_->set_visible(known && state_->state == v::waiting && state_->can_merge_when_ready &&
                             !state_->merge_when_ready_set);
    when_ready_->set_enabled(!busy_);
    method_->set_enabled(!busy_ && known);
}

void merge_pr_dialog::merge(bool when_ready)
{
    if (!state_) return;
    settings::get().set("pr.deleteBranch", delete_->checked() ? "1" : "0");
    if (!when_ready && state_->state != merge_state::verdict::ready && bypass_->checked())
    {
        // Going round the rules is the administrator's call, made knowingly.
        show_message(ui_, window_.get(),
                     {.icon = rap::message_spec::icon::warning,
                      .title = rap::i18n::format(_("Merge Pull Request #{0}"), pr_.pr.number),
                      .text = _("Merge it despite the branch's rules?"),
                      .detail = state_->why + "\n\n" +
                                _("The rules are there for a reason: this merges without the "
                                  "reviews or checks they ask for, and the forge records that you "
                                  "did."),
                      .buttons = {rap::dialog_role::yes, rap::dialog_role::no}},
                     [this, alive = std::weak_ptr<bool>(alive_)](rap::dialog_role r)
                     {
                         if (alive.lock() && r == rap::dialog_role::yes) send(false);
                     });
        return;
    }
    send(when_ready);
}

void merge_pr_dialog::send(bool when_ready)
{
    const merge_method m = state_->methods[size_t(std::max(0, method_->current()))];
    std::string message = message_->text();
    std::erase(message, '\r');
    merge_request request{.method = m,
                          .commit_title = m == merge_method::rebase ? "" : trimmed(title_->text()),
                          .commit_message = m == merge_method::rebase ? "" : trimmed(message),
                          .head_sha = state_->head_sha,
                          .when_ready = when_ready,
                          .node_id = state_->node_id};
    const bool delete_after = !when_ready && can_delete_ && delete_->checked();
    busy_ = true;
    update();
    status_->set_text(when_ready ? _("Setting it to merge when ready...") : _("Merging..."));
    with_token(
        ui_, alive_, pr_.into.account,
        [p = pr_, request,
         delete_after](const std::string& token) -> std::expected<std::string, std::string>
        {
            const forge_kind k = p.into.account.kind;
            if (std::string why = merge_pull_request(k, p.into.repo, p.pr.number, request, token);
                !why.empty())
                return std::unexpected(why);
            // The merge stands even if the branch stays: said, not failed.
            if (delete_after)
                if (std::string why = delete_branch(k, p.into.repo, p.pr.head_branch, token);
                    !why.empty())
                    return rap::i18n::format(_("Merged; the branch couldn't be deleted: {0}"), why);
            return std::string();
        },
        [this, when_ready](std::expected<std::string, std::string> r)
        {
            busy_ = false;
            if (!r)
            {
                status_->set_text(r.error());
                update();
                return;
            }
            if (!r->empty())
                show_message(
                    ui_, nullptr,
                    {.icon = rap::message_spec::icon::warning,
                     .title = rap::i18n::format(_("Merge Pull Request #{0}"), pr_.pr.number),
                     .text = *r});
            if (done_) done_(!when_ready);
            window_->close();
            if (closed_) closed_();
        });
}

// -- Close --------------------------------------------------------------------

std::unique_ptr<close_pr_dialog> close_pr_dialog::open(rap::ui& ui, const open_pr& p)
{
    std::unique_ptr<close_pr_dialog> d(new close_pr_dialog(ui, p));
    using namespace rap::layout;
    using namespace rap::literals;
    d->window_ = create_window(
        ui,
        dialog{
            rap::i18n::format(_("Close Pull Request #{0}"), p.pr.number),
            {vbox{
                label{p.pr.title}.wrap(),
                group{_("Why (posted as a comment)"),
                      text_view{}.bind(d->reason_).height(6_lines).stretch()}
                    .stretch(),
                check{_("_Delete the branch too")}.bind(d->delete_).visible(own_branch(p)),
                label{}.bind(d->status_).wrap(),
                buttons{dialog_button{rap::dialog_role::ok, _("_Close it")}.bind(d->ok_).on_click(
                            [raw = d.get()] { raw->close_it(); }),
                        cancel},
            }}}
            .size(560_px, 340_px));
    d->window_->on_finished(
        [raw = d.get()](rap::dialog_role)
        {
            if (raw->closed_) raw->closed_();
        });
    return d;
}

close_pr_dialog::close_pr_dialog(rap::ui& ui, open_pr p)
    : ui_(ui)
    , pr_(std::move(p))
{
}

void close_pr_dialog::show()
{
    window_->show();
    reason_->focus();
}

void close_pr_dialog::close_it()
{
    std::string reason = reason_->text();
    std::erase(reason, '\r');
    reason = trimmed(reason);
    const bool delete_after = own_branch(pr_) && delete_->checked();
    ok_->set_enabled(false);
    status_->set_text(_("Closing it..."));
    with_token(
        ui_, alive_, pr_.into.account,
        [p = pr_, reason,
         delete_after](const std::string& token) -> std::expected<void, std::string>
        {
            const forge_kind k = p.into.account.kind;
            if (std::string why = close_pull_request(k, p.into.repo, p.pr.number, reason, token);
                !why.empty())
                return std::unexpected(why);
            if (delete_after)
                if (std::string why = delete_branch(k, p.into.repo, p.pr.head_branch, token);
                    !why.empty())
                    return std::unexpected(
                        rap::i18n::format(_("Closed; the branch couldn't be deleted: {0}"), why));
            return {};
        },
        [this](std::expected<void, std::string> r)
        {
            if (!r)
            {
                status_->set_text(r.error());
                ok_->set_enabled(true);
                return;
            }
            if (done_) done_();
            window_->close();
            if (closed_) closed_();
        });
}

}  // namespace tortilla
