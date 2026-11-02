#include "history_dialogs.h"

#include "i18n.h"
#include "ui/repo_icon.h"

#include <memory>

namespace tortilla
{

void history_message_dialog(rap::ui& ui, rap::window* parent, const std::string& title,
                            const std::string& intro, const std::string& text,
                            const std::string& warning, std::function<void(std::string)> done)
{
    struct state
    {
        std::unique_ptr<rap::window> window;
        rap::text_view* text = nullptr;
    };
    auto st = std::make_shared<state>();
    using namespace rap::layout;
    using namespace rap::literals;
    st->window = create_window(
        ui,
        dialog{title,
               {vbox{label{intro}.wrap(), text_view{text}.bind(st->text).stretch(),
                     label{warning}.wrap().visible(!warning.empty()), buttons{ok, cancel}}}}
            .size(72_ch, 26_lines),
        parent);
    st->window->on_finished(
        [&ui, st, done = std::move(done)](rap::dialog_role role)
        {
            std::string message = st->text->text();
            ui.post([st] {});  // the window goes after this returns
            if (role == rap::dialog_role::ok) done(std::move(message));
        });
    st->window->show();
    st->text->focus();
}

void move_commits_dialog(rap::ui& ui, rap::window* parent, const std::string& intro,
                         std::vector<std::string> branches, const std::string& warning,
                         std::function<void(move_target)> done)
{
    struct state
    {
        std::unique_ptr<rap::window> window;
        std::vector<std::string> branches;
        rap::drop_down* branch = nullptr;
        rap::entry* name = nullptr;
    };
    auto st = std::make_shared<state>();
    st->branches = std::move(branches);
    // The branches, then a new one, named in the box below.
    std::vector<std::string> items = st->branches;
    items.push_back(_("A new branch"));
    using namespace rap::layout;
    using namespace rap::literals;
    st->window = create_window(
        ui,
        dialog{
            _("Move Commits to Branch"),
            {vbox{label{intro}.wrap(),
                  form{
                      {_("Branch"), drop_down{items, 0}
                                        .bind(st->branch)
                                        .on_changed(
                                            [w = std::weak_ptr(st)](int i)
                                            {
                                                if (auto s = w.lock(); s && s->name)
                                                {
                                                    const bool create =
                                                        size_t(i) == s->branches.size();
                                                    s->name->set_enabled(create);
                                                    if (create) s->name->focus();
                                                }
                                            })},
                      {_("New branch"), entry{}
                                            .bind(st->name)
                                            .placeholder(_("Its name"))
                                            .enabled(st->branches.empty())
                                            .width(40_ch)},
                  },
                  label{warning}.wrap().visible(!warning.empty()), spacer{}, buttons{ok, cancel}}}}
            .size(64_ch, 14_lines),
        parent);
    st->window->on_finished(
        [&ui, st, done = std::move(done)](rap::dialog_role role)
        {
            const size_t i = size_t(std::max(0, st->branch->current()));
            move_target t;
            if (i < st->branches.size())
                t.branch = st->branches[i];
            else
            {
                t.branch = st->name->text();
                t.create = true;
            }
            ui.post([st] {});  // the window goes after this returns
            if (role == rap::dialog_role::ok && !t.branch.empty()) done(std::move(t));
        });
    st->window->show();
}

}  // namespace tortilla
