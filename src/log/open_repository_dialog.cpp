#include "open_repository_dialog.h"

#include "i18n.h"
#include "settings.h"
#include "ui/modal_dialogs.h"
#include "ui/repo_icon.h"
#include "util/repo_path.h"

#include <rapport/sys.h>

#include <filesystem>
#include <memory>

namespace tortilla
{
namespace
{

constexpr const char* k_recent_key = "log.recentRepositories";
constexpr size_t k_recent_kept = 20;

// The recent repositories: each folder's name, then where it is.
class recent_rows : public rap::list_model
{
public:
    std::vector<std::string> paths;
    size_t row_count() const override { return paths.size(); }
    std::string text(size_t row, size_t column) const override
    {
        const std::filesystem::path p = rap::sys::path_from_utf8(paths[row]);
        if (column == 1) return rap::sys::path_to_utf8(std::filesystem::path(p).make_preferred());
        std::string name = rap::sys::path_to_utf8(p.filename());
        return name.empty() ? paths[row] : name;
    }
    rap::icon icon(size_t row, size_t column) const override
    {
        if (column != 0) return {};
        return rap::icon::for_path(paths[row], "folder");
    }
};

}  // namespace

std::vector<std::string> recent_repositories()
{
    std::vector<std::string> out;
    for (const std::string& p : settings::get().history(k_recent_key))
    {
        std::error_code ec;
        if (std::filesystem::is_directory(rap::sys::path_from_utf8(p), ec)) out.push_back(p);
    }
    return out;
}

void remember_repository(const std::string& path)
{
    if (path.empty()) return;
    settings::get().add_to_history(k_recent_key, folder_text(path), k_recent_kept);
}

void open_repository_dialog(rap::ui& ui, rap::window* parent,
                            std::function<void(const std::string& path)> done)
{
    struct state
    {
        std::unique_ptr<rap::window> window;
        recent_rows rows;
        rap::list* list = nullptr;
        bool finished = false;
    };
    auto st = std::make_shared<state>();
    st->rows.paths = recent_repositories();

    auto browse = [&ui, done](rap::window* over, std::function<void()> chosen)
    {
        pick_files(ui, over,
                   {.mode = rap::file_dialog_spec::mode::folder, .title = _("Open repository")},
                   [done, chosen](std::vector<std::string> paths)
                   {
                       if (paths.empty()) return;
                       if (chosen) chosen();
                       done(paths.front());
                   });
    };
    // Nothing recent: the folder picker is all there is to show.
    if (st->rows.paths.empty()) return browse(parent, {});

    // Once; the window (and with it what its callbacks hold) goes after
    // the callback that ended it returns.
    auto finish = [&ui, done](const std::shared_ptr<state>& s, const std::string& path)
    {
        if (s->finished) return;
        s->finished = true;
        s->window->close();
        ui.post([s] { s->window.reset(); });
        if (!path.empty()) done(path);
    };

    using namespace rap::layout;
    using namespace rap::literals;
    st->window = create_window(
        ui,
        dialog{_("Open Repository"),
               {vbox{
                   label{_("Recent repositories:")},
                   list{{{_("Name"), 20_ch}, {_("Location"), 44_ch, true}}, &st->rows}
                       .bind(st->list)
                       .selection(rap::selection_mode::single)
                       .on_activate(
                           [st, finish](size_t row)
                           {
                               if (row < st->rows.paths.size()) finish(st, st->rows.paths[row]);
                           })
                       .height(10_lines)
                       .stretch(),
                   hbox{
                       // The folder chosen opens; this has done its job.
                       button{_("_Browse...")}.on_click(
                           [st, browse, finish]
                           { browse(st->window.get(), [st, finish] { finish(st, {}); }); }),
                       spacer{},
                   },
                   buttons{ok(_("_Open")), cancel},
               }}}
            .size(72_ch, 22_lines),
        parent);
    if (st->list)
    {
        st->list->select({0});
        st->list->set_current(0);
    }
    st->window->on_finished(
        [st, finish](rap::dialog_role role)
        {
            std::string path;
            if (role == rap::dialog_role::ok && st->list)
                if (auto sel = st->list->selection();
                    !sel.empty() && sel.front() < st->rows.paths.size())
                    path = st->rows.paths[sel.front()];
            finish(st, path);
        });
    st->window->show();
}

}  // namespace tortilla
