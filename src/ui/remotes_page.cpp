#include "remotes_page.h"

#include "git/command.h"
#include "git/remotes.h"
#include "git/repo.h"
#include "i18n.h"

#include <algorithm>
#include <cctype>
#include <memory>
#include <vector>

namespace tortilla
{
namespace
{

// Which tags a fetch brings, as remote.<name>.tagOpt has it: unset (those
// reachable from what's fetched), --tags (all), --no-tags (none).
constexpr const char* k_tag_options[] = {"", "--tags", "--no-tags"};

class remote_rows : public rap::list_model
{
public:
    using rap::list_model::notify_reset;
    std::vector<std::string> names;
    size_t row_count() const override { return names.size(); }
    std::string text(size_t row, size_t) const override
    {
        return row < names.size() ? names[row] : "";
    }
};

struct remotes_state
{
    rap::ui& ui;
    std::function<rap::window*()> parent;
    std::string dir;
    remote_rows rows;
    std::string editing;  // the remote the fields show; "" for a new one
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
                       const std::string& repo_dir)
{
    using namespace rap::layout;
    using namespace rap::literals;
    if (!repo::open(repo_dir.empty() ? "." : repo_dir))
        return vbox{
            label{_("Remotes belong to a repository: open Settings from one to see its remotes.")}
                .wrap(),
            spacer{}};
    auto st = std::make_shared<remotes_state>(
        remotes_state{.ui = ui, .parent = std::move(parent), .dir = repo_dir});
    // The widgets' callbacks keep the state, which goes with the window;
    // the state keeps no widget alive, so there's no cycle.
    auto with = [st](auto f) { return [st, f] { f(*st); }; };
    rap::node page = hbox{
        list{std::vector<rap::column>{{.title = _("Remote"), .width = 16_ch}}, &st->rows}
            .bind(st->list)
            .selection(rap::selection_mode::single)
            .on_selection_changed(
                [st]
                {
                    const auto sel = st->list->selection();
                    if (!sel.empty() && sel.front() < st->rows.names.size())
                        st->show(st->rows.names[sel.front()]);
                })
            .width(20_ch),
        vbox{
            form{
                {_("Name"), entry{}.bind(st->name).placeholder("origin")},
                {_("URL"), entry{}.bind(st->url)},
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
            st->reload(names.empty() ? std::string() : names.front());
        });
    return page;
}

}  // namespace tortilla
