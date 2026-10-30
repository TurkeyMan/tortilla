#include "format_patch_dialog.h"

#include "i18n.h"
#include "settings.h"
#include "ui/progress_dialog.h"

#include <filesystem>
#include <memory>

namespace tortilla
{

namespace fs = std::filesystem;

void format_patch(rap::ui& ui, rap::window* parent, const std::string& workdir,
                  const patch_range& range, const std::string& what)
{
    struct state
    {
        std::unique_ptr<rap::window> window;
        rap::entry* folder = nullptr;
        rap::check* signoff = nullptr;
    };
    auto st = std::make_shared<state>();

    // The folder last used for this repository, else its top folder.
    fs::path top = rap::sys::path_from_utf8(workdir);
    if (!top.has_filename()) top = top.parent_path();  // workdir ends in a separator
    top.make_preferred();
    const std::string key = "formatPatch.dir." + workdir;
    std::string folder = settings::get().value(key);
    if (folder.empty()) folder = rap::sys::path_to_utf8(top);

    using namespace rap::layout;
    using namespace rap::literals;
    st->window = ui.create(
        dialog{_("Format Patch"),
               {vbox{
                   form{
                       {_("Commits"), label{what}},
                       {_("Directory"),
                        hbox{
                            entry{folder}.bind(st->folder).stretch().width(50_ch),
                            button{_("Bro_wse...")}.on_click(
                                [&ui, w = std::weak_ptr(st)]
                                {
                                    auto s = w.lock();
                                    if (!s) return;
                                    ui.choose_files(s->window.get(),
                                                    {.mode = rap::file_dialog_spec::mode::folder,
                                                     .title = _("Write patches into"),
                                                     .initial_path = s->folder->text()},
                                                    [w](std::vector<std::string> chosen)
                                                    {
                                                        if (auto s = w.lock(); s && !chosen.empty())
                                                            s->folder->set_text(chosen.front());
                                                    });
                                }),
                        }},
                       {"", check{_("Add _Signed-off-by"),
                                  settings::get().value("formatPatch.signoff") == "1"}
                                .bind(st->signoff)},
                   },
                   buttons{ok, cancel},
               }}}
            .resizable(false),
        parent);
    st->window->on_finished(
        [st, &ui, parent, workdir, top, key, range](rap::dialog_role role) mutable
        {
            auto keep = std::move(st);
            if (role == rap::dialog_role::ok)
            {
                // git runs in the top folder, so a relative folder is taken
                // from there; opening it afterwards needs it whole.
                std::string folder = keep->folder->text();
                if (folder.empty()) folder = rap::sys::path_to_utf8(top);
                const fs::path out = top / rap::sys::path_from_utf8(folder);
                const bool signoff = keep->signoff->checked();
                settings::get().set(key, folder);
                settings::get().set("formatPatch.signoff", signoff ? "1" : "0");
                std::vector<progress_follow_up> follow_ups;
                follow_ups.push_back({.label = _("_Open folder"),
                                      .after_close = [out] { rap::sys::open_path(out); }});
                run_progress_dialog(
                    ui, parent, _("Format Patch"), workdir,
                    {format_patch_args(range, rap::sys::path_to_utf8(out), signoff)}, {},
                    std::move(follow_ups));
            }
            ui.post([keep] {});  // destroy after this callback returns
        });
    st->window->show();
}

}  // namespace tortilla
