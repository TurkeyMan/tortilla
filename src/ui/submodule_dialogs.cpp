#include "submodule_dialogs.h"

#include "git/command.h"
#include "git/remotes.h"
#include "git/repo.h"
#include "git/submodule.h"
#include "i18n.h"
#include "settings.h"
#include "ui/progress_dialog.h"

#include <rapport/sys.h>

#include <algorithm>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <memory>
#include <thread>
#include <utility>

namespace tortilla
{
namespace
{

namespace fs = std::filesystem;

// The URLs added before, newest first (TortoiseGit's History\SubModuleRepoURLS).
std::vector<std::string> url_history()
{
    std::vector<std::string> out;
    const std::string s = settings::get().value("subadd.urls");
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
    settings::get().set("subadd.urls", s);
}

// `folder` from the working tree's top, with '/'; "" for the top itself (or
// a folder outside it).
std::string relative_folder(const std::string& workdir, const std::string& folder)
{
    std::error_code ec;
    const fs::path top = fs::weakly_canonical(rap::sys::path_from_utf8(workdir), ec);
    const fs::path at = fs::weakly_canonical(rap::sys::path_from_utf8(folder), ec);
    std::string rel = rap::sys::path_to_utf8(at.lexically_relative(top).generic_string());
    while (rel.ends_with('/')) rel.pop_back();
    if (rel == "." || rel.starts_with("..")) return {};
    return rel;
}

std::string shown_path(const std::string& workdir)
{
    std::string root = workdir;
    while (root.size() > 1 && (root.back() == '/' || root.back() == '\\')) root.pop_back();
    return rap::sys::path_to_utf8(rap::sys::path_from_utf8(root).make_preferred());
}

struct add_state
{
    rap::ui& ui;
    rap::window* parent = nullptr;
    std::string workdir, base;  // base: the chosen folder, from the top
    std::function<void(bool)> done;
    std::unique_ptr<rap::window> window;
    std::string module;  // the name last derived from the URL
    rap::combo* url = nullptr;
    rap::entry* path = nullptr;
    rap::check* branch = nullptr;
    rap::entry* branch_name = nullptr;
    rap::check* force = nullptr;

    // The window (and with it this, which its callbacks hold) goes after
    // the callback that closed it returns.
    void close()
    {
        std::shared_ptr<rap::window> w(std::move(window));
        if (!w) return;
        w->close();
        ui.post([w] {});
    }

    // The path follows the URL: <folder>/<name>, replacing the name it last
    // added, as Clone's directory does.
    void url_changed(const std::string& u)
    {
        std::string p = path->text();
        if (!module.empty() && p.ends_with(module)) p.resize(p.size() - module.size());
        while (p.ends_with('/')) p.pop_back();
        const std::string name = clone_folder_name(u);
        if (name.empty())
        {
            module.clear();
            return;
        }
        module = (p.empty() ? "" : "/") + name;
        path->set_text(p + module);
    }

    void ok()
    {
        submodule_add_options o{
            .url = url->text(), .path = path->text(), .force = force->checked()};
        auto refuse = [this](const std::string& text)
        {
            ui.message(window.get(), {.icon = rap::message_spec::icon::warning,
                                      .title = _("Submodule Add"),
                                      .text = text});
        };
        if (o.url.empty() || o.path.empty())
            return refuse(_("Repository and path must not be empty."));
        if (branch->checked())
        {
            o.branch = branch_name->text();
            if (!run_git(".", {"check-ref-format", "--branch", o.branch}).ok())
                return refuse(rap::i18n::format(_("“{0}” isn't a valid branch name."), o.branch));
        }
        remember_url(o.url);
        const git_command c = submodule_add_args(o);
        std::vector<progress_follow_up> follow_ups{
            {.label = _("_Retry"),
             .commands = [c] { return std::vector<git_command>{c}; },
             .offered = [](bool ok) { return !ok; }},
        };
        run_progress_dialog(ui, parent,
                            rap::i18n::format(_("{0} - Submodule Add"), shown_path(workdir)),
                            workdir, {c}, std::exchange(done, {}), std::move(follow_ups));
        close();
    }
};

// The Update dialog's list: a box per submodule, its path, URL and state.
class submodule_rows : public rap::list_model
{
public:
    std::vector<submodule_info> items;
    std::vector<char> on;
    std::function<void()> changed;

    void reset(std::vector<submodule_info> list, const std::string& base)
    {
        items = std::move(list);
        // Those in the chosen folder, or the one it's in; all if none is.
        auto inside = [&](const submodule_info& s)
        {
            return base.empty() || s.path == base || s.path.starts_with(base + "/") ||
                   base.starts_with(s.path + "/");
        };
        const bool any = std::ranges::any_of(items, inside);
        on.assign(items.size(), 0);
        for (size_t i = 0; i < items.size(); ++i) on[i] = !any || inside(items[i]);
        notify_reset();
        if (changed) changed();
    }
    void set_all(bool v)
    {
        std::ranges::fill(on, char(v));
        if (!on.empty()) notify_changed(0, on.size());
    }
    std::vector<std::string> checked_paths() const
    {
        std::vector<std::string> out;
        for (size_t i = 0; i < items.size(); ++i)
            if (on[i]) out.push_back(items[i].path);
        return out;
    }

    size_t row_count() const override { return items.size(); }
    std::string text(size_t row, size_t column) const override
    {
        const submodule_info& s = items[row];
        switch (column)
        {
        case 0: return s.path;
        case 1: return s.url;
        default:
            return !s.initialized   ? _("Not initialized")
                   : !s.checked_out ? _("Not checked out")
                                    : "";
        }
    }
    rap::check_state checked(size_t row) const override
    {
        return on[row] ? rap::check_state::checked : rap::check_state::unchecked;
    }
    void toggle_checked(size_t row) override
    {
        on[row] = !on[row];
        notify_changed(row, 1);
        if (changed) changed();
    }
};

struct update_state
{
    rap::ui& ui;
    rap::window* parent = nullptr;
    std::string workdir, base;
    std::function<void(bool)> done;
    submodule_rows rows;
    std::unique_ptr<rap::window> window;
    rap::check* all = nullptr;
    rap::label* note = nullptr;
    rap::button* ok_button = nullptr;
    rap::check *init = nullptr, *recursive = nullptr, *remote = nullptr, *no_fetch = nullptr,
               *force = nullptr;
    rap::drop_down* mode = nullptr;

    void close()
    {
        std::shared_ptr<rap::window> w(std::move(window));
        if (!w) return;
        w->close();
        ui.post([w] {});
    }

    // Select/deselect all follows the boxes; OK needs one.
    void checks_changed()
    {
        const size_t n = std::ranges::count(rows.on, char(1));
        all->set_state(n == 0                ? rap::check_state::unchecked
                       : n == rows.on.size() ? rap::check_state::checked
                                             : rap::check_state::indeterminate);
        ok_button->set_enabled(n > 0);
    }

    void loaded(std::expected<std::vector<submodule_info>, std::string> list)
    {
        if (!list)
            note->set_text(list.error());
        else if (list->empty())
            note->set_text(_("This repository has no submodules."));
        else
            note->set_text("");
        rows.reset(list ? std::move(*list) : std::vector<submodule_info>{}, base);
    }

    void ok()
    {
        settings& set = settings::get();
        auto keep = [&](const char* key, rap::check* c) { set.set(key, c->checked() ? "1" : "0"); };
        keep("subupdate.init", init);
        keep("subupdate.recursive", recursive);
        keep("subupdate.remote", remote);
        keep("subupdate.nofetch", no_fetch);
        keep("subupdate.force", force);
        set.set("subupdate.mode", std::to_string(std::max(0, mode->current())));
        submodule_update_options o{
            .init = init->checked(),
            .recursive = recursive->checked(),
            .remote = remote->checked(),
            .no_fetch = no_fetch->checked(),
            .force = force->checked(),
            .mode = submodule_update_options::how(std::clamp(mode->current(), 0, 3)),
        };
        const git_command c = submodule_update_args(o, rows.checked_paths());
        std::vector<progress_follow_up> follow_ups{
            {.label = _("_Retry"),
             .commands = [c] { return std::vector<git_command>{c}; },
             .offered = [](bool ok) { return !ok; }},
        };
        run_progress_dialog(ui, parent,
                            rap::i18n::format(_("{0} - Submodule Update"), shown_path(workdir)),
                            workdir, {c}, std::exchange(done, {}), std::move(follow_ups));
        close();
    }
};

}  // namespace

void add_submodule(rap::ui& ui, rap::window* parent, const std::string& workdir,
                   const std::string& folder, std::function<void(bool ok)> done)
{
    std::shared_ptr<add_state> st(new add_state{.ui = ui,
                                                .parent = parent,
                                                .workdir = workdir,
                                                .base = relative_folder(workdir, folder),
                                                .done = std::move(done)});
    add_state* s = st.get();
    const std::vector<std::string> history = url_history();
    const std::string first_url = history.empty() ? "" : history.front();

    using namespace rap::layout;
    using namespace rap::literals;
    st->window = ui.create(
        dialog{
            _("Submodule Add"),
            {vbox{
                form{
                    {_("Repository"),
                     hbox{
                         combo{history, first_url}.bind(s->url).width(60_ch).stretch().on_changed(
                             [s](const std::string& u) { s->url_changed(u); }),
                         button{_("Bro_wse...")}.on_click(
                             [s]
                             {
                                 // A repository on this computer.
                                 s->ui.choose_files(s->window.get(),
                                                    {.mode = rap::file_dialog_spec::mode::folder,
                                                     .title = _("Repository"),
                                                     .initial_path = s->workdir},
                                                    [s](std::vector<std::string> chosen)
                                                    {
                                                        if (chosen.empty() || !s->window) return;
                                                        s->url->set_text(chosen.front());
                                                        s->url_changed(chosen.front());
                                                    });
                             }),
                     }},
                    {_("Path"), entry{s->base}.bind(s->path)},
                    {"",
                     hbox{
                         check{_("_Branch")}.bind(s->branch).on_toggled(
                             [s](bool on) { s->branch_name->set_enabled(on); }),
                         entry{}.bind(s->branch_name).enabled(false).width(20_ch),
                         spacer{},
                     }},
                    {"", check{_("_Force")}.bind(s->force)},
                },
                buttons{dialog_button{rap::dialog_role::ok, ""}.on_click([s] { s->ok(); }), cancel},
            }}}
            .resizable(false),
        parent);
    st->window->on_finished(
        [st](rap::dialog_role)
        {
            // Cancelled or closed (OK closes it itself).
            st->close();
        });
    if (!first_url.empty()) s->url_changed(first_url);
    s->window->show();
}

void update_submodules(rap::ui& ui, rap::window* parent, const std::string& workdir,
                       const std::string& folder, std::function<void(bool ok)> done)
{
    std::shared_ptr<update_state> st(new update_state{.ui = ui,
                                                      .parent = parent,
                                                      .workdir = workdir,
                                                      .base = relative_folder(workdir, folder),
                                                      .done = std::move(done)});
    update_state* s = st.get();
    const settings& set = settings::get();
    auto saved = [&](const char* key, const char* fallback)
    { return set.value(key, fallback) == "1"; };
    const int mode = std::clamp(std::atoi(set.value("subupdate.mode", "0").c_str()), 0, 3);

    using namespace rap::layout;
    using namespace rap::literals;
    st->window = ui.create(
        dialog{
            _("Submodule Update"),
            {vbox{
                list{{{_("Path"), 24_ch}, {_("URL"), 36_ch, true}, {_("Status"), 16_ch}}, &s->rows}
                    .checkboxes()
                    .selection(rap::selection_mode::single)
                    .compact()
                    .height(10_lines)
                    .stretch(),
                hbox{
                    check{_("Select/deselect _all")}.bind(s->all).on_toggled(
                        [s](bool on)
                        {
                            // From some checked, a click checks none, as the file lists do.
                            const bool some = s->all->state() == rap::check_state::indeterminate;
                            s->rows.set_all(on && !some);
                            s->checks_changed();
                        }),
                    spacer{},
                    label{_("Loading…")}.bind(s->note),
                },
                group{
                    _("Options"),
                    vbox{
                        hbox{
                            check{_("_Initialize submodules (--init)"),
                                  saved("subupdate.init", "1")}
                                .bind(s->init),
                            check{_("_Recursive (--recursive)"), saved("subupdate.recursive", "1")}
                                .bind(s->recursive),
                            spacer{},
                        },
                        hbox{
                            check{_("Remote _tracking branch (--remote)"),
                                  saved("subupdate.remote", "0")}
                                .bind(s->remote),
                            check{_("_No fetch (--no-fetch)"), saved("subupdate.nofetch", "0")}
                                .bind(s->no_fetch),
                            check{_("_Force (--force)"), saved("subupdate.force", "0")}.bind(
                                s->force),
                            spacer{},
                        },
                        form{{_("Update by"), drop_down{{_("Default (as configured)"),
                                                         _("Checkout"), _("Merge"), _("Rebase")},
                                                        mode}
                                                  .bind(s->mode)}},
                    }},
                buttons{dialog_button{rap::dialog_role::ok, ""}
                            .bind(s->ok_button)
                            .on_click([s] { s->ok(); }),
                        cancel},
            }}}
            .size(80_ch, {}),
        parent);
    st->window->on_finished([st](rap::dialog_role) { st->close(); });
    s->rows.changed = [s] { s->checks_changed(); };
    s->ok_button->set_enabled(false);

    // Read off the UI thread: libgit2 reads .gitmodules, the index and
    // each submodule's folder, which over a network share takes a while.
    std::thread(
        [&ui, weak = std::weak_ptr<update_state>(st), workdir]
        {
            auto r = repo::open(workdir);
            std::expected<std::vector<submodule_info>, std::string> list =
                r ? list_submodules(*r) : std::unexpected(r.error());
            ui.post(
                [weak, list = std::move(list)]() mutable
                {
                    if (auto s = weak.lock(); s && s->window) s->loaded(std::move(list));
                });
        })
        .detach();
    s->window->show();
}

void sync_submodules(rap::ui& ui, rap::window* parent, const std::string& workdir,
                     std::function<void(bool ok)> done)
{
    run_progress_dialog(ui, parent,
                        rap::i18n::format(_("{0} - Submodule Sync"), shown_path(workdir)), workdir,
                        {submodule_sync_args(true)}, std::move(done));
}

}  // namespace tortilla
