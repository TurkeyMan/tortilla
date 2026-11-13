#include "log_window.h"

#include "settings.h"
#include "ui/repo_icon.h"

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
    tab_->close_patch();
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

}  // namespace tortilla
