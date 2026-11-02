#include "patch_serial_dialogs.h"

#include "git/patch_serial.h"
#include "git/repo.h"
#include "i18n.h"
#include "ui/progress_dialog.h"
#include "ui/repo_icon.h"

#include <rapport/sys.h>

#include <algorithm>
#include <memory>
#include <utility>

namespace tortilla
{
namespace
{

std::string shown_path(const std::string& workdir)
{
    std::string root = workdir;
    while (root.size() > 1 && (root.back() == '/' || root.back() == '\\')) root.pop_back();
    return rap::sys::path_to_utf8(rap::sys::path_from_utf8(root).make_preferred());
}

}  // namespace

void create_patch_serial(rap::ui& ui, rap::window* parent, const std::string& workdir,
                         const std::string& folder, std::function<void(bool ok)> done)
{
    struct state
    {
        std::unique_ptr<rap::window> window;
        rap::entry* folder = nullptr;
        rap::drop_down* which = nullptr;
        rap::combo* since = nullptr;
        rap::spin* number = nullptr;
        rap::entry* from = nullptr;
        rap::entry* to = nullptr;
        // Only the fields of the choice made can be changed.
        void choose(int i)
        {
            since->set_enabled(i == 0);
            number->set_enabled(i == 1);
            from->set_enabled(i == 2);
            to->set_enabled(i == 2);
        }
    };
    auto st = std::make_shared<state>();

    // Since the current branch's upstream, by default, as TortoiseGit's.
    std::vector<std::string> refs;
    std::string upstream;
    if (auto r = repo::open(workdir))
    {
        refs = r->branch_names(true, true, true);
        upstream = r->upstream_of(r->head_name());
    }

    using namespace rap::layout;
    using namespace rap::literals;
    state* s = st.get();
    st->window = create_window(
        ui,
        dialog{_("Create Patch Serial"),
               {vbox{
                   form{
                       {_("Directory"),
                        hbox{entry{folder}.bind(s->folder).width(40_ch).stretch(),
                             button{_("_Browse...")}.on_click(
                                 [&ui, s]
                                 {
                                     ui.choose_files(s->window.get(),
                                                     {.mode = rap::file_dialog_spec::mode::folder,
                                                      .title = _("Write patches into"),
                                                      .initial_path = s->folder->text()},
                                                     [s](std::vector<std::string> chosen)
                                                     {
                                                         if (!chosen.empty())
                                                             s->folder->set_text(chosen.front());
                                                     });
                                 })}},
                       {_("Commits"),
                        drop_down{{_("Since a revision"), _("The newest commits"), _("A range")},
                                  upstream.empty() ? 1 : 0}
                            .bind(s->which)
                            .on_changed([s](int i) { s->choose(i); })},
                       {_("Since"), combo{refs, upstream}.bind(s->since)},
                       {_("Number"), spin{1, 9999, 1}.bind(s->number)},
                       {_("From"), entry{}.bind(s->from).placeholder(_("a revision"))},
                       {_("To"), entry{}.bind(s->to).placeholder("HEAD")},
                   },
                   buttons{dialog_button{rap::dialog_role::ok, _("_Create")},
                           dialog_button{rap::dialog_role::cancel}},
               }}}
            .resizable(false),
        parent);
    s->choose(s->which->current());
    st->window->on_finished(
        [st, &ui, parent, workdir, done](rap::dialog_role role) mutable
        {
            auto keep = std::move(st);
            ui.post([keep] {});  // destroy after this callback returns
            if (role != rap::dialog_role::ok)
            {
                if (done) done(false);
                return;
            }
            format_patch_options o{
                .folder = keep->folder->text(),
                .commits = format_patch_options::which(std::clamp(keep->which->current(), 0, 2)),
                .since = keep->since->text(),
                .number = int(keep->number->value()),
                .from = keep->from->text(),
                .to = keep->to->text()};
            const bool missing =
                o.folder.empty() ||
                (o.commits == format_patch_options::which::since && o.since.empty()) ||
                (o.commits == format_patch_options::which::range && o.from.empty());
            if (missing)
            {
                ui.message(parent,
                           {.icon = rap::message_spec::icon::error,
                            .title = _("Create Patch Serial"),
                            .text = o.folder.empty() ? _("Choose a directory for the patches.")
                                                     : _("Choose a revision to start from.")},
                           [done](rap::dialog_role)
                           {
                               if (done) done(false);
                           });
                return;
            }
            std::vector<progress_follow_up> follow_ups;
            follow_ups.push_back({.label = _("_Open folder"),
                                  .after_close = [out = o.folder]
                                  { rap::sys::open_path(rap::sys::path_from_utf8(out)); }});
            run_progress_dialog(
                ui, parent, rap::i18n::format(_("{0} - Create Patch Serial"), shown_path(workdir)),
                workdir, {format_patch_serial_args(o)}, done, std::move(follow_ups));
        });
    st->window->show();
}

}  // namespace tortilla
