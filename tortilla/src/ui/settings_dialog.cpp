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
constexpr ActionIconStyle kIconStyles[] = {ActionIconStyle::Document, ActionIconStyle::Glyph,
                                           ActionIconStyle::Bevel, ActionIconStyle::Line};

struct State
{
    std::unique_ptr<rap::Window> window;
    std::string repoDir;
    // Bound when their page is first shown; null until then, and an
    // unvisited page saves nothing.
    rap::Select* iconStyle = nullptr;
    rap::Select* diffMode = nullptr;
    rap::Entry* diffCommand = nullptr;
    std::vector<DetectedTool> tools;
};

rap::Node generalPage(std::shared_ptr<State> st)
{
    using namespace rap::layout;
    const auto saved = ActionIconStyle(
        std::clamp(std::atoi(Settings::get().value("log.actionIcons", "1").c_str()), 0, 3));
    const int current = int(std::find(std::begin(kIconStyles), std::end(kIconStyles), saved) -
                            std::begin(kIconStyles));
    return VBox{
        Group{"Log",
              Form{
                  {"Action icons",
                   Select{{"Classic", "Glyphs", "Bevelled", "Line"}, current}.bind(st->iconStyle)},
              }},
        Spacer{},
    };
}

rap::Node diffViewerPage(std::shared_ptr<State> st)
{
    using namespace rap::layout;
    const std::string gitTool = gitDiffTool(st->repoDir);
    st->tools = detectDiffTools();
    std::weak_ptr<State> weak = st;
    auto showCommand = [weak](int mode)
    {
        if (auto s = weak.lock(); s && s->diffCommand)
            s->diffCommand->setEnabled(mode == int(DiffMode::Custom));
    };
    const DiffMode mode = diffMode(st->repoDir);
    return VBox{
        Group{
            "Comparing revisions",
            VBox{
                Form{
                    {"Compare with",
                     Select{{"Tortilla's unified diff viewer",
                             gitTool.empty() ? "git difftool (no diff.tool configured)"
                                             : "git difftool (diff.tool: " + gitTool + ")",
                             "Another program:"},
                            int(mode)}
                         .bind(st->diffMode)
                         .onChanged(showCommand)},
                    {"Command",
                     HBox{
                         Entry{diffCommand()}
                             .bind(st->diffCommand)
                             .placeholder("program %base %mine")
                             .enabled(mode == DiffMode::Custom)
                             .stretch(),
                         MenuButton{"_Detect"}.onMenu(
                             [weak]
                             {
                                 rap::Menu m;
                                 auto s = weak.lock();
                                 if (!s) return m;
                                 for (const DetectedTool& t : s->tools)
                                     m.push_back({.label = t.name,
                                                  .action = [weak, command = t.command]
                                                  {
                                                      // Picking a tool means using it.
                                                      if (auto s = weak.lock())
                                                      {
                                                          s->diffMode->setCurrent(
                                                              int(DiffMode::Custom));
                                                          s->diffCommand->setEnabled(true);
                                                          s->diffCommand->setText(command);
                                                      }
                                                  }});
                                 if (m.empty())
                                     m.push_back(
                                         {.label = "No diff tools found", .enabled = false});
                                 return m;
                             }),
                     }},
                },
                Label{"%base and %mine are the two sides compared: folders when a whole commit is\n"
                      "compared. %bname and %yname are their names. Without placeholders the two\n"
                      "sides go at the end of the command."},
            }},
        Spacer{},
    };
}

}  // namespace

void showSettings(rap::Ui& ui, rap::Window* parent, const std::string& repoDir,
                  SettingsPage first, std::function<void()> saved, std::function<void()> closed)
{
    auto st = std::make_shared<State>();
    st->repoDir = repoDir;
    std::weak_ptr<State> weak = st;
    auto build = [](const std::weak_ptr<State>& w, rap::Node (*page)(std::shared_ptr<State>))
    {
        auto s = w.lock();
        return s ? page(s) : rap::Node{};
    };
    rap::Tabs* tabs = nullptr;

    using namespace rap::layout;
    // Lazy pages don't size the dialog, so it has a size of its own.
    st->window = ui.create(
        Dialog{"Settings - Tortilla",
               {VBox{
                   Tabs{
                       // Weak: the window that keeps these is the state's own.
                       Page{"General", Lazy{[weak, build] { return build(weak, generalPage); }}},
                       Page{"Diff Viewer",
                            Lazy{[weak, build] { return build(weak, diffViewerPage); }}},
                   }
                       .sidebar()
                       .bind(tabs)
                       .stretch(),
                   Buttons{Ok, Cancel},
               }}}
            .size(96_ch, 22_lines),
        parent);

    st->window->onFinished(
        [st, saved, closed, &ui](rap::DialogRole role) mutable
        {
            auto keep = std::move(st);
            if (role == rap::DialogRole::Ok)
            {
                Settings& settings = Settings::get();
                if (keep->iconStyle)
                    settings.set("log.actionIcons",
                                 std::to_string(int(
                                     kIconStyles[std::clamp(keep->iconStyle->current(), 0, 3)])));
                if (keep->diffMode)
                    saveDiffSettings(DiffMode(std::clamp(keep->diffMode->current(), 0, 2)),
                                     keep->diffCommand->text());
                if (saved) saved();
            }
            if (closed) closed();
            ui.post([keep] {});  // destroy after this callback returns
        });
    tabs->setCurrent(int(first));
    st->window->show();
}

}  // namespace tortilla
