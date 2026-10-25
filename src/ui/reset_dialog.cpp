#include "reset_dialog.h"

#include "changes/cleanup_dialog.h"
#include "i18n.h"
#include "ui/progress_dialog.h"

#include <algorithm>
#include <memory>

namespace tortilla
{

void reset_to(rap::ui& ui, rap::window* parent, const std::string& workdir,
              const std::string& branch, const std::string& hash, const std::string& summary,
              std::function<void(bool ok)> done)
{
    struct state
    {
        std::unique_ptr<rap::window> window;
        rap::drop_down* type = nullptr;
    };
    auto st = std::make_shared<state>();

    using namespace rap::layout;
    st->window = ui.create(
        dialog{_("Reset"),
               {vbox{
                   label{rap::i18n::format(_("Reset \"{0}\" to {1} {2}"), branch, hash.substr(0, 8),
                                           summary)},
                   form{
                       {_("Reset Type"),
                        drop_down{
                            {_("Soft: Leave working tree and index untouched"),
                             _("Mixed: Leave working tree untouched, reset index"),
                             _("Hard: Reset working tree and index (discard all local changes)")},
                            1}
                            .bind(st->type)},
                   },
                   buttons{ok, cancel},
               }}}
            .resizable(false),
        parent);
    st->window->on_finished(
        [st, &ui, parent, workdir, hash, done](rap::dialog_role role) mutable
        {
            auto keep = std::move(st);
            if (role == rap::dialog_role::ok)
            {
                static const char* modes[] = {"--soft", "--mixed", "--hard"};
                const int i = std::clamp(keep->type->current(), 0, 2);
                // After a hard reset, Clean up for what it left untracked.
                std::vector<progress_follow_up> follow_ups;
                if (i == 2)
                    follow_ups.push_back({.label = _("_Clean up..."),
                                          .after_close = [&ui, parent, workdir]
                                          { clean_up(ui, parent, {workdir}, [](bool) {}); }});
                run_progress_dialog(ui, parent, _("Reset"), workdir, {{"reset", modes[i], hash}},
                                    done, std::move(follow_ups));
            }
            ui.post([keep] {});  // destroy after this callback returns
        });
    st->window->show();
}

}  // namespace tortilla
