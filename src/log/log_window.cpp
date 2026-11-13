#include "log_window.h"

#include "git/command.h"
#include "git/git_install.h"
#include "git/patch_text.h"
#include "i18n.h"
#include "settings.h"
#include "ui/repo_icon.h"

#include <thread>

namespace tortilla
{

std::expected<std::unique_ptr<log_window>, std::string>
log_window::open(rap::ui& ui, const std::string& path, bool all_branches, bool outer)
{
    auto tab = log_tab::open(ui, path, outer);
    if (!tab) return std::unexpected(tab.error());
    std::unique_ptr<log_window> w(new log_window(ui, std::move(*tab)));
    w->build();
    w->tab_->start(all_branches);
    return w;
}

log_window::log_window(rap::ui& ui, std::unique_ptr<log_tab> tab)
    : ui_(ui)
    , tab_(std::move(tab))
{
}

// The page's own (its date pop-up, over the window, and its timers) go
// before the window; the rest of the tab after it.
log_window::~log_window()
{
    if (tab_) tab_->drop_page();
}

void log_window::build()
{
    using namespace rap::literals;

    tab_->set_host(this);
    // (rap::layout's window: log_window::window is the tab's parent.)
    window_ = create_window(
        ui_, rap::layout::window{tab_->title_text(), {tab_->build_page()}}.size(140_ch, 50_lines));
    // The tab's icon; its app id is the window's from the start (create_window),
    // never changed after, as the taskbar would regroup it.
    if (tab_->identity().icon) window_->set_icon(tab_->identity().icon);
    window_->on_close_request(
        [this]
        {
            closing();
            if (closed_) closed_();
            return true;
        });
    window_->on_key([this](const rap::key_event& e) { return tab_ && tab_->key(e); });
    // Size and position persist across runs, as TortoiseGit's do; position
    // only where the platform lets apps place windows (not on Wayland).
    settings::get().restore_window("log.window", *window_);
    tab_->bind_page();
}

void log_window::closing()
{
    settings::get().save_window("log.window", *window_);
    tab_->save_view_settings();
    tab_->save_options();
    close_patch();
}

void log_window::tab_changed(log_tab& tab)
{
    window_->set_title(tab.title_text());
}

void log_window::finish(bool)
{
    closing();
    window_->close();
    if (closed_) closed_();
}

void log_window::show_patch(bool on)
{
    settings::get().set("log.viewPatch", on ? "1" : "0");
    if (on == patch_.has_value()) return;
    if (!on) return close_patch();
    // Closed by the user: off, and so next time. (Closed with the window, or
    // from the menu, patch_ has gone already.)
    patch_ = open_patch_viewer(ui_, _("View Patch"),
                               [this, alive = std::weak_ptr<bool>(alive_)]
                               {
                                   if (!alive.lock() || !patch_) return;
                                   patch_.reset();
                                   settings::get().set("log.viewPatch", "0");
                               });
    update_patch();
}

void log_window::close_patch()
{
    if (!patch_) return;
    patch_viewer p = std::move(*patch_);
    patch_.reset();  // first, so its closing leaves the setting as it is
    p.close();
}

void log_window::selection_moved(log_tab& tab)
{
    if (&tab == tab_.get()) update_patch();
}

// git runs off the UI thread, a moment after the selection settles.
void log_window::update_patch()
{
    if (!patch_) return;
    patch_timer_ = ui_.timer(
        100, false,
        [this]
        {
            if (!patch_ || !tab_) return;
            log_tab::patch_source source = tab_->patch_request();
            const unsigned gen = ++patch_generation_;
            if (source.args.empty()) return patch_->show(source.title, {});
            std::thread(
                [this, &ui = ui_, gen, source = std::move(source),
                 alive = std::weak_ptr<bool>(alive_)]
                {
                    // libgit2's patch without git.
                    const auto own =
                        git_installed() ? std::nullopt : patch_in_process(source.dir, source.args);
                    auto r = std::make_shared<git_result>(
                        own ? git_result{.exit_code = 0, .output = *own}
                            : run_git(source.dir, source.args, git_output::stdout_only));
                    ui.post(
                        [this, gen, title = source.title, r, alive]
                        {
                            if (!alive.lock() || !patch_ || gen != patch_generation_) return;
                            patch_->show(title, r->ok() ? std::move(r->output) : std::string());
                        });
                })
                .detach();
        });
}

}  // namespace tortilla
