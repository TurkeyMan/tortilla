// The log's page: its widgets, as built, and what lives only as long as
// they do. The log's state is kept apart from it (log_tab), so the page
// can be drawn again from that state.
#pragma once

#include <rapport/rapport.h>

#include <chrono>
#include <memory>
#include <optional>
#include <string>

namespace tortilla
{

// The author's picture, top right of the details, in logical px.
inline constexpr int k_avatar_size = 96;

// The Fetch button's badge: changes waiting (a dot with their count), a
// remote that failed (a red "!"), or no network (greyed, Offline).
enum class fetch_badge
{
    none,
    changes,
    failure,
    offline,
};

// The last press on a label in the commit list, which a double-click acts
// on (label_drag's begin sees it).
struct label_press
{
    size_t row;
    std::string full_name;
    std::chrono::steady_clock::time_point at;
};

struct log_page
{
    // The hint bar, when the current branch is behind its upstream.
    rap::widget* hint_bar = nullptr;
    rap::label* hint_text = nullptr;
    rap::button* hint_ff = nullptr;
    rap::button* hint_pull = nullptr;
    // The commit list, the commit's details and files, the forge panel.
    rap::list* commit_list = nullptr;
    rap::text_view* details = nullptr;
    rap::canvas* avatar_canvas = nullptr;
    rap::splitter* splitter = nullptr;
    rap::splitter* details_splitter = nullptr;
    rap::splitter* forge_splitter = nullptr;
    // The status bar and the bottom rows.
    rap::canvas* status_mark = nullptr;
    rap::label* status = nullptr;
    rap::check* only_path = nullptr;
    rap::entry* path_filter = nullptr;
    rap::button* statistics = nullptr;
    rap::progress_bar* progress = nullptr;
    rap::menu_button* walk = nullptr;
    rap::menu_button* view = nullptr;
    rap::button* ok = nullptr;
    rap::button* cancel = nullptr;

    // The date pop-up, while open, and its pickers.
    std::unique_ptr<rap::window> dates_popup;
    rap::date_picker* from = nullptr;
    rap::date_picker* to = nullptr;

    std::optional<label_press> last_label_press;

    // The page's own timers: the progress bar's pulse, the filters' delays,
    // and the file list fitted to its column.
    rap::subscription pulse_timer, path_filter_timer, fit_timer;
    // The forge column's width watched while it's shown.
    rap::subscription forge_width_watch;
};

}  // namespace tortilla
