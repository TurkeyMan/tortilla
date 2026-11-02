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

// Apply Patch Serial's list: the patch files, in the order git am takes
// them.
class patch_rows : public rap::list_model
{
public:
    std::vector<std::string> paths;
    void changed() { notify_reset(); }
    size_t row_count() const override { return paths.size(); }
    std::string text(size_t row, size_t column) const override
    {
        const auto p = rap::sys::path_from_utf8(paths[row]);
        return rap::sys::path_to_utf8(column == 0 ? p.filename()
                                                  : p.parent_path().make_preferred());
    }
};

// Where git am stops (a patch that doesn't apply): go on once it's
// resolved, skip that patch, or abort, as TortoiseGit's dialog offers.
std::vector<progress_follow_up> am_follow_ups()
{
    auto when_stopped = [](bool ok) { return !ok; };
    return {{.label = _("_Resolved"),
             .commands = [] { return std::vector<git_command>{{"am", "--continue"}}; },
             .offered = when_stopped},
            {.label = _("_Skip patch"),
             .commands = [] { return std::vector<git_command>{{"am", "--skip"}}; },
             .offered = when_stopped},
            {.label = _("_Abort"),
             .commands = [] { return std::vector<git_command>{{"am", "--abort"}}; },
             .offered = when_stopped}};
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

void apply_patch_serial(rap::ui& ui, rap::window* parent, const std::string& workdir,
                        std::vector<std::string> patches, std::function<void(bool ok)> done)
{
    const std::string title = rap::i18n::format(_("{0} - Apply Patch Serial"), shown_path(workdir));

    // A git am stopped part way: that first.
    auto r = repo::open(workdir);
    if (r && am_in_progress(r->git_dir()))
    {
        auto w = std::make_shared<std::unique_ptr<rap::window>>();
        using namespace rap::layout;
        *w = create_window(
            ui,
            dialog{_("Apply Patch Serial"),
                   {vbox{
                       label{_("git am stopped part way through a series of patches. Once the "
                               "patch that stopped it is resolved, go on; or skip that patch, or "
                               "abort the series.")}
                           .wrap(),
                       buttons{dialog_button{rap::dialog_role::yes, _("_Resolved")},
                               dialog_button{rap::dialog_role::no, _("_Skip patch")},
                               dialog_button{rap::dialog_role::reset, _("_Abort")},
                               dialog_button{rap::dialog_role::cancel}},
                   }}}
                .resizable(false),
            parent);
        (*w)->on_finished(
            [w, &ui, parent, workdir, title, done](rap::dialog_role role)
            {
                ui.post([w] {});  // destroy after this callback returns
                const char* step = role == rap::dialog_role::yes     ? "--continue"
                                   : role == rap::dialog_role::no    ? "--skip"
                                   : role == rap::dialog_role::reset ? "--abort"
                                                                     : nullptr;
                if (!step)
                {
                    if (done) done(false);
                    return;
                }
                run_progress_dialog(ui, parent, title, workdir, {{"am", step}}, done,
                                    am_follow_ups());
            });
        (*w)->show();
        return;
    }

    struct state
    {
        std::unique_ptr<rap::window> window;
        patch_rows rows;
        rap::list* list = nullptr;
        rap::check* three_way = nullptr;
        rap::check* ignore_space = nullptr;
        rap::check* keep_cr = nullptr;
        rap::check* signoff = nullptr;
        rap::button* apply = nullptr;

        void changed(std::optional<size_t> select = {})
        {
            rows.changed();
            if (select) list->select({*select});
            apply->set_enabled(!rows.paths.empty());
        }
        // The selected patch moved by `by` (-1 up, +1 down).
        void move(int by)
        {
            const auto cur = list->current();
            if (!cur) return;
            const size_t to = *cur + by;
            if (by < 0 && *cur == 0) return;
            if (to >= rows.paths.size()) return;
            std::swap(rows.paths[*cur], rows.paths[to]);
            changed(to);
        }
    };
    auto st = std::make_shared<state>();
    st->rows.paths = std::move(patches);

    using namespace rap::layout;
    using namespace rap::literals;
    state* s = st.get();
    st->window = create_window(
        ui,
        dialog{_("Apply Patch Serial"),
               {vbox{
                   label{_("The patches are applied in this order, a commit each.")},
                   hbox{list{{{_("Patch"), 30_ch, true}, {_("Folder"), 30_ch, true}}, &s->rows}
                            .bind(s->list)
                            .selection(rap::selection_mode::single)
                            .compact()
                            .height(10_lines)
                            .stretch(),
                        vbox{button{_("_Add...")}.on_click(
                                 [&ui, s]
                                 {
                                     ui.choose_files(
                                         s->window.get(),
                                         {.mode = rap::file_dialog_spec::mode::open_many,
                                          .title = _("Add patches"),
                                          .filters = {{_("Patches"), "*.patch;*.diff"},
                                                      {_("All files"), "*"}}},
                                         [s](std::vector<std::string> chosen)
                                         {
                                             // In name order, as format-patch numbers them.
                                             std::ranges::sort(chosen);
                                             for (std::string& p : chosen)
                                                 if (!std::ranges::contains(s->rows.paths, p))
                                                     s->rows.paths.push_back(std::move(p));
                                             s->changed();
                                         });
                                 }),
                             button{_("_Remove")}.on_click(
                                 [s]
                                 {
                                     const auto cur = s->list->current();
                                     if (!cur || *cur >= s->rows.paths.size()) return;
                                     s->rows.paths.erase(s->rows.paths.begin() + long(*cur));
                                     s->changed();
                                 }),
                             button{_("_Up")}.on_click([s] { s->move(-1); }),
                             button{_("_Down")}.on_click([s] { s->move(1); }), spacer{}}}
                       .stretch(),
                   check{_("_3-way merge where a patch doesn't apply (--3way)")}.bind(s->three_way),
                   check{_("_Ignore changes in whitespace (--ignore-space-change)")}.bind(
                       s->ignore_space),
                   check{_("_Keep carriage returns (--keep-cr)")}.bind(s->keep_cr),
                   check{_("Add _Signed-off-by (--signoff)")}.bind(s->signoff),
                   buttons{dialog_button{rap::dialog_role::ok, _("_Apply")}.bind(s->apply),
                           dialog_button{rap::dialog_role::cancel}},
               }}}
            .size(72_ch, 24_lines),
        parent);
    s->changed();
    st->window->on_finished(
        [st, &ui, parent, workdir, title, done](rap::dialog_role role) mutable
        {
            auto keep = std::move(st);
            ui.post([keep] {});  // destroy after this callback returns
            if (role != rap::dialog_role::ok || keep->rows.paths.empty())
            {
                if (done) done(false);
                return;
            }
            const am_options o{.three_way = keep->three_way->checked(),
                               .ignore_space = keep->ignore_space->checked(),
                               .keep_cr = keep->keep_cr->checked(),
                               .signoff = keep->signoff->checked()};
            run_progress_dialog(ui, parent, title, workdir, {am_args(o, keep->rows.paths)}, done,
                                am_follow_ups());
        });
    st->window->show();
}

}  // namespace tortilla
