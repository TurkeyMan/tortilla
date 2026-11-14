#include "accounts_page.h"

#include "forge/forge.h"
#include "forge/pr_watch.h"
#include "i18n.h"
#include "log/ref_labels.h"
#include "ui/modal_dialogs.h"

#include <rapport/sys.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <memory>
#include <thread>
#include <vector>

namespace tortilla
{
namespace
{

std::string browser_unavailable()
{
    return _("Only for github.com and gitlab.com: for another server, make a token there and "
             "paste it below.");
}

// The open repositories' remotes that couldn't be seen as no one, on a
// forge still not signed in to: signing in may show their pull requests.
std::string hints_text()
{
    const auto all = accounts();
    std::string names;
    for (const forge_remote& r : pr_watch::all_hints())
        if (!account_for_url("https://" + r.repo.host + "/", all))
            names += (names.empty() ? "" : ", ") + r.repo.host + "/" + r.repo.path();
    return names.empty() ? std::string()
                         : rap::i18n::format(_("Sign in to see pull requests for: {0}"), names);
}

constexpr forge_kind k_kinds[] = {forge_kind::github, forge_kind::gitlab, forge_kind::gitea};

// Whether an account's token works, as asked when the page opens.
struct account_status
{
    check_state state = check_state::pending;  // success: signed in; failure: not
    std::string why;
};

class account_rows : public rap::list_model
{
public:
    using rap::list_model::notify_changed;
    using rap::list_model::notify_reset;
    std::vector<forge_account> accounts;
    std::vector<account_status> status;  // alongside
    size_t row_count() const override { return accounts.size(); }
    rap::icon icon(size_t row, size_t column) const override
    {
        if (column != 0 || row >= status.size()) return {};
        return rap::icon::drawn(
            [s = status[row].state](rap::painter& p, rap::size_f size)
            {
                const float d = std::min(size.w, size.h) - 2;
                paint_check_mark(p, {size.w / 2, size.h / 2}, d, s);
            });
    }
    std::string text(size_t row, size_t column) const override
    {
        if (row >= accounts.size()) return "";
        const forge_account& a = accounts[row];
        switch (column)
        {
        case 0:
            return a.name.empty() || a.name == a.login ? a.login : a.login + " (" + a.name + ")";
        case 1: return a.via_gh ? forge_name(a.kind) + _(" (through gh)") : forge_name(a.kind);
        case 2: return a.host;
        default:
            if (row >= status.size()) return "";
            switch (status[row].state)
            {
            case check_state::success: return _("Signed in");
            case check_state::failure: return status[row].why;
            default: return _("Checking...");
            }
        }
    }
};

struct accounts_state : std::enable_shared_from_this<accounts_state>
{
    rap::ui& ui;
    std::function<rap::window*()> parent;
    account_rows rows;
    rap::list* list = nullptr;
    rap::button* sign_out = nullptr;
    rap::drop_down* kind = nullptr;
    rap::entry* host = nullptr;
    rap::button* browser = nullptr;
    rap::button* stop = nullptr;
    rap::button* use_gh = nullptr;
    bool has_gh = gh_installed();
    rap::entry* token = nullptr;
    rap::button* use_token = nullptr;
    rap::label* scopes = nullptr;
    rap::label* status = nullptr;
    rap::label* hints = nullptr;
    // The sign-in under way, told to stop when the page goes.
    std::shared_ptr<std::atomic<bool>> cancelled;

    accounts_state(rap::ui& u, std::function<rap::window*()> p)
        : ui(u)
        , parent(std::move(p))
    {
    }
    ~accounts_state()
    {
        if (cancelled) *cancelled = true;
    }

    forge_kind chosen_kind() const { return k_kinds[std::clamp(kind->current(), 0, 2)]; }

    // The server as typed, without a scheme or a trailing slash.
    std::string chosen_host() const
    {
        const std::string typed = host->text();  // kept: h views it
        std::string_view h = typed;
        while (!h.empty() && std::isspace(static_cast<unsigned char>(h.front())))
            h.remove_prefix(1);
        while (!h.empty() &&
               (std::isspace(static_cast<unsigned char>(h.back())) || h.back() == '/'))
            h.remove_suffix(1);
        if (const size_t scheme = h.find("://"); scheme != std::string_view::npos)
            h.remove_prefix(scheme + 3);
        return h.empty() ? std::string(default_host(chosen_kind())) : std::string(h);
    }

    void complain(const std::string& text, const std::string& detail = {})
    {
        show_message(ui, parent(),
                     {.icon = rap::message_spec::icon::error,
                      .title = _("Accounts"),
                      .text = text,
                      .detail = detail},
                     [](rap::dialog_role) {});
    }

    // The buttons as the choices and what's under way allow.
    void update()
    {
        const bool busy = cancelled != nullptr;
        const bool can_browse = browser_sign_in(chosen_kind(), chosen_host());
        browser->set_enabled(!busy && can_browse);
        browser->set_tooltip(can_browse ? "" : browser_unavailable());
        stop->set_visible(busy);
        use_gh->set_visible(has_gh && chosen_kind() == forge_kind::github);
        use_gh->set_enabled(!busy);
        use_token->set_enabled(!busy && !token->text().empty());
        kind->set_enabled(!busy);
        host->set_enabled(!busy);
        token->set_enabled(!busy);
        sign_out->set_enabled(!busy && !list->selection().empty());
        scopes->set_text(token_scopes(chosen_kind()));
    }

    // Set for another page's remote; left alone while a sign-in is under way.
    void prefill(forge_kind k, const std::string& h)
    {
        if (!kind || cancelled) return;
        kind->set_current(int(std::ranges::find(k_kinds, k) - std::begin(k_kinds)));
        host->set_text(h);
        update();
        if (browser_sign_in(k, h))
            browser->focus();
        else
            token->focus();
    }

    void reload()
    {
        rows.accounts = accounts();
        rows.status.assign(rows.accounts.size(), {});
        rows.notify_reset();
        const std::string h = hints_text();
        hints->set_text(h);
        hints->set_visible(!h.empty());
        update();
        check_accounts();
    }

    // Each account's token tried on the forge (GET /user), on a worker: the
    // Status column says whether it's signed in, and why not.
    void check_accounts()
    {
        std::weak_ptr<accounts_state> weak = weak_from_this();
        std::thread(
            [weak, list = rows.accounts, &ui = ui]
            {
                for (const forge_account& a : list)
                {
                    account_status st{.state = check_state::success};
                    std::string why;
                    if (auto t = load_token(a, &why))
                    {
                        if (auto who = who_am_i(a.kind, a.host, t->access); !who)
                            st = {check_state::failure, who.error()};
                    }
                    else
                        st = {check_state::failure,
                              why.empty() ? std::string(_("No token in the keychain.")) : why};
                    ui.post(
                        [weak, a, st]
                        {
                            auto s = weak.lock();
                            if (!s) return;
                            for (size_t i = 0; i < s->rows.accounts.size(); ++i)
                            {
                                const forge_account& b = s->rows.accounts[i];
                                if (b.host != a.host || b.login != a.login || b.kind != a.kind ||
                                    i >= s->rows.status.size())
                                    continue;
                                s->rows.status[i] = st;
                                s->rows.notify_changed(i, 1);
                            }
                        });
                }
            })
            .detach();
    }

    // On a worker: `work` with the flag that stops it; then `done` here.
    template <class Work, class Done>
    void in_background(Work work, Done done)
    {
        cancelled = std::make_shared<std::atomic<bool>>(false);
        update();
        std::weak_ptr<accounts_state> weak = weak_from_this();
        std::thread(
            [weak, flag = cancelled, work = std::move(work), done = std::move(done),
             &ui = ui]() mutable
            {
                auto result = work(*flag);
                ui.post(
                    [weak, flag, done = std::move(done), result = std::move(result)]() mutable
                    {
                        auto s = weak.lock();
                        if (!s || s->cancelled != flag) return;  // gone, or stopped
                        s->cancelled.reset();
                        done(*s, std::move(result));
                        s->update();
                    });
            })
            .detach();
    }

    // The account the token is for, kept with it; the page shows it.
    using signed_in = std::expected<forge_account, std::string>;
    static signed_in keep(forge_kind k, const std::string& host, const forge_token& t)
    {
        auto who = who_am_i(k, host, t.access);
        if (!who) return who;
        if (!store_token(*who, t))
            return std::unexpected(rap::i18n::format(_("The keychain couldn't keep the token: {0}"),
                                                     rap::sys::secret_error()));
        return who;
    }

    void add(signed_in who)
    {
        if (!who)
        {
            status->set_text(who.error());
            return;
        }
        auto accounts = saved_accounts();
        std::erase_if(
            accounts, [&](const forge_account& a)
            { return a.kind == who->kind && a.host == who->host && a.login == who->login; });
        accounts.push_back(*who);
        save_accounts(accounts);
        if (who->via_gh) set_gh_account_off(*who, false);
        token->set_text("");
        status->set_text(rap::i18n::format(_("Signed in as {0} on {1}."), who->login, who->host));
        reload();
    }

    void sign_in_with_token()
    {
        const forge_kind k = chosen_kind();
        const std::string h = chosen_host();
        std::string t = token->text();
        while (!t.empty() && std::isspace(static_cast<unsigned char>(t.back()))) t.pop_back();
        while (!t.empty() && std::isspace(static_cast<unsigned char>(t.front())))
            t.erase(t.begin());
        if (t.empty()) return;
        status->set_text(rap::i18n::format(_("Checking the token with {0}..."), h));
        in_background([k, h, t](const std::atomic<bool>&) { return keep(k, h, {.access = t}); },
                      [](accounts_state& s, signed_in who) { s.add(std::move(who)); });
    }

    // The GitHub CLI's sign-in, asked for each time rather than copied.
    void sign_in_with_gh()
    {
        const std::string h = chosen_host();
        status->set_text(
            rap::i18n::format(_("Asking the GitHub CLI for its sign-in to {0}..."), h));
        in_background(
            [h](const std::atomic<bool>&) -> signed_in
            {
                auto token = gh_token(h);
                if (!token) return std::unexpected(token.error());
                auto who = who_am_i(forge_kind::github, h, *token);
                if (!who) return who;
                erase_token(*who);  // a token kept before, now gh's to hold
                who->via_gh = true;
                return who;
            },
            [](accounts_state& s, signed_in who) { s.add(std::move(who)); });
    }

    void sign_in_with_browser()
    {
        const forge_kind k = chosen_kind();
        const std::string h = chosen_host();
        status->set_text(rap::i18n::format(_("Asking {0} for a code..."), h));
        // The code comes first, shown while the user approves it.
        in_background([k, h](const std::atomic<bool>&) { return start_sign_in(k, h); },
                      [k, h](accounts_state& s, std::expected<device_code, std::string> code)
                      {
                          if (!code)
                          {
                              s.status->set_text(code.error());
                              return;
                          }
                          s.wait_for_approval(k, h, *code);
                      });
    }

    void wait_for_approval(forge_kind k, const std::string& h, const device_code& code)
    {
        status->set_text(rap::i18n::format(
            _("Enter the code {0} at {1}, in the browser that just opened, and approve Tortilla."),
            code.user, code.verification));
        rap::sys::open_url(code.verification_full.empty() ? code.verification
                                                          : code.verification_full);
        in_background(
            [k, h, code](const std::atomic<bool>& stop) -> signed_in
            {
                auto t = finish_sign_in(k, h, code, stop);
                if (!t) return std::unexpected(t.error());
                return keep(k, h, *t);
            },
            [](accounts_state& s, signed_in who) { s.add(std::move(who)); });
    }

    void sign_out_selected()
    {
        const auto sel = list->selection();
        if (sel.empty() || sel.front() >= rows.accounts.size()) return;
        const forge_account a = rows.accounts[sel.front()];
        show_message(
            ui, parent(),
            {.icon = rap::message_spec::icon::question,
             .title = _("Accounts"),
             .text = rap::i18n::format(_("Sign out of {0} on {1}?"), a.login, a.host),
             .detail = a.via_gh ? _("Tortilla stops using the GitHub CLI's sign-in; gh stays "
                                    "signed in.")
                                : _("Tortilla forgets its token. The token itself stays valid "
                                    "until it's revoked on the server, in your account's "
                                    "settings."),
             .buttons = {rap::dialog_role::yes, rap::dialog_role::no}},
            [weak = weak_from_this(), a](rap::dialog_role role)
            {
                auto s = weak.lock();
                if (!s || role != rap::dialog_role::yes) return;
                s->in_background(
                    [a](const std::atomic<bool>&)
                    { return erase_token(a) ? std::string() : rap::sys::secret_error(); },
                    [a](accounts_state& s, std::string error)
                    {
                        if (!error.empty())
                            s.complain(_("The keychain couldn't remove the token."), error);
                        auto accounts = saved_accounts();
                        std::erase_if(
                            accounts, [&](const forge_account& b)
                            { return b.kind == a.kind && b.host == a.host && b.login == a.login; });
                        save_accounts(accounts);
                        if (a.via_gh) set_gh_account_off(a, true);  // or it's back from gh
                        s.status->set_text("");
                        s.reload();
                    });
            });
    }
};

}  // namespace

rap::node accounts_page(rap::ui& ui, std::function<rap::window*()> parent, forge_sign_in* prefill)
{
    using namespace rap::layout;
    using namespace rap::literals;
    auto st = std::make_shared<accounts_state>(ui, std::move(parent));
    st->rows.accounts = accounts();
    st->rows.status.assign(st->rows.accounts.size(), {});
    st->check_accounts();
    if (prefill)
        *prefill = [weak = std::weak_ptr(st)](forge_kind k, const std::string& h)
        {
            if (auto s = weak.lock()) s->prefill(k, h);
        };
    // The widgets' callbacks keep the state, which goes with the window; the
    // state keeps no widget alive, so there's no cycle.
    auto with = [st](auto f) { return [st, f] { f(*st); }; };
    std::vector<std::string> kinds;
    for (forge_kind k : k_kinds) kinds.push_back(forge_name(k));
    const std::string hints = hints_text();
    return vbox{
        group{_("Signed in"),
              vbox{
                  list{std::vector<rap::column>{{.title = _("Account"), .width = 20_ch},
                                                {.title = _("Forge"), .width = 23_ch},
                                                {.title = _("Server"), .width = 14_ch},
                                                {.title = _("Status"), .width = 18_ch}},
                       &st->rows}
                      .bind(st->list)
                      .selection(rap::selection_mode::single)
                      .on_selection_changed(with([](accounts_state& s) { s.update(); }))
                      .stretch(),
                  label{hints}.bind(st->hints).wrap().visible(!hints.empty()),
                  hbox{spacer{},
                       button{_("Sign _out")}
                           .bind(st->sign_out)
                           .enabled(false)
                           .on_click(with([](accounts_state& s) { s.sign_out_selected(); }))},
              }}
            .stretch(),
        group{
            _("Sign in"),
            vbox{
                form{
                    {_("Forge"), drop_down{kinds, 0}.bind(st->kind).on_changed(
                                     [weak = std::weak_ptr(st)](int)
                                     {
                                         auto s = weak.lock();
                                         if (!s || !s->host) return;
                                         // A server that's one of the usual ones follows the forge.
                                         const std::string h = s->host->text();
                                         if (h.empty() ||
                                             std::ranges::any_of(k_kinds, [&](forge_kind k)
                                                                 { return h == default_host(k); }))
                                             s->host->set_text(
                                                 std::string(default_host(s->chosen_kind())));
                                         s->update();
                                     })},
                    {_("Server"),
                     entry{std::string(default_host(forge_kind::github))}.bind(st->host).on_changed(
                         [weak = std::weak_ptr(st)](const std::string&)
                         {
                             if (auto s = weak.lock(); s && s->browser) s->update();
                         })},
                    {"",
                     hbox{button{_("Sign in with the _browser")}
                              .bind(st->browser)
                              .enabled(browser_sign_in(forge_kind::github, "github.com"))
                              .tooltip(browser_sign_in(forge_kind::github, "github.com")
                                           ? ""
                                           : browser_unavailable())
                              .on_click(with([](accounts_state& s) { s.sign_in_with_browser(); })),
                          button{_("Use _gh's sign-in")}
                              .bind(st->use_gh)
                              .visible(st->has_gh)
                              .tooltip(_("The GitHub CLI's sign-ins are used as they are, with "
                                         "nothing to set up: this brings one back after signing "
                                         "out of it here."))
                              .on_click(with([](accounts_state& s) { s.sign_in_with_gh(); })),
                          button{_("Stop")}.bind(st->stop).visible(false).on_click(with(
                              [](accounts_state& s)
                              {
                                  if (s.cancelled) *s.cancelled = true;
                                  s.cancelled.reset();
                                  s.status->set_text("");
                                  s.update();
                              })),
                          spacer{}}},
                    {_("Or a token"),
                     hbox{entry{}
                              .bind(st->token)
                              .password()
                              .on_changed(
                                  [weak = std::weak_ptr(st)](const std::string&)
                                  {
                                      if (auto s = weak.lock(); s && s->use_token) s->update();
                                  })
                              .on_activate(with([](accounts_state& s) { s.sign_in_with_token(); }))
                              .stretch(),
                          button{_("Sign _in")}
                              .bind(st->use_token)
                              .enabled(false)
                              .on_click(with([](accounts_state& s) { s.sign_in_with_token(); }))}},
                    {"", label{token_scopes(forge_kind::github)}.bind(st->scopes)},
                    {"", label{_("Make a token on the server...")}.link(
                             [weak = std::weak_ptr(st)]
                             {
                                 if (auto s = weak.lock())
                                     rap::sys::open_url(
                                         token_page(s->chosen_kind(), s->chosen_host()));
                             })},
                },
                label{}.bind(st->status).wrap(),
            }},
        label{_("Tortilla keeps tokens in the system's keychain, and uses them only to talk to the "
                "forge: its pull requests and builds.")}
            .wrap(),
    };
}

}  // namespace tortilla
