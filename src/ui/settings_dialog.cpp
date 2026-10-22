#include "settings_dialog.h"

#include "git/diff_tool.h"
#include "log/action_icons.h"
#include "settings.h"

#include <algorithm>
#include <cstdlib>
#include <memory>

namespace tortilla
{
namespace
{

// The View > Action Icons choices, in the menu's order.
constexpr action_icon_style k_icon_styles[] = {action_icon_style::document, action_icon_style::glyph,
                                           action_icon_style::bevel, action_icon_style::line};

struct state
{
    std::unique_ptr<rap::window> window;
    std::string repo_dir;
    // Bound when their page is first shown; null until then, and an
    // unvisited page saves nothing.
    rap::drop_down* icon_style = nullptr;
    rap::drop_down* diff_mode_select = nullptr;
    rap::entry* diff_command = nullptr;
    std::vector<detected_tool> tools;
};

rap::node general_page(std::shared_ptr<state> st)
{
    using namespace rap::layout;
    const auto saved = action_icon_style(
        std::clamp(std::atoi(settings::get().value("log.actionIcons", "1").c_str()), 0, 3));
    const int current = int(std::find(std::begin(k_icon_styles), std::end(k_icon_styles), saved) -
                            std::begin(k_icon_styles));
    return vbox{
        group{"Log",
              form{
                  {"Action icons",
                   drop_down{{"Classic", "Glyphs", "Bevelled", "Line"}, current}.bind(st->icon_style)},
              }},
        spacer{},
    };
}

rap::node diff_viewer_page(std::shared_ptr<state> st)
{
    using namespace rap::layout;
    const std::string git_tool = git_diff_tool(st->repo_dir);
    st->tools = detect_diff_tools();
    std::weak_ptr<state> weak = st;
    auto show_command = [weak](int mode)
    {
        if (auto s = weak.lock(); s && s->diff_command)
            s->diff_command->set_enabled(mode == int(diff_mode::custom));
    };
    const diff_mode mode = saved_diff_mode(st->repo_dir);
    return vbox{
        group{
            "Comparing revisions",
            vbox{
                form{
                    {"Compare with",
                     drop_down{{"Tortilla's unified diff viewer",
                             git_tool.empty() ? "git difftool (no diff.tool configured)"
                                             : "git difftool (diff.tool: " + git_tool + ")",
                             "Another program:"},
                            int(mode)}
                         .bind(st->diff_mode_select)
                         .on_changed(show_command)},
                    {"Command",
                     hbox{
                         entry{diff_command()}
                             .bind(st->diff_command)
                             .placeholder("program %base %mine")
                             .enabled(mode == diff_mode::custom)
                             .stretch(),
                         menu_button{"_Detect"}.on_menu(
                             [weak]
                             {
                                 rap::menu m;
                                 auto s = weak.lock();
                                 if (!s) return m;
                                 for (const detected_tool& t : s->tools)
                                     m.push_back({.label = t.name,
                                                  .action = [weak, command = t.command]
                                                  {
                                                      // Picking a tool means using it.
                                                      if (auto s = weak.lock())
                                                      {
                                                          s->diff_mode_select->set_current(
                                                              int(diff_mode::custom));
                                                          s->diff_command->set_enabled(true);
                                                          s->diff_command->set_text(command);
                                                      }
                                                  }});
                                 if (m.empty())
                                     m.push_back(
                                         {.label = "No diff tools found", .enabled = false});
                                 return m;
                             }),
                     }},
                },
                label{"%base and %mine are the two sides compared: folders when a whole commit is\n"
                      "compared. %bname and %yname are their names. Without placeholders the two\n"
                      "sides go at the end of the command."},
            }},
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
                       page{"Diff Viewer",
                            lazy{[weak, build] { return build(weak, diff_viewer_page); }}},
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
                    settings.set("log.actionIcons",
                                 std::to_string(int(
                                     k_icon_styles[std::clamp(keep->icon_style->current(), 0, 3)])));
                if (keep->diff_mode_select)
                    save_diff_settings(diff_mode(std::clamp(keep->diff_mode_select->current(), 0, 2)),
                                     keep->diff_command->text());
                if (saved) saved();
            }
            if (closed) closed();
            ui.post([keep] {});  // destroy after this callback returns
        });
    page_tabs->set_current(int(first));
    st->window->show();
}

}  // namespace tortilla
