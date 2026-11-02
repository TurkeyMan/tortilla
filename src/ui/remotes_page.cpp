#include "remotes_page.h"

#include "git/command.h"
#include "git/remotes.h"
#include "git/repo.h"
#include "forge/remote_sign_in.h"
#include "i18n.h"
#include "log/ref_labels.h"

#include <algorithm>
#include <cctype>
#include <memory>
#include <optional>
#include <thread>
#include <tuple>
#include <vector>

namespace tortilla
{
namespace
{

// Which tags a fetch brings, as remote.<name>.tagOpt has it: unset (those
// reachable from what's fetched), --tags (all), --no-tags (none).
constexpr const char* k_tag_options[] = {"", "--tags", "--no-tags"};

// A remote's forge, as far as it's known: its repository there, which forge
// that is, the account for it, and what the forge says of the repository.
struct remote_forge
{
    std::string url;
    std::optional<forge_repo> repo;
    std::optional<forge_kind> kind;  // by the account, the host's name, or asked
    std::optional<forge_account> account;
    repo_seen seen = repo_seen::unknown;
    bool asking = false;

    remote_sign_in describe() const
    {
        return describe_remote(repo && kind, account ? &*account : nullptr, seen, asking);
    }
    // The same remote, as the same account: what's known of it still holds.
    bool same(const remote_forge& o) const
    {
        using key = std::tuple<forge_kind, std::string_view, std::string_view>;
        auto key_of = [](const std::optional<forge_account>& a)
        { return a ? key(a->kind, a->host, a->login) : key(); };
        return url == o.url && bool(account) == bool(o.account) &&
               key_of(account) == key_of(o.account);
    }
};

void paint_mark(rap::painter& p, rap::size_f size, check_state mark)
{
    const float d = std::min(size.w, size.h) - 2;
    paint_check_mark(p, {size.w / 2, size.h / 2}, d, mark);
}

class remote_rows : public rap::list_model
{
public:
    using rap::list_model::notify_changed;
    using rap::list_model::notify_reset;
    std::vector<std::string> names;
    std::vector<remote_forge> forges;  // alongside
    size_t row_count() const override { return names.size(); }
    rap::icon icon(size_t row, size_t column) const override
    {
        if (column != 1 || row >= forges.size()) return {};
        const check_state mark = forges[row].describe().mark;
        if (mark == check_state::none) return {};
        return rap::icon::drawn([mark](rap::painter& p, rap::size_f size)
                                { paint_mark(p, size, mark); });
    }
    std::string text(size_t row, size_t column) const override
    {
        if (row >= names.size()) return "";
        if (column == 0) return names[row];
        return row < forges.size() ? forges[row].describe().summary : "";
    }
};

struct remotes_state
{
    rap::ui& ui;
    std::function<rap::window*()> parent;
    std::string dir;
    sign_in_to sign_in;
    std::weak_ptr<remotes_state> self;  // for the workers' answers
    remote_rows rows;
    std::string editing;  // the remote the fields show; "" for a new one
    rap::canvas* mark = nullptr;
    check_state shown_mark = check_state::none;
    rap::label* forge_text = nullptr;
    rap::button* sign_in_button = nullptr;
    rap::label* sign_in_link = nullptr;
    rap::list* list = nullptr;
    rap::entry* name = nullptr;
    rap::entry* url = nullptr;
    rap::entry* push_url = nullptr;
    rap::drop_down* tags = nullptr;
    rap::check* prune = nullptr;
    rap::button* remove = nullptr;

    void complain(const std::string& text, const std::string& detail = {})
    {
        ui.message(parent(),
                   {.icon = rap::message_spec::icon::error,
                    .title = _("Remotes"),
                    .text = text,
                    .detail = detail},
                   [](rap::dialog_role) {});
    }

    // The remotes again, showing `select` (or a new one's empty fields).
    void reload(const std::string& select)
    {
        rows.names = remote_names(dir);
        look();
        rows.notify_reset();
        const auto it = std::ranges::find(rows.names, select);
        if (it == rows.names.end()) return show({});
        const size_t row = size_t(it - rows.names.begin());
        list->select({row});
        list->set_current(row);
        show(select);
    }

    void show(const std::string& remote)
    {
        editing = remote;
        auto value = [&](const char* key)
        {
            return remote.empty() ? std::string()
                                  : config_value(dir, "remote." + remote + "." + key);
        };
        name->set_text(remote);
        url->set_text(value("url"));
        push_url->set_text(value("pushurl"));
        const std::string tag_opt = value("tagOpt");
        int tag = 0;
        for (int i = 1; i < 3; ++i)
            if (tag_opt == k_tag_options[i]) tag = i;
        tags->set_current(tag);
        prune->set_checked(value("prune") == "true");
        remove->set_enabled(!remote.empty());
        if (remote.empty()) list->select(std::vector<size_t>{});
        show_forge();
    }

    // Each remote's forge, with the accounts as they are now (one may have
    // been signed in to since): what's the same as before is kept, the rest
    // asked on a worker, a remote at a time.
    void look()
    {
        const auto signed_in = accounts();
        std::vector<remote_forge> forges, to_ask;
        for (const std::string& n : rows.names)
        {
            remote_forge f{.url = config_value(dir, "remote." + n + ".url")};
            f.repo = repo_of_url(f.url);
            if (f.repo) f.account = account_for_url(f.url, signed_in);
            if (auto old = std::ranges::find_if(rows.forges, [&](const remote_forge& o)
                                                { return o.same(f); });
                old != rows.forges.end())
            {
                forges.push_back(*old);
                continue;
            }
            if (f.repo)
            {
                f.kind = f.account ? std::optional(f.account->kind) : known_forge(f.repo->host);
                f.asking = true;
                to_ask.push_back(f);
            }
            forges.push_back(std::move(f));
        }
        rows.forges = std::move(forges);
        if (to_ask.empty()) return;
        std::thread(
            [weak = self, to_ask = std::move(to_ask), &ui = ui]() mutable
            {
                for (remote_forge& f : to_ask)
                {
                    if (weak.expired()) return;  // the page is gone
                    if (!f.kind) f.kind = probe_forge(f.repo->host);
                    // With an account, as it: whether it can see the repository.
                    std::string token;
                    if (f.account)
                        if (auto t = load_token(*f.account)) token = std::move(t->access);
                    if (f.kind && (!f.account || !token.empty()))
                        f.seen = look_for_repo(*f.kind, *f.repo, token);
                    f.asking = false;
                    ui.post(
                        [weak, f]
                        {
                            if (auto s = weak.lock()) s->answered(f);
                        });
                }
            })
            .detach();
    }

    void answered(const remote_forge& f)
    {
        for (size_t i = 0; i < rows.forges.size(); ++i)
        {
            if (!rows.forges[i].asking || !rows.forges[i].same(f)) continue;
            rows.forges[i] = f;
            rows.notify_changed(i, 1);
        }
        show_forge();
    }

    // The forge's line for the remote shown: signed in, or Sign in...
    const remote_forge* shown_forge() const
    {
        const auto it = std::ranges::find(rows.names, editing);
        if (editing.empty() || it == rows.names.end()) return nullptr;
        const size_t row = size_t(it - rows.names.begin());
        return row < rows.forges.size() ? &rows.forges[row] : nullptr;
    }
    void show_forge()
    {
        const remote_forge* f = shown_forge();
        const remote_sign_in d = f ? f->describe() : remote_sign_in{};
        using offer = remote_sign_in::offer;
        forge_text->set_text(d.text);
        sign_in_button->set_visible(d.sign_in == offer::button && sign_in);
        sign_in_link->set_visible(d.sign_in == offer::link && sign_in);
        shown_mark = d.mark;
        mark->set_visible(d.mark != check_state::none);
        mark->invalidate();
    }
    void sign_in_shown()
    {
        const remote_forge* f = shown_forge();
        if (f && f->repo && f->kind && sign_in) sign_in(*f->kind, f->repo->host);
    }

    // Adds the remote, or renames and changes the one shown, through git.
    void save()
    {
        std::string new_name = name->text(), new_url = url->text();
        auto trim = [](std::string s)
        {
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
                s.erase(s.begin());
            return s;
        };
        new_name = trim(new_name);
        new_url = trim(new_url);
        if (new_name.empty() || new_url.empty())
            return complain(_("A remote needs a name and a URL."));
        const auto commands = save_remote_commands(
            editing, {.name = new_name,
                      .url = new_url,
                      .push_url = trim(push_url->text()),
                      .tag_opt = k_tag_options[std::clamp(tags->current(), 0, 2)],
                      .prune = prune->checked()});
        for (const auto& args : commands)
        {
            const git_result r = run_git(dir, args);
            // Unsetting what isn't set (exit 5) is nothing to report.
            const bool unset =
                args.size() > 1 && args[0] == "config" && args[1].starts_with("--unset");
            if (!r.ok() && !(unset && r.exit_code == 5))
            {
                complain(_("git couldn't save the remote."), command_line(args) + "\n" + r.output);
                break;
            }
        }
        reload(new_name);
    }

    void remove_shown()
    {
        if (editing.empty()) return;
        const std::string remote = editing;
        ui.message(
            parent(),
            {.icon = rap::message_spec::icon::question,
             .title = _("Remotes"),
             .text = rap::i18n::format(_("Remove the remote “{0}”?"), remote),
             .detail =
                 _("Its remote-tracking branches go too; the repository it names isn't touched."),
             .buttons = {rap::dialog_role::yes, rap::dialog_role::no}},
            [this, remote](rap::dialog_role role)
            {
                if (role != rap::dialog_role::yes) return;
                if (const git_result r = run_git(dir, {"remote", "remove", "--", remote}); !r.ok())
                    complain(_("git couldn't remove the remote."), r.output);
                reload({});
            });
    }
};

}  // namespace

rap::node remotes_page(rap::ui& ui, std::function<rap::window*()> parent,
                       const std::string& repo_dir, sign_in_to sign_in,
                       std::function<void()>* shown)
{
    using namespace rap::layout;
    using namespace rap::literals;
    if (!repo::open(repo_dir.empty() ? "." : repo_dir))
        return vbox{
            label{_("Remotes belong to a repository: open Settings from one to see its remotes.")}
                .wrap(),
            spacer{}};
    auto st = std::make_shared<remotes_state>(remotes_state{.ui = ui,
                                                            .parent = std::move(parent),
                                                            .dir = repo_dir,
                                                            .sign_in = std::move(sign_in)});
    st->self = st;
    if (shown)
        *shown = [weak = std::weak_ptr(st)]
        {
            auto s = weak.lock();
            if (!s || !s->list) return;
            // The same remotes, and the fields as they are (edits unsaved
            // stay): only what's said of each one's forge.
            s->look();
            s->rows.notify_changed(0, s->rows.names.size());
            s->show_forge();
        };
    // The widgets' callbacks keep the state, which goes with the window;
    // the state keeps no widget alive, so there's no cycle.
    auto with = [st](auto f) { return [st, f] { f(*st); }; };
    rap::node page = hbox{
        list{std::vector<rap::column>{{.title = _("Remote"), .width = 12_ch},
                                      {.title = _("Forge"), .width = 16_ch}},
             &st->rows}
            .bind(st->list)
            .selection(rap::selection_mode::single)
            .on_selection_changed(
                [st]
                {
                    const auto sel = st->list->selection();
                    if (!sel.empty() && sel.front() < st->rows.names.size())
                        st->show(st->rows.names[sel.front()]);
                })
            .width(32_ch),
        vbox{
            form{
                {_("Name"), entry{}.bind(st->name).placeholder("origin")},
                {_("URL"), entry{}.bind(st->url)},
                {"",
                 hbox{canvas{}
                          .bind(st->mark)
                          .on_paint([st](rap::painter& p, rap::size_f size)
                                    { paint_mark(p, size, st->shown_mark); })
                          .width(16_px)
                          .height(16_px)
                          .visible(false),
                      label{}.bind(st->forge_text).wrap().stretch(),
                      button{_("Sign _in...")}
                          .bind(st->sign_in_button)
                          .visible(false)
                          .on_click(with([](remotes_state& s) { s.sign_in_shown(); })),
                      label{_("Sign in...")}
                          .bind(st->sign_in_link)
                          .visible(false)
                          .link(with([](remotes_state& s) { s.sign_in_shown(); }))}},
                {_("Push URL"), entry{}.bind(st->push_url).placeholder(_("the same as the URL"))},
                {_("Tags"),
                 drop_down{{_("Those on what's fetched (git's default)"), _("All"), _("None")}, 0}
                     .bind(st->tags)},
                {"",
                 check{_("_Prune: remove remote-tracking branches gone from the remote, on fetch")}
                     .bind(st->prune)},
            },
            hbox{
                button{_("_Add New")}.on_click(with(
                    [](remotes_state& s)
                    {
                        s.show({});
                        s.name->focus();
                    })),
                button{_("_Save")}.on_click(with([](remotes_state& s) { s.save(); })),
                button{_("_Remove")}
                    .bind(st->remove)
                    .on_click(with([](remotes_state& s) { s.remove_shown(); })),
                spacer{},
            },
            label{_("Changes apply when saved, to this repository's git config.")}.wrap(),
            spacer{},
        }
            .stretch(),
    };
    // Once its widgets are made: the first remote (origin, usually), or a
    // new one's fields.
    st->ui.post(
        [st]
        {
            const auto names = remote_names(st->dir);
            st->reload(names.empty() ? std::string() : names.front());        });
    return page;
}

}  // namespace tortilla
