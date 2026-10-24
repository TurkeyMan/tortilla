#include "stash_dialog.h"

#include "ui/progress_dialog.h"

#include <memory>

namespace tortilla
{

void stash_changes(rap::ui& ui, rap::window* parent, const std::string& workdir,
                   std::function<void(bool ok)> done)
{
    struct state
    {
        std::unique_ptr<rap::window> window;
        rap::entry* message = nullptr;
        rap::check* untracked = nullptr;
    };
    auto st = std::make_shared<state>();

    using namespace rap::layout;
    using namespace rap::literals;
    st->window = ui.create(
        dialog{"Stash",
               {vbox{
                   form{
                       {"Message", entry{}.bind(st->message).placeholder("optional").width(40_ch)},
                       {"", check{"Include _untracked files"}.bind(st->untracked)},
                   },
                   buttons{ok, cancel},
               }}}
            .resizable(false),
        parent);
    st->window->on_finished(
        [st, &ui, parent, workdir, done](rap::dialog_role role) mutable
        {
            auto keep = std::move(st);
            if (role == rap::dialog_role::ok)
            {
                git_command args{"stash", "push"};
                if (keep->untracked->checked()) args.push_back("--include-untracked");
                if (std::string msg = keep->message->text(); !msg.empty())
                    args.insert(args.end(), {"-m", msg});
                run_progress_dialog(ui, parent, "Stash", workdir, {args}, done);
            }
            ui.post([keep] {});  // destroy after this callback returns
        });
    st->window->show();
}

}  // namespace tortilla
