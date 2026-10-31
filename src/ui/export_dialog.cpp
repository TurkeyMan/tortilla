#include "export_dialog.h"

#include "git/archive.h"
#include "i18n.h"
#include "settings.h"
#include "ui/progress_dialog.h"
#include "ui/repo_icon.h"

#include <rapport/sys.h>

#include <cctype>
#include <chrono>
#include <filesystem>
#include <memory>
#include <thread>

namespace tortilla
{
namespace
{

namespace fs = std::filesystem;

enum target
{
    zip_file,
    folder,
};

struct state
{
    std::unique_ptr<rap::window> window;
    rap::entry* version = nullptr;
    rap::drop_down* to = nullptr;
    rap::entry* path = nullptr;
    std::string browsed;  // the path the file chooser gave, which asked about replacing it
};

std::string with_zip(std::string path, bool zip)
{
    const bool has = path.size() > 4 && path.ends_with(".zip");
    if (zip && !has && !path.empty()) path += ".zip";
    if (!zip && has) path.resize(path.size() - 4);
    return path;
}

// Folder mode: git archive's tar into a temporary file, unpacked into the
// folder once git is done, off the UI thread; then the folder opens if Open
// folder was chosen. `parent` may be gone by then, so errors show alone.
void export_to_folder(rap::ui& ui, rap::window* parent, const std::string& workdir,
                      const std::string& rev, const fs::path& out, std::function<void()> done)
{
    std::error_code ec;
    const fs::path tmp_dir = fs::temp_directory_path(ec) / "tortilla";
    fs::create_directories(tmp_dir, ec);
    const fs::path tar =
        tmp_dir /
        ("export-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
         ".tar");
    auto open = std::make_shared<bool>(false);
    std::vector<progress_follow_up> follow_ups;
    follow_ups.push_back({.label = _("_Open folder"), .after_close = [open] { *open = true; }});
    run_progress_dialog(
        ui, parent, _("Export"), workdir,
        {archive_args(archive_format::tar, rev, rap::sys::path_to_utf8(tar))},
        [&ui, tar, out, open, done](bool ok)
        {
            if (!ok)
            {
                std::error_code ec;
                fs::remove(tar, ec);
                if (done) done();
                return;
            }
            std::thread(
                [&ui, tar, out, open, done]
                {
                    extract_result r = extract_tar(tar, out);
                    std::error_code ec;
                    fs::remove(tar, ec);
                    ui.post(
                        [&ui, r = std::move(r), out, open, done]
                        {
                            if (!r.ok())
                            {
                                std::string detail = r.error;
                                for (const std::string& f : r.failed) detail += f + "\n";
                                ui.message(nullptr,
                                           {.icon = rap::message_spec::icon::error,
                                            .title = _("Export"),
                                            .text = r.error.empty()
                                                        ? _("Some files could not be exported.")
                                                        : rap::i18n::format(
                                                              _("Could not export into {0}."),
                                                              rap::sys::path_to_utf8(out)),
                                            .detail = r.error.empty() ? detail : ""},
                                           [done](rap::dialog_role)
                                           {
                                               if (done) done();
                                           });
                                return;
                            }
                            if (*open) rap::sys::open_path(out);
                            if (done) done();
                        });
                })
                .detach();
        },
        std::move(follow_ups));
}

void start(rap::ui& ui, rap::window* parent, const std::string& workdir, const std::string& rev,
           bool zip, const fs::path& out, std::function<void()> done)
{
    if (!zip) return export_to_folder(ui, parent, workdir, rev, out, std::move(done));
    std::vector<progress_follow_up> follow_ups;
    follow_ups.push_back(
        {.label = _("_Open folder"), .after_close = [out] { rap::sys::reveal(out); }});
    run_progress_dialog(
        ui, parent, _("Export"), workdir,
        {archive_args(archive_format::zip, rev, rap::sys::path_to_utf8(out))},
        [done](bool)
        {
            if (done) done();
        },
        std::move(follow_ups));
}

}  // namespace

void export_revision(rap::ui& ui, rap::window* parent, const std::string& workdir,
                     const std::string& rev, std::function<void()> done)
{
    auto st = std::make_shared<state>();

    // The folder last exported into, else the one around the repository.
    std::string top = workdir;
    while (top.size() > 1 && (top.back() == '/' || top.back() == '\\')) top.pop_back();
    fs::path dir = rap::sys::path_from_utf8(settings::get().value("export.dir"));
    if (dir.empty()) dir = rap::sys::path_from_utf8(top).parent_path();
    dir.make_preferred();
    const bool zip = settings::get().value("export.folder") != "1";
    const std::string version = rev.empty() ? "HEAD" : rev;
    const std::string path =
        rap::sys::path_to_utf8(dir / rap::sys::path_from_utf8(export_name(workdir, version)));

    using namespace rap::layout;
    using namespace rap::literals;
    st->window = create_window(
        ui,
        dialog{_("Export"),
               {vbox{
                   form{
                       {_("Version"), entry{version}.bind(st->version).width(40_ch)},
                       {_("Export to"),
                        hbox{
                            drop_down{{_("Zip file"), _("Folder")}, zip ? zip_file : folder}
                                .bind(st->to)
                                .on_changed(
                                    [w = std::weak_ptr(st)](int to)
                                    {
                                        if (auto s = w.lock(); s && s->path)
                                            s->path->set_text(
                                                with_zip(s->path->text(), to == zip_file));
                                    }),
                            entry{with_zip(path, zip)}.bind(st->path).stretch().width(50_ch),
                            button{_("Bro_wse...")}.on_click(
                                [&ui, w = std::weak_ptr(st)]
                                {
                                    auto s = w.lock();
                                    if (!s) return;
                                    const bool zip = s->to->current() == zip_file;
                                    rap::file_dialog_spec spec{
                                        .mode = zip ? rap::file_dialog_spec::mode::save
                                                    : rap::file_dialog_spec::mode::folder,
                                        .title =
                                            zip ? _("Export to zip file") : _("Export into folder"),
                                        .initial_path = s->path->text()};
                                    if (zip) spec.filters = {{_("Zip files"), "*.zip"}};
                                    ui.choose_files(s->window.get(), spec,
                                                    [w](std::vector<std::string> chosen)
                                                    {
                                                        auto s = w.lock();
                                                        if (!s || chosen.empty()) return;
                                                        s->browsed = chosen.front();
                                                        s->path->set_text(chosen.front());
                                                    });
                                }),
                        }},
                   },
                   buttons{ok, cancel},
               }}}
            .resizable(false),
        parent);
    st->window->on_finished(
        [st, &ui, parent, workdir, dir, done](rap::dialog_role role) mutable
        {
            auto keep = std::move(st);
            ui.post([keep] {});  // destroy after this callback returns
            if (role != rap::dialog_role::ok)
            {
                if (done) done();
                return;
            }
            std::string rev = keep->version->text();
            while (!rev.empty() && std::isspace(static_cast<unsigned char>(rev.back())))
                rev.pop_back();
            while (!rev.empty() && std::isspace(static_cast<unsigned char>(rev.front())))
                rev.erase(0, 1);
            if (rev.empty()) rev = "HEAD";
            const bool zip = keep->to->current() == zip_file;
            std::string typed = keep->path->text();
            if (typed.empty())
                typed = with_zip(rap::sys::path_to_utf8(
                                     dir / rap::sys::path_from_utf8(export_name(workdir, rev))),
                                 zip);
            // git runs in the repository, so a relative path is taken from
            // the suggested folder instead, as it was shown.
            const fs::path out = (dir / rap::sys::path_from_utf8(typed)).lexically_normal();
            settings::get().set("export.dir", rap::sys::path_to_utf8(out.parent_path()));
            settings::get().set("export.folder", zip ? "0" : "1");

            // Writing over what's there asks first, unless the file chooser
            // just did.
            std::error_code ec;
            const bool replaces = zip ? typed != keep->browsed && fs::exists(out, ec)
                                      : fs::is_directory(out, ec) && !fs::is_empty(out, ec);
            if (!replaces) return start(ui, parent, workdir, rev, zip, out, done);
            const std::string shown = rap::sys::path_to_utf8(out);
            ui.message(parent,
                       {.icon = rap::message_spec::icon::question,
                        .title = _("Export"),
                        .text = zip ? rap::i18n::format(_("{0} already exists. Replace it?"), shown)
                                    : rap::i18n::format(_("{0} is not empty. Export into it, "
                                                          "replacing files of the same name?"),
                                                        shown),
                        .buttons = {rap::dialog_role::yes, rap::dialog_role::no}},
                       [&ui, parent, workdir, rev, zip, out, done](rap::dialog_role r)
                       {
                           if (r == rap::dialog_role::yes)
                               start(ui, parent, workdir, rev, zip, out, done);
                           else if (done)
                               done();
                       });
        });
    st->window->show();
}

}  // namespace tortilla
