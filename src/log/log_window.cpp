#include "log_window.h"

#include "git/command.h"
#include "git/git_install.h"
#include "git/patch_text.h"
#include "i18n.h"
#include "log/open_repository_dialog.h"
#include "settings.h"
#include "ui/modal_dialogs.h"
#include "ui/repo_icon.h"
#include "ui/trust_repository.h"
#include "ui/windows.h"

#include <algorithm>
#include <map>
#include <thread>

namespace tortilla
{
namespace
{

// The log windows' strips trade tabs with each other, and only with each
// other.
constexpr const char* k_tab_group = "tortilla.log";

// The log windows open, by frame: where a tab dropped in comes from.
std::map<const rap::window*, log_window*>& log_windows()
{
    static std::map<const rap::window*, log_window*> all;
    return all;
}

// The last log_window::used() stamp given, counting up: the latest is the
// window used last.
uint64_t g_last_used = 0;

}  // namespace

std::expected<log_window*, std::string>
log_window::open_forwarded(rap::ui& ui, const log_request& r,
                           const std::function<void(std::unique_ptr<log_window>)>& keep)
{
    if (!r.new_window)
        if (log_window* into = forward_target())
        {
            // As the + picker opens one: a tab, or the one there already.
            auto tab = into->open_tab(r.path, r.all_branches, r.submodule);
            if (!tab) return std::unexpected(tab.error());
            if (!r.select.empty()) (*tab)->select_commit(r.select);
            into->show();
            return into;
        }
    auto log = open(ui, r.path, r.all_branches, r.submodule);
    if (!log) return std::unexpected(log.error());
    if (!r.select.empty()) (*log)->select_commit(r.select);
    log_window* w = log->get();
    keep(std::move(*log));
    return w;
}

log_window* log_window::forward_target()
{
    log_window* best = nullptr;
    for (const auto& [frame, w] : log_windows())
        // Not under a dialog: the tab would show behind it, and the dialog's
        // answer land in a tab no longer on show.
        if (w->takes_tabs() && !modals_over(frame) && (!best || w->last_used_ > best->last_used_))
            best = w;
    return best;
}

void log_window::used()
{
    last_used_ = ++g_last_used;
}

void log_window::show()
{
    used();
    window_->show();
}

std::expected<std::unique_ptr<log_window>, std::string>
log_window::open(rap::ui& ui, const std::string& path, bool all_branches, bool outer)
{
    auto tab = log_tab::open(ui, path, outer);
    if (!tab) return std::unexpected(tab.error());
    log_tab& first = **tab;
    std::unique_ptr<log_window> w(new log_window(ui));
    w->build(std::move(*tab));
    first.start(all_branches);
    remember_repository(first.repo_location());
    return w;
}

log_window::log_window(rap::ui& ui)
    : ui_(ui)
{
}

// The pages' own (their date pop-ups, over the window, and their timers) go
// before the window; the rest of the tabs after it.
log_window::~log_window()
{
    log_windows().erase(window_.get());
    for (auto& tab : tabs_) tab->drop_page();
}

log_window* log_window::of(const rap::window* w)
{
    auto it = log_windows().find(w);
    return it == log_windows().end() ? nullptr : it->second;
}

void log_window::build(std::unique_ptr<log_tab> first, std::optional<placement> where)
{
    using namespace rap::literals;
    namespace layout = rap::layout;

    // (rap::layout's window: log_window::window is the tabs' parent.) The
    // pages go into the stack as the tabs come.
    window_ = create_window(
        ui_,
        layout::window{first->title_text(), {layout::stack{}.bind(stack_)}}
            .size(140_ch, 50_lines)
            .tabs(layout::document_tabs{}
                      .on_select([this](const std::string& id) { show_tab(id); })
                      .on_close([this](const std::string& id) { close_tab(id); })
                      .on_new([this] { new_tab(); })
                      .on_reorder([this](const std::string& id, int index) { reorder(id, index); })
                      .group(k_tab_group)
                      .on_detach([this](const std::string& id, std::optional<rap::point_f> at)
                                 { return tear_off(id, at); })
                      .on_receive([this](const rap::document_tabs::transfer& t)
                                  { return receive(t); })
                      .bind(strip_)));
    log_windows()[window_.get()] = this;
    // The first tab's icon; its app id is the window's from the start, and
    // never changed after (as other tabs show), as the taskbar would regroup
    // the window. What dialogs on it take, though, is the tab on show's
    // (show_tab).
    if (first->identity().icon) window_->set_icon(first->identity().icon);
    if (!first->identity().app_id.empty()) window_->set_app_id(first->identity().app_id);
    window_->on_close_request(
        [this]
        {
            if (closing_) return true;
            closing_ = true;
            closing();
            if (closed_) closed_();
            return true;
        });
    window_->on_key(
        [this](const rap::key_event& e)
        {
            // Typed in: the window in use, so far as rapport tells.
            used();
            log_tab* tab = current_tab();
            return tab && tab->key(e);
        });
    // Size and position persist across runs, as TortoiseGit's do; position
    // only where the platform lets apps place windows (not on Wayland). A
    // torn-off window is the size of the one it came from, where it's put.
    if (!where)
        settings::get().restore_window("log.window", *window_);
    else
    {
        if (where->size.w > 0 && where->size.h > 0) window_->resize(where->size);
        if (where->at) window_->move(*where->at);
    }
    adopt(std::move(first));
}

log_tab& log_window::place(std::unique_ptr<log_tab> tab, int index)
{
    log_tab& t = *tab;
    t.set_host(this);
    stack_->add(t.id(), t.build_page());
    strip_->insert(index, strip_item(t));
    auto at = index < 0 || size_t(index) >= tabs_.size() ? tabs_.end() : tabs_.begin() + index;
    tabs_.insert(at, std::move(tab));
    t.bind_page(t.take_carried_view());
    return t;
}

log_tab& log_window::adopt(std::unique_ptr<log_tab> tab, int index)
{
    log_tab& t = place(std::move(tab), index);
    select_tab(t.id());
    return t;
}

rap::document_tab log_window::strip_item(const log_tab& tab) const
{
    return {.id = tab.id(),
            .title = tab.tab_title(),
            .icon = tab.identity().icon,
            .tooltip = tab.tab_tooltip()};
}

std::vector<std::unique_ptr<log_tab>>::iterator log_window::find(const std::string& id)
{
    return std::ranges::find_if(tabs_, [&](const auto& t) { return t->id() == id; });
}

log_tab* log_window::current_tab() const
{
    for (const auto& t : tabs_)
        if (t->id() == current_) return t.get();
    return nullptr;
}

void log_window::show_tab(const std::string& id)
{
    auto it = find(id);
    if (it == tabs_.end()) return;
    log_tab& tab = **it;
    current_ = id;
    used();
    stack_->show(id);
    window_->set_title(tab.title_text());
    if (tab.identity().icon) window_->set_icon(tab.identity().icon);
    // Its dialogs, and the windows its actions open, are its repository's
    // (ui/repo_icon.h); the window's taskbar group stays the first tab's.
    set_repo_identity(window_.get(), tab.shared_identity());
    follow_patch(tab);
    update_patch();
}

void log_window::select_tab(const std::string& id)
{
    if (find(id) == tabs_.end()) return;
    strip_->select(id);
    show_tab(id);
}

std::expected<log_tab*, std::string> log_window::open_tab(const std::string& path,
                                                          bool all_branches, bool outer)
{
    auto opened = log_tab::open(ui_, path, outer);
    if (!opened) return std::unexpected(opened.error());
    // Open here already: that tab (and this one goes, never started).
    for (const auto& t : tabs_)
        if (t->shows(**opened))
        {
            select_tab(t->id());
            return t.get();
        }
    log_tab& tab = adopt(std::move(*opened));
    tab.start(all_branches);
    remember_repository(tab.repo_location());
    return &tab;
}

void log_window::new_tab()
{
    if (!takes_tabs()) return;
    open_repository_dialog(ui_, window_.get(),
                           [this, alive = std::weak_ptr<bool>(alive_)](const std::string& path)
                           {
                               if (!alive.lock() || !takes_tabs()) return;
                               // One git refuses for its folder's owner: offered to trust it
                               // first, as Tortilla does for any it's started on (main.cpp).
                               // Not trusted: no tab, and nothing more to say.
                               ensure_trusted(ui_, path,
                                              [this, alive, path](bool go_on)
                                              {
                                                  if (!go_on || !alive.lock() || !takes_tabs())
                                                      return;
                                                  auto opened = open_tab(path);
                                                  if (!opened)
                                                      show_message(
                                                          ui_, window_.get(),
                                                          {.icon = rap::message_spec::icon::error,
                                                           .title = _("Open Repository"),
                                                           .text = opened.error()});
                                              });
                           });
}

bool log_window::close_tab(const std::string& id)
{
    auto it = find(id);
    if (it == tabs_.end() || closing_) return false;
    log_tab& tab = **it;
    // What's being written isn't given up half done.
    if (std::string why = tab.busy_reason(); !why.empty())
    {
        show_message(ui_, window_.get(),
                     {.icon = rap::message_spec::icon::warning,
                      .title = _("Close Tab"),
                      .text = std::move(why)});
        return false;
    }
    // The last: the window with it, once the strip's (or the page's)
    // handler has returned.
    if (tabs_.size() == 1)
    {
        ui_.post(
            [this, alive = std::weak_ptr<bool>(alive_)]
            {
                if (alive.lock()) close_window();
            });
        return true;
    }
    tab.save_view_settings();
    tab.save_options();
    tab.drop_page();
    stack_->remove(id);
    strip_->remove(id);
    std::shared_ptr<log_tab> gone(std::move(*it));
    tabs_.erase(it);
    gone->stop();
    // Its page's widgets go once the toolkit next gets to them (this may be
    // one of them calling). They hold nothing of the tab by now: drop_page
    // took its models from their lists and turned their callbacks off
    // (ui/page_guard.h), so the tab could go at once. It's kept for two
    // turns of the queue as a margin only; nothing depends on it.
    ui_.post([&ui = ui_, gone] { ui.post([gone] {}); });
    if (id == current_)
    {
        current_.clear();
        show_tab(strip_->current());
    }
    return true;
}

void log_window::reorder(const std::string& id, int index)
{
    auto it = find(id);
    if (it == tabs_.end() || index < 0) return;
    index = std::min(index, int(tabs_.size()) - 1);
    // Dragged, the strip has it there already; asked from here, it's moved.
    const std::vector<std::string> ids = strip_->ids();
    if (size_t(index) >= ids.size() || ids[size_t(index)] != id)
    {
        strip_->remove(id);
        strip_->insert(index, strip_item(**it));
        strip_->select(current_);
    }
    std::unique_ptr<log_tab> moved = std::move(*it);
    tabs_.erase(it);
    tabs_.insert(tabs_.begin() + index, std::move(moved));
}

// Moving a tab: refused while it's busy, as closing it is; and while a
// dialog is open over the window, which would be left over the wrong one,
// its answer landing in the tab elsewhere.
std::string log_window::move_refusal(const std::string& id) const
{
    const log_tab* tab = nullptr;
    for (const auto& t : tabs_)
        if (t->id() == id) tab = t.get();
    if (!tab || picking_ || closing_ || emptied_) return _("This tab can't be moved now.");
    if (std::string why = tab->busy_reason(true); !why.empty()) return why;
    if (modals_over(window_.get()))
        return rap::i18n::format(
            _("{0}: a dialog is open over its window. Close the dialog, then move the tab."),
            tab->tab_title());
    return {};
}

std::unique_ptr<log_tab> log_window::detach(const std::string& id)
{
    if (!move_refusal(id).empty()) return nullptr;
    auto it = find(id);
    log_tab& tab = **it;
    // Its view for the page it'll have; this one gone, with its pop-up and
    // its hold on the models (its widgets go once the handler has returned).
    tab.carry_view(tab.capture_view());
    tab.drop_page();
    stack_->remove(id);
    strip_->remove(id);
    std::unique_ptr<log_tab> out = std::move(*it);
    tabs_.erase(it);
    out->set_host(nullptr);
    if (tabs_.empty())
    {
        // The window goes too, but not under the handler that asked (the
        // strip's, a drop's): once it's returned.
        current_.clear();
        emptied_ = true;
        // Said to be no repository's now: a scope the tab's work had open
        // on this window (repo_identity_scope) doesn't put it back.
        set_repo_identity(window_.get(), nullptr);
        ui_.post(
            [this, alive = std::weak_ptr<bool>(alive_)]
            {
                if (alive.lock()) close_window();
            });
    }
    else if (id == current_)
    {
        current_.clear();
        show_tab(strip_->current());
    }
    return out;
}

rap::window* log_window::tear_off(const std::string& id, std::optional<rap::point_f> at)
{
    if (std::string why = move_refusal(id); !why.empty())
    {
        // From the menu, said why once it's closed; dragged, the drag goes
        // on along the row.
        if (!at && !picking_ && !closing_ && !emptied_)
            ui_.post(
                [this, alive = std::weak_ptr<bool>(alive_), why]
                {
                    if (alive.lock())
                        show_message(ui_, window_.get(),
                                     {.icon = rap::message_spec::icon::warning,
                                      .title = _("Move to New Window"),
                                      .text = why});
                });
        return nullptr;
    }
    placement where{.size = window_->size(), .at = at};
    // From the menu: a little down and right of this one (unless the
    // desktop places windows itself).
    if (!at)
        if (auto from = window_->position()) where.at = rap::point_f{from->x + 32, from->y + 32};
    std::unique_ptr<log_tab> tab = detach(id);
    if (!tab) return nullptr;
    std::unique_ptr<log_window> w(new log_window(ui_));
    w->build(std::move(tab), where);
    rap::window* frame = w->window_.get();
    // Kept until it closes, as the window it came from is; rapport shows it.
    if (keep_for_tests && *keep_for_tests)
        (*keep_for_tests)(std::move(w));
    else
    {
        log_window* raw = w.get();
        const size_t kept = windows::hold(std::shared_ptr<log_window>(std::move(w)));
        raw->on_closed([kept] { windows::release(kept); });
    }
    return frame;
}

bool log_window::receive(const rap::document_tabs::transfer& t)
{
    if (!takes_tabs()) return false;
    log_window* source = of(t.source);
    if (!source || source == this) return false;
    std::unique_ptr<log_tab> tab = source->detach(t.id);
    if (!tab) return false;
    adopt(std::move(tab), t.index);
    return true;
}

void log_window::pick_commits(const std::string& purpose, const std::string& label,
                              std::function<void(std::vector<commit>)> chosen)
{
    picking_ = true;
    // Its one tab stays: no strip, and none torn off or dropped in.
    strip_->set_group("");
    strip_->on_detach({});
    strip_->on_receive({});
    strip_->set_visible(false);
    tabs_.front()->pick_commits(purpose, label, std::move(chosen));
}

void log_window::closing()
{
    // (A window its tabs were moved out of wasn't the user's to close.)
    if (!emptied_) settings::get().save_window("log.window", *window_);
    log_tab* now = current_tab();
    for (const auto& t : tabs_)
        if (t.get() != now)
        {
            t->save_view_settings();
            t->save_options();
        }
    if (now)
    {
        now->save_view_settings();
        now->save_options();
    }
    close_patch();
}

void log_window::close_window()
{
    if (closing_) return;
    closing_ = true;
    closing();
    window_->close();
    if (closed_) closed_();
}

void log_window::tab_changed(log_tab& tab)
{
    if (strip_) strip_->update(strip_item(tab));
    if (window_ && tab.id() == current_) window_->set_title(tab.title_text());
}

// OK closes the tab, as Ctrl+W does; picking, OK (having handed over the
// commits) and Cancel close the window.
void log_window::finish(log_tab& tab, bool)
{
    if (picking_) return close_window();
    close_tab(tab.id());
}

void log_window::show_patch(bool on)
{
    settings::get().set("log.viewPatch", on ? "1" : "0");
    if (on == patch_.has_value()) return;
    if (!on) return close_patch();
    // The tab on show's, as it follows it (show_tab): whichever tab asked
    // (one loaded hidden, or the menu), and whatever was opened since.
    log_tab* tab = current_tab();
    const repo_identity_scope scope(tab ? std::optional(tab->shared_identity()) : std::nullopt);
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

// View Patch's window is the tab on show's too: what its dialogs take, and
// its icon (its taskbar group stays as it opened, as the log window's).
void log_window::follow_patch(const log_tab& tab)
{
    rap::window* viewer = patch_ && patch_->window ? patch_->window() : nullptr;
    if (!viewer) return;
    set_repo_identity(viewer, tab.shared_identity());
    if (tab.identity().icon) viewer->set_icon(tab.identity().icon);
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
    if (tab.id() == current_) update_patch();
}

// git runs off the UI thread, a moment after the selection (or the tab)
// settles.
void log_window::update_patch()
{
    if (!patch_) return;
    patch_timer_ = ui_.timer(
        100, false,
        [this]
        {
            log_tab* tab = current_tab();
            if (!patch_ || !tab) return;
            log_tab::patch_source source = tab->patch_request();
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
