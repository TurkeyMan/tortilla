#include "settings_dialog.h"

#include "git/diff_tool.h"
#include "git/merge_tool.h"
#include "git/ssh_client.h"
#include "log/action_icons.h"
#include "settings.h"
#include "shell/menu_table.h"

#include <algorithm>
#include <cstdlib>
#include <memory>

namespace tortilla
{
namespace
{

// The View > Action Icons choices, in the menu's order.
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
    // Context Menu: an entry's id and its "at the top level" box.
    std::vector<std::pair<std::string, rap::check*>> top_entries;
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
    return vbox{
        group{
            "Log",
            form{
                {"Action icons",
                 hbox{
                     drop_down{{"Classic", "Glyphs", "Bevelled", "Line"}, current}
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

// Which of Explorer's entries are at the top level of its menu rather than
// in the Tortilla submenu [TortoiseGit: Settings > Context Menu].
rap::node context_menu_page(std::shared_ptr<state> st)
{
    using namespace rap::layout;
    using namespace shell_menu;
    // The saved choice, else the rows TortoiseGit puts at the top.
    const std::string saved = settings::get().value(std::string(k_top_setting), "\x01");
    auto chosen = [&](const row& r)
    {
        if (saved == "\x01") return r.where == place::top;
        return ("," + saved + ",").find("," + std::string(r.id) + ",") != std::string::npos;
    };
    rap::nodes rows;
    bool group_started = false;
    // The checks bind to these elements: no reallocation once they're made.
    st->top_entries.reserve(std::size(k_rows));
    for (const row& r : k_rows)
    {
        if (!r.label)
        {
            if (group_started) rows.push_back(rap::layout::separator{});
            group_started = false;
            continue;
        }
        std::string text;
        for (const char* c = r.label; *c; ++c)
            if (*c != '&') text += *c;
        // Inaccessible always leads the menu, as in TortoiseGit.
        if (std::string_view(r.id) == "inaccessible")
        {
            rows.push_back(check{text, true}.enabled(false));
            group_started = true;
            continue;
        }
        st->top_entries.emplace_back(r.id, nullptr);
        rows.push_back(check{text, chosen(r)}.bind(st->top_entries.back().second));
        group_started = true;
    }
    return vbox{
        label{"Checked entries appear directly in the file manager's context menu, with\n"
              "\"Git\" before them; the rest are in its Tortilla submenu."},
        // (vbox takes a fixed list; this one's built from the table.)
        scroll{rap::node{rap::spec::box{rap::layout::vertical, std::move(rows)}, {}}}.stretch(),
    };
}

rap::node diff_viewer_page(std::shared_ptr<state> st)
{
    using namespace rap::layout;
    const std::string git_tool = git_diff_tool(st->repo_dir);
    st->tools = detect_diff_tools();

    std::vector<std::string> choices{"Tortilla's unified diff viewer",
                                     git_tool.empty()
                                         ? "git difftool (no diff.tool configured)"
                                         : "git difftool (diff.tool: " + git_tool + ")"};
    for (const detected_tool& t : st->tools) choices.push_back(t.name);
    choices.push_back("Custom...");

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
    merge_choices.push_back("Custom...");
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
            "Comparing revisions",
            vbox{
                form{
                    {"Compare with",
                     drop_down{choices, current}.bind(st->diff_tool_select).on_changed(chosen)},
                    {"Command", entry{command}
                                    .bind(st->diff_command)
                                    .placeholder("program %base %mine")
                                    .visible(custom)},
                },
                label{"%base and %mine are the two sides compared: folders when a whole commit is\n"
                      "compared. %bname and %yname are their names. Without placeholders the two\n"
                      "sides go at the end of the command."}
                    .handle(st->custom_help)
                    .visible(custom),
            }},
        group{"Resolving conflicts",
              vbox{
                  form{
                      {"Merge with", drop_down{merge_choices, merge_current}
                                         .bind(st->merge_select)
                                         .on_changed(merge_chosen)},
                      {"Command", entry{saved_merge.empty() && !st->merge_tools.empty()
                                            ? st->merge_tools.front().command
                                            : saved_merge}
                                      .bind(st->merge_command)
                                      .placeholder("program %base %theirs %mine %merged")
                                      .visible(merge_custom)},
                  },
                  label{"%base, %theirs and %mine are the conflict's three sides and %merged the "
                        "file to save;\n"
                        "%bname, %tname, %yname and %mname their titles. Without placeholders the "
                        "four\n"
                        "files go at the end."}
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

    std::vector<std::string> choices{git_command.empty() ? "git's own (ssh)"
                                                         : "git's own (" + git_command + ")"};
    for (const ssh_client& c : st->ssh_clients) choices.push_back(c.name);
    choices.push_back("Custom...");

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
            "SSH client",
            vbox{
                form{
                    {"SSH client",
                     drop_down{choices, current}.bind(st->ssh_select).on_changed(chosen)},
                    {"Command", entry{saved.command}
                                    .bind(st->ssh_command)
                                    .placeholder("ssh -i ~/.ssh/key")
                                    .visible(custom)},
                },
                label{"Run for each remote as git's GIT_SSH_COMMAND: git adds the host and the\n"
                      "command to run there. plink and TortoiseGitPlink are known by their names."
#ifdef _WIN32
                      // Git for Windows runs it through its MSYS shell, which
                      // turns /tmp/key into C:/.../Temp/key before WSL sees it.
                      "\nGit for Windows rewrites absolute Linux paths (/...) in it: for WSL ssh,\n"
                      "write them from home (~/...)."
#endif
                }
                    .handle(st->ssh_help)
                    .visible(custom),
            }},
        label{"Used by the git Tortilla runs (fetch, pull, push, clone); the repository's\n"
              "core.sshCommand applies only with git's own."},
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
        dialog{"Settings - Tortilla",
               {vbox{
                   tabs{
                       // Weak: the window that keeps these is the state's own.
                       page{"General", lazy{[weak, build] { return build(weak, general_page); }}},
                       page{"Context Menu",
                            lazy{[weak, build] { return build(weak, context_menu_page); }}},
                       page{"Diff Viewer",
                            lazy{[weak, build] { return build(weak, diff_viewer_page); }}},
                       page{"Network", lazy{[weak, build] { return build(weak, network_page); }}},
                   }
                       .sidebar()
                       .bind(page_tabs)
                       .stretch(),
                   buttons{ok, cancel},
               }}}
            .size(96_ch, 22_lines),
        parent);

    st->window->on_finished(
        [st, saved, closed, &ui](rap::dialog_role role) mutable
        {
            auto keep = std::move(st);
            if (role == rap::dialog_role::ok)
            {
                settings& settings = settings::get();
                if (keep->icon_style)
                    settings.set(
                        "log.actionIcons",
                        std::to_string(
                            int(k_icon_styles[std::clamp(keep->icon_style->current(), 0, 3)])));
                if (!keep->top_entries.empty())
                {
                    std::string ids;
                    for (auto& [id, box] : keep->top_entries)
                        if (box && box->checked()) ids += (ids.empty() ? "" : ",") + id;
                    settings.set(std::string(shell_menu::k_top_setting), ids);
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
