#include "welcome_window.h"

#include "i18n.h"
#include "log/log_window.h"
#include "remote/clone_dialog.h"
#include "ui/github_mark.h"
#include "ui/settings_dialog.h"
#include "ui/windows.h"

#include <rapport/sys.h>

#include <algorithm>
#include <system_error>

namespace tortilla
{

// The mascot, built in from data/icons (CMakeLists.txt).
extern const uint8_t k_mascot_png[];
extern const size_t k_mascot_png_size;

namespace
{

constexpr const char* k_project_url = "https://github.com/TurkeyMan/tortilla";
constexpr float k_mascot_size = 144;

// A document shipped beside the program, else the project's copy online.
void open_document(const char* name)
{
    namespace fs = std::filesystem;
    const fs::path bin = rap::sys::executable_path().parent_path();
    for (const fs::path& p :
         {bin.parent_path() / name, bin.parent_path() / "share" / "doc" / "tortilla" / name})
    {
        std::error_code ec;
        if (fs::is_regular_file(p, ec) && rap::sys::open_path(p)) return;
    }
    rap::sys::open_url(std::string(k_project_url) + "/blob/master/" + name);
}

}  // namespace

std::unique_ptr<welcome_window> welcome_window::open(rap::ui& ui)
{
    std::unique_ptr<welcome_window> w(new welcome_window(ui));
    w->build();
    return w;
}

void welcome_window::build()
{
    if (auto png = rap::sys::decode_image({k_mascot_png, k_mascot_png_size}))
        mascot_ = ui_.create_image(png->width, png->height, png->format, png->pixels.data(),
                                   size_t(png->width) * 4);

    using namespace rap::layout;
    window_ = ui_.create(
        dialog{
            _("Welcome to Tortilla"),
            {vbox{
                canvas{}
                    .on_paint([this](rap::painter& p, rap::size_f size) { paint_header(p, size); })
                    .height(rap::extent{k_mascot_size + 8, rap::extent::unit::px}),
                label{_("Tortilla is a git client that lives where your files do: right-click any\n"
                        "folder in your file manager to commit, pull, push, browse the history or\n"
                        "untangle a merge.")},
                label{_("It began with a simple wish: TortoiseGit's way of working, without the\n"
                        "waiting, on every desktop. So every window is your system's own, and\n"
                        "everything on screen keeps up with your display, however long the history.")},
                label{_("To start, right-click a folder and look for Tortilla, or open one here.")},
                hbox{
                    button{_("_Open repository...")}.on_click([this] { open_repository(); }),
                    button{_("_Clone...")}.on_click(
                        [this]
                        {
                            if (auto d = clone_dialog::open(ui_, {})) windows::keep(std::move(d));
                        }),
                    button{_("_Settings...")}.on_click(
                        [this] { show_settings(ui_, window_.get(), {}, settings_page::general); }),
                    spacer{},
                },
                separator{},
                hbox{
                    // GitHub's mark is part of the link: one click, one pointer.
                    label{"github.com/TurkeyMan/tortilla"}
                        .link([] { rap::sys::open_url(k_project_url); })
                        .icon(rap::icon{.draw =
                                            [](rap::painter& p, rap::size_f size)
                                        {
                                            const float side = std::min(size.w, size.h);
                                            paint_github_mark(
                                                p, (size.w - side) / 2, (size.h - side) / 2, side,
                                                rap::paint{rap::theme_color::window_text});
                                        }}),
                    spacer{},
                    label{_("Made with care by Manu Evans")},
                },
                hbox{
                    label{_("Licence (GPL 3.0)")}.link([] { open_document("LICENSE"); }),
                    label{_("Third-party notices")}.link([] { open_document("NOTICES.md"); }),
                    spacer{},
                },
                buttons{rap::layout::close()},
            }}}
            .resizable(false),  // it's read, not worked in
        nullptr);
    window_->on_finished([this](rap::dialog_role) { close(); });
}

// The mascot and the name beside it.
void welcome_window::paint_header(rap::painter& p, rap::size_f size)
{
    // A canvas starts undefined (black on some backends): the window's colour.
    p.rect({0, 0, size.w, size.h}, 0, rap::paint{rap::theme_color::window}, std::nullopt);
    if (mascot_)
        p.image(*mascot_, {0, 0, float(mascot_->width()), float(mascot_->height())},
                {0, (size.h - k_mascot_size) / 2, k_mascot_size, k_mascot_size});
    const float x = k_mascot_size + 20;
    const rap::text_style title{.fg = rap::paint{rap::theme_color::window_text}, .bold = true,
                                .size = 26};
    const rap::text_style line{.fg = rap::paint{rap::theme_color::window_text}};
    const rap::text_style motto{.fg = rap::paint{rap::theme_color::placeholder_text},
                                .italic = true, .size = 12};
    const rap::size_f t = p.measure("Tortilla", title);
    float y = (size.h - t.h * 2.4f) / 2;
    p.text({x, y}, "Tortilla", title);
    y += t.h;
    const std::string version = rap::i18n::format(_("Version {0}"), TORTILLA_VERSION);
    p.text({x + 2, y}, version, line);
    y += p.measure(version, line).h + 4;
    p.text({x + 2, y}, _("Git, wrapped up warm."), motto);
}

void welcome_window::open_repository()
{
    ui_.choose_files(window_.get(),
                     {.mode = rap::file_dialog_spec::mode::folder, .title = _("Open repository")},
                     [this](std::vector<std::string> paths)
                     {
                         if (paths.empty()) return;
                         auto log = log_window::open(ui_, paths.front());
                         if (!log)
                         {
                             ui_.message(window_.get(), {.icon = rap::message_spec::icon::error,
                                                         .title = "Tortilla",
                                                         .text = log.error()});
                             return;
                         }
                         windows::keep(std::move(*log));
                         // The log takes over; this has done its job.
                         window_->close();
                         close();
                     });
}

// window::close() only hides it: what closing means is ours.
void welcome_window::close()
{
    if (closed_) std::exchange(closed_, {})();
}

}  // namespace tortilla
