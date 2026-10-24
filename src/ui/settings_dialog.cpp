#include "settings_dialog.h"

#include "git/diff_tool.h"
#include "git/merge_tool.h"
#include "git/ssh_client.h"
#include "i18n.h"
#include "log/action_icons.h"
#include "settings.h"
#include "shell/menu_icons.h"
#include "shell/menu_table.h"

#include <rapport/sys.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <memory>

namespace tortilla
{
namespace
{

// The Action icons choices, in the drop-down's order.
constexpr action_icon_style k_icon_styles[] = {action_icon_style::document,
                                               action_icon_style::glyph, action_icon_style::bevel,
                                               action_icon_style::line};

struct state
{
    std::unique_ptr<rap::window> window;
    std::string repo_dir;
    // Bound when their page is first shown; null until then, and an
    // unvisited page saves nothing.
    rap::drop_down* icon_style = nullptr;
    rap::canvas* icon_preview = nullptr;
    // Language: the tag each choice saves ("" for the system's).
    rap::drop_down* language = nullptr;
    std::vector<std::string> language_tags;
    // Compare with: Tortilla's viewer, git difftool, each detected tool, then
    // Custom (the command line shown only for it).
    rap::drop_down* diff_tool_select = nullptr;
    rap::entry* diff_command = nullptr;
    rap::widget* custom_help = nullptr;
    std::vector<detected_tool> tools;
    // Resolving conflicts: each merge tool found, then Custom.
    rap::drop_down* merge_select = nullptr;
    rap::entry* merge_command = nullptr;
    rap::widget* merge_help = nullptr;
    std::vector<detected_merge_tool> merge_tools;
    int merge_custom_index() const { return int(merge_tools.size()); }
    // Context Menu: where each entry shows, and the options below it.
    std::unique_ptr<class menu_entries> menu_entries;
    rap::list* menu_list = nullptr;
    rap::drop_down* menu_shows = nullptr;
    rap::check* hide_outside = nullptr;
    rap::text_view* no_menu = nullptr;
    // SSH client: git's own, each detected client, then Custom (the command
    // line shown only for it).
    rap::drop_down* ssh_select = nullptr;
    rap::entry* ssh_command = nullptr;
    rap::widget* ssh_help = nullptr;
    std::vector<ssh_client> ssh_clients;

    int custom_index() const { return 2 + int(tools.size()); }
    int ssh_custom_index() const { return 1 + int(ssh_clients.size()); }
};

rap::node general_page(std::shared_ptr<state> st)
{
    using namespace rap::layout;
    const auto saved = action_icon_style(
        std::clamp(std::atoi(settings::get().value("log.actionIcons", "1").c_str()), 0, 3));
    const int current = int(std::find(std::begin(k_icon_styles), std::end(k_icon_styles), saved) -
                            std::begin(k_icon_styles));
    std::weak_ptr<state> weak = st;
    // The five kinds of change in the chosen style, at the log's size and at
    // twice that, on the list's background.
    auto preview = [weak](rap::painter& p, rap::size_f size)
    {
        auto s = weak.lock();
        if (!s) return;
        const int chosen = s->icon_style ? std::clamp(s->icon_style->current(), 0, 3) : 1;
        const action_icon_style style = k_icon_styles[chosen];
        p.rect({0, 0, size.w, size.h}, 3, rap::paint{rap::theme_color::base},
               rap::paint{rap::theme_color::mid});
        constexpr action_bits kinds[] = {action_modified, action_added, action_deleted,
                                         action_renamed, action_conflicted};
        float x = 8;
        for (float s : {13.f, 26.f})
        {
            for (action_bits a : kinds)
            {
                paint_action_icon(p, a, x, (size.h - s) / 2, s, style);
                x += s + s * 0.4f;
            }
            x += 10;
        }
    };
    // The system's language, English (the source, so always there), then
    // each translation installed.
    const auto system = rap::sys::ui_languages();
    std::vector<std::string> languages{rap::i18n::format(
        _("System default ({0})"),
        system.empty() ? std::string("English") : rap::i18n::native_name(system.front()))};
    st->language_tags = {"", "en"};
    languages.push_back("English");
    if (const auto dir = translations_dir(); !dir.empty())
        for (const auto& l : rap::i18n::available("tortilla", dir))
            if (l.tag != "en")
            {
                languages.push_back(l.name);
                st->language_tags.push_back(l.tag);
            }
    const std::string chosen = settings::get().value(k_language_setting);
    int current_language = 0;
    for (size_t i = 1; i < st->language_tags.size(); ++i)
        if (st->language_tags[i] == chosen) current_language = int(i);
    return vbox{
        group{_("Language"),
              form{
                  {_("Language"), drop_down{languages, current_language}.bind(st->language)},
                  {"", label{_("Applies the next time Tortilla starts.")}},
              }},
        group{
            _("Log"),
            form{
                {_("Action icons"),
                 hbox{
                     drop_down{{_("Classic"), _("Glyphs"), _("Bevelled"), _("Line")}, current}
                         .bind(st->icon_style)
                         .on_changed(
                             [weak](int)
                             {
                                 if (auto s = weak.lock(); s && s->icon_preview)
                                     s->icon_preview->invalidate();
                             }),
                     canvas{}.on_paint(preview).width(300_px).height(36_px).bind(st->icon_preview),
                 }},
            }},
        spacer{},
    };
}

// The Context Menu page's list: one row per entry TortoiseGit sets as one
// (shell_menu::group_of), in the menu's order, and where each shows.
class menu_entries : public rap::list_model
{
public:
    struct entry
    {
        std::string group, label;
        shell_menu::shows where, fallback;  // as chosen; by default
        bool runs = false;                  // Tortilla does it yet
    };

    explicit menu_entries(const shell_menu::placements& saved)
    {
        using namespace shell_menu;
        for (const row& r : k_rows)
        {
            if (!r.label || std::string_view(r.id) == "inaccessible") continue;  // always at the top
            const std::string group(group_of(r.id));
            auto it = std::find_if(entries_.begin(), entries_.end(),
                                   [&](const entry& e) { return e.group == group; });
            if (it == entries_.end())
            {
                entries_.push_back({group, label_of(r, group), where_shown(r, saved),
                                    where_shown(r, {})});
                it = entries_.end() - 1;
            }
            it->runs = it->runs || r.command != run::none;
        }
    }

    const std::vector<entry>& entries() const { return entries_; }

    void set(const std::vector<size_t>& rows, shell_menu::shows where)
    {
        for (size_t i : rows) entries_[i].where = where;
        if (!rows.empty()) notify_changed(rows.front(), rows.back() - rows.front() + 1);
    }
    void restore_defaults()
    {
        for (entry& e : entries_) e.where = e.fallback;
        notify_changed(0, entries_.size());
    }

    // The groups that show `where`, as the settings file lists them; nullopt
    // when that's the default.
    std::optional<std::string> saved_list(shell_menu::shows where) const
    {
        std::string ids;
        bool is_default = true;
        for (const entry& e : entries_)
        {
            if (e.where == where) ids += (ids.empty() ? "" : ",") + e.group;
            is_default = is_default && (e.where == where) == (e.fallback == where);
        }
        if (is_default) return std::nullopt;
        return ids;
    }

    size_t row_count() const override { return entries_.size(); }
    // Columns: the entry (its icon painted before it), where it shows.
    std::string text(size_t row, size_t column) const override
    {
        const entry& e = entries_[row];
        if (column == 0)
            return e.runs ? e.label : rap::i18n::format(_("{0} (not yet in Tortilla)"), e.label);
        return _(k_shows_titles[size_t(e.where)]);
    }
    void paint(size_t row, size_t, rap::painter& p, const rap::cell_info& cell) const override
    {
        const entry& e = entries_[row];
        const float side = std::round(std::min(cell.size.h - 4, 16.f));
        const float y = std::round((cell.size.h - side) / 2);
        paint_menu_icon(p, e.group, 4, y, side);
        rap::text_style style;
        style.fg = cell.selected ? rap::theme_color::highlight_text
                   : e.runs      ? rap::theme_color::text
                                 : rap::theme_color::placeholder_text;
        const rap::size_f m = p.measure(e.label, style);
        p.text({side + 10, (cell.size.h - m.h) / 2}, e.label, style);
    }
    rap::text_style style(size_t row, size_t) const override
    {
        if (entries_[row].runs) return {};
        return {.fg = rap::theme_color::placeholder_text};
    }

    // In the order of shell_menu::shows.
    // Marked here, translated where shown.
    static constexpr const char* k_shows_titles[] = {T_("Top level"), T_("Submenu"),
                                                     T_("With Shift"), T_("Hidden")};

private:
    static std::string label_of(const shell_menu::row& r, std::string_view group)
    {
        if (group == "bisect") return _("Bisect");
        if (group == "merge") return _("Merge, Abort Merge");
        std::string text;
        for (const char* c = r.label; *c; ++c)
            if (*c != '&') text += *c;
        while (text.ends_with('.') || text.ends_with(' ')) text.pop_back();
        return text;
    }

    std::vector<entry> entries_;
};

shell_menu::placements saved_placements()
{
    using namespace shell_menu;
    auto list = [](std::string_view key) -> std::optional<id_set>
    {
        const std::string value = settings::get().value(std::string(key), "\x01");
        if (value == "\x01") return std::nullopt;
        return parse_ids(value);
    };
    return {list(k_top_setting), list(k_shift_setting), list(k_hidden_setting)};
}

// Where each of the file manager's entries shows, and when there's no menu
// at all [TortoiseGit: Settings > Context Menu, spec 4.6].
rap::node context_menu_page(std::shared_ptr<state> st)
{
    using namespace rap::layout;
    using namespace shell_menu;
    st->menu_entries = std::make_unique<menu_entries>(saved_placements());
    std::weak_ptr<state> weak = st;
    // The drop-down shows the selection's placement, blank when it's mixed.
    auto selection_changed = [weak]
    {
        auto s = weak.lock();
        if (!s || !s->menu_list || !s->menu_shows) return;
        const auto rows = s->menu_list->selection();
        int common = -1;
        for (size_t i = 0; i < rows.size(); ++i)
        {
            const int w = int(s->menu_entries->entries()[rows[i]].where);
            common = i == 0 || w == common ? w : -2;
        }
        s->menu_shows->set_enabled(!rows.empty());
        s->menu_shows->set_current(std::max(common, -1));
    };
    std::vector<std::string> shows_choices;
    for (const char* title : menu_entries::k_shows_titles) shows_choices.push_back(_(title));
    std::string no_menu = settings::get().value(std::string(k_no_menu_setting));
    std::replace(no_menu.begin(), no_menu.end(), '\t', '\n');
    return vbox{
        label{_("Top-level entries appear directly in the file manager's context menu, with\n"
                "\"Git\" before them; the rest are in its Tortilla submenu. Greyed entries\n"
                "aren't in Tortilla yet.")},
        // The last column stretches: it keeps the right edge as the page
        // (built when first shown) takes its size.
        list{{{.title = _("Entry"), .width = 44_ch, .painted = true},
              {.title = _("Shows"), .width = 14_ch, .stretch = true}},
             st->menu_entries.get()}
            .compact()
            .soft_selection()
            .on_selection_changed(selection_changed)
            .bind(st->menu_list)
            .stretch(),
        hbox{
            label{_("Selected entries show:")},
            drop_down{shows_choices, -1}
                .enabled(false)
                .on_changed(
                    [weak](int choice)
                    {
                        auto s = weak.lock();
                        if (!s || !s->menu_list || choice < 0) return;
                        s->menu_entries->set(s->menu_list->selection(), shows(choice));
                    })
                .bind(st->menu_shows),
            spacer{},
            button{_("Restore defaults")}.on_click(
                [weak, selection_changed]
                {
                    auto s = weak.lock();
                    if (!s) return;
                    s->menu_entries->restore_defaults();
                    if (s->hide_outside) s->hide_outside->set_checked(false);
                    if (s->no_menu) s->no_menu->set_text({});
                    selection_changed();
                }),
        },
        check{_("Hide the menu outside repositories (Shift shows it)"),
              settings::get().value(std::string(k_hide_outside_setting)) == "1"}
            .bind(st->hide_outside),
        label{_("No menu for these paths, one a line (end one with \\ for a folder and all in "
                "it,\nor with * for every path that starts so):")},
        text_view{no_menu}.bind(st->no_menu).height(3_lines),
    };
}

rap::node diff_viewer_page(std::shared_ptr<state> st)
{
    using namespace rap::layout;
    const std::string git_tool = git_diff_tool(st->repo_dir);
    st->tools = detect_diff_tools();

    std::vector<std::string> choices{
        _("Tortilla's unified diff viewer"),
        git_tool.empty() ? std::string(_("git difftool (no diff.tool configured)"))
                         : rap::i18n::format(_("git difftool (diff.tool: {0})"), git_tool)};
    for (const detected_tool& t : st->tools) choices.push_back(t.name);
    choices.push_back(_("Custom..."));

    // What's saved: a custom command that's a detected tool's shows as that tool.
    int current = int(saved_diff_mode(st->repo_dir));
    const std::string command = diff_command();
    if (current == int(diff_mode::custom))
    {
        current = st->custom_index();
        for (size_t i = 0; i < st->tools.size(); ++i)
            if (st->tools[i].command == command) current = 2 + int(i);
    }
    const bool custom = current == st->custom_index();

    std::weak_ptr<state> weak = st;
    auto chosen = [weak](int choice)
    {
        auto s = weak.lock();
        if (!s || !s->diff_command) return;
        const bool custom = choice == s->custom_index();
        s->diff_command->set_visible(custom);
        if (s->custom_help) s->custom_help->set_visible(custom);
        // Custom starts from the last tool picked, to adjust.
        if (choice >= 2 && !custom) s->diff_command->set_text(s->tools[size_t(choice - 2)].command);
    };
    // Resolving conflicts [spec 9.2]: the saved command shows as the tool it
    // is, else Custom; with nothing saved, the first tool found.
    st->merge_tools = detect_merge_tools();
    std::vector<std::string> merge_choices;
    for (const detected_merge_tool& t : st->merge_tools) merge_choices.push_back(t.name);
    merge_choices.push_back(_("Custom..."));
    const std::string saved_merge = saved_merge_command();
    int merge_current = saved_merge.empty() ? 0 : st->merge_custom_index();
    for (size_t i = 0; i < st->merge_tools.size(); ++i)
        if (st->merge_tools[i].command == saved_merge) merge_current = int(i);
    const bool merge_custom = merge_current == st->merge_custom_index();
    auto merge_chosen = [weak](int choice)
    {
        auto s = weak.lock();
        if (!s || !s->merge_command) return;
        const bool custom = choice == s->merge_custom_index();
        s->merge_command->set_visible(custom);
        if (s->merge_help) s->merge_help->set_visible(custom);
        if (!custom && choice >= 0)
            s->merge_command->set_text(s->merge_tools[size_t(choice)].command);
    };
    return vbox{
        group{
            _("Comparing revisions"),
            vbox{
                form{
                    {_("Compare with"),
                     drop_down{choices, current}.bind(st->diff_tool_select).on_changed(chosen)},
                    {_("Command"), entry{command}
                                    .bind(st->diff_command)
                                    .placeholder(_("program %base %mine"))
                                    .visible(custom)},
                },
                label{_("%base and %mine are the two sides compared: folders when a whole commit is\n"
                        "compared. %bname and %yname are their names. Without placeholders the two\n"
                        "sides go at the end of the command.")}
                    .handle(st->custom_help)
                    .visible(custom),
            }},
        group{_("Resolving conflicts"),
              vbox{
                  form{
                      {_("Merge with"), drop_down{merge_choices, merge_current}
                                         .bind(st->merge_select)
                                         .on_changed(merge_chosen)},
                      {_("Command"), entry{saved_merge.empty() && !st->merge_tools.empty()
                                            ? st->merge_tools.front().command
                                            : saved_merge}
                                      .bind(st->merge_command)
                                      .placeholder(_("program %base %theirs %mine %merged"))
                                      .visible(merge_custom)},
                  },
                  label{_("%base, %theirs and %mine are the conflict's three sides and %merged the "
                          "file to save;\n"
                          "%bname, %tname, %yname and %mname their titles. Without placeholders the "
                          "four\n"
                          "files go at the end.")}
                      .handle(st->merge_help)
                      .visible(merge_custom),
              }},
        spacer{},
    };
}

// Which SSH client git runs for remotes [TortoiseGit: Settings > Network].
rap::node network_page(std::shared_ptr<state> st)
{
    using namespace rap::layout;
    const std::string git_command = git_ssh_command(st->repo_dir);
    st->ssh_clients = detect_ssh_clients();

    std::vector<std::string> choices{git_command.empty()
                                         ? std::string(_("git's own (ssh)"))
                                         : rap::i18n::format(_("git's own ({0})"), git_command)};
    for (const ssh_client& c : st->ssh_clients) choices.push_back(c.name);
    choices.push_back(_("Custom..."));

    // What's saved: a command that's a detected client's shows as that client.
    const ssh_client saved = saved_ssh_client();
    int current = 0;
    if (!saved.command.empty())
    {
        current = st->ssh_custom_index();
        for (size_t i = 0; i < st->ssh_clients.size(); ++i)
            if (st->ssh_clients[i].command == saved.command) current = 1 + int(i);
    }
    const bool custom = current == st->ssh_custom_index();

    std::weak_ptr<state> weak = st;
    auto chosen = [weak](int choice)
    {
        auto s = weak.lock();
        if (!s || !s->ssh_command) return;
        const bool custom = choice == s->ssh_custom_index();
        s->ssh_command->set_visible(custom);
        if (s->ssh_help) s->ssh_help->set_visible(custom);
        // Custom starts from the last client picked, to adjust.
        if (choice >= 1 && !custom)
            s->ssh_command->set_text(s->ssh_clients[size_t(choice - 1)].command);
    };
    return vbox{
        group{
            _("SSH client"),
            vbox{
                form{
                    {_("SSH client"),
                     drop_down{choices, current}.bind(st->ssh_select).on_changed(chosen)},
                    {_("Command"), entry{saved.command}
                                    .bind(st->ssh_command)
                                    .placeholder("ssh -i ~/.ssh/key")
                                    .visible(custom)},
                },
                label{std::string(_("Run for each remote as git's GIT_SSH_COMMAND: git adds the "
                                    "host and the\ncommand to run there. plink and "
                                    "TortoiseGitPlink are known by their names."))
#ifdef _WIN32
                      // Git for Windows runs it through its MSYS shell, which
                      // turns /tmp/key into C:/.../Temp/key before WSL sees it.
                      + _("\nGit for Windows rewrites absolute Linux paths (/...) in it: for WSL "
                          "ssh,\nwrite them from home (~/...).")
#endif
                }
                    .handle(st->ssh_help)
                    .visible(custom),
            }},
        label{_("Used by the git Tortilla runs (fetch, pull, push, clone); the repository's\n"
                "core.sshCommand applies only with git's own.")},
        spacer{},
    };
}

}  // namespace

void show_settings(rap::ui& ui, rap::window* parent, const std::string& repo_dir,
                   settings_page first, std::function<void()> saved, std::function<void()> closed)
{
    auto st = std::make_shared<state>();
    st->repo_dir = repo_dir;
    std::weak_ptr<state> weak = st;
    auto build = [](const std::weak_ptr<state>& w, rap::node (*page)(std::shared_ptr<state>))
    {
        auto s = w.lock();
        return s ? page(s) : rap::node{};
    };
    rap::tabs* page_tabs = nullptr;

    using namespace rap::layout;
    // Lazy pages don't size the dialog, so it has a size of its own.
    st->window = ui.create(
        dialog{_("Settings - Tortilla"),
               {vbox{
                   tabs{
                       // Weak: the window that keeps these is the state's own.
                       page{_("General"), lazy{[weak, build] { return build(weak, general_page); }}},
                       page{_("Context Menu"),
                            lazy{[weak, build] { return build(weak, context_menu_page); }}},
                       page{_("Diff Viewer"),
                            lazy{[weak, build] { return build(weak, diff_viewer_page); }}},
                       page{_("Network"), lazy{[weak, build] { return build(weak, network_page); }}},
                   }
                       .sidebar()
                       .bind(page_tabs)
                       .stretch(),
                   buttons{ok, cancel},
               }}}
            .size(96_ch, 30_lines),
        parent);

    st->window->on_finished(
        [st, saved, closed, &ui](rap::dialog_role role) mutable
        {
            auto keep = std::move(st);
            if (role == rap::dialog_role::ok)
            {
                settings& settings = settings::get();
                if (keep->language)
                {
                    const int i = keep->language->current();
                    const std::string tag =
                        i > 0 && size_t(i) < keep->language_tags.size() ? keep->language_tags[size_t(i)] : "";
                    if (tag.empty())
                        settings.remove(k_language_setting);
                    else
                        settings.set(k_language_setting, tag);
                }
                if (keep->icon_style)
                    settings.set(
                        "log.actionIcons",
                        std::to_string(
                            int(k_icon_styles[std::clamp(keep->icon_style->current(), 0, 3)])));
                if (keep->menu_entries)
                {
                    using namespace shell_menu;
                    auto save = [&](std::string_view key, shows where)
                    {
                        if (auto ids = keep->menu_entries->saved_list(where))
                            settings.set(std::string(key), *ids);
                        else
                            settings.remove(std::string(key));
                    };
                    save(k_top_setting, shows::top);
                    save(k_shift_setting, shows::shift);
                    save(k_hidden_setting, shows::hidden);
                    if (keep->hide_outside->checked())
                        settings.set(std::string(k_hide_outside_setting), "1");
                    else
                        settings.remove(std::string(k_hide_outside_setting));
                    // One a line in the box; tab-separated in the file.
                    std::string paths;
                    const std::string text = keep->no_menu->text();
                    for (size_t at = 0; at <= text.size();)
                    {
                        size_t end = text.find('\n', at);
                        if (end == std::string::npos) end = text.size();
                        size_t first = at, last = end;
                        while (first < last && std::isspace(static_cast<unsigned char>(text[first])))
                            ++first;
                        while (last > first && std::isspace(static_cast<unsigned char>(text[last - 1])))
                            --last;
                        if (last > first)
                            paths += (paths.empty() ? "" : "\t") + text.substr(first, last - first);
                        at = end + 1;
                    }
                    if (paths.empty())
                        settings.remove(std::string(k_no_menu_setting));
                    else
                        settings.set(std::string(k_no_menu_setting), paths);
                }
                if (keep->diff_tool_select)
                {
                    // A detected tool is saved as the custom command it stands for.
                    const int choice = keep->diff_tool_select->current();
                    if (choice >= 2 && choice < keep->custom_index())
                        save_diff_settings(diff_mode::custom,
                                           keep->tools[size_t(choice - 2)].command,
                                           keep->tools[size_t(choice - 2)].folders);
                    else
                        save_diff_settings(choice >= 2 ? diff_mode::custom
                                                       : diff_mode(std::max(choice, 0)),
                                           keep->diff_command->text());
                }
                if (keep->merge_select)
                {
                    const int choice = keep->merge_select->current();
                    save_merge_command(choice >= 0 && choice < keep->merge_custom_index()
                                           ? keep->merge_tools[size_t(choice)].command
                                           : keep->merge_command->text());
                }
                if (keep->ssh_select)
                {
                    // A detected client is saved as its command and variant;
                    // Custom leaves the variant to git.
                    const int choice = keep->ssh_select->current();
                    if (choice >= 1 && choice < keep->ssh_custom_index())
                        save_ssh_client(keep->ssh_clients[size_t(choice - 1)]);
                    else if (choice == keep->ssh_custom_index())
                        save_ssh_client({.command = keep->ssh_command->text()});
                    else
                        save_ssh_client({});
                }
                if (saved) saved();
            }
            if (closed) closed();
            ui.post([keep] {});  // destroy after this callback returns
        });
    page_tabs->set_current(int(first));
    st->window->show();
}

}  // namespace tortilla
