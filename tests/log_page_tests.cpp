// The log's page dropped and built again into another window, on the null
// backend: what the page showed (the selection, the panes, the columns,
// the filter box, the selected files, the focus) comes back, and what
// changed meanwhile shows. With them, the remotes' poll in two logs at once,
// and the forge panel's download line across a page built again; and
// repositories as tabs of one log window, and the recent repositories.
//
// And tabs torn off into windows of their own, and dropped from one log
// window into another.
//
// The null backend's widgets keep nothing (or, its entries, no callbacks),
// so this test's backend wraps it and builds lists, splitters, entries,
// labels, buttons, progress bars, stacks and document tabs itself, keeping
// what they're told and what they'd call. Its windows poison their widgets
// as they go, and its stacks a page's as it goes: a call that reaches one
// afterwards fails the test.
#include "log_page_tests.h"

#include "changes/file_model.h"
#include "git/command.h"
#include "git/ssh_client.h"
#include "git/sync.h"
#include "log/forge_panel.h"
#include "log/log_model.h"
#include "log/log_tab.h"
#include "log/log_window.h"
#include "log/open_repository_dialog.h"
#include "settings.h"
#include "test_support.h"
#include "ui/modal_dialogs.h"
#include "ui/repo_icon.h"
#include "ui/settings_dialog.h"
#include "ui/trust_repository.h"
#include "ui/windows.h"
#include "util/repo_path.h"

#include <rapport/rapport.h>
#include <rapport/sys.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <numeric>
#include <thread>

namespace tortilla
{

// The forge panel's download line, from outside (forge_panel.h befriends it).
struct forge_panel_test
{
    static void seed(forge_panel& p)
    {
        open_pr pr;
        pr.pr.number = 42;
        pr.pr.title = "A useful change";
        pr.pr.head_sha = "head";
        pr.pr.body_known = pr.pr.reviews_known = true;
        pr.pr.labels = {{"bug", rap::color{200, 40, 40}}, {"review", std::nullopt}};
        p.prs_ = {{.pr = pr, .commits = {"head", "one", "two"}, .known = true}};
        for (const char* sha : {"head", "one", "two"})
            p.builds_[sha] = {.asked = true, .answered = true, .local = true};
    }
    static void refresh(forge_panel& p) { p.update(); }
    static void forge_possible(forge_panel& p, bool on) { p.set_forge_possible(on); }
    static rap::canvas* checks(forge_panel& p) { return p.pr_checks_mark_; }
    static void checks(forge_panel& p, check_state state) { p.prs_[0].pr.pr.checks = state; }
    static rap::canvas* labels(forge_panel& p) { return p.labels_canvas_; }
    static rap::stack* label_stack(forge_panel& p) { return p.pr_labels_; }
    static rap::label* title(forge_panel& p) { return p.pr_title_; }
    static float label_width(const forge_panel& p)
    {
        return p.labels_rendered_ ? p.labels_rendered_->width : 0;
    }
    static float label_height(const forge_panel& p)
    {
        return p.labels_rendered_ ? p.labels_rendered_->height : 0;
    }
    static void labels(forge_panel& p, std::vector<pr_label> labels)
    {
        p.prs_[0].pr.pr.labels = std::move(labels);
    }
    static void build(forge_panel& p)
    {
        auto& b = p.builds_["head"];
        b.local = false;
        b.ci.workflows = {{.jobs = {{.name = "test", .state = check_state::failure}}}};
    }
    static void build_state(forge_panel& p, check_state state)
    {
        auto& b = p.builds_["head"];
        b.asked = b.answered = true;
        b.local = false;
        b.ci.workflows = {{.jobs = {{.name = "test", .state = state}}}};
    }
    static void presence(forge_panel& p, bool pr, bool ci, bool artifacts)
    {
        if (pr)
        {
            open_pr item;
            item.pr.number = 42;
            item.pr.title = "A useful change";
            item.pr.head_sha = "head";
            item.pr.body_known = item.pr.reviews_known = true;
            p.prs_ = {{.pr = item, .commits = {"head", "one", "two"}, .known = true}};
        }
        else
            p.prs_.clear();
        for (const char* sha : {"head", "one", "two"})
        {
            auto& b = p.builds_[sha];
            b.asked = b.answered = b.artifacts_known = true;
            b.local = false;
            b.ci.workflows =
                ci ? std::vector<ci_workflow>{{.jobs = {{.name = "test",
                                                         .state = check_state::success}}}}
                   : std::vector<ci_workflow>{};
            ci_artifact artifact;
            artifact.name = "app.zip";
            b.artifacts =
                artifacts ? std::vector<ci_artifact>{artifact} : std::vector<ci_artifact>{};
        }
    }
    static void awaiting(forge_panel& p, const std::string& sha)
    {
        p.builds_[sha] = {.asked = true, .asking = true};
    }
    static void answer_ci(forge_panel& p, const std::string& sha, bool present)
    {
        auto& b = p.builds_[sha];
        b.asked = b.answered = b.artifacts_known = true;
        b.asking = false;
        b.ci.workflows = present
                             ? std::vector<ci_workflow>{{.jobs = {{.name = "test",
                                                                   .state = check_state::success}}}}
                             : std::vector<ci_workflow>{};
    }
    static void present_commit(forge_panel& p, const std::string& sha)
    {
        presence(p, true, true, true);
        p.prs_[0].commits.push_back(sha);
        p.prs_[0].pr.pr.head_sha = sha;
        p.builds_[sha] = p.builds_["head"];
    }
    static rap::tabs* pages(forge_panel& p) { return p.tabs_; }
    static void azure_artifact_error(forge_panel& p, std::string error)
    { p.builds_["head"].artifact_error = std::move(error); }
    static rap::label* azure_summary(const forge_panel& p) { return p.build_summary_; }
    static void azure_lookup_error(forge_panel& p, std::string error)
    {
        p.builds_["azure-error-fixture"] = {.asked = true, .answered = true, .error = std::move(error)};
        p.show_commit("azure-error-fixture");
    }
    static rap::list* matrix(forge_panel& p) { return p.matrix_list_; }
    static bool picked(const forge_panel& p) { return p.failed_job_.has_value(); }
    static std::optional<ci_job> picked_job(const forge_panel& p) { return p.failed_job_; }
    static std::string failure_title(const forge_panel& p) { return p.failure_title_->text(); }
    static const std::string& failure_url(const forge_panel& p) { return p.full_log_url_; }
    static void job_metadata(forge_panel& p, std::int64_t id, std::string url, std::string reason)
    {
        auto& job = p.builds_["head"].ci.workflows[0].jobs[0];
        job.id = id;
        job.url = std::move(url);
        job.failure_reason = std::move(reason);
        p.failures_[id] = {};
        p.failures_asked_.insert(id);  // details already answered, no network in the fixture
    }
    static void started(forge_panel& p, std::string text, bool pulsing)
    {
        p.download_started(std::move(text), pulsing);
    }
    static void progressed(forge_panel& p, float fraction) { p.download_progressed(fraction); }
    static void ended(forge_panel& p, const std::string& to, const std::string& error)
    {
        p.download_ended(to, error);
    }
    static rap::label* status(forge_panel& p) { return p.download_status_; }
    static rap::progress_bar* progress(forge_panel& p) { return p.download_progress_; }
    static rap::button* download(forge_panel& p) { return p.download_; }
};

// A log tab held busy, from outside (log_tab.h befriends it): git changing
// its labels, as far as busy_reason can tell.
struct log_tab_test
{
    static forge_panel& forge(log_tab& t) { return *t.forge_panel_; }
    static forge_panel& azure_panel(log_tab& t) { return *t.forge_panel_; }
    static rap::label* azure_status(log_tab& t) { return t.page_->status; }
    static void edit_filter(log_tab& t, const std::string& text) { t.edit_filter(text); }
    static const std::string& search(const log_tab& t) { return t.search_text_; }
    static const log_filter& filter(const log_tab& t) { return t.filter_; }
    static show_preset preset(const log_tab& t) { return t.show_.preset; }
    static int jump(const log_tab& t) { return t.jump_type_; }
    static void branch(log_tab& t, std::string name)
    {
        t.toolbar_.branch = std::move(name);
        t.toolbar_changed();
    }
    static void hold_ref_command(log_tab& t, std::shared_ptr<git_process> p)
    {
        t.ref_command_ = std::move(p);
    }
};

namespace test
{
namespace
{

// Calls that reached a widget after its window had gone.
int g_stale_calls = 0;
int g_forge_writes = 0;
int g_forge_pages = 0;
int g_forge_invalidates = 0;
int g_forge_selects = 0;
int g_focus_calls = 0;
class kept_canvas;
std::function<void(kept_canvas&, rap::size_f)> g_allocated_canvas_paint;
std::map<rap::stack*, rap::size_f> g_stack_allocations;
// While set, the pages stacks remove are held here, their widgets there
// until the test lets them go.
std::vector<std::function<void()>>* g_held_pages = nullptr;

// A widget's common part: its focus, whether it's shown and enabled, and
// whether its window has gone.
template <class Base>
class kept : public Base
{
public:
    void set_enabled(bool on) override
    {
        touch();
        ++g_forge_writes;
        enabled = on;
    }
    void set_visible(bool on) override
    {
        touch();
        ++g_forge_writes;
        shown = on;
    }
    void set_tooltip(const std::string& value) override
    {
        touch();
        ++g_forge_writes;
        tip = value;
    }
    void focus() override
    {
        touch();
        ++g_focus_calls;
        focused = true;
    }
    bool has_focus() const override
    {
        touch();
        return focused;
    }
    void touch() const
    {
        if (dead) ++g_stale_calls;
    }
    std::string tip;
    bool focused = false, shown = true, enabled = true, dead = false;
};

class kept_list : public kept<rap::list>
{
public:
    explicit kept_list(const std::vector<rap::column>& columns)
        : widths(columns.size(), 100.f)
        , order(columns.size())
    {
        std::iota(order.begin(), order.end(), size_t(0));
        for (const rap::column& c : columns) visible.push_back(c.visible);
    }

    // The user's click: the rows, then the list's word of it.
    void click(std::vector<size_t> rows, size_t at)
    {
        select(rows);
        set_current(at);
        if (changed) changed();
    }

    void set_model(rap::list_model* m) override
    {
        touch();
        model = m;
    }
    std::vector<size_t> selection() const override
    {
        touch();
        return selected;
    }
    std::optional<size_t> current() const override
    {
        touch();
        return current_row;
    }
    void select(const std::vector<size_t>& rows) override
    {
        touch();
        ++g_forge_writes;
        ++g_forge_selects;
        selected = rows;
        std::ranges::sort(selected);
    }
    void set_current(size_t row) override
    {
        touch();
        current_row = row;
    }
    void scroll_to(size_t, rap::scroll_align) override { touch(); }
    std::optional<float> row_offset(size_t) const override
    {
        touch();
        return std::nullopt;
    }
    void set_row_offset(size_t, float) override { touch(); }
    void set_sort_indicator(std::optional<size_t>, rap::sort_order) override { touch(); }
    std::vector<float> column_widths() const override
    {
        touch();
        return widths;
    }
    void set_column_widths(const std::vector<float>& w) override
    {
        touch();
        for (size_t i = 0; i < w.size() && i < widths.size(); ++i)
            if (w[i] > 0) widths[i] = w[i];
    }
    bool column_visible(size_t c) const override
    {
        touch();
        return c < visible.size() && visible[c];
    }
    void set_column_visible(size_t c, bool on) override
    {
        touch();
        if (c < visible.size()) visible[c] = on;
    }
    std::vector<size_t> column_order() const override
    {
        touch();
        return order;
    }
    void set_column_order(const std::vector<size_t>& o) override
    {
        touch();
        std::vector<size_t> sorted = o;
        std::ranges::sort(sorted);
        for (size_t i = 0; i < sorted.size(); ++i)
            if (sorted[i] != i) return;
        if (sorted.size() == order.size()) order = o;
    }
    void on_selection_changed(std::function<void()> f) override { changed = std::move(f); }
    void on_activate(std::function<void(size_t)>) override {}
    void on_sort_requested(std::function<void(size_t, rap::sort_order)>) override {}
    void on_context_menu(std::function<rap::menu(std::optional<size_t>)> f) override
    {
        context = std::move(f);
    }
    void on_header_menu(std::function<rap::menu(size_t)>) override {}
    void on_cell_context_menu(std::function<rap::menu(size_t, size_t, rap::point_f)>) override {}
    void on_reorder(reorder_fn) override {}
    void on_reorder_or_copy(reorder_copy_fn, unsigned) override {}
    void set_reorderable(bool) override {}
    void on_cell_drag(cell_drag) override {}
    void on_drag_out(drag_out) override {}
    void on_drop_in(drop_in) override {}

    rap::list_model* model = nullptr;
    std::vector<size_t> selected;
    std::optional<size_t> current_row;
    std::vector<float> widths;
    std::vector<size_t> order;
    std::vector<bool> visible;
    std::function<void()> changed;
    std::function<rap::menu(std::optional<size_t>)> context;  // a right click's menu
};

class kept_splitter : public kept<rap::splitter>
{
public:
    // Laid out: each pane its share.
    explicit kept_splitter(size_t panes)
        : sizes_(panes, 100.f)
    {
    }
    std::vector<float> sizes() const override
    {
        touch();
        return sizes_;
    }
    void set_sizes(const std::vector<float>& s) override
    {
        touch();
        ++g_forge_writes;
        for (size_t i = 0; i < s.size() && i < sizes_.size(); ++i)
            if (s[i] > 0) sizes_[i] = s[i];
    }
    void on_moved(std::function<void()>) override {}
    float divider() const override { return 4; }
    std::vector<float> sizes_;
};

class kept_entry : public kept<rap::entry>
{
public:
    explicit kept_entry(const rap::spec::entry& s)
        : value(s.text)
        , placeholder(s.placeholder)
        , clearable(s.clearable)
    {
        for (const auto& a : s.actions)
            if (a.on_menu) menu = a.on_menu;
    }
    // The user's typing: the text, then the entry's word of it.
    void type(const std::string& text)
    {
        value = text;
        if (changed) changed(text);
    }
    std::string text() const override
    {
        touch();
        return value;
    }
    void set_text(const std::string& t) override
    {
        touch();
        value = t;
    }
    void set_placeholder(const std::string& p) override
    {
        touch();
        placeholder = p;
    }
    void on_changed(std::function<void(const std::string&)> f) override
    {
        changed = [this, f = std::move(f)](const std::string& v)
        {
            ++edits;
            if (f) f(v);
        };
    }
    void on_activate(std::function<void()>) override {}
    std::string value, placeholder;
    bool clearable = false;
    int edits = 0;
    std::function<rap::menu()> menu;
    std::function<void(const std::string&)> changed;
};

// A null canvas with observable app repaint requests.
class kept_canvas : public kept<rap::canvas>
{
public:
    explicit kept_canvas(rap::canvas& inner, std::optional<rap::size_f> allocated = {},
                         std::function<void(std::function<void()>)> post = {})
        : inner_(inner)
        , allocated_(allocated)
        , post_(std::move(post))
    {
    }
    void invalidate() override
    {
        touch();
        ++g_forge_invalidates;
        inner_.invalidate();
        request_paint();
    }
    rap::size_f size() const override { return allocated_ ? *allocated_ : inner_.size(); }
    float device_scale() const override { return inner_.device_scale(); }
    void set_cursor(rap::cursor c) override { inner_.set_cursor(c); }
    void on_paint(std::function<void(rap::painter&, rap::size_f)> f) override
    {
        paint = f;
        inner_.on_paint(std::move(f));
        request_paint();
    }
    void on_resize(std::function<void(rap::size_f)> f) override { inner_.on_resize(std::move(f)); }
    void on_pointer(std::function<bool(const rap::pointer_event&)> f) override
    {
        inner_.on_pointer(std::move(f));
    }
    void on_scroll(std::function<bool(const rap::scroll_event&)> f) override
    {
        inner_.on_scroll(std::move(f));
    }
    void on_key(std::function<bool(const rap::key_event&)> f) override
    {
        inner_.on_key(std::move(f));
    }
    rap::subscription on_frame(std::function<void(double)> f) override
    {
        return inner_.on_frame(std::move(f));
    }
    rap::frame_target target() const override { return inner_.target(); }
    void on_frame_target(std::function<void(const rap::frame_target&)> f) override
    {
        inner_.on_frame_target(std::move(f));
    }
    void present(std::shared_ptr<const rap::image> image, rap::frame_color c = {}) override
    {
        inner_.present(std::move(image), c);
    }
    void clear_frame() override { inner_.clear_frame(); }
    std::function<void(rap::painter&, rap::size_f)> paint;
    rap::size_f painted_size;
    std::vector<std::string> painted_strings;
    std::vector<rap::rect_f> painted_rectangles;
    int allocated_paints = 0;

private:
    void request_paint()
    {
        if (!allocated_ || !post_ || !g_allocated_canvas_paint) return;
        post_(
            [this]
            {
                if (!dead && paint && g_allocated_canvas_paint && allocated_->w > 0 &&
                    allocated_->h > 0)
                    g_allocated_canvas_paint(*this, *allocated_);
            });
    }
    rap::canvas& inner_;
    std::optional<rap::size_f> allocated_;
    std::function<void(std::function<void()>)> post_;
};

class kept_label : public kept<rap::label>
{
public:
    void set_text(const std::string& t) override
    {
        touch();
        ++g_forge_writes;
        text = t;
    }
    std::string text;
};

class kept_button : public kept<rap::button>
{
public:
    void set_text(const std::string&) override
    {
        touch();
        ++g_forge_writes;
    }
    void set_icon(const rap::icon&) override { touch(); }
    void on_click(std::function<void()> f) override { click = std::move(f); }
    std::function<void()> click;
    std::string text;  // as built
};

class kept_menu : public kept<rap::menu_button>
{
public:
    void set_text(const std::string& t) override
    {
        touch();
        text = t;
        ++writes;
    }
    void set_icon(const rap::icon&) override { touch(); }
    void set_pushed(bool) override { touch(); }
    void on_menu(std::function<rap::menu()> f) override { menu = std::move(f); }
    std::string text;
    int writes = 0;
    std::function<rap::menu()> menu;
};

class kept_drop_down : public kept<rap::drop_down>
{
public:
    explicit kept_drop_down(const rap::spec::drop_down& s)
        : items(s.items)
        , value(s.current)
    {
    }
    void set_items(std::vector<std::string> v) override
    {
        touch();
        items = std::move(v);
    }
    int current() const override
    {
        touch();
        return value;
    }
    void set_current(int v) override
    {
        touch();
        value = v;
    }
    void on_changed(std::function<void(int)> f) override
    {
        changed = [this, f = std::move(f)](int v)
        {
            ++edits;
            if (f) f(v);
        };
    }
    void choose(int v)
    {
        value = v;
        if (changed) changed(v);
    }
    std::vector<std::string> items;
    int value;
    int edits = 0;
    std::function<void(int)> changed;
};

class kept_progress : public kept<rap::progress_bar>
{
public:
    void set_fraction(float f) override
    {
        touch();
        fraction = f;
    }
    void pulse() override
    {
        touch();
        ++pulses;
    }
    float fraction = 0;
    int pulses = 0;
};

class kept_stack;
class kept_tabs;
class kept_pages;
class kept_window;

// What one window was built with, of those kept here, and how to poison
// them all.
struct built_window
{
    std::vector<kept_list*> lists;
    std::vector<kept_splitter*> splitters;
    std::vector<kept_entry*> entries;
    std::vector<kept_button*> buttons;
    std::vector<kept_menu*> menus;
    std::vector<kept_drop_down*> choices;
    bool band_hbox = false;
    std::vector<std::function<void()>> poison;
    kept_window* window = nullptr;
    kept_tabs* tabs = nullptr;
    std::vector<kept_pages*> page_tabs;
    kept_stack* stack = nullptr;
    // While a stack's page is built: where its widgets' poison goes too.
    std::vector<std::function<void()>>* page = nullptr;
    std::optional<rap::size_f> page_canvas_size;
};

// ABI 89 (#92) honors explicit extents on a stack page's root too.
rap::size_f allocated_page_size(const rap::node& n);
rap::size_f natural_page_size(const rap::node& n)
{
    if (std::holds_alternative<rap::spec::canvas>(n.spec)) return {64, 64};
    if (const auto* label = std::get_if<rap::spec::label>(&n.spec))
    {
        const auto* kept_label_widget =
            label->bind ? dynamic_cast<kept_label*>(*label->bind) : nullptr;
        const auto& text = kept_label_widget ? kept_label_widget->text : label->text;
        return {float(text.size() * 7), 14};
    }
    if (const auto* stack = std::get_if<rap::spec::stack>(&n.spec))
    {
        auto* widget = stack->bind ? *stack->bind : nullptr;
        const auto* state = dynamic_cast<kept<rap::stack>*>(widget);
        if (!state || !state->shown) return {};
        const auto found = g_stack_allocations.find(widget);
        return found == g_stack_allocations.end() ? rap::size_f{} : found->second;
    }
    const auto* box = std::get_if<rap::spec::box>(&n.spec);
    if (!box) return {};
    auto extent = [](rap::extent e, float natural)
    {
        switch (e.unit)
        {
        case rap::extent::unit::px: return e.value;
        case rap::extent::unit::chars: return e.value * 7;
        case rap::extent::unit::lines: return e.value * 14;
        default: return natural;
        }
    };
    rap::size_f size;
    for (const auto& child : box->items)
    {
        auto s = allocated_page_size(child);
        s.w = extent(child.common.width, s.w);
        s.h = extent(child.common.height, s.h);
        if (box->dir == rap::orientation::horizontal)
        {
            size.w += s.w;
            size.h = std::max(size.h, s.h);
        }
        else
        {
            size.h += s.h;
            size.w = std::max(size.w, s.w);
        }
    }
    return size;
}

rap::size_f allocated_page_size(const rap::node& n)
{
    auto size = natural_page_size(n);
    auto extent = [](rap::extent e, float natural)
    {
        switch (e.unit)
        {
        case rap::extent::unit::px: return e.value;
        case rap::extent::unit::chars: return e.value * 7;
        case rap::extent::unit::lines: return e.value * 14;
        default: return natural;
        }
    };
    size.w = extent(n.common.width, size.w);
    size.h = extent(n.common.height, size.h);
    return size;
}

// A real null tabs control wrapped for mutation/strip/poison observations.
// New ABI 89 virtuals forward to null, so silent fallback is its actual behavior.
class kept_pages : public kept<rap::tabs>
{
public:
    kept_pages(rap::tabs& inner, const rap::spec::tabs& spec)
        : inner_(inner)
        , hide_single(spec.hide_single_tab)
        , initial(spec.visible)
        , titles(spec.titles)
        , icons(spec.icons)
    {
    }
    int current() const override
    {
        touch();
        return dead ? -1 : inner_.current();
    }
    void set_current(int page) override
    {
        touch();
        ++g_forge_writes;
        if (!dead)
        {
            // Exercise #98's silent programmatic contract (null is already
            // silent), independently of user-driven changed notifications.
            inner_.on_changed({});
            inner_.set_current(page);
            inner_.on_changed(changed);
        }
    }
    void on_changed(std::function<void(int)> f) override
    {
        touch();
        changed = std::move(f);
        if (!dead) inner_.on_changed(changed);
    }
    void set_page_visible(int page, bool visible) override
    {
        touch();
        ++g_forge_writes;
        if (!dead) inner_.set_page_visible(page, visible);
    }
    bool page_visible(int page) const override
    {
        touch();
        return !dead && inner_.page_visible(page);
    }
    void set_page_icon(int page, const rap::icon& icon) override
    {
        touch();
        ++g_forge_writes;
        if (dead) return;
        ++icon_changes;
        icons.resize(titles.size());
        if (page >= 0 && size_t(page) < icons.size()) icons[page] = icon;
        inner_.set_page_icon(page, icon);
    }
    void set_page_title(int page, const std::string& title) override
    {
        touch();
        ++g_forge_writes;
        if (dead) return;
        if (page >= 0 && size_t(page) < titles.size()) titles[page] = title;
        inner_.set_page_title(page, title);
    }
    void set_hide_single_tab(bool on) override
    {
        touch();
        ++g_forge_writes;
        if (dead) return;
        ++hide_single_calls;
        hide_single = on;
        inner_.set_hide_single_tab(on);
    }
    bool hide_single_tab() const override
    {
        touch();
        return !dead && inner_.hide_single_tab();
    }
    int hide_single_calls = 0;
    void click(int page)
    {
        const int before = current();
        set_current(page);
        // The existing native API reports only selection changes. It cannot
        // report a click on the already-current fallback page.
        if (changed && current() != before)
        {
            ++user_changes;
            changed(current());
        }
    }
    bool strip_shown() const
    {
        std::vector<bool> visible;
        for (int i = 0; i < int(titles.size()); ++i) visible.push_back(page_visible(i));
        return rap::tabs_strip_shown(visible, int(titles.size()), hide_single);
    }
    int user_changes = 0;
    bool hide_single;
    std::vector<bool> initial;
    std::vector<std::string> titles;
    std::vector<rap::icon> icons;
    int icon_changes = 0;
    std::function<void(int)> changed;

private:
    rap::tabs& inner_;
};

// A window's document tabs: what they're told, and the user's clicks, which
// call the handlers as the strip would.
class kept_tabs : public kept<rap::document_tabs>
{
public:
    void insert(int index, const rap::document_tab& t) override
    {
        touch();
        if (index < 0 || size_t(index) > items.size()) index = int(items.size());
        items.insert(items.begin() + index, t);
        if (current_id.empty()) current_id = t.id;
    }
    void remove(const std::string& id) override
    {
        touch();
        const int i = index_of(id);
        if (i < 0) return;
        items.erase(items.begin() + i);
        if (current_id != id) return;
        current_id =
            items.empty() ? std::string() : items[std::min(size_t(i), items.size() - 1)].id;
    }
    void update(const rap::document_tab& t) override
    {
        touch();
        if (const int i = index_of(t.id); i >= 0) items[size_t(i)] = t;
    }
    void select(const std::string& id) override
    {
        touch();
        if (index_of(id) >= 0) current_id = id;
    }
    std::string current() const override
    {
        touch();
        return current_id;
    }
    std::vector<std::string> ids() const override
    {
        touch();
        std::vector<std::string> out;
        for (const rap::document_tab& t : items) out.push_back(t.id);
        return out;
    }
    void on_select(std::function<void(const std::string&)> f) override { selected = std::move(f); }
    void on_close(std::function<void(const std::string&)> f) override { closed = std::move(f); }
    void on_new(std::function<void()> f) override { added = std::move(f); }
    void on_reorder(std::function<void(const std::string&, int)> f) override
    {
        reordered = std::move(f);
    }
    void on_detach(detach_fn f) override { detached = std::move(f); }
    void on_receive(receive_fn f) override { received = std::move(f); }
    void set_group(const std::string& g) override
    {
        touch();
        group = g;
    }
    void on_context_menu(std::function<rap::menu(const std::string&)> f) override
    {
        menu = std::move(f);
    }

    // The user's clicks and keys, as the strip passes them on: a tab
    // clicked (Ctrl+Tab), its close box (Ctrl+W), + (Ctrl+T), a drag.
    void click(const std::string& id)
    {
        if (id == current_id || index_of(id) < 0) return;
        current_id = id;
        if (selected) selected(id);
    }
    void close_box(const std::string& id)
    {
        if (closed) closed(id);
    }
    void plus()
    {
        if (added) added();
    }
    void drag(const std::string& id, int to)
    {
        const int from = index_of(id);
        if (from < 0 || from == to) return;
        rap::document_tab t = items[size_t(from)];
        items.erase(items.begin() + from);
        items.insert(items.begin() + to, t);
        if (reordered) reordered(id, to);
    }
    const rap::document_tab* find(const std::string& id) const
    {
        const int i = index_of(id);
        return i < 0 ? nullptr : &items[size_t(i)];
    }

    std::vector<rap::document_tab> items;
    std::string current_id;
    std::function<void(const std::string&)> selected, closed;
    std::function<void()> added;
    std::function<void(const std::string&, int)> reordered;
    // Tearing off and dropping in: unset, a log window's tabs stay put.
    detach_fn detached;
    receive_fn received;
    std::string group;
    std::function<rap::menu(const std::string&)> menu;

private:
    int index_of(const std::string& id) const
    {
        for (size_t i = 0; i < items.size(); ++i)
            if (items[i].id == id) return int(i);
        return -1;
    }
};

// The null backend's stack, its pages' widgets poisoned as the stack's own
// go: once the queue next runs after remove().
class kept_stack : public kept<rap::stack>
{
public:
    kept_stack(rap::stack& inner, built_window& b, std::function<void(std::function<void()>)> post)
        : inner_(inner)
        , built_(b)
        , post_(std::move(post))
    {
    }
    void add(const std::string& id, rap::node content) override
    {
        touch();
        ++g_forge_pages;
        if (pages_.contains(id)) return;
        if (g_allocated_canvas_paint) allocated_sizes_[id] = allocated_page_size(content);
        auto& poison = pages_[id];
        auto* outer = std::exchange(built_.page, &poison);
        const auto old_size = std::exchange(
            built_.page_canvas_size,
            g_allocated_canvas_paint ? std::optional{allocated_page_size(content)} : std::nullopt);
        inner_.add(id, std::move(content));
        built_.page_canvas_size = old_size;
        built_.page = outer;
    }
    void remove(const std::string& id) override
    {
        touch();
        ++g_forge_pages;
        auto it = pages_.find(id);
        if (it == pages_.end()) return;
        inner_.remove(id);
        auto gone = [poison = std::move(it->second)]
        {
            for (const auto& p : poison) p();
        };
        // Held: the page outlives whatever the queue runs meanwhile, as a
        // toolkit may keep it past any number of posts.
        if (g_held_pages)
            g_held_pages->push_back(std::move(gone));
        else
            post_(std::move(gone));
        pages_.erase(it);
        allocated_sizes_.erase(id);
        if (g_allocated_canvas_paint) g_stack_allocations[this] = allocated_sizes_[inner_.shown()];
    }
    void show(const std::string& id) override
    {
        touch();
        inner_.show(id);
        if (g_allocated_canvas_paint) g_stack_allocations[this] = allocated_sizes_[id];
    }
    std::string shown() const override
    {
        touch();
        return inner_.shown();
    }
    std::vector<std::string> ids() const override
    {
        touch();
        return inner_.ids();
    }

private:
    rap::stack& inner_;
    built_window& built_;
    std::function<void(std::function<void()>)> post_;
    std::map<std::string, std::vector<std::function<void()>>> pages_;
    std::map<std::string, rap::size_f> allocated_sizes_;
};

// The null backend's window, which poisons its widgets as it goes (they're
// kept, so a call after that is counted rather than undefined).
class kept_window : public rap::window
{
public:
    kept_window(std::unique_ptr<rap::window> inner, std::unique_ptr<rap::build_sink> sink,
                built_window& b)
        : inner_(std::move(inner))
        , sink_(std::move(sink))
        , built_(b)
    {
        built_.window = this;
    }
    ~kept_window() override
    {
        inner_.reset();
        built_.window = nullptr;
        for (const auto& p : built_.poison) p();
    }
    void set_title(const std::string& t) override
    {
        title = t;
        inner_->set_title(t);
    }
    void show() override
    {
        ++shown;
        inner_->show();
    }
    void present() override
    {
        ++presented;
        inner_->present();
    }
    void close() override
    {
        closed = true;
        inner_->close();
    }
    rap::size_f size() const override { return inner_->size(); }
    void resize(rap::size_f s) override { inner_->resize(s); }
    void set_min_size(rap::size_f s) override { inner_->set_min_size(s); }
    std::optional<rap::point_f> position() const override { return inner_->position(); }
    void move(rap::point_f p) override { inner_->move(p); }
    rap::rect_f work_area() const override { return inner_->work_area(); }
    bool fullscreen() const override { return inner_->fullscreen(); }
    bool maximized() const override { return inner_->maximized(); }
    void set_fullscreen(bool on) override { inner_->set_fullscreen(on); }
    void on_fullscreen_changed(std::function<void(bool)> f) override
    {
        inner_->on_fullscreen_changed(std::move(f));
    }
    void set_icon(const rap::icon& i) override
    {
        icon = i;
        inner_->set_icon(i);
    }
    void set_app_id(const std::string& id) override
    {
        app_id = id;
        inner_->set_app_id(id);
    }
    void set_menu_bar_visible(bool on) override { inner_->set_menu_bar_visible(on); }
    void on_key(std::function<bool(const rap::key_event&)> f) override
    {
        key = f;
        inner_->on_key(std::move(f));
    }
    void on_finished(std::function<void(rap::dialog_role)> f) override
    {
        finished = f;
        inner_->on_finished(std::move(f));
    }
    void on_close_request(std::function<bool()> f) override
    {
        close_request = f;
        inner_->on_close_request(std::move(f));
    }
    void on_destroyed(std::function<void()> f) override { inner_->on_destroyed(std::move(f)); }
    void on_files_dropped(std::function<void(const std::vector<std::string>&)> f) override
    {
        inner_->on_files_dropped(std::move(f));
    }

    std::string title, app_id;
    rap::icon icon;
    bool closed = false;
    int shown = 0;  // times show() was called: brought to the front, once open
    // Times present() was called: restored if minimised, raised, activated.
    int presented = 0;
    // The window's keys, as the user would press them.
    std::function<bool(const rap::key_event&)> key;
    // The close box's question (the null window never asks it).
    std::function<bool()> close_request;
    // A dialog's answer: OK, Cancel... (the null window never gives it).
    std::function<void(rap::dialog_role)> finished;

protected:
    void busy_changed(bool) override {}

private:
    std::unique_ptr<rap::window> inner_;
    std::unique_ptr<rap::build_sink> sink_;  // what the null window's builds call
    built_window& built_;
};

// The null backend, with the widgets above of this test's own.
class keeping_backend : public rap::backend
{
public:
    explicit keeping_backend(std::unique_ptr<rap::ui> inner)
        : inner_(std::move(inner))
    {
    }

    // (A deque: each window's builds keep a reference to theirs.)
    std::deque<built_window> built;

    std::string_view name() const override { return "tortilla-test"; }
    std::unique_ptr<rap::window> create_window(const rap::window_spec& spec, rap::window* parent,
                                               rap::build_sink& sink) override
    {
        built_window& b = built.emplace_back();
        if (const auto* box = std::get_if<rap::spec::box>(&spec.content.spec);
            spec.base_surface && box && box->dir == rap::orientation::vertical &&
            box->items.size() == 2)
            if (const auto* row = std::get_if<rap::spec::box>(&box->items.front().spec))
                b.band_hbox = row->dir == rap::orientation::horizontal && row->items.size() == 8 &&
                              std::holds_alternative<rap::spec::stack>(box->items.back().spec);
        auto mine = std::make_unique<window_sink>(*this, sink, b);
        auto w = null().create_window(spec, parent, *mine);
        return std::make_unique<kept_window>(std::move(w), std::move(mine), b);
    }
    int run() override { return null().run(); }
    void quit(int code) override
    {
        ++quit_requests;
        if (!suppress_quit) null().quit(code);
    }
    void set_color_scheme(rap::color_scheme s) override { null().set_color_scheme(s); }
    void post(std::function<void()> f) override { null().post(std::move(f)); }
    void message(rap::window* parent, const rap::message_spec& m,
                 std::function<void(rap::dialog_role)> done) override
    {
        ++messages;
        last_message = m.text;
        last_message_detail = m.detail;
        if (hold_dialogs)
        {
            held_messages.push_back(std::move(done));
            return;
        }
        if (answer)
        {
            if (done) done(*answer);
            return;
        }
        null().message(parent, m, std::move(done));
    }
    void choose_files(rap::window* parent, const rap::file_dialog_spec& f,
                      std::function<void(std::vector<std::string>)> done) override
    {
        ++choosers;
        if (hold_dialogs)
        {
            held_choosers.push_back(std::move(done));
            return;
        }
        if (choose)
        {
            std::vector<std::string> paths = std::move(*choose);
            choose.reset();
            if (done) done(std::move(paths));
            return;
        }
        null().choose_files(parent, f, std::move(done));
    }
    void print(rap::window* parent, rap::print_job job) override
    {
        null().print(parent, std::move(job));
    }
    void set_clipboard(const std::string& s) override { null().set_clipboard(s); }
    void read_clipboard_text(std::function<void(std::optional<std::string>)> done) override
    {
        null().read_clipboard_text(std::move(done));
    }
    void set_clipboard_image(int w, int h, rap::pixel_format f, const uint8_t* p,
                             size_t stride) override
    {
        null().set_clipboard_image(w, h, f, p, stride);
    }
    void
    read_clipboard_image(std::function<void(std::optional<rap::sys::decoded_image>)> done) override
    {
        null().read_clipboard_image(std::move(done));
    }
    bool save_snapshot(rap::window* w, const std::string& path) override
    {
        return null().save_snapshot(w, path);
    }
    std::unique_ptr<rap::image> create_image(int w, int h, rap::pixel_format f, const uint8_t* p,
                                             size_t stride) override
    {
        return null().create_image(w, h, f, p, stride);
    }
    rap::subscription timer(int ms, bool repeat, std::function<void()> fn) override
    {
        // Recurring background polls must not requeue forever on null's
        // clockless timer. Tests advance these long timers explicitly.
        if (ms >= 60000)
        {
            auto alive = std::make_shared<bool>(true);
            long_timers.push_back([alive, fn = std::move(fn)] { if (*alive) fn(); });
            return rap::subscription([alive] { *alive = false; });
        }
        return null().timer(ms, repeat, std::move(fn));
    }
    std::vector<std::function<void()>> long_timers;
    void set_busy(bool on) override { null().set_busy(on); }

    // While set, messages and file dialogs stay open (as a desktop's do
    // until the user answers): their answers kept here, for the test to give.
    bool hold_dialogs = false;
    std::vector<std::function<void(rap::dialog_role)>> held_messages;
    std::vector<std::function<void(std::vector<std::string>)>> held_choosers;
    // Messages shown (the null backend answers each with its first button).
    int messages = 0;
    int quit_requests = 0;
    bool suppress_quit = false;
    std::string last_message_detail;
    std::string last_message;
    // The next messages' answer, while set (else the null backend's).
    std::optional<rap::dialog_role> answer;
    // Folder and file dialogs asked for, and the next one's answer (else none).
    int choosers = 0;
    std::optional<std::vector<std::string>> choose;

private:
    rap::backend& null() { return inner_->backend(); }

    // One window's builds, its first and its stack's pages after: the null
    // backend's widgets, but for those kept here, made here and handed to the
    // core in their place.
    class window_sink : public rap::build_sink
    {
    public:
        window_sink(keeping_backend& backend, rap::build_sink& core, built_window& b)
            : backend_(backend)
            , core_(core)
            , built_(b)
        {
        }
        void created(const rap::node& n, rap::widget& w) override
        {
            rap::widget* mine = nullptr;
            built_window& b = built_;
            if (const auto* s = std::get_if<rap::spec::list>(&n.spec))
            {
                b.lists.push_back(keep<kept_list>(s->columns));
                mine = b.lists.back();
            }
            else if (const auto* s = std::get_if<rap::spec::splitter>(&n.spec))
            {
                b.splitters.push_back(keep<kept_splitter>(s->panes.size()));
                mine = b.splitters.back();
            }
            else if (const auto* s = std::get_if<rap::spec::entry>(&n.spec))
            {
                b.entries.push_back(keep<kept_entry>(*s));
                mine = b.entries.back();
            }
            else if (std::holds_alternative<rap::spec::canvas>(n.spec))
                mine = keep<kept_canvas>(static_cast<rap::canvas&>(w), b.page_canvas_size,
                                         [&backend = backend_](std::function<void()> f)
                                         { backend.post(std::move(f)); });
            else if (std::holds_alternative<rap::spec::label>(n.spec))
                mine = keep<kept_label>();
            else if (const auto* s = std::get_if<rap::spec::button>(&n.spec))
            {
                auto* button = keep<kept_button>();
                button->text = s->text;
                b.buttons.push_back(button);
                mine = button;
            }
            else if (std::holds_alternative<rap::spec::menu_button>(n.spec))
            {
                b.menus.push_back(keep<kept_menu>());
                mine = b.menus.back();
            }
            else if (const auto* s = std::get_if<rap::spec::drop_down>(&n.spec))
            {
                b.choices.push_back(keep<kept_drop_down>(*s));
                mine = b.choices.back();
            }
            else if (std::holds_alternative<rap::spec::progress_bar>(n.spec))
                mine = keep<kept_progress>();
            else if (const auto* s = std::get_if<rap::spec::tabs>(&n.spec))
            {
                auto* tabs = keep<kept_pages>(static_cast<rap::tabs&>(w), *s);
                b.page_tabs.push_back(tabs);
                mine = tabs;
            }
            else if (std::holds_alternative<rap::spec::stack>(n.spec))
            {
                auto* stack = keep<kept_stack>(static_cast<rap::stack&>(w), b,
                                               [&backend = backend_](std::function<void()> f)
                                               { backend.post(std::move(f)); });
                // Keep the frame's stack, not a nested chip surface's.
                if (!b.stack) b.stack = stack;
                mine = stack;
            }
            core_.created(n, mine ? *mine : w);
        }
        void created(const rap::spec::dialog_button& b, rap::button& w) override
        {
            core_.created(b, w);
        }
        void created(const rap::spec::document_tabs& s, rap::document_tabs&) override
        {
            kept_tabs* t = built_.tabs = keep<kept_tabs>();
            for (const rap::document_tab& d : s.items) t->insert(-1, d);
            if (!s.current.empty()) t->select(s.current);
            core_.created(s, *t);
        }

    private:
        template <class W, class... Args>
        W* keep(Args&&... args)
        {
            auto w = std::make_unique<W>(std::forward<Args>(args)...);
            W* raw = w.get();
            auto poison = [raw] { raw->dead = true; };
            built_.poison.push_back(poison);
            if (built_.page) built_.page->push_back(poison);
            backend_.widgets_.push_back(std::move(w));
            return raw;
        }

        keeping_backend& backend_;
        rap::build_sink& core_;
        built_window& built_;
    };

    std::unique_ptr<rap::ui> inner_;
    // For as long as the backend, poisoned once their window or page goes:
    // they keep no pointer that could go stale (a model is only remembered,
    // never asked).
    std::vector<std::unique_ptr<rap::widget>> widgets_;
};

keeping_backend* g_backend = nullptr;

rap::backend* create_keeping(int abi, int* argc, char** argv, const rap::look_options& look)
{
    if (abi != rap::abi_version) return nullptr;
    auto inner = rap::ui::load(*argc, argv, {.backend = "null", .look = look});
    if (!inner)
    {
        std::fprintf(stderr, "%s", inner.error().c_str());
        return nullptr;
    }
    return g_backend = new keeping_backend(std::move(*inner));
}

// The window around the tab: only a parent for its dialogs here.
class test_host : public tab_host
{
public:
    rap::window* w = nullptr;
    rap::entry* search = nullptr;
    void focus_filter(log_tab&) override
    {
        if (search) search->focus();
    }
    bool filter_focused(const log_tab&) const override { return search && search->has_focus(); }
    void toolbar_changed(log_tab& t) override
    {
        if (w && search) search->set_text(log_tab_test::search(t));
    }
    rap::window* window() override { return w; }
    void tab_changed(log_tab&) override {}
    void finish(log_tab&, bool) override {}
    bool patch_shown() const override { return false; }
    void show_patch(bool) override {}
    void selection_moved(log_tab&) override {}
};

// The remotes' polls, held here: armed, and fired only when a test says.
struct polls
{
    struct armed
    {
        std::shared_ptr<bool> live;
        std::function<void()> tick;
    };
    std::map<const log_tab*, armed> pending;
    std::map<const log_tab*, bool> allowed;  // what each tab's last tick heard
    log_tab::poll_hooks hooks{.arm =
                                  [this](log_tab& tab, std::function<void()> tick)
                              {
                                  auto live = std::make_shared<bool>(true);
                                  pending[&tab] = {live, std::move(tick)};
                                  return rap::subscription([live] { *live = false; });
                              },
                              .ticked = [this](log_tab& tab, bool on) { allowed[&tab] = on; }};

    bool is_armed(const log_tab& tab) const
    {
        auto it = pending.find(&tab);
        return it != pending.end() && *it->second.live;
    }
    // Every poll armed now, fired once (each arms itself again as it goes).
    void fire()
    {
        auto now = std::move(pending);
        pending.clear();
        for (auto& [tab, a] : now)
            if (*a.live) a.tick();
    }
};

// The page's widgets this test reads and drives, from the window built last.
struct page
{
    kept_list* commits = nullptr;
    kept_list* files = nullptr;
    kept_splitter* panes = nullptr;
    kept_splitter* details = nullptr;
    kept_entry* search = nullptr;
    kept_entry* path_filter = nullptr;
    log_model* log = nullptr;
    file_model* file_rows = nullptr;
};

page last_page()
{
    page p;
    const built_window& b = g_backend->built.back();
    for (kept_list* l : b.lists)
        if (auto* m = dynamic_cast<log_model*>(l->model))
        {
            p.commits = l;
            p.log = m;
        }
        else if (auto* f = dynamic_cast<file_model*>(l->model))
        {
            p.files = l;
            p.file_rows = f;
        }
    // The list over the lower tabs, then Commit's text over its files.
    if (b.splitters.size() >= 2)
    {
        p.panes = b.splitters[0];
        p.details = b.splitters[1];
    }
    for (kept_entry* e : b.entries)
        if (e->clearable)
            p.search = e;
        else
            p.path_filter = e;
    return p;
}

// The UI thread's queue run until `done`, or for `ms` at most.
template <class F>
bool pump(rap::ui& ui, F done, int ms = 20000)
{
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!done())
    {
        if (std::chrono::steady_clock::now() > until) return false;
        ui.run();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

void settle(rap::ui& ui)
{
    pump(
        ui, [] { return false; }, 50);
}

std::optional<size_t> row_of(const log_model& log, const std::string& summary)
{
    for (size_t r = 0; r < log.row_count(); ++r)
        if (const commit* c = log.commit(r); c && c->summary == summary) return r;
    return std::nullopt;
}

// The page dropped and built again, twice; `git` runs in the repository.
void page_tests(rap::ui& ui, const std::string& repo,
                const std::function<bool(std::vector<std::string>)>& git)
{
    auto opened = log_tab::open(ui, repo);
    check(opened.has_value(), "log page: the tab opens the repository");
    if (!opened) return;
    std::unique_ptr<log_tab> tab = std::move(*opened);
    test_host host;
    tab->set_host(&host);
    auto build = [&](std::optional<log_view_state> view)
    {
        auto w = ui.create(rap::layout::window{
            "Log",
            {rap::layout::vbox{rap::layout::entry{log_tab_test::search(*tab)}
                                   .clearable()
                                   .bind(host.search)
                                   .on_changed([&](const std::string& text)
                                               { log_tab_test::edit_filter(*tab, text); }),
                               tab->build_page()}}});
        host.w = w.get();
        tab->bind_page(std::move(view));
        return w;
    };
    settings::get().set_numbers("log.panes.forge", {300, 200});
    settings::get().set("log.panes.forge.parts", "");
    settings::get().set_numbers("log.panes.details", {90, 110});
    auto window = build(std::nullopt);
    page p = last_page();
    check(p.commits && p.files && p.panes && p.details && p.search && p.path_filter,
          "log page: its lists, panes and filter boxes are found");
    if (!p.commits || !p.files || !p.panes || !p.details || !p.search || !p.path_filter) return;
    check(!settings::get().contains("log.panes.forge") &&
              !settings::get().contains("log.panes.forge.parts") &&
              p.details->sizes_ == std::vector<float>{90, 110},
          "forge tabs: obsolete divider settings dropped while Commit message/files split is "
          "retained");
    log_model& log = *p.log;
    tab->start(false);
    const bool ready = pump(
        ui, [&] { return !log.loading() && log.working_tree_known() && log.commit_rows() == 3; });
    check(ready && log.is_working_tree(0), "log page: the log loads, the working tree's row first");
    if (!ready) return;

    auto& azure_panel = log_tab_test::azure_panel(*tab);
    forge_panel_test::azure_lookup_error(azure_panel, "Azure DevOps HTTP 401: fixture denied. Required scope: Build (Read).");
    auto* azure_status = dynamic_cast<kept_label*>(log_tab_test::azure_status(*tab));
    check(azure_status && azure_status->shown && azure_status->text.find("HTTP 401: fixture denied") != std::string::npos &&
              azure_status->text.find("Build (Read)") != std::string::npos,
          "azure: lookup error reaches always-visible log status without Build data");
    const auto azure_writes = g_forge_writes;
    forge_panel_test::refresh(azure_panel);
    check(g_forge_writes == azure_writes, "azure: identical lookup error does not write widgets again");
    azure_panel.show_commit(std::nullopt);
    check(azure_status && azure_status->text.find("Azure DevOps HTTP") == std::string::npos,
          "azure: changing row clears stale lookup status");

    // -- what the user changes, carried to a second window ------------------
    const auto beta = row_of(log, "Add beta"), gamma = row_of(log, "Add gamma");
    check(beta && gamma, "log page: the commits are there");
    if (!beta || !gamma) return;
    p.commits->click({*beta, *gamma}, *beta);
    p.panes->set_sizes({420, 260});
    p.details->set_sizes({150, 110});
    std::vector<float> widths;
    for (size_t i = 0; i < log_model::column_count; ++i) widths.push_back(30.f + float(i) * 7);
    p.commits->set_column_widths(widths);
    std::vector<size_t> order(log_model::column_count);
    std::iota(order.begin(), order.end(), size_t(0));
    std::swap(order[log_model::author], order[log_model::date]);
    p.commits->set_column_order(order);
    p.commits->set_column_visible(log_model::hash, true);
    p.commits->set_column_visible(log_model::author, false);
    const std::vector<bool> visible = p.commits->visible;
    // (Path takes what the others leave: kept as 0.)
    const std::vector<float> file_widths{0, 70, 90, 50, 60};
    const std::vector<size_t> file_order{0, 2, 1, 3, 4};
    p.files->set_column_widths(file_widths);
    p.files->set_column_order(file_order);
    // Typed, and not yet applied: applied as the page goes.
    p.search->type("Add");
    p.search->focus();

    log_view_state view = tab->capture_view();
    check(view.selection && view.selection->commits.size() == 2 && view.selection->current &&
              *view.selection->current == log.commit(*beta)->id,
          "log page: the selection is captured by commit");
    tab->drop_page();
    window.reset();
    host.w = nullptr;

    window = build(std::move(view));
    p = last_page();
    settle(ui);
    // (Newest first: gamma's row, then beta's.)
    check(p.commits->selected ==
                  std::vector<size_t>{*row_of(log, "Add gamma"), *row_of(log, "Add beta")} &&
              p.commits->current_row == row_of(log, "Add beta"),
          "log page: rebuilt, the same commits selected, the same one current");
    check(p.panes->sizes_ == std::vector<float>{420, 260} &&
              p.details->sizes_ == std::vector<float>{150, 110},
          "log page: rebuilt, the panes as they were");
    check(p.commits->widths == widths && p.commits->order == order && p.commits->visible == visible,
          "log page: rebuilt, the commit list's columns as they were");
    std::vector<float> files_now = p.files->widths;
    files_now[file_model::path] = 0;  // fitted to the list's width
    check(files_now == file_widths && p.files->order == file_order,
          "log page: rebuilt, the file list's columns as they were");
    check(p.search->value == "Add" && p.search->focused && !p.commits->focused,
          "log page: rebuilt, the filter box's text and the focus in it");

    // -- what changes while there's no page shows when there's one again ----
    p.search->type("");
    p.search->focused = false;
    p.commits->click({0}, 0);
    std::optional<size_t> beta_file;
    for (size_t r = 0; r < p.file_rows->files().size(); ++r)
        if (p.file_rows->files()[r].path == "beta.txt") beta_file = r;
    check(beta_file.has_value(), "log page: the working tree's files listed");
    if (!beta_file) return;
    p.files->click({*beta_file}, *beta_file);
    p.files->focus();
    view = tab->capture_view();
    check(view.selected_files == std::vector<std::string>{"beta.txt"} &&
              view.focused == log_view_state::focus::files,
          "log page: the selected files and the focus captured");
    tab->drop_page();
    window.reset();
    host.w = nullptr;

    // A commit made elsewhere: the repository's watch reloads the log,
    // with no page to draw it on.
    const bool committed = git({"commit", "-q", "--allow-empty", "-m", "Add delta"});
    const bool heard =
        committed &&
        pump(ui, [&] { return !log.loading() && row_of(log, "Add delta").has_value(); });
    check(heard, "log page: a commit made while there's no page is loaded");

    window = build(std::move(view));
    p = last_page();
    const bool files_back = pump(
        ui, [&] { return !p.files->selected.empty(); }, 10000);
    check(p.commits->model == &log && row_of(log, "Add delta") &&
              p.commits->selected == std::vector<size_t>{0} && p.commits->current_row == 0,
          "log page: rebuilt after a reload, the new commit listed and the working tree still "
          "selected");
    check(files_back && p.files->selected.size() == 1 &&
              p.file_rows->files()[p.files->selected.front()].path == "beta.txt" &&
              p.files->focused,
          "log page: rebuilt, the same file selected once the files are back, and focused");

    tab->drop_page();
    window.reset();
    tab.reset();
    settle(ui);
}

// Two logs open while checking is Never; turned on (from either's Settings,
// or anywhere: it's one setting), both poll again.
void poll_tests(rap::ui& ui, const std::string& repo, polls& polled)
{
    const sync_when was = check_setting();
    set_check_setting(sync_when::never);
    polled.pending.clear();
    polled.allowed.clear();
    auto a = log_tab::open(ui, repo), b = log_tab::open(ui, repo);
    check(a && b, "log poll: two logs open");
    if (!a || !b) return set_check_setting(was);
    log_tab& one = **a;
    log_tab& two = **b;
    one.start(false);
    two.start(false);
    check(polled.is_armed(one) && polled.is_armed(two), "log poll: both logs' polls armed");
    polled.fire();
    check(polled.allowed.contains(&one) && !polled.allowed[&one] && polled.allowed.contains(&two) &&
              !polled.allowed[&two],
          "log poll: while checking is Never, neither may ask");
    check(polled.is_armed(one) && polled.is_armed(two),
          "log poll: while checking is Never, both still armed");
    set_check_setting(sync_when::always);
    polled.fire();
    check(polled.allowed[&one] && polled.allowed[&two] && polled.is_armed(one) &&
              polled.is_armed(two),
          "log poll: checking turned on, both logs' next ticks may ask, and go on");
    settle(ui);
    a->reset();
    b->reset();
    settle(ui);
    polled.pending.clear();
    polled.allowed.clear();
    set_check_setting(was);
}

class forge_painter : public rap::painter
{
public:
    rap::color resolve(const rap::paint&) const override { return {0, 0, 0}; }
    void line(rap::point_f, rap::point_f, const rap::paint&, float, rap::line_cap) override {}
    void curve(rap::point_f, rap::point_f, rap::point_f, rap::point_f, const rap::paint&,
               float) override
    {
    }
    void circle(rap::point_f, float, std::optional<rap::paint>, std::optional<rap::paint>) override
    {
    }
    void rect(rap::rect_f r, float, std::optional<rap::paint>, std::optional<rap::paint>) override
    {
        rectangles.push_back(r);
    }
    void polygon(const std::vector<rap::point_f>&, std::optional<rap::paint>,
                 std::optional<rap::paint>, float) override
    {
    }
    void arc(rap::point_f, float, float, float, const rap::paint&, float, rap::line_cap) override {}
    void image(const rap::image&, rap::rect_f, rap::rect_f, rap::filter, float) override {}
    float device_scale() const override { return 1; }
    void push_clip(rap::rect_f) override {}
    void pop_clip() override {}
    void text(rap::point_f, const std::string& text, const rap::text_style&) override
    {
        strings.push_back(text);
    }
    rap::size_f measure(const std::string& text, const rap::text_style&) const override
    {
        return {float(text.size() * 7), 12};
    }
    std::vector<rap::rect_f> rectangles;
    std::vector<std::string> strings;
};

class mark_painter : public forge_painter
{
public:
    void circle(rap::point_f, float, std::optional<rap::paint> fill,
                std::optional<rap::paint>) override
    {
        if (fill)
            if (auto theme = std::get_if<rap::theme_color>(&*fill)) fills.push_back(*theme);
    }
    std::vector<rap::theme_color> fills;
};

void build_tab_icon_tests(rap::ui& ui, const std::string& repo)
{
    forge_panel panel({.ui = ui, .workdir = repo});
    const auto layout = panel.layout();
    const auto& spec = std::get<rap::spec::tabs>(layout.spec);
    const bool initial = spec.icons.size() == 4 && bool(spec.icons[2].draw);
    check(initial, "build tab icon: pending drawn icon reserves its slot at construction");
    if (!initial) return;
    const auto initial_icon = spec.icons[2];
    mark_painter pending;
    initial_icon.draw(pending, {16, 16});
    check(pending.fills == std::vector{rap::theme_color::warning} && pending.rectangles.empty(),
          "build tab icon: initial pending mark uses the existing themed drawing");
    auto window = ui.create(rap::layout::window{"Build icon", {layout}});
    panel.built();
    auto tabs = [&] { return dynamic_cast<kept_pages*>(forge_panel_test::pages(panel)); };
    check(tabs() && tabs()->icon_changes == 0 && tabs()->icons[2].draw,
          "build tab icon: unknown hidden build keeps pending without a setter");
    forge_panel_test::seed(panel);
    panel.show_commit("head");
    forge_panel_test::build_state(panel, check_state::pending);
    forge_panel_test::refresh(panel);
    check(tabs()->icon_changes == 0,
          "build tab icon: first pending answer matches the constructed icon");
    for (auto [state, theme] :
         {std::pair{check_state::success, rap::theme_color::positive},
          std::pair{check_state::failure, rap::theme_color::negative},
          std::pair{check_state::pending, rap::theme_color::warning}})
    {
        const int before = tabs()->icon_changes;
        forge_panel_test::build_state(panel, state);
        forge_panel_test::refresh(panel);
        mark_painter paint;
        tabs()->icons[2].draw(paint, {24, 24});
        check(tabs()->icon_changes == before + 1 && paint.fills == std::vector{theme} &&
                  paint.rectangles.empty(),
              "build tab icon: changed overall state replaces one themed drawn icon");
        forge_panel_test::refresh(panel);
        check(tabs()->icon_changes == before + 1,
              "build tab icon: unchanged state makes no drawn-icon setter call");
    }
    forge_panel_test::presence(panel, false, false, false);
    forge_panel_test::refresh(panel);
    check(!tabs()->page_visible(2) && tabs()->icons[2].draw,
          "build tab icon: no-CI page retains the pending icon slot while hidden");
    forge_panel_test::build_state(panel, check_state::success);
    forge_panel_test::refresh(panel);
    const auto success_icon = tabs()->icons[2];
    panel.unbind();
    window.reset();
    settle(ui);
    window = ui.create(rap::layout::window{"Build icon rebound", {panel.layout()}});
    panel.built();
    mark_painter rebound;
    tabs()->icons[2].draw(rebound, {16, 16});
    check(tabs()->icon_changes == 1 &&
              rebound.fills == std::vector{rap::theme_color::positive},
          "build tab icon: rebuilt page resets pending cache then applies known state once");
    panel.unbind();
    window.reset();
    settle(ui);
    mark_painter kept;
    success_icon.draw(kept, {16, 16});
    mark_painter old_pending;
    initial_icon.draw(old_pending, {16, 16});
    check(kept.fills == std::vector{rap::theme_color::positive} &&
              old_pending.fills == std::vector{rap::theme_color::warning},
          "build tab icon: retained drawings keep value state without touching unbound widgets");
}

class forge_observer : public rap::list_observer
{
public:
    void model_reset() override { ++resets; }
    void rows_changed(size_t, size_t) override {}
    void rows_inserted(size_t, size_t) override {}
    void rows_removed(size_t, size_t) override {}
    int resets = 0;
};

// Cached/async answers and commits in one PR can leave every visible
// value unchanged: no setters, stack-page rebuild or canvas invalidation.
void forge_presentation_tests(rap::ui& ui, const std::string& repo)
{
    forge_panel panel({.ui = ui, .workdir = repo});
    panel.set_available_width(192);
    const auto layout = panel.layout();
    const auto& pages = std::get<rap::spec::tabs>(layout.spec);
    const auto& pr = std::get<rap::spec::box>(pages.pages[1].spec);
    const auto* header = std::get_if<rap::spec::box>(&pr.items.front().spec);
    check(header && header->dir == rap::orientation::horizontal && header->items.size() == 2 &&
              std::holds_alternative<rap::spec::label>(header->items[0].spec) &&
              header->items[0].common.stretch > 0 &&
              std::holds_alternative<rap::spec::stack>(header->items[1].spec) &&
              header->items[1].common.width.unit == rap::extent::unit::auto_,
          "forge presentation: title and chips share one stretching title row");
    auto window = ui.create(rap::layout::window{"Forge", {layout}});
    panel.built();
    auto* label_stack = dynamic_cast<kept<rap::stack>*>(forge_panel_test::label_stack(panel));
    check(label_stack && !label_stack->shown && forge_panel_test::label_width(panel) == 0,
          "forge presentation: no labels leave no visible or reserved strip");
    forge_panel_test::seed(panel);
    panel.show_commit("one");
    settle(ui);
    auto reset = [] { g_forge_writes = g_forge_pages = g_forge_invalidates = g_forge_selects = 0; };
    forge_painter chips;
    auto* chip_canvas = dynamic_cast<kept_canvas*>(forge_panel_test::labels(panel));
    if (chip_canvas && chip_canvas->paint) chip_canvas->paint(chips, {1, 24});
    settle(ui);
    chip_canvas = dynamic_cast<kept_canvas*>(forge_panel_test::labels(panel));
    reset();
    if (chip_canvas && chip_canvas->paint) chip_canvas->paint(chips, {87, 24});
    settle(ui);
    check(g_forge_pages == 0 && chips.rectangles.size() == 2 && chips.rectangles[0].x == 0 &&
              chips.rectangles[1].x + chips.rectangles[1].w == 87 &&
              forge_panel_test::label_width(panel) == 87,
          "forge presentation: chip strip reserves intrinsic shown width, no paint feedback");
    check(chip_canvas && chip_canvas->tip == "bug\nreview",
          "forge presentation: tooltip lists every label");
    reset();
    panel.set_available_width(80);
    panel.set_available_width(192);
    settle(ui);
    check(g_forge_pages == 0 && g_forge_writes == 0 && g_forge_invalidates == 0,
          "forge presentation: coalesced width burst cancels obsolete layout");
    reset();
    panel.set_available_width(80);
    panel.set_available_width(100);
    settle(ui);
    check(g_forge_pages == 2 && forge_panel_test::label_width(panel) == 24,
          "forge presentation: width burst replaces one page for whole overflow chip");
    panel.set_available_width(192);
    settle(ui);
    reset();
    forge_panel_test::refresh(panel);
    check(g_forge_writes == 0 && g_forge_pages == 0 && g_forge_invalidates == 0,
          "forge presentation: identical refresh touches no controls, pages or paint");
    reset();
    panel.show_commit("two");
    check(g_forge_writes == 0 && g_forge_pages == 0 && g_forge_invalidates == 0,
          "forge presentation: same PR on another commit has no presentation churn");
    reset();
    panel.show_commit("head");
    check(g_forge_writes == 1 && g_forge_pages == 0 && g_forge_invalidates == 0,
          "forge presentation: head switch changes title only");
    auto* mark = dynamic_cast<kept_canvas*>(forge_panel_test::checks(panel));
    check(mark && !mark->shown, "forge presentation: no checks has no empty mark square");
    forge_panel_test::checks(panel, check_state::success);
    forge_panel_test::refresh(panel);
    check(mark && mark->shown, "forge presentation: checks mark reappears for a result");
    reset();
    forge_panel_test::refresh(panel);
    check(g_forge_writes == 0 && g_forge_pages == 0 && g_forge_invalidates == 0,
          "forge presentation: repeated result has no setters or invalidation");
    for (check_state state : {check_state::none, check_state::success, check_state::failure})
    {
        forge_panel_test::checks(panel, state);
        forge_panel_test::refresh(panel);
        forge_painter mark_paint;
        if (mark && mark->paint) mark->paint(mark_paint, {16, 16});
        check(mark_paint.rectangles.empty(),
              "forge presentation: marks leave panel background untouched");
    }
    forge_panel_test::build(panel);
    forge_panel_test::refresh(panel);
    forge_panel_test::azure_artifact_error(panel, "Azure DevOps HTTP 403: Build (Read) required");
    forge_panel_test::refresh(panel);
    const auto* azure_summary = dynamic_cast<kept_label*>(forge_panel_test::azure_summary(panel));
    check(azure_summary && azure_summary->shown && azure_summary->text.find("Azure DevOps HTTP 403") != std::string::npos,
          "azure: artifact failure is visible beside intact CI even without artifacts");
    forge_panel_test::azure_artifact_error(panel, {});
    forge_panel_test::refresh(panel);
    auto* matrix = dynamic_cast<kept_list*>(forge_panel_test::matrix(panel));
    if (matrix) matrix->click({0}, 0);
    check(forge_panel_test::picked(panel),
          "forge presentation: selecting a CI job records the picked job");
    forge_observer observer;
    if (matrix && matrix->model) matrix->model->add_observer(&observer);
    reset();
    forge_panel_test::refresh(panel);
    check(observer.resets == 0 && g_forge_writes == 0 && g_forge_invalidates == 0,
          "forge presentation: unchanged build preserves matrix and widgets");
    reset();
    forge_panel_test::job_metadata(panel, 71, "https://forge.invalid/job/71", "new failure reason");
    forge_panel_test::refresh(panel);
    const auto picked = forge_panel_test::picked_job(panel);
    check(picked && picked->id == 71 && picked->url == "https://forge.invalid/job/71" &&
              picked->failure_reason == "new failure reason" && g_forge_selects == 0 &&
              forge_panel_test::failure_url(panel) == picked->url &&
              forge_panel_test::failure_title(panel).find("new failure reason") !=
                  std::string::npos,
          "forge presentation: same selected row refreshes id, URL and failure reason without "
          "reselect");
    reset();
    forge_panel_test::job_metadata(panel, 71, "https://forge.invalid/job/71-new",
                                   "new failure reason");
    forge_panel_test::refresh(panel);
    check(forge_panel_test::failure_url(panel) == "https://forge.invalid/job/71-new" &&
              g_forge_writes == 0 && g_forge_selects == 0 && g_forge_invalidates == 0,
          "forge presentation: changed URL refreshes link without repainting identical content");
    reset();
    forge_panel_test::refresh(panel);
    check(g_forge_writes == 0 && g_forge_selects == 0 && g_forge_pages == 0 &&
              g_forge_invalidates == 0,
          "forge presentation: identical metadata poll touches no native state");
    if (matrix && matrix->model) matrix->model->remove_observer(&observer);
    reset();
    forge_panel_test::labels(panel,
                             {{"a-very-long-label", std::nullopt}, {"second", std::nullopt}});
    forge_panel_test::refresh(panel);
    check(g_forge_pages == 2, "forge presentation: changed labels build one measurement page");
    chip_canvas = dynamic_cast<kept_canvas*>(forge_panel_test::labels(panel));
    forge_painter measured;
    if (chip_canvas && chip_canvas->paint) chip_canvas->paint(measured, {1, 24});
    settle(ui);
    panel.set_available_width(80);
    settle(ui);
    chip_canvas = dynamic_cast<kept_canvas*>(forge_panel_test::labels(panel));
    forge_painter narrow;
    if (chip_canvas && chip_canvas->paint) chip_canvas->paint(narrow, {24, 24});
    settle(ui);
    check(narrow.rectangles.size() == 1 && narrow.strings == std::vector<std::string>{"+2"} &&
              forge_panel_test::label_width(panel) == 24 && chip_canvas &&
              chip_canvas->tip == "a-very-long-label\nsecond",
          "forge presentation: narrow intrinsic strip shows whole overflow chip and full tooltip");
    panel.set_available_width(180);
    settle(ui);
    chip_canvas = dynamic_cast<kept_canvas*>(forge_panel_test::labels(panel));
    forge_painter partial;
    if (chip_canvas && chip_canvas->paint) chip_canvas->paint(partial, {80, 24});
    check(partial.rectangles.size() == 2 &&
              partial.strings == std::vector<std::string>{"second", "+1"} &&
              partial.rectangles[0].y == partial.rectangles[1].y && partial.rectangles[0].x == 0 &&
              partial.rectangles[1].x + partial.rectangles[1].w == 80,
          "forge presentation: complete label plus counter take only their content width");
    panel.set_available_width(46);
    settle(ui);
    auto* title = dynamic_cast<kept_label*>(forge_panel_test::title(panel));
    check(label_stack && !label_stack->shown && forge_panel_test::label_width(panel) == 0 &&
              title && title->tip == "a-very-long-label\nsecond",
          "forge presentation: too narrow for counter hides strip without losing tooltip");
    panel.set_available_width(192);
    forge_panel_test::labels(panel, {{"a", std::nullopt}});
    forge_panel_test::refresh(panel);
    chip_canvas = dynamic_cast<kept_canvas*>(forge_panel_test::labels(panel));
    forge_painter short_label;
    if (chip_canvas && chip_canvas->paint) chip_canvas->paint(short_label, {1, 24});
    settle(ui);
    check(forge_panel_test::label_width(panel) == 17,
          "forge presentation: one short label reserves only its17 pixel intrinsic width");
    chip_canvas = dynamic_cast<kept_canvas*>(forge_panel_test::labels(panel));
    reset();
    forge_painter stable;
    if (chip_canvas && chip_canvas->paint) chip_canvas->paint(stable, {17, 24});
    panel.set_available_width(192);
    forge_panel_test::refresh(panel);
    settle(ui);
    check(g_forge_pages == 0 && g_forge_writes == 0 && g_forge_invalidates == 0,
          "forge presentation: stable width, row and label paints change no widgets or layout");
    const auto late_paint = chip_canvas ? chip_canvas->paint : decltype(chip_canvas->paint){};
    panel.set_available_width(20);  // queues a width update for the old label set
    forge_panel_test::labels(panel, {});
    forge_panel_test::refresh(panel);
    reset();
    settle(ui);
    check(label_stack && !label_stack->shown && forge_panel_test::label_width(panel) == 0 &&
              g_forge_pages == 0 && g_forge_writes == 0,
          "forge presentation: removing labels leaves zero width and cancels stale width post");
    panel.unbind();
    reset();
    if (late_paint) late_paint(stable, {40, 100});
    check(g_forge_writes == 0 && g_forge_pages == 0 && g_forge_invalidates == 0,
          "forge presentation: old chip paint is inert after page unbind");
    window.reset();
    settle(ui);
    window = ui.create(rap::layout::window{"Forge", {panel.layout()}});
    panel.built();
    matrix = dynamic_cast<kept_list*>(forge_panel_test::matrix(panel));
    check(matrix && matrix->selected == std::vector<size_t>{0},
          "forge presentation: rebuilt page restores picked job without resetting unchanged model");
    panel.unbind();
    window.reset();
    settle(ui);
}

// Exercise allocation/queued paint through the actual declared stack page,
// not by passing an invented nonzero size to the app's paint callback.
void forge_fresh_canvas_tests(rap::ui& ui, const std::string& repo)
{
    std::vector<rap::size_f> allocations;
    g_allocated_canvas_paint = [&](kept_canvas& canvas, rap::size_f size)
    {
        forge_painter painter;
        canvas.paint(painter, size);
        canvas.painted_size = size;
        canvas.painted_strings = painter.strings;
        canvas.painted_rectangles = painter.rectangles;
        ++canvas.allocated_paints;
        allocations.push_back(size);
    };
    forge_panel panel({.ui = ui, .workdir = repo});
    const auto layout = panel.layout();
    const auto& pr = std::get<rap::spec::box>(std::get<rap::spec::tabs>(layout.spec).pages[1].spec);
    const auto& header = pr.items[0];
    auto window = ui.create(rap::layout::window{"Forge fresh", {layout}});
    panel.built();
    forge_panel_test::seed(panel);
    forge_panel_test::labels(panel, {});
    panel.show_commit("head");
    settle(ui);
    check(allocated_page_size(header).h == 14 && forge_panel_test::label_height(panel) == 0,
          "forge fresh: empty strip contributes no height to one-line title row");
    forge_panel_test::labels(panel, {{"pm-approved", std::nullopt}});
    forge_panel_test::refresh(panel);  // outer pane width has not arrived yet
    settle(ui);
    auto canvas = [&] { return dynamic_cast<kept_canvas*>(forge_panel_test::labels(panel)); };
    auto* visible_stack = dynamic_cast<kept<rap::stack>*>(forge_panel_test::label_stack(panel));
    check(!allocations.empty() && allocations.front().w >= 64 && allocations.front().h == 24 &&
              visible_stack && visible_stack->shown && canvas() && canvas()->shown &&
              canvas()->allocated_paints > 0 &&
              canvas()->painted_strings == std::vector<std::string>{"+1"},
          "forge fresh: unknown-cap label set gets viable allocation and paints naturally");
    check(canvas() && canvas()->painted_size.h == 18 &&
              forge_panel_test::label_height(panel) == 18 && allocated_page_size(header).h == 18,
          "forge fresh: measured chip height replaces bootstrap without a reserved canvas gap");
    panel.set_available_width(240);
    settle(ui);
    check(canvas() && canvas()->painted_size.w == 87 &&
              canvas()->painted_strings == std::vector<std::string>{"pm-approved"} &&
              canvas()->painted_rectangles.size() == 1 &&
              canvas()->painted_rectangles[0].w <= canvas()->painted_size.w,
          "forge fresh: stack allocation exceeds raw canvas natural width and draws label");
    const auto& header_box = std::get<rap::spec::box>(header.spec);
    const float title_height = allocated_page_size(header_box.items[0]).h;
    const float row_height = allocated_page_size(header).h;
    check(header_box.dir == rap::orientation::horizontal && row_height == 18 &&
              (row_height - title_height) / 2 + title_height / 2 == row_height / 2,
          "forge fresh: ordinary horizontal row centers one-line title against chip-height strip");
    g_forge_writes = g_forge_pages = g_forge_invalidates = 0;
    panel.set_available_width(240);
    forge_panel_test::refresh(panel);
    settle(ui);
    check(g_forge_writes == 0 && g_forge_pages == 0 && g_forge_invalidates == 0,
          "forge fresh: settled allocation and identical refresh do no native work");
    forge_panel_test::labels(panel, {});
    forge_panel_test::refresh(panel);
    settle(ui);
    check(allocated_page_size(header).h == 14 && forge_panel_test::label_height(panel) == 0,
          "forge fresh: removing labels returns header to title height");
    forge_panel_test::labels(panel, {{"pm-approved", std::nullopt}});
    forge_panel_test::refresh(panel);
    settle(ui);
    check(canvas() && canvas()->allocated_paints > 0 && canvas()->painted_size.w == 87 &&
              canvas()->painted_size.h == 18 && allocated_page_size(header).h == 18 &&
              canvas()->painted_strings == std::vector<std::string>{"pm-approved"},
          "forge fresh: empty-to-new labels bootstrap through fixed root sizing");
    forge_panel_test::labels(panel, {{"stale measurement", std::nullopt}});
    forge_panel_test::refresh(panel);
    forge_panel_test::labels(panel, {});
    forge_panel_test::refresh(panel);
    settle(ui);
    auto* stack = dynamic_cast<kept<rap::stack>*>(forge_panel_test::label_stack(panel));
    check(!canvas() && stack && !stack->shown && forge_panel_test::label_width(panel) == 0,
          "forge fresh: stale allocated paint cannot revive removed labels");
    panel.unbind();
    window.reset();
    settle(ui);
    g_allocated_canvas_paint = {};
    g_stack_allocations.clear();
}

// Stable lower-page indices and silent fallback, with the real null tabs
// implementation behind the keeping wrapper (not an emulated selection).
void forge_tabs_tests(rap::ui& ui, const std::string& repo)
{
    using namespace rap::layout;
    // A separate fixture keeps remote/account discovery out of the other log tests.
    const std::string forge_repo = repo + "-forge-tabs";
    std::filesystem::create_directories(rap::sys::path_from_utf8(forge_repo));
    check(run_git(forge_repo, {"init", "-q"}, git_output::stdout_only).ok() &&
              run_git(forge_repo, {"remote", "add", "origin", "https://github.com/example/repo.git"},
                      git_output::stdout_only).ok(),
          "forge tabs: recognized public forge fixture needs no account");
    forge_panel local({.ui = ui, .workdir = repo});
    local.set_forge_possible(false);
    const auto local_layout = local.layout(label{"Commit contents"});
    check(forge_remotes(repo).empty() && std::get<rap::spec::tabs>(local_layout.spec).hide_single_tab,
          "forge tabs: no forge uses the zero-strip-space single-page contract");
    auto local_window = ui.create(rap::layout::window{"Local lower tabs", {local_layout}});
    local.built();
    auto* local_tabs = dynamic_cast<kept_pages*>(forge_panel_test::pages(local));
    check(local_tabs && local_tabs->current() == 0 && !local_tabs->strip_shown(),
          "forge tabs: repository without a forge gives Commit the entire area");
    // Eligibility as pr_watch reads it again (an account added, a remote
    // changed): the strip follows it live, only on a change.
    {
        const std::vector<forge_remote> eligible{
            {.name = "origin",
             .repo = {.host = "github.com", .owner = "example", .name = "repo"},
             .account = {.kind = forge_kind::github, .host = "github.com"}}};
        const int page_before = local_tabs->current(), selected = local.selected_page(),
                  remembered = local.remembered_page();
        local.set_pull_requests({}, eligible);
        check(local_tabs->hide_single_calls == 1 && !local_tabs->hide_single_tab() &&
                  local_tabs->strip_shown() && local_tabs->page_visible(0) &&
                  !local_tabs->page_visible(1) && !local_tabs->page_visible(2) &&
                  !local_tabs->page_visible(3),
              "forge tabs: becoming forge-capable shows the strip with only Commit");
        local.set_pull_requests({}, eligible);
        check(local_tabs->hide_single_calls == 1,
              "forge tabs: unchanged eligibility makes no strip call");
        local.set_pull_requests({}, {});
        check(local_tabs->hide_single_calls == 2 && local_tabs->hide_single_tab() &&
                  !local_tabs->strip_shown() && local_tabs->page_visible(0),
              "forge tabs: losing forge eligibility hides the strip again");
        local.set_pull_requests({}, {});
        check(local_tabs->hide_single_calls == 2,
              "forge tabs: unchanged ineligibility makes no strip call");
        check(local_tabs->current() == page_before && local.selected_page() == selected &&
                  local.remembered_page() == remembered && local_tabs->user_changes == 0,
              "forge tabs: eligibility changes keep the current page and choice");
    }
    local.unbind();
    local_window.reset();

    // One source: what pr_watch said, not a fresh read, decides each build.
    {
        const std::vector<forge_remote> eligible{
            {.name = "origin",
             .repo = {.host = "github.com", .owner = "example", .name = "repo"},
             .account = {.kind = forge_kind::github, .host = "github.com"}}};
        // Said before the page is built: the build uses it, though the
        // repository's own remotes (none) would say otherwise.
        forge_panel early({.ui = ui, .workdir = repo});
        early.set_forge_possible(true);
        auto first = early.layout(label{"Commit contents"});
        check(!std::get<rap::spec::tabs>(first.spec).hide_single_tab,
              "forge tabs: eligibility set before the build is the build's");
        auto early_window = ui.create(rap::layout::window{"Early lower tabs", {first}});
        early.built();
        auto* early_tabs = dynamic_cast<kept_pages*>(forge_panel_test::pages(early));
        check(early_tabs && early_tabs->strip_shown() && early_tabs->hide_single_calls == 0,
              "forge tabs: the early value shows the strip with no live call");
        // A rebuild (tear-off, view rebuild), then a reload with the same
        // pr_watch state: never a toggle.
        early.unbind();
        early_window.reset();
        auto again = early.layout(label{"Commit contents"});
        check(!std::get<rap::spec::tabs>(again.spec).hide_single_tab,
              "forge tabs: a rebuild keeps pr_watch's eligibility, not a fresh read");
        early_window = ui.create(rap::layout::window{"Rebuilt lower tabs", {again}});
        early.built();
        early_tabs = dynamic_cast<kept_pages*>(forge_panel_test::pages(early));
        early.set_pull_requests({}, eligible);
        early.set_pull_requests({}, eligible);
        check(early_tabs && early_tabs->hide_single_calls == 0 && early_tabs->strip_shown(),
              "forge tabs: rebuild then reload with the same remotes never toggles the strip");
        early.unbind();
        early_window.reset();

        forge_panel unbuilt({.ui = ui, .workdir = forge_repo});
        unbuilt.set_forge_possible(false);
        const auto hidden = unbuilt.layout(label{"Commit contents"});
        check(std::get<rap::spec::tabs>(hidden.spec).hide_single_tab,
              "forge tabs: ineligibility set before the build hides the single tab");
        unbuilt.unbind();
    }

    // pr_watch tells eligibility as it reads the remotes again: an account
    // saved, or removed. No worker: nothing asked of a forge, no keychain.
    {
        const std::string acct_repo = repo + "-forge-account";
        std::filesystem::create_directories(rap::sys::path_from_utf8(acct_repo));
        check(run_git(acct_repo, {"init", "-q"}, git_output::stdout_only).ok() &&
                  run_git(acct_repo,
                          {"remote", "add", "origin", "https://forge.example.test/team/repo.git"},
                          git_output::stdout_only).ok(),
              "forge watch: a remote on a forge known only by an account");
        const auto saved = saved_accounts();
        save_accounts({});
        pr_watch::no_worker_for_tests = true;
        int changed = 0;
        std::vector<bool> said;
        {
            pr_watch watch(ui, acct_repo, [&] { ++changed; });
            watch.on_forge_possible([&](bool on) { said.push_back(on); });
            check(watch.remotes().empty() && said.empty() && changed == 0,
                  "forge watch: no account, no forge remote");
            save_accounts({{.kind = forge_kind::gitlab,
                            .host = "forge.example.test",
                            .login = "tester"}});
            pr_watch::forge_settings_changed();
            check(watch.remotes().size() == 1 && said == std::vector<bool>{true} && changed == 1,
                  "forge watch: an account saved makes it forge-capable at once");
            pr_watch::forge_settings_changed();
            check(said == std::vector<bool>{true} && changed == 1,
                  "forge watch: the same remotes again: not told");
            save_accounts({});
            pr_watch::forge_settings_changed();
            check(watch.remotes().empty() && said == std::vector<bool>{true, false} && changed == 2,
                  "forge watch: the account removed: told, and changed though no PRs");
            pr_watch::forge_settings_changed();
            check(said.size() == 2 && changed == 2,
                  "forge watch: still none: neither told nor changed");
            save_accounts({{.kind = forge_kind::gitlab,
                            .host = "forge.example.test",
                            .login = "tester"}});
            // No Settings notification: an elapsed slow poll still discovers it.
            auto timers = std::move(g_backend->long_timers);
            g_backend->long_timers.clear();
            for (const auto& tick : timers) tick();
            check(watch.remotes().size() == 1 && changed == 3 &&
                      said == std::vector<bool>{true, false, true},
                  "forge watch: the slow local poll discovers a new account without a nudge");
        }
        pr_watch::no_worker_for_tests = false;
        save_accounts(saved);
    }

    forge_panel panel({.ui = ui, .workdir = forge_repo});
    panel.set_forge_possible(true);
    const auto layout = panel.layout(label{"Commit contents"});
    const auto& spec = std::get<rap::spec::tabs>(layout.spec);
    check(spec.titles == std::vector<std::string>{"Commit", "PR", "Build", "Artifacts"} &&
              spec.visible == std::vector<bool>{true, false, false, false} && !spec.hide_single_tab,
          "forge tabs: Commit fills lower area, conditional pages initially hidden and strip "
          "stays present for a forge-capable repository");
    auto window = ui.create(rap::layout::window{"Lower tabs", {layout}});
    panel.built();
    auto tabs = [&] { return dynamic_cast<kept_pages*>(forge_panel_test::pages(panel)); };
    check(tabs() && tabs()->current() == 0 && tabs()->strip_shown(),
          "forge tabs: forge-capable Commit alone starts with a strip");
    forge_panel_test::presence(panel, true, false, false);
    panel.show_commit("head");
    check(tabs()->page_visible(1) && !tabs()->page_visible(2) && !tabs()->page_visible(3) &&
              tabs()->strip_shown() && panel.selected_page() == 0,
          "forge tabs: PR appears without selecting it or changing strip policy");
    tabs()->click(1);
    forge_panel_test::presence(panel, true, true, true);
    forge_panel_test::refresh(panel);
    check(tabs()->current() == 1 && tabs()->page_visible(2) && tabs()->page_visible(3),
          "forge tabs: CI and artifacts appearing keep the chosen PR page");
    tabs()->click(2);
    panel.show_commit("one");
    check(tabs()->current() == 2 && panel.selected_page() == 2,
          "forge tabs: a new log row with CI keeps the chosen CI page");
    {
        // Several pages visible: the option changes, the page and choice stay.
        const int calls = tabs()->hide_single_calls, remembered = panel.remembered_page();
        forge_panel_test::forge_possible(panel, false);
        check(tabs()->hide_single_calls == calls + 1 && tabs()->hide_single_tab() &&
                  tabs()->strip_shown() && tabs()->current() == 2 && panel.selected_page() == 2 &&
                  panel.remembered_page() == remembered,
              "forge tabs: losing eligibility with pages shown keeps the chosen page");
        forge_panel_test::forge_possible(panel, false);
        check(tabs()->hide_single_calls == calls + 1,
              "forge tabs: the same eligibility again makes no strip call");
        forge_panel_test::forge_possible(panel, true);
        check(tabs()->hide_single_calls == calls + 2 && !tabs()->hide_single_tab() &&
                  tabs()->current() == 2 && panel.selected_page() == 2 &&
                  panel.remembered_page() == remembered,
              "forge tabs: regaining eligibility keeps the chosen page");
    }
    g_forge_writes = g_forge_pages = g_forge_invalidates = 0;
    forge_panel_test::refresh(panel);
    check(g_forge_writes == 0 && g_forge_pages == 0 && g_forge_invalidates == 0,
          "forge tabs: identical presentation and visibility do no native work");
    forge_panel_test::presence(panel, true, false, true);
    forge_panel_test::refresh(panel);
    check(tabs()->current() == 3 && panel.selected_page() == 3 && panel.remembered_page() == 2,
          "forge tabs: CI fallback uses nearest-after without overwriting remembered CI");
    forge_panel_test::presence(panel, true, false, false);
    forge_panel_test::refresh(panel);
    check(tabs()->current() == 1 && panel.selected_page() == 1 && panel.remembered_page() == 2,
          "forge tabs: a later fallback to PR still does not overwrite remembered CI");
    forge_panel_test::presence(panel, false, false, false);
    forge_panel_test::refresh(panel);
    check(tabs()->current() == 0 && tabs()->strip_shown() && panel.selected_page() == 0 &&
              panel.remembered_page() == 2,
          "forge tabs: Commit-only keeps strip without overwriting remembered CI");
    forge_panel_test::presence(panel, false, true, true);
    forge_panel_test::refresh(panel);
    check(tabs()->current() == 2 && tabs()->strip_shown() && panel.remembered_page() == 2,
          "forge tabs: appearing pages restore the still-remembered user choice");
    forge_panel_test::awaiting(panel, "unknown");
    panel.show_commit("unknown");
    check(tabs()->current() == 0 && panel.remembered_page() == 2,
          "forge tabs: unknown CI falls back to Commit while remembering CI");
    const int user_changes = tabs()->user_changes;
    tabs()->click(0);  // accepted no-op: Commit is already the visible fallback
    forge_panel_test::answer_ci(panel, "unknown", true);
    forge_panel_test::refresh(panel);
    check(tabs()->current() == 2 && panel.remembered_page() == 2 &&
              tabs()->user_changes == user_changes,
          "forge tabs: late CI restores explicit state with silent set_current and no-op Commit "
          "re-click");
    forge_panel_test::awaiting(panel, "another");
    panel.show_commit("another");
    // Make PR a real alternative to fallback Commit; selecting a different
    // page is observable through the current API, unlike same-current clicks.
    forge_panel_test::presence(panel, true, false, false);
    forge_panel_test::present_commit(panel, "another");
    forge_panel_test::awaiting(panel, "another");
    forge_panel_test::refresh(panel);
    check(tabs()->page_visible(1) && tabs()->current() == 0,
          "forge tabs: PR is a visible different-page choice while CI is unknown");
    tabs()->click(1);
    forge_panel_test::answer_ci(panel, "another", true);
    forge_panel_test::refresh(panel);
    check(tabs()->current() == 1 && panel.remembered_page() == 1,
          "forge tabs: a different user page prevents a late CI restoration");
    tabs()->click(2);
    forge_panel_test::awaiting(panel, "none");
    panel.show_commit("none");
    forge_panel_test::answer_ci(panel, "none", false);
    forge_panel_test::refresh(panel);
    check(tabs()->current() == 0 && panel.remembered_page() == 2,
          "forge tabs: confirmed no CI keeps Commit but remembers CI for next row");
    panel.show_commit("head");
    forge_panel_test::presence(panel, false, true, true);
    forge_panel_test::refresh(panel);
    check(tabs()->current() == 2, "forge tabs: next CI row restores the retained user choice");
    tabs()->click(3);
    const int chosen = panel.selected_page();
    panel.unbind();
    window.reset();
    settle(ui);
    window = ui.create(rap::layout::window{"Lower tabs rebuilt", {panel.layout(label{"Commit"})}});
    panel.built();
    check(chosen == 3 && tabs()->current() == 3 && panel.selected_page() == 3,
          "forge tabs: rebuilding the log page restores its own visible choice");
    forge_panel_test::presence(panel, true, false, false);
    forge_panel_test::refresh(panel);
    check(tabs()->current() == 1 && panel.remembered_page() == 3,
          "forge tabs: chosen Artifacts falls back to PR without replacing user memory");
    forge_panel_test::presence(panel, true, false, true);
    forge_panel_test::refresh(panel);
    check(tabs()->current() == 3 && panel.remembered_page() == 3,
          "forge tabs: same-row artifacts returning restore the remembered Artifacts page");
    forge_panel_test::presence(panel, true, false, false);
    forge_panel_test::refresh(panel);
    tabs()->click(0);  // a real different-page change, observable by this API
    forge_panel_test::presence(panel, true, false, true);
    forge_panel_test::refresh(panel);
    check(tabs()->current() == 0 && panel.remembered_page() == 0,
          "forge tabs: a later real user choice cancels same-row Artifacts restoration");
    forge_panel other({.ui = ui, .workdir = repo});
    forge_panel_test::presence(other, false, true, true);
    auto other_window =
        ui.create(rap::layout::window{"Other log", {other.layout(label{"Commit"})}});
    other.built();
    other.show_commit("head");
    dynamic_cast<kept_pages*>(forge_panel_test::pages(other))->click(2);
    check(other.selected_page() == 2 && panel.selected_page() == 0,
          "forge tabs: choices belong to each log tab, not the window or all logs");
    panel.unbind();
    other.unbind();
    other_window.reset();
    window.reset();
    settle(ui);
}

void lower_page_view_tests(rap::ui& ui, const std::string& repo)
{
    auto opened = log_tab::open(ui, repo);
    if (!opened)
    {
        check(false, "forge tabs: repository opens for view carry");
        return;
    }
    auto tab = std::move(*opened);
    test_host host;
    tab->set_host(&host);
    auto build = [&](std::optional<log_view_state> view)
    {
        auto window = ui.create(rap::layout::window{"View carry", {tab->build_page()}});
        host.w = window.get();
        tab->bind_page(std::move(view));
        return window;
    };
    auto window = build(std::nullopt);
    tab->start(false);
    auto page = last_page();
    if (!pump(ui, [&] { return !page.log->loading() && page.log->commit_rows() > 0; }))
    {
        check(false, "forge tabs: history loads for view carry");
        tab->drop_page();
        return;
    }
    size_t row = 0;
    while (row < page.log->row_count() && !page.log->commit(row)) ++row;
    if (row >= page.log->row_count())
    {
        tab->drop_page();
        return;
    }
    const auto sha = page.log->commit(row)->id.hex();
    auto& panel = log_tab_test::forge(*tab);
    forge_panel_test::present_commit(panel, sha);
    page.commits->click({row}, row);
    dynamic_cast<kept_pages*>(forge_panel_test::pages(panel))->click(2);
    auto view = tab->capture_view();
    check(view.lower_page == 2,
          "forge tabs: selected CI page is captured in per-log-tab view state");
    tab->drop_page();
    window.reset();
    host.w = nullptr;
    window = build(std::move(view));
    settle(ui);
    check(panel.selected_page() == 2 && forge_panel_test::pages(panel)->current() == 2,
          "forge tabs: rebuilt or adopted log page restores its CI choice with the commit");
    // Carry user intent even while the visible fallback is Commit.
    forge_panel_test::presence(panel, false, false, false);
    forge_panel_test::awaiting(panel, sha);
    forge_panel_test::refresh(panel);
    auto hidden_view = tab->capture_view();
    check(hidden_view.lower_page == 2 && panel.selected_page() == 0,
          "forge tabs: view captures remembered CI while the actual page is Commit");
    tab->drop_page();
    window.reset();
    host.w = nullptr;
    window = build(std::move(hidden_view));
    settle(ui);
    check(panel.remembered_page() == 2 && panel.selected_page() == 0,
          "forge tabs: rebuilding an unknown-CI row preserves memory without a visible CI page");
    last_page().commits->focus();
    g_focus_calls = 0;
    forge_panel_test::answer_ci(panel, sha, true);
    forge_panel_test::refresh(panel);
    check(
        panel.selected_page() == 2 && g_focus_calls == 0 && last_page().commits->focused,
        "forge tabs: late restoration after rebuild makes no focus call and keeps log-list focus");
    tab->drop_page();
    tab.reset();
    window.reset();
    settle(ui);
}

// A download's line under the forge panel's artifacts, across pages built
// while it runs and after it ends.
void forge_download_tests(rap::ui& ui, const std::string& repo)
{
    using t = forge_panel_test;
    forge_panel panel(forge_panel::host{
        .ui = ui, .window = [] { return nullptr; }, .workdir = repo, .actions = {}, .watch = {}});
    auto build = [&]
    {
        auto w = ui.create(rap::layout::window{"Forge", {panel.layout()}});
        panel.built();
        return w;
    };
    auto label = [&] { return dynamic_cast<kept_label*>(t::status(panel)); };
    auto bar = [&] { return dynamic_cast<kept_progress*>(t::progress(panel)); };
    auto button = [&] { return dynamic_cast<kept_button*>(t::download(panel)); };
    auto window = build();
    check(label() && bar() && button(), "forge download: its line's widgets are found");
    if (!label() || !bar() || !button()) return;

    t::started(panel, "Downloading app.zip (2 MB)...", false);
    t::progressed(panel, 0.4f);
    check(label()->text == "Downloading app.zip (2 MB)..." && bar()->shown &&
              bar()->fraction == 0.4f && !button()->enabled,
          "forge download: under way, said, and its share drawn");
    panel.unbind();
    window.reset();
    t::progressed(panel, 0.6f);
    window = build();
    check(label()->text == "Downloading app.zip (2 MB)..." && bar()->shown &&
              bar()->fraction == 0.6f && !button()->enabled,
          "forge download: a page built mid-download shows its line and how far it's got");

    panel.unbind();
    window.reset();
    t::ended(panel, "D:/out/app.zip", "");
    window = build();
    check(label()->text.find("D:/out/app.zip") != std::string::npos && !bar()->shown,
          "forge download: finished while there was no page, said on the next");

    t::started(panel, "Downloading logs...", true);
    settle(ui);
    check(bar()->shown && bar()->pulses > 0, "forge download: its size unknown, the bar pulses");
    panel.unbind();
    window.reset();
    t::ended(panel, "D:/out/logs.zip", "the forge said no");
    window = build();
    check(label()->text.find("the forge said no") != std::string::npos && !bar()->shown,
          "forge download: failed while there was no page, why said on the next");
    panel.unbind();
    window.reset();
    settle(ui);
}

// A page's widgets, of those built into `b` from the lists and entries
// counted (the pages before it in the window's stack) on.
page page_in(const built_window& b, size_t lists_from, size_t entries_from)
{
    page p;
    for (size_t i = lists_from; i < b.lists.size(); ++i)
        if (auto* m = dynamic_cast<log_model*>(b.lists[i]->model))
        {
            p.commits = b.lists[i];
            p.log = m;
        }
        else if (auto* f = dynamic_cast<file_model*>(b.lists[i]->model))
        {
            p.files = b.lists[i];
            p.file_rows = f;
        }
    for (kept_entry* e : b.entries)
        if (e->clearable && !e->dead)
        {
            p.search = e;
            break;
        }
    for (size_t i = entries_from; i < b.entries.size(); ++i)
        if (!b.entries[i]->clearable) p.path_filter = b.entries[i];
    return p;
}

bool loaded(rap::ui& ui, const page& p)
{
    return p.log && pump(ui, [&] { return !p.log->loading() && p.log->commit_rows() >= 2; });
}

std::string folder_name(const std::string& path)
{
    return rap::sys::path_to_utf8(rap::sys::path_from_utf8(path).filename());
}

// Repositories as document tabs in one log window: each keeps what it
// shows as the others are shown, one already open is selected rather than
// opened again, they're reordered, one closes mid-load without anything
// reaching its page after, the + opens another, and the last one closes the
// window.
void tab_tests(rap::ui& ui, const std::string& first, const std::string& second,
               const std::string& third)
{
    auto opened = log_window::open(ui, first);
    check(opened.has_value(), "log tabs: the window opens");
    if (!opened) return;
    std::unique_ptr<log_window> w = std::move(*opened);
    bool closed = false;
    w->on_closed([&] { closed = true; });
    built_window& b = g_backend->built.back();
    kept_tabs* strip = b.tabs;
    kept_stack* stack = b.stack;
    kept_window* frame = b.window;
    check(strip && stack && frame && strip->items.size() == 1 && w->tabs().size() == 1 &&
              stack->ids() == std::vector<std::string>{w->tabs()[0]->id()},
          "log tabs: a window's one tab, its page in the stack");
    if (!strip || !stack || !frame || w->tabs().size() != 1) return;
    log_tab* one = w->tabs()[0].get();
    const std::string one_id = one->id();
    check(strip->items[0].title == folder_name(first) &&
              strip->items[0].tooltip.find(folder_name(first)) != std::string::npos &&
              bool(strip->items[0].icon),
          "log tabs: the tab says the repository's folder, in full as its tooltip, with its icon");

    page p1 = page_in(b, 0, 0);
    check(loaded(ui, p1), "log tabs: the first tab's log loads");
    if (!p1.log || !p1.search) return;
    const auto beta = row_of(*p1.log, "Add beta");
    if (!beta) return check(false, "log tabs: the first tab's commits are there");
    p1.commits->click({*beta}, *beta);
    p1.search->type("Add");
    settle(ui);

    // -- a second repository: its own tab, page and title -------------------
    const size_t lists = b.lists.size(), entries = b.entries.size();
    auto two_opened = w->open_tab(second);
    check(two_opened.has_value() && w->tabs().size() == 2 && strip->items.size() == 2,
          "log tabs: a second repository opens in a second tab");
    if (!two_opened) return;
    log_tab* two = *two_opened;
    const std::string two_id = two->id();
    check(two_id != one_id && w->current_tab() == two && strip->current_id == two_id &&
              stack->shown() == two_id && stack->ids().size() == 2,
          "log tabs: the new tab is current, its page on show");
    check(frame->title.find(folder_name(second)) != std::string::npos,
          "log tabs: the window's title is the current tab's");
    page p2 = page_in(b, lists, entries);
    check(p2.commits != p1.commits && loaded(ui, p2), "log tabs: the second tab's log loads");
    if (!p2.log) return;
    const auto make_one = row_of(*p2.log, "Make one");
    if (!make_one) return check(false, "log tabs: the second tab's commits are there");
    p2.commits->click({*make_one}, *make_one);

    // -- switching keeps each tab's selection and filter --------------------
    strip->click(one_id);
    check(w->current_tab() == one && stack->shown() == one_id &&
              frame->title.find(folder_name(first)) != std::string::npos,
          "log tabs: a click on the first tab shows it, its title the window's");
    check(p1.commits->selected == std::vector<size_t>{*row_of(*p1.log, "Add beta")} &&
              p1.search->value == "Add" && !p1.commits->dead,
          "log tabs: switched back, the first tab's selection and filter are as they were");
    check(p2.commits->selected == std::vector<size_t>{*row_of(*p2.log, "Make one")} &&
              !p2.commits->dead,
          "log tabs: the second tab, hidden, keeps its selection");

    // -- one open already: its tab ------------------------------------------
    auto again = w->open_tab(second);
    check(again && *again == two && w->tabs().size() == 2 && strip->items.size() == 2 &&
              w->current_tab() == two && stack->shown() == two_id,
          "log tabs: a repository open already selects its tab instead");

    // -- reordering -----------------------------------------------------------
    strip->drag(two_id, 0);
    check(w->tabs()[0].get() == two && w->tabs()[1].get() == one,
          "log tabs: dragged first, the tabs follow the strip");
    w->reorder(two_id, 1);
    check(w->tabs()[1].get() == two && strip->ids() == std::vector<std::string>{one_id, two_id} &&
              strip->current_id == two_id,
          "log tabs: moved from the window, the strip follows, the current tab kept");

    // -- closing one mid-load -------------------------------------------------
    // F5 reloads it; closed at once, with the load under way on its worker.
    two->key(rap::key_event{.press = true, .key = rap::key::f5});
    check(p2.log->loading(), "log tabs: the second tab is reloading");
    strip->close_box(two_id);
    check(w->tabs().size() == 1 && strip->ids() == std::vector<std::string>{one_id} &&
              stack->ids() == std::vector<std::string>{one_id} && w->current_tab() == one &&
              stack->shown() == one_id &&
              frame->title.find(folder_name(first)) != std::string::npos,
          "log tabs: closed, its tab and page go, and the one beside it shows");
    settle(ui);
    pump(
        ui, [] { return false; }, 300);
    check(p2.commits->dead && !p2.search->dead && !p1.commits->dead && !closed,
          "log tabs: the closed tab's page went once the queue ran, the window stays");

    // -- + : the repository picker, then its tab -------------------------------
    // Nothing recent: the folder picker at once.
    settings::get().set("log.recentRepositories", "");
    const int asked = g_backend->choosers;
    g_backend->choose = std::vector<std::string>{third};
    strip->plus();
    settle(ui);
    check(g_backend->choosers == asked + 1 && w->tabs().size() == 2 &&
              w->current_tab() == w->tabs()[1].get() && strip->current_id == w->tabs()[1]->id() &&
              w->tabs()[1]->repo_location().find(folder_name(third)) != std::string::npos,
          "log tabs: + asks for a folder, and it opens in a tab of its own");
    if (w->tabs().size() == 2)
    {
        const std::string three_id = w->tabs()[1]->id();
        strip->close_box(three_id);
        settle(ui);
        check(w->tabs().size() == 1 && !closed, "log tabs: closed again");
    }

    // -- the last tab closes the window -----------------------------------------
    strip->close_box(one_id);
    check(!closed && w->tabs().size() == 1, "log tabs: the last tab's close waits for the handler");
    settle(ui);
    check(closed && frame->closed, "log tabs: closing the last tab closes the window");
    const auto recent = settings::get().history("log.recentRepositories");
    check(std::ranges::any_of(recent, [&](const std::string& r)
                              { return r.find(folder_name(third)) != std::string::npos; }),
          "log tabs: a repository opened is remembered as recent");
    w.reset();
    settle(ui);
}

// The windows torn off, kept here (as windows::hold keeps them in the app)
// until the test lets them go; each one's closing heard.
struct torn_off
{
    std::vector<std::unique_ptr<log_window>> windows;
    std::vector<int> closed;  // by index: times its on_closed ran
    std::function<void(std::unique_ptr<log_window>)> keep = [this](std::unique_ptr<log_window> w)
    {
        const size_t i = windows.size();
        closed.push_back(0);
        w->on_closed([this, i] { ++closed[i]; });
        windows.push_back(std::move(w));
    };
};

built_window* built_for(const rap::window* frame)
{
    for (built_window& b : g_backend->built)
        if (b.window == frame) return &b;
    return nullptr;
}

// A window's tabs, strip and stack as ids, and its current tab: what a
// refusal leaves as it was.
struct window_shape
{
    std::vector<std::string> tabs, strip, stack;
    std::string current, shown;
    bool operator==(const window_shape&) const = default;
};

window_shape shape_of(const log_window& w, const built_window& b)
{
    window_shape s;
    for (const auto& t : w.tabs()) s.tabs.push_back(t->id());
    s.strip = b.tabs->ids();
    s.stack = b.stack->ids();
    s.current = w.current_tab() ? w.current_tab()->id() : std::string();
    s.shown = b.stack->shown();
    return s;
}

bool has_ref(const log_model& log, const std::string& name)
{
    for (const auto& [id, refs] : log.refs())
        for (const ref& r : refs)
            if (r.name == name) return true;
    return false;
}

bool git_in(const std::string& dir, std::vector<std::string> args)
{
    args.insert(args.begin(), {"-c", "user.name=Tortilla Test", "-c",
                               "user.email=test@tortilla.invalid", "-c", "commit.gpgsign=false"});
    return run_git(dir, args, git_output::stdout_only).ok();
}

const rap::menu_item* find_item(const rap::menu& m, std::string_view label);

// Log tabs moved between windows: torn off into a window of their own (with
// their selection, filter and columns), dropped into another window's strip
// at a place, the window left empty closing after the handler, refusals
// while busy, under a dialog or picking commits, and a tab's work going on
// while it's between windows.
void tearoff_tests(rap::ui& ui, const std::string& first, const std::string& second,
                   const std::string& third)
{
    torn_off kept;
    log_window::keep_for_tests = &kept.keep;
    auto opened = log_window::open(ui, first);
    check(opened.has_value(), "log tear-off: the window opens");
    if (!opened) return;
    std::unique_ptr<log_window> a = std::move(*opened);
    built_window& ab = g_backend->built.back();
    kept_tabs* strip = ab.tabs;
    kept_window* a_frame = ab.window;
    if (!strip || !ab.stack || !a_frame) return check(false, "log tear-off: the window's strip");
    check(strip->group == "tortilla.log" && strip->detached && strip->received,
          "log tear-off: a log window's tabs tear off and take drops, in the log windows' group");
    log_tab* one = a->tabs()[0].get();
    const std::string one_id = one->id();
    page p1 = page_in(ab, 0, 0);
    check(loaded(ui, p1), "log tear-off: the first tab's log loads");
    size_t lists = ab.lists.size(), entries = ab.entries.size();
    auto two_opened = a->open_tab(second);
    if (!two_opened) return check(false, "log tear-off: a second tab");
    log_tab* two = *two_opened;
    const std::string two_id = two->id();
    page p2 = page_in(ab, lists, entries);
    check(loaded(ui, p2), "log tear-off: the second tab's log loads");
    lists = ab.lists.size();
    entries = ab.entries.size();
    auto three_opened = a->open_tab(third);
    if (!three_opened) return check(false, "log tear-off: a third tab");
    const std::string three_id = (*three_opened)->id();
    page p3 = page_in(ab, lists, entries);
    check(loaded(ui, p3), "log tear-off: the third tab's log loads");
    if (!p1.log || !p2.log || !p3.log || !p2.search) return;

    // -- the middle tab, as the user left it, torn off ----------------------
    const auto make_one = row_of(*p2.log, "Make one");
    const auto beta = row_of(*p1.log, "Add beta");
    if (!make_one || !beta) return check(false, "log tear-off: the tabs' commits are there");
    p1.commits->click({*beta}, *beta);
    p2.commits->click({*make_one}, *make_one);
    std::vector<float> widths;
    for (size_t i = 0; i < log_model::column_count; ++i) widths.push_back(40.f + float(i) * 3);
    p2.commits->set_column_widths(widths);
    p2.commits->set_column_visible(log_model::hash, true);
    const std::vector<bool> visible = p2.commits->visible;
    a->select_tab(two_id);
    if (!ab.choices.empty()) ab.choices.front()->choose(4);
    p2.search->type("Make");  // typed, not yet applied: applied as it goes
    p2.commits->focus();
    log_model& two_log = *p2.log;
    // A menu taken on its page here, its action kept (as a toolkit's menu
    // still open would keep it).
    std::function<void()> kept_notes;
    if (p2.commits->context)
    {
        const rap::menu menu = p2.commits->context(*make_one);
        if (const rap::menu_item* item = find_item(menu, "Edit Notes")) kept_notes = item->action;
    }
    check(bool(kept_notes), "log tear-off: the tab's menu, its action kept");

    rap::window* torn = strip->detached(two_id, rap::point_f{300, 200});
    check(torn && kept.windows.size() == 1 && log_window::of(torn) == kept.windows[0].get(),
          "log tear-off: torn off, a log window of its own is made and handed back");
    if (!torn || kept.windows.size() != 1) return;
    log_window& b = *kept.windows[0];
    built_window* bb = built_for(torn);
    if (!bb || !bb->tabs || !bb->stack) return check(false, "log tear-off: the new window's strip");
    check(b.tabs().size() == 1 && b.tabs()[0].get() == two && b.current_tab() == two &&
              bb->tabs->ids() == std::vector<std::string>{two_id} &&
              bb->tabs->current_id == two_id && bb->stack->shown() == two_id &&
              bb->tabs->group == "tortilla.log",
          "log tear-off: the new window holds the tab, selected, its page on show");
    check(a->tabs().size() == 2 && a->tabs()[0].get() == one &&
              strip->ids() == std::vector<std::string>{one_id, three_id} &&
              ab.stack->ids().size() == 2 && a->current_tab() &&
              a->current_tab()->id() == three_id && !p1.commits->dead && !p3.commits->dead,
          "log tear-off: the source keeps the other tabs, the current one still on show");
    check(!a_frame->closed && kept.closed[0] == 0, "log tear-off: neither window closes");
    check(bb->window->title.find(folder_name(second)) != std::string::npos &&
              bool(bb->window->icon) && bb->window->app_id == two->identity().app_id &&
              a_frame->title.find(folder_name(third)) != std::string::npos,
          "log tear-off: the new window's title, icon and taskbar group are the tab's");
    check(repo_identity_of(torn) == two->shared_identity() &&
              repo_identity_of(a_frame) == a->current_tab()->shared_identity() &&
              two->shared_identity() != a->current_tab()->shared_identity(),
          "log tear-off: the new window is recorded as the tab's repository's, the source as "
          "its tab on show's");
    {
        using namespace rap::layout;
        auto asked = create_window(ui, dialog{"Q", {vbox{label{"?"}, buttons{ok}}}}, torn);
        check(repo_identity_of(asked.get()) == two->shared_identity(),
              "log tear-off: a dialog on the new window is the moved tab's repository's");
    }
    check(torn->size().w == a_frame->size().w && torn->size().h == a_frame->size().h,
          "log tear-off: the new window is the source's size");
    settle(ui);
    page q2 = page_in(*bb, 0, 0);
    check(q2.log == &two_log && q2.commits &&
              q2.commits->selected == std::vector<size_t>{*row_of(two_log, "Make one")} &&
              q2.search && q2.search->value == "Make" && q2.commits->widths == widths &&
              q2.commits->visible == visible && q2.commits->focused,
          "log tear-off: its selection, filter, columns and focus are as they were");
    check(!bb->choices.empty() && bb->choices.front()->value == 4 && !ab.choices.empty() &&
              ab.choices.front()->value != 4 && p2.search->value.empty(),
          "log tear-off: destination row keeps jump state, source row shows its next tab");
    check(p2.commits->dead && !p2.search->dead,
          "log tear-off: its page in the source went once the handler had returned");
    {
        const size_t windows = g_backend->built.size();
        const int said = g_backend->messages;
        if (kept_notes) kept_notes();
        settle(ui);
        check(g_backend->built.size() == windows && g_backend->messages == said,
              "log tear-off: an action kept from its menu in the source does nothing after "
              "the move");
        kept_notes = nullptr;
    }

    // -- dropped into another window at a place: the window it left closes ---
    lists = ab.lists.size();
    entries = ab.entries.size();
    q2.commits->click({*row_of(two_log, "Make two")}, *row_of(two_log, "Make two"));
    // Dropped from within the tab's own work (its scope open on the window
    // it leaves, which empties): that window isn't put back as the tab's.
    bool took = false;
    two->later_for_tests(
        [&]
        {
            took = strip->received(
                {.id = two_id, .title = "", .icon = {}, .tooltip = "", .index = 1, .source = torn});
        })();
    check(repo_identity_of(torn) != two->shared_identity(),
          "log tear-off: the window it left isn't put back as its repository's after its scope");
    check(took && a->tabs().size() == 3 && a->tabs()[1].get() == two &&
              strip->ids() == std::vector<std::string>{one_id, two_id, three_id} &&
              a->current_tab() == two && strip->current_id == two_id &&
              ab.stack->shown() == two_id && b.tabs().empty() && bb->tabs->ids().empty(),
          "log tear-off: dropped into the first window at index 1, there and selected; gone "
          "from the other");
    check(repo_identity_of(a_frame) == two->shared_identity(),
          "log tear-off: dropped in and selected, the window is recorded as its repository's");
    check(!kept.closed[0] && !bb->window->closed && !q2.commits->dead && bb->window,
          "log tear-off: the window it left is still there while the handler runs");
    settle(ui);
    check(kept.closed[0] == 1 && bb->window && bb->window->closed && q2.commits->dead,
          "log tear-off: the window it left closes once the handler has returned");
    check(log_window::of(torn) == &b, "log tear-off: closed, it's still a log window until freed");
    page r2 = page_in(ab, lists, entries);
    check(r2.log == &two_log && r2.commits &&
              r2.commits->selected == std::vector<size_t>{*row_of(two_log, "Make two")} &&
              r2.search && r2.search->value == "Make",
          "log tear-off: dropped in, its selection and filter are as they were");
    const bool none_in_closed =
        !b.receive({.id = one_id, .index = 0, .source = a_frame}) && a->tabs().size() == 3;
    check(none_in_closed, "log tear-off: a window its tabs left takes no more");
    kept.windows[0].reset();
    settle(ui);
    check(!log_window::of(torn), "log tear-off: freed, it's no log window");
    torn = nullptr;

    // -- refused while busy: nothing changes --------------------------------
    const window_shape before = shape_of(*a, ab);
    log_tab_test::hold_ref_command(
        *one, git_process::start(
                  ui, first, {"--version"}, [](const std::string&) {}, [](int) {}));
    check(!strip->detached(one_id, rap::point_f{10, 10}) && kept.windows.size() == 1 &&
              shape_of(*a, ab) == before && !p1.commits->dead,
          "log tear-off: refused while git changes its labels, the source as it was");
    const int messages = g_backend->messages;
    check(!strip->detached(one_id, std::nullopt), "log tear-off: refused from the menu too");
    settle(ui);
    check(g_backend->messages == messages + 1 &&
              g_backend->last_message.find("Move the tab") != std::string::npos,
          "log tear-off: from the menu, said why");
    log_tab_test::hold_ref_command(*one, nullptr);

    // A dialog open over the window: refused until it's gone.
    {
        using namespace rap::layout;
        auto dialog_open = create_window(
            ui, dialog{"Question", {vbox{label{"?"}, buttons{ok, cancel}}}}, a->frame());
        check(!strip->detached(three_id, rap::point_f{10, 10}) && shape_of(*a, ab) == before,
              "log tear-off: refused while a dialog is open over the window");
    }
    check(modals_over(a->frame()) == 0, "log tear-off: the dialog gone, none counted");

    // Torn off again, then refused a drop back while busy there.
    torn = strip->detached(three_id, std::nullopt);
    check(torn && kept.windows.size() == 2 && a->tabs().size() == 2,
          "log tear-off: from the menu (no place), torn off");
    if (!torn || kept.windows.size() != 2) return;
    log_window& c = *kept.windows[1];
    built_window* cb = built_for(torn);
    log_tab* three = c.tabs()[0].get();
    if (!cb) return;
    if (auto from = a_frame->position(); from && torn->position())
        check(torn->position()->x == from->x + 32 && torn->position()->y == from->y + 32,
              "log tear-off: from the menu, a little down and right of the source");
    const window_shape c_before = shape_of(c, *cb), a_now = shape_of(*a, ab);
    log_tab_test::hold_ref_command(
        *three, git_process::start(
                    ui, third, {"--version"}, [](const std::string&) {}, [](int) {}));
    check(!strip->received({.id = three_id, .index = 0, .source = torn}) &&
              shape_of(c, *cb) == c_before && shape_of(*a, ab) == a_now && !kept.closed[1],
          "log tear-off: a busy tab's drop refused, both windows as they were");
    log_tab_test::hold_ref_command(*three, nullptr);
    check(!strip->received({.id = three_id, .index = 0, .source = a_frame}) &&
              !strip->received({.id = "t-none", .index = 0, .source = torn}) &&
              shape_of(c, *cb) == c_before,
          "log tear-off: a drop from itself, or of a tab not there, refused");

    // -- picking commits: no tearing off, no drops ---------------------------
    {
        auto picker = log_window::open(ui, first);
        if (!picker) return check(false, "log tear-off: a window to pick commits");
        built_window& pb = g_backend->built.back();
        (*picker)->pick_commits("Add", "Add", [](std::vector<commit>) {});
        const std::string pick_id = (*picker)->tabs()[0]->id();
        const window_shape p_before = shape_of(**picker, pb);
        check(pb.tabs->group.empty() && !pb.tabs->detached && !pb.tabs->received,
              "log tear-off: a window picking commits has no group, and no tearing off");
        check(!(*picker)->tear_off(pick_id, rap::point_f{1, 1}) && !(*picker)->detach(pick_id) &&
                  shape_of(**picker, pb) == p_before,
              "log tear-off: a window picking commits refuses to let its tab go");
        check(!strip->received({.id = pick_id, .index = 0, .source = (*picker)->frame()}) &&
                  shape_of(**picker, pb) == p_before && a->tabs().size() == 2,
              "log tear-off: its tab isn't taken by another window");
        check(!(*picker)->receive({.id = one_id, .index = 0, .source = a_frame}) &&
                  a->tabs().size() == 2 && shape_of(**picker, pb) == p_before,
              "log tear-off: and it takes none");
        settle(ui);
        picker->reset();
        settle(ui);
    }

    // -- between windows: its work goes on, and lands ------------------------
    // Taken out of the first window by hand, as a drop does before it
    // lands: a commit made meanwhile reloads its log with no page or window.
    // Taken out while on show, from within its own work: the window shows
    // the other tab, and is that one's after the scope, not put back.
    a->select_tab(one_id);
    std::unique_ptr<log_tab> between;
    one->later_for_tests([&] { between = a->detach(one_id); })();
    check(a->current_tab() == two && repo_identity_of(a_frame) == two->shared_identity(),
          "log tear-off: taken out within its own work, the window is the tab shown after's");
    check(between && a->tabs().size() == 1 && strip->ids() == std::vector<std::string>{two_id},
          "log tear-off: taken out of its window");
    if (!between) return;
    const bool committed = git_in(first, {"commit", "-q", "--allow-empty", "-m", "Add delta"});
    check(committed && pump(ui, [&] { return !p1.log->loading() && row_of(*p1.log, "Add delta"); }),
          "log tear-off: a commit made while it's between windows is loaded");
    // A branch made meanwhile: its label known with no page to draw it on.
    const bool branched = git_in(first, {"branch", "-q", "between"});
    check(branched && pump(ui, [&] { return !p1.log->loading() && has_ref(*p1.log, "between"); }),
          "log tear-off: a branch made while it's between windows is heard");
    // Lands in the torn-off window, first.
    const size_t c_lists = cb->lists.size(), c_entries = cb->entries.size();
    c.adopt(std::move(between), 0);
    check(c.tabs().size() == 2 && c.tabs()[0]->id() == one_id && c.current_tab() &&
              c.current_tab()->id() == one_id &&
              cb->tabs->ids() == std::vector<std::string>{one_id, three_id} &&
              strip->ids() == std::vector<std::string>{two_id},
          "log tear-off: landed in another window, first and selected; only that one holds it");
    settle(ui);
    page s1 = page_in(*cb, c_lists, c_entries);
    check(s1.log == p1.log && s1.commits && row_of(*s1.log, "Add delta") &&
              s1.commits->selected == std::vector<size_t>{*row_of(*s1.log, "Add beta")},
          "log tear-off: landed, the new commit listed and its selection kept");
    check(cb->window->title.find(folder_name(first)) != std::string::npos &&
              repo_identity_of(torn) == c.current_tab()->shared_identity() &&
              repo_identity_of(a_frame) == two->shared_identity(),
          "log tear-off: landed, the window's title and repository are its");
    // After landing: a reload reaches the new page, not the old.
    const bool again = git_in(first, {"commit", "-q", "--allow-empty", "-m", "Add epsilon"});
    check(again && pump(ui, [&] { return !s1.log->loading() && row_of(*s1.log, "Add epsilon"); }),
          "log tear-off: a commit made after it landed is loaded");
    settle(ui);
    check(s1.commits->selected == std::vector<size_t>{*row_of(*s1.log, "Add beta")} &&
              !s1.commits->dead && p1.commits->dead,
          "log tear-off: reloaded in its new window, the selection kept");

    // -- the rest closed --------------------------------------------------------
    c.close_tab(three_id);
    settle(ui);
    c.close_tab(one_id);
    settle(ui);
    check(kept.closed[1] == 1, "log tear-off: the torn-off window's last tab closes it");
    a->close_tab(two_id);
    settle(ui);
    a.reset();
    kept.windows.clear();
    settle(ui);
    log_window::keep_for_tests = nullptr;
}

// A tab with a native message or file dialog open over its window (from
// its file list's menu: Revert's question, Save revision to's picker)
// doesn't move; answered or cancelled, it may.
void native_dialog_tests(rap::ui& ui, const std::string& first, const std::string& second,
                         const std::string& root)
{
    auto opened = log_window::open(ui, first);
    if (!opened) return check(false, "log tear-off dialogs: the window opens");
    std::unique_ptr<log_window> a = std::move(*opened);
    built_window& ab = g_backend->built.back();
    page p1 = page_in(ab, 0, 0);
    check(loaded(ui, p1), "log tear-off dialogs: the log loads");
    log_tab* one = a->tabs()[0].get();
    const std::string one_id = one->id();
    auto two = a->open_tab(second);
    if (!two || !p1.log || !p1.files) return check(false, "log tear-off dialogs: a second tab");
    a->select_tab(one_id);
    // A commit's file, selected: its menu has Revert to this revision and
    // Save revision to.
    const auto beta = row_of(*p1.log, "Add beta");
    if (!beta) return check(false, "log tear-off dialogs: the commit is there");
    p1.commits->click({*beta}, *beta);
    const bool files = pump(
        ui, [&] { return p1.file_rows && p1.file_rows->row_count() > 0; }, 10000);
    if (!files || !p1.files->context) return check(false, "log tear-off dialogs: its files");
    p1.files->click({0}, 0);
    const rap::menu menu = p1.files->context(size_t(0));
    const rap::menu_item* save = find_item(menu, "Save revision to...");
    const rap::menu_item* revert = find_item(menu, "Revert to this revision");
    check(save && save->action && revert && revert->action,
          "log tear-off dialogs: the file list's Save revision to and Revert");
    if (!save || !save->action || !revert || !revert->action) return;
    rap::window* frame = a->frame();
    const window_shape before = shape_of(*a, ab);
    g_backend->hold_dialogs = true;

    // -- a file dialog --------------------------------------------------------
    save->action();
    check(g_backend->held_choosers.size() == 1 && modals_over(frame) == 1,
          "log tear-off dialogs: Save revision to's file dialog open, counted over the window");
    check(!a->detach(one_id) && !ab.tabs->detached(one_id, rap::point_f{5, 5}) &&
              shape_of(*a, ab) == before,
          "log tear-off dialogs: refused while the file dialog is open, the window as it was");
    if (!g_backend->held_choosers.empty())
    {
        auto done = std::move(g_backend->held_choosers.front());
        g_backend->held_choosers.clear();
        done({});  // cancelled
    }
    check(modals_over(frame) == 0 && a->move_refusal(one_id).empty(),
          "log tear-off dialogs: the file dialog cancelled, it may move");
    save->action();
    const std::string saved = root + "/saved-beta.txt";
    if (!g_backend->held_choosers.empty())
    {
        auto done = std::move(g_backend->held_choosers.front());
        g_backend->held_choosers.clear();
        check(modals_over(frame) == 1, "log tear-off dialogs: asked again, counted again");
        done({saved});  // chosen
    }
    check(modals_over(frame) == 0 && a->move_refusal(one_id).empty(),
          "log tear-off dialogs: a file chosen, it may move");

    // -- a message ------------------------------------------------------------
    revert->action();
    check(g_backend->held_messages.size() == 1 && modals_over(frame) == 1,
          "log tear-off dialogs: Revert's question open, counted over the window");
    check(!a->detach(one_id) && !ab.tabs->detached(one_id, rap::point_f{5, 5}) &&
              shape_of(*a, ab) == before,
          "log tear-off dialogs: refused while the question is open, the window as it was");
    if (!g_backend->held_messages.empty())
    {
        auto done = std::move(g_backend->held_messages.front());
        g_backend->held_messages.clear();
        done(rap::dialog_role::no);
    }
    check(modals_over(frame) == 0 && a->move_refusal(one_id).empty(),
          "log tear-off dialogs: the question answered, it may move");
    // Gone unanswered (its window closed under it): not counted either.
    revert->action();
    check(modals_over(frame) == 1, "log tear-off dialogs: asked again");
    g_backend->held_messages.clear();
    check(modals_over(frame) == 0, "log tear-off dialogs: dropped unanswered, not counted");
    g_backend->hold_dialogs = false;

    settle(ui);
    a->close_tab((*two)->id());
    settle(ui);
    a->close_tab(one_id);
    settle(ui);
    a.reset();
    settle(ui);
}

// The recent repositories leave out folders that have gone.
void recent_tests(const std::string& root, const std::string& one, const std::string& two)
{
    namespace fs = std::filesystem;
    const fs::path gone = rap::sys::path_from_utf8(root) / "gone-repo";
    std::error_code ec;
    fs::create_directories(gone, ec);
    settings::get().set("log.recentRepositories", "");
    remember_repository(one);
    remember_repository(rap::sys::path_to_utf8(gone));
    remember_repository(two);
    remember_repository(one + "/");  // the same, moved first
    fs::remove(gone, ec);
    const std::vector<std::string> recent = recent_repositories();
    check(recent.size() == 2 && recent[0] == one && recent[1] == two,
          "log tabs: the recent repositories, newest first, without a folder that's gone");
    check(settings::get().history("log.recentRepositories").size() == 3,
          "log tabs: one gone is left in the history, only not offered");
#ifndef _WIN32
    // A newline is a legal POSIX folder byte; histories use tabs, not newlines.
    const auto multiline = rap::sys::path_from_utf8(root) / "repo\nwith-percent%3D=bytes";
    fs::create_directories(multiline, ec);
    const auto path = rap::sys::path_to_utf8(multiline);
    remember_repository(path);
    settings::get().set("recent.unrelated", "saved after the repository history");
    settings::get().use_file(rap::sys::path_from_utf8(root) / "tortilla-settings");
    const auto reloaded = recent_repositories();
    check(!ec && !reloaded.empty() && reloaded.front() == path &&
              settings::get().history("log.recentRepositories").front() == path,
          "log tabs: newline repository history survives an unrelated save and disk reread exactly");
#endif
}

// An environment variable set, or unset with "" (Windows has no setenv).
void set_env(const char* name, const std::string& value)
{
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    if (value.empty())
        unsetenv(name);
    else
        setenv(name, value.c_str(), 1);
#endif
}

// A repository of two commits at `at`.
bool make_repository(const std::string& at)
{
    std::error_code ec;
    const std::filesystem::path dir = rap::sys::path_from_utf8(at);
    std::filesystem::create_directories(dir, ec);
    auto in = [&](std::vector<std::string> args)
    {
        args.insert(args.begin(),
                    {"-c", "user.name=Tortilla Test", "-c", "user.email=test@tortilla.invalid",
                     "-c", "commit.gpgsign=false"});
        return run_git(at, args, git_output::stdout_only).ok();
    };
    bool ok = in({"init", "-q", "-b", "main"});
    for (const char* file : {"one", "two"})
    {
        std::ofstream(dir / (std::string(file) + ".txt"), std::ios::binary) << file << "\n";
        ok = ok && in({"add", "."}) && in({"commit", "-qm", std::string("Make ") + file});
    }
    return ok;
}

// The item labelled `label` (its access key's "_" left out), in `m` or a
// submenu.
const rap::menu_item* find_item(const rap::menu& m, std::string_view label)
{
    for (const rap::menu_item& item : m)
    {
        std::string text = item.label;
        std::erase(text, '_');
        if (text == label) return &item;
        if (const rap::menu_item* in = find_item(item.children, label)) return in;
    }
    return nullptr;
}

// A tab's + choosing a repository git refuses for its owner: the question
// whether to trust it (main.cpp's, ui/trust_repository.h), declined (no
// tab, nothing more said), then accepted (its tab). git is told the owner
// differs (GIT_TEST_ASSUME_DIFFERENT_OWNER), and its global config is a
// scratch file (GIT_CONFIG_GLOBAL), so the user's own is never touched.
void trust_tests(rap::ui& ui, const std::string& root, const std::string& first)
{
    namespace fs = std::filesystem;
    const std::string refused =
        rap::sys::path_to_utf8(rap::sys::path_from_utf8(root) / "owned-repo");
    const fs::path global = rap::sys::path_from_utf8(root) / "trust-gitconfig";
    check(make_repository(refused), "log trust: a repository to refuse");
    // GIT_CONFIG_GLOBAL is git 2.32's, the owner test's variable 2.35's;
    // without them, accepting would write to the user's own config.
    const git_result version = run_git(".", {"version"}, git_output::stdout_only);
    int major = 0, minor = 0;
    if (std::sscanf(version.output.c_str(), "git version %d.%d", &major, &minor) != 2 ||
        major < 2 || (major == 2 && minor < 35))
        return check(false,
                     "log trust: git 2.35 or later, for GIT_CONFIG_GLOBAL and the owner test");

    auto opened = log_window::open(ui, first);
    check(opened.has_value(), "log trust: the window opens");
    if (!opened) return;
    std::unique_ptr<log_window> w = std::move(*opened);
    built_window& b = g_backend->built.back();
    kept_tabs* strip = b.tabs;
    check(strip && loaded(ui, page_in(b, 0, 0)), "log trust: the first tab loads");
    if (!strip) return;

    std::ofstream(global, std::ios::binary).flush();
    // git's config is the test's own while this runs: no system config (a
    // CI image's may trust every folder: safe.directory=*), the global one
    // a scratch file, none from the environment; and git told the owner
    // differs. Put back as they were after.
    const char* const vars[] = {"GIT_CONFIG_GLOBAL", "GIT_CONFIG_NOSYSTEM",
                                "GIT_TEST_ASSUME_DIFFERENT_OWNER", "GIT_CONFIG_COUNT",
                                "GIT_CONFIG_PARAMETERS"};
    std::vector<std::string> were;
    for (const char* v : vars) were.push_back(rap::sys::env(v).value_or(""));
    set_env("GIT_CONFIG_GLOBAL", rap::sys::path_to_utf8(global));
    set_env("GIT_CONFIG_NOSYSTEM", "1");
    set_env("GIT_TEST_ASSUME_DIFFERENT_OWNER", "1");
    set_env("GIT_CONFIG_COUNT", "");
    set_env("GIT_CONFIG_PARAMETERS", "");
    // Whether git itself refuses it decides: only where it opens the
    // repository all the same (nothing here can make it refuse) is the
    // question untestable, and said so, with why, rather than failed.
    const git_result opens = run_git(refused, {"rev-parse", "--git-dir"});
    bool asks = false;
    if (opens.ok())
    {
        const git_result where =
            run_git(refused, {"config", "--show-origin", "--get-all", "safe.directory"});
        std::printf("skip log trust: git opens the repository whatever its owner here, so the "
                    "trust question isn't tested\n  safe.directory: %s\n  rev-parse: %s\n",
                    where.output.c_str(), opens.output.c_str());
    }
    else
    {
        check(opens.output.find("safe.directory") != std::string::npos,
              "log trust: git refuses the repository for its owner");
        asks = untrusted_repository(refused).has_value();
        check(asks, "log trust: Tortilla sees git refuse it, and asks");
    }
    if (asks)
    {
        settings::get().set("log.recentRepositories", "");
        // Declined: no tab, and nothing more said than the question.
        const int before = g_backend->messages;
        g_backend->answer = rap::dialog_role::no;
        g_backend->choose = std::vector<std::string>{refused};
        strip->plus();
        settle(ui);
        check(g_backend->messages == before + 1 && w->tabs().size() == 1 &&
                  g_backend->last_message.find(folder_name(refused)) != std::string::npos,
              "log trust: + on a repository git refuses asks to trust it; declined, no tab, no "
              "error");
        // Accepted: trusted (in the scratch config), and its tab.
        g_backend->answer = rap::dialog_role::yes;
        g_backend->choose = std::vector<std::string>{refused};
        strip->plus();
        settle(ui);
        std::ifstream in(global, std::ios::binary);
        const std::string config{std::istreambuf_iterator<char>(in), {}};
        check(g_backend->messages == before + 2 && w->tabs().size() == 2 &&
                  w->tabs()[1]->repo_location().find(folder_name(refused)) != std::string::npos &&
                  config.find("[safe]") != std::string::npos &&
                  config.find(folder_name(refused)) != std::string::npos,
              "log trust: accepted, it's trusted and opens in a tab");
        check(!untrusted_repository(refused), "log trust: git opens it now");
        if (w->tabs().size() == 2)
        {
            strip->close_box(w->tabs()[1]->id());
            settle(ui);
        }
    }
    g_backend->answer.reset();
    for (size_t i = 0; i < std::size(vars); ++i) set_env(vars[i], were[i]);
    strip->close_box(w->tabs()[0]->id());
    settle(ui);
    w.reset();
    settle(ui);
}

// Windows a tab's actions open are its repository's (ui/repo_icon.h), in a
// window showing two: the second's action, the first's, and what the
// second opens later, hidden; each window holds its identity (and its
// colour) until it goes.
void identity_tests(rap::ui& ui, const std::string& first, const std::string& second)
{
    auto opened = log_window::open(ui, first);
    check(opened.has_value(), "log identity: the window opens");
    if (!opened) return;
    std::unique_ptr<log_window> w = std::move(*opened);
    built_window& b = g_backend->built.back();
    kept_tabs* strip = b.tabs;
    kept_window* frame = b.window;
    page p1 = page_in(b, 0, 0);
    check(strip && frame && loaded(ui, p1), "log identity: the first tab loads");
    if (!strip || !frame || !p1.log) return;
    log_tab* one = w->tabs()[0].get();
    const size_t lists = b.lists.size(), entries = b.entries.size(), buttons = b.buttons.size();
    auto two_opened = w->open_tab(second);
    if (!two_opened) return check(false, "log identity: a second repository's tab");
    log_tab* two = *two_opened;
    page p2 = page_in(b, lists, entries);
    check(loaded(ui, p2), "log identity: the second tab loads");
    if (!p2.log) return;
    const std::shared_ptr<const repo_identity> a = one->shared_identity();
    const std::shared_ptr<const repo_identity> bee = two->shared_identity();
    check(a && bee && a != bee && a->repo != bee->repo && bee->lease,
          "log identity: each tab has its repository's identity, its colour held");
    check(repo_identity_of(w->frame()) == bee && frame->app_id == a->app_id,
          "log identity: the window's dialogs are the shown tab's; its taskbar group stays the "
          "first's");

    // Edit Notes, from the tab's commit list: a dialog on the window.
    auto notes = [&](page& p, const char* summary) -> kept_window*
    {
        const auto row = row_of(*p.log, summary);
        if (!row || !p.commits->context) return nullptr;
        p.commits->click({*row}, *row);
        const rap::menu m = p.commits->context(*row);
        const rap::menu_item* item = find_item(m, "Edit Notes");
        if (!item || !item->action) return nullptr;
        const size_t before = g_backend->built.size();
        item->action();
        return g_backend->built.size() > before ? g_backend->built.back().window : nullptr;
    };
    kept_window* from_two = notes(p2, "Make one");
    check(from_two && repo_identity_of(from_two) == bee && from_two->app_id == bee->app_id,
          "log identity: the second tab's action opens a window of the second repository");

    strip->click(one->id());
    check(repo_identity_of(w->frame()) == a,
          "log identity: the first tab shown, the window's dialogs are its");
    kept_window* from_one = notes(p1, "Add beta");
    check(from_one && from_one != from_two && repo_identity_of(from_one) == a &&
              from_one->app_id == a->app_id,
          "log identity: the first tab's action opens a window of the first repository");

    // Later, from the second tab, hidden: a post's work, or a worker's.
    std::unique_ptr<rap::window> late_dialog, late_own;
    ui.post(two->later_for_tests(
        [&]
        {
            late_dialog = create_window(ui, rap::layout::dialog{"Late", {rap::layout::label{""}}},
                                        w->frame());
            late_own = create_window(ui, rap::layout::window{"Late", {rap::layout::label{""}}});
        }));
    settle(ui);
    check(late_dialog && late_own && repo_identity_of(late_dialog.get()) == bee &&
              repo_identity_of(late_own.get()) == bee &&
              static_cast<kept_window*>(late_own.get())->app_id == bee->app_id,
          "log identity: what the hidden second tab opens later is the second repository's, on "
          "the window or of its own");
    check(repo_identity_of(w->frame()) == a,
          "log identity: after it, the window's dialogs are the shown tab's again");
    // And the first's: the first repository's.
    std::unique_ptr<rap::window> own_one;
    one->later_for_tests(
        [&] {
            own_one = create_window(ui, rap::layout::window{"Own", {rap::layout::label{""}}});
        })();
    check(own_one && repo_identity_of(own_one.get()) == a,
          "log identity: the first tab's late work is the first repository's");

    // Each window holds its identity, and with it its colour, as long as
    // it's there.
    const long held = bee.use_count();
    rap::window* gone_dialog = late_dialog.get();
    late_dialog.reset();
    late_own.reset();
    own_one.reset();
    check(bee.use_count() == held - 2 && !repo_identity_of(gone_dialog),
          "log identity: a window lets go of its repository's identity as it goes");
    // As a toolkit does, the window lets go of its answer's callback (which
    // holds the dialog) once it's given.
    for (kept_window* d : {from_one, from_two})
        if (auto finished = d ? std::exchange(d->finished, {}) : nullptr)
            finished(rap::dialog_role::cancel);
    settle(ui);
    check(bee.use_count() == held - 3, "log identity: so does the action's dialog");

    // The tab on show changed during a tab's own work: the window is the one
    // shown after it, not put back as it was.
    ui.post(two->later_for_tests([&] { w->select_tab(two->id()); }));
    settle(ui);
    check(w->current_tab() == two && repo_identity_of(w->frame()) == bee,
          "log identity: a tab shown during a tab's work is the window's after it");
    ui.post(two->later_for_tests([&] { w->select_tab(one->id()); }));
    settle(ui);
    check(w->current_tab() == one && repo_identity_of(w->frame()) == a,
          "log identity: a tab left during its own work, the window is the one shown after it");

    // The second, on show, closed by its OK: the first shows, and the
    // window is the first's, not the closed tab's put back.
    w->select_tab(two->id());
    kept_button* ok = nullptr;
    for (size_t i = buttons; i < b.buttons.size(); ++i)
        if (b.buttons[i]->text == "OK") ok = b.buttons[i];
    check(ok && ok->click, "log identity: the second tab's OK");
    if (ok && ok->click) ok->click();
    check(w->tabs().size() == 1 && w->current_tab() == one && repo_identity_of(w->frame()) == a,
          "log identity: the tab on show closed by its OK, the window is the one shown after");
    settle(ui);
    check(repo_identity_of(w->frame()) == a,
          "log identity: and still, once the closed tab has gone");

    strip->close_box(one->id());
    settle(ui);
    w.reset();
    settle(ui);
}

// A frame owns one row, while its selected tab supplies every value and action.
void toolbar_tests(rap::ui& ui, const std::string& first, const std::string& second)
{
    auto opened = log_window::open(ui, first);
    if (!opened) return check(false, "log row: window opens");
    auto w = std::move(*opened);
    auto& b = g_backend->built.back();
    auto* one = w->current_tab();
    auto p1 = page_in(b, 0, 0);
    if (!loaded(ui, p1) || b.menus.empty() || b.choices.empty())
        return check(false, "log row: widgets");
    auto* search = p1.search;
    auto* branch = b.menus.front();
    auto* jump = b.choices.front();
    check(b.band_hbox, "log row: window vbox starts with a single eight-control hbox above stack");
    search->type("Add");
    jump->choose(1);
    log_tab_test::branch(*one, "one branch");
    auto opened_two = w->open_tab(second);
    if (!opened_two) return check(false, "log row: second tab opens");
    auto* two = *opened_two;
    check(log_tab_test::filter(*one).text == "Add",
          "log row: switching flushes pending typed filter");
    check(search->value.empty() && jump->value == 0,
          "log row: second tab has its own filter and jump");
    search->type("Make");
    jump->choose(7);
    log_tab_test::branch(*two, "two branch");
    check(log_tab_test::search(*one) == "Add" && log_tab_test::jump(*one) == 1 &&
              log_tab_test::search(*two) == "Make" && log_tab_test::jump(*two) == 7,
          "log row: edits and choices apply only to the current tab");
    auto type_menu = search->menu ? search->menu() : rap::menu{};
    if (const auto* subject = find_item(type_menu, "Subject"); subject && subject->action)
        subject->action();
    const auto second_fields = log_tab_test::filter(*two).fields;
    const auto second_cue = search->placeholder;
    check(second_fields != log_tab_test::filter(*one).fields,
          "log row: filter type action applies to the selected tab only");
    const int edits = search->edits, choices = jump->edits;
    w->select_tab(one->id());
    check(search->value == "Add" && jump->value == 1 && branch->text == "one branch" &&
              search->placeholder != second_cue,
          "log row: first tab restores all row values");
    const int writes = branch->writes;
    log_tab_test::branch(*two, "hidden branch");
    check(branch->writes == writes && branch->text == "one branch",
          "log row: hidden state updates do not touch shared widgets");
    w->select_tab(two->id());
    check(search->value == "Make" && jump->value == 7 && branch->text == "hidden branch" &&
              search->edits == edits && jump->edits == choices &&
              search->placeholder == second_cue &&
              log_tab_test::filter(*two).fields == second_fields,
          "log row: switching back restores hidden updates without user edit handlers");
    auto filter_menu = search->menu ? search->menu() : rap::menu{};
    const auto* field = find_item(filter_menu, "Subject");
    std::function<void()> kept_field = field ? field->action : std::function<void()>{};
    const auto* dates = find_item(filter_menu, "Date range...");
    std::function<void()> kept_dates = dates ? dates->action : std::function<void()>{};
    auto branch_menu = branch->menu ? branch->menu() : rap::menu{};
    std::function<void()> kept_branch;
    if (const auto* all = find_item(branch_menu, "All branches")) kept_branch = all->action;
    check(kept_field && kept_branch && kept_dates,
          "log row: filter, dates and branch menu actions retained");
    auto moved = w->detach(two->id());
    check(moved && search->value == "Add" && jump->value == 1,
          "log row: source row follows new current tab after detach");
    const auto before = log_tab_test::filter(*two).fields;
    const auto preset_before = log_tab_test::preset(*two);
    const auto windows_before = g_backend->built.size();
    if (kept_field) kept_field();
    if (kept_branch) kept_branch();
    if (kept_dates) kept_dates();
    check(log_tab_test::filter(*two).fields == before &&
              log_tab_test::preset(*two) == preset_before &&
              g_backend->built.size() == windows_before,
          "log row: old menu actions inert after move");
    w->adopt(std::move(moved));
    check(search->value == "Make" && jump->value == 7,
          "log row: adopted tab restores filter and jump selection");
    filter_menu = search->menu ? search->menu() : rap::menu{};
    field = find_item(filter_menu, "Subject");
    kept_field = field ? field->action : std::function<void()>{};
    dates = find_item(filter_menu, "Date range...");
    kept_dates = dates ? dates->action : std::function<void()>{};
    branch_menu = branch->menu ? branch->menu() : rap::menu{};
    kept_branch = {};
    if (const auto* all = find_item(branch_menu, "All branches")) kept_branch = all->action;
    const auto id = two->id();
    w->close_tab(id);
    settle(ui);
    const auto fields = log_tab_test::filter(*one).fields;
    const auto windows_after = g_backend->built.size();
    if (kept_field) kept_field();
    if (kept_dates) kept_dates();
    if (kept_branch) kept_branch();
    check(log_tab_test::filter(*one).fields == fields && g_stale_calls == 0 &&
              g_backend->built.size() == windows_after,
          "log row: retained menu inert after close, no poisoned widget calls");
    search->focused = false;
    check(b.window->key &&
              b.window->key({.press = true,
                             .key = rap::key::character,
                             .codepoint = 'f',
                             .modifiers = rap::mod_ctrl}) &&
              search->focused,
          "log row: Ctrl+F focuses the window-owned filter");
    filter_menu = search->menu ? search->menu() : rap::menu{};
    field = find_item(filter_menu, "Subject");
    kept_field = field ? field->action : std::function<void()>{};
    const auto last_fields = log_tab_test::filter(*one).fields;
    const auto last_text = log_tab_test::search(*one);
    if (b.window->close_request) b.window->close_request();
    if (kept_field) kept_field();
    search->type("closing");
    check(log_tab_test::filter(*one).fields == last_fields &&
              log_tab_test::search(*one) == last_text,
          "log row: callbacks and kept menus inert while frame is closing");
    w.reset();
    settle(ui);
}

// A tab closed while a nested loop (a modal dialog's, say) runs the queue
// before the toolkit gets to its page: the posts the window keeps the tab
// for run, and the tab goes, while the page's widgets are still there.
// They hold nothing of it by then: its models taken from the lists, their
// callbacks doing nothing.
void teardown_tests(rap::ui& ui, const std::string& first, const std::string& second)
{
    auto opened = log_window::open(ui, first);
    if (!opened) return check(false, "log teardown: the window opens");
    std::unique_ptr<log_window> w = std::move(*opened);
    built_window& b = g_backend->built.back();
    kept_tabs* strip = b.tabs;
    check(strip && loaded(ui, page_in(b, 0, 0)), "log teardown: the first tab loads");
    const size_t lists = b.lists.size(), entries = b.entries.size();
    auto two = w->open_tab(second);
    if (!strip || !two) return check(false, "log teardown: a second tab");
    page p2 = page_in(b, lists, entries);
    check(loaded(ui, p2) && p2.files && p2.search, "log teardown: the second tab loads");
    if (!p2.log || !p2.files || !p2.search) return;
    // Menus taken before the close, their actions kept (as a toolkit's menu
    // still open would keep them): the commit list's Edit Notes, and the
    // file list's Show log.
    std::function<void()> kept_notes, kept_log;
    if (const auto row = row_of(*p2.log, "Make two"); row && p2.commits->context)
    {
        p2.commits->click({*row}, *row);
        const rap::menu commits_menu = p2.commits->context(*row);
        if (const rap::menu_item* item = find_item(commits_menu, "Edit Notes"))
            kept_notes = item->action;
        pump(ui, [&] { return p2.file_rows && p2.file_rows->row_count() > 0; });
        if (p2.files->context && p2.file_rows && p2.file_rows->row_count() > 0)
        {
            p2.files->click({0}, 0);
            const rap::menu files_menu = p2.files->context(size_t(0));
            if (const rap::menu_item* item = find_item(files_menu, "Show log"))
                kept_log = item->action;
        }
    }
    check(kept_notes && kept_log, "log teardown: the tab's menus, their actions kept");
    const std::weak_ptr<const repo_identity> tab_held = (*two)->shared_identity();

    std::vector<std::function<void()>> held;
    g_held_pages = &held;
    strip->close_box((*two)->id());
    settle(ui);
    pump(
        ui, [] { return false; }, 300);
    // (The file list's kept action holds the repository's identity, as
    // its scope; the tab, gone, doesn't: checked once that goes, below.)
    check(held.size() == 1 && !p2.commits->dead && tab_held.use_count() <= 1,
          "log teardown: the tab has gone, its page's widgets not yet");
    check(!p2.commits->model && !p2.files->model,
          "log teardown: its lists let go of its models first");
    // The toolkit calling them still: nothing reaches the tab.
    if (p2.commits->changed) p2.commits->changed();
    check(p2.commits->context && p2.commits->context(size_t(0)).empty(),
          "log teardown: its callbacks do nothing, its menus are empty");
    // The actions kept from before: nothing opens, nothing's said.
    const size_t windows = g_backend->built.size();
    const int said = g_backend->messages;
    if (kept_notes) kept_notes();
    if (kept_log) kept_log();
    settle(ui);
    check(g_backend->built.size() == windows && g_backend->messages == said,
          "log teardown: menu actions kept from before the close do nothing after it");
    kept_notes = nullptr;
    kept_log = nullptr;
    check(tab_held.expired(), "log teardown: nothing holds the closed tab's identity after");
    if (p2.search->changed) p2.search->changed("Make");
    if (p2.files->changed) p2.files->changed();
    if (p2.files->context) p2.files->context(size_t(0));
    g_held_pages = nullptr;
    for (const auto& f : held) f();
    check(p2.commits->dead && w->tabs().size() == 1, "log teardown: the page goes after");

    strip->close_box(w->tabs()[0]->id());
    settle(ui);
    w.reset();
    settle(ui);
}

// View Patch's window is the tab on show's: opened as the log loads (the
// setting on), with another repository's windows the process default, and
// following the tab on show after.
void patch_identity_tests(rap::ui& ui, const std::string& first, const std::string& second)
{
    use_repo_icon(ui, first);  // the process's default: the first repository's
    settings::get().set("log.viewPatch", "1");
    const size_t before = g_backend->built.size();
    auto opened = log_window::open(ui, second);
    if (!opened) return check(false, "log patch: the window opens");
    std::unique_ptr<log_window> w = std::move(*opened);
    auto built_at = [&](rap::window* window) -> built_window*
    {
        for (built_window& b : g_backend->built)
            if (b.window == window) return &b;
        return nullptr;
    };
    built_window* b = built_at(w->frame());
    check(b && loaded(ui, page_in(*b, 0, 0)), "log patch: the log loads");
    if (!b) return;
    settle(ui);
    // The one window made since that isn't the log's: View Patch's.
    auto viewer = [&]() -> kept_window*
    {
        kept_window* found = nullptr;
        for (size_t i = before; i < g_backend->built.size(); ++i)
            if (kept_window* k = g_backend->built[i].window; k && k != b->window) found = k;
        return found;
    };
    log_tab* two = w->tabs()[0].get();
    kept_window* patch = viewer();
    check(patch && repo_identity_of(patch) == two->shared_identity() &&
              patch->app_id == two->identity().app_id,
          "log patch: View Patch, opened as the log loads, is the log's repository's");
    auto one = w->open_tab(first);
    check(one && loaded(ui, page_in(*b, 0, 0)), "log patch: a second tab");
    if (!one || !patch) return;
    check(viewer() == patch && repo_identity_of(patch) == (*one)->shared_identity(),
          "log patch: it follows the tab on show");
    w->select_tab(two->id());
    check(repo_identity_of(patch) == two->shared_identity(), "log patch: and back");

    b->tabs->close_box((*one)->id());
    settle(ui);
    const auto closing_transactions = settings::get().transactions_for_tests();
    b->tabs->close_box(two->id());
    settle(ui);
    check(settings::get().transactions_for_tests() == closing_transactions + 1 &&
              settings::get().contains("log.patch.window.size"),
          "settings: closing Log with View Patch saves both geometries in exactly one transaction");
    w.reset();
    settle(ui);
    settings::get().set("log.viewPatch", "0");
}

// The subject of `rev`'s commit in the repository at `dir`.
std::string subject_of(const std::string& dir, const std::string& rev)
{
    std::string s = run_git(dir, {"log", "-1", "--format=%s", rev}, git_output::stdout_only).output;
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

// The page a log window's tab shows, its log loaded and `summary`'s commit
// selected (--select's), as the user would see it.
bool selected_in(rap::ui& ui, const page& p, const std::string& summary)
{
    if (!loaded(ui, p)) return false;
    return pump(
        ui,
        [&]
        {
            const auto row = row_of(*p.log, summary);
            return row && p.commits->selected == std::vector<size_t>{*row};
        },
        5000);
}

// A log another `tortilla log` handed over (log.openIn, log_window::
// open_forwarded): with no log window open, a window of its own; a new
// window when it asks for one; otherwise a tab of the log window used last
// (not one picking commits, nor one under a dialog), brought to the front,
// or that window's tab showing it already, selected; --select in the tab as
// in a window.
void forward_tests(rap::ui& ui, const std::string& first, const std::string& second,
                   const std::string& third)
{
    torn_off kept;  // the windows of their own it opens
    check(!log_window::forward_target(), "log forward: no log window open, none to go into");

    // -- no log window: one of its own, --select in it -----------------------
    auto opened = log_window::open_forwarded(ui, {.path = first, .select = "HEAD~1"}, kept.keep);
    check(opened && kept.windows.size() == 1 && *opened == kept.windows[0].get() &&
              (*opened)->tabs().size() == 1,
          "log forward: with no log window open, a window of its own");
    if (!opened || kept.windows.size() != 1) return;
    log_window* a = kept.windows[0].get();
    built_window* ab = built_for(a->frame());
    if (!ab || !ab->tabs) return check(false, "log forward: the first window's widgets");
    check(selected_in(ui, page_in(*ab, 0, 0), subject_of(first, "HEAD~1")),
          "log forward: --select's commit selected in the window");
    check(ab->window->presented == 1, "log forward: the window of its own brought forward, once");
    const std::string a_first = a->tabs()[0]->id();

    // -- log.openIn=window: a window of its own, one open or not -------------
    auto own = log_window::open_forwarded(ui, {.path = second, .new_window = true}, kept.keep);
    check(own && kept.windows.size() == 2 && *own == kept.windows[1].get() &&
              a->tabs().size() == 1 && (*own)->tabs().size() == 1,
          "log forward: asked for a window, a window of its own beside the one open");
    if (kept.windows.size() != 2) return;
    log_window* b = kept.windows[1].get();
    check(log_window::forward_target() == b, "log forward: the window opened last is the one used");
    {
        built_window* bb = built_for(b->frame());
        check(bb && bb->window->presented == 1 && ab->window->presented == 1,
              "log forward: asked for a window, that one brought forward, once, the other not");
    }

    // -- a tab of the window used last, brought to the front ------------------
    // A key pressed in the first: used since.
    check(bool(ab->window->key), "log forward: the window takes keys");
    if (ab->window->key) ab->window->key(rap::key_event{.press = true, .key = rap::key::other});
    check(log_window::forward_target() == a,
          "log forward: a key pressed in it, that one's used last");
    const int shown = ab->window->shown, presented = ab->window->presented;
    const size_t lists = ab->lists.size(), entries = ab->entries.size();
    auto tabbed = log_window::open_forwarded(ui, {.path = third, .select = "HEAD~1"}, kept.keep);
    check(tabbed && *tabbed == a && kept.windows.size() == 2 && a->tabs().size() == 2 &&
              b->tabs().size() == 1 && a->current_tab() == a->tabs()[1].get() &&
              ab->tabs->current_id == a->tabs()[1]->id() &&
              a->tabs()[1]->repo_location().find(folder_name(third)) != std::string::npos,
          "log forward: a new tab in the window used last, selected, and no window more");
    // present(), not show(): a minimised window is restored as it's raised.
    check(ab->window->presented == presented + 1 && ab->window->shown == shown,
          "log forward: that window brought forward (present), once");
    if (a->tabs().size() != 2) return;
    const std::string a_third = a->tabs()[1]->id();
    page third_page = page_in(*ab, lists, entries);
    check(third_page.commits && selected_in(ui, third_page, subject_of(third, "HEAD~1")),
          "log forward: --select's commit selected in the tab");

    // -- open there already: its tab selected, --select honoured there -------
    ab->tabs->click(a_first);
    check(a->current_tab() == a->tabs()[0].get(), "log forward: back to the first tab");
    auto again = log_window::open_forwarded(ui, {.path = third, .select = "HEAD"}, kept.keep);
    check(again && *again == a && a->tabs().size() == 2 && a->current_tab()->id() == a_third &&
              ab->tabs->current_id == a_third && kept.windows.size() == 2,
          "log forward: a repository open there already selects its tab");
    check(ab->window->presented == presented + 2,
          "log forward: and that window's brought forward again");
    check(selected_in(ui, third_page, subject_of(third, "HEAD")),
          "log forward: and --select moves that tab's selection");

    // -- not a repository: said, nothing opened ------------------------------
    const std::string nowhere =
        rap::sys::path_to_utf8(rap::sys::path_from_utf8(third).parent_path() / "no-such-repo");
    auto failed = log_window::open_forwarded(ui, {.path = nowhere}, kept.keep);
    check(!failed && !failed.error().empty() && a->tabs().size() == 2 && kept.windows.size() == 2,
          "log forward: no repository there: why, and no tab or window");

    // -- a window picking commits is passed over -----------------------------
    {
        auto picker = log_window::open(ui, first);
        if (!picker) return check(false, "log forward: a window to pick commits");
        (*picker)->pick_commits("Add", "Add", [](std::vector<commit>) {});
        // Made last, so used last; but it takes no tabs.
        check(log_window::forward_target() == a,
              "log forward: a window picking commits isn't gone into");
        auto past = log_window::open_forwarded(ui, {.path = second}, kept.keep);
        check(past && *past == a && (*picker)->tabs().size() == 1 && a->tabs().size() == 3 &&
                  kept.windows.size() == 2,
              "log forward: the tab goes to the window used before it");
        settle(ui);
        picker->reset();
        settle(ui);
    }

    // -- nor one with a dialog open over it ----------------------------------
    {
        const auto dialog = count_modal(a->frame());
        check(log_window::forward_target() == b,
              "log forward: a window under a dialog isn't gone into; the one used before it is");
    }
    check(log_window::forward_target() == a, "log forward: the dialog gone, it is again");

    settle(ui);
    kept.windows.clear();
    settle(ui);
    check(!log_window::forward_target(), "log forward: the windows gone, none to go into");
}

void settings_close_failure_tests(rap::ui& ui, const std::string& first,
                                   const std::string& second, const std::string& root)
{
    auto& st = settings::get();
    const auto file = rap::sys::path_from_utf8(root) / "close-report-settings";
    st.use_file(file);
    (void)st.take_save_report();
    auto opened = log_window::open(ui, first);
    if (!opened) return check(false, "settings reporting: window opens");
    auto w = std::move(*opened);
    kept_window* frame = g_backend->built.back().window;
    if (!w->open_tab(second)) return check(false, "settings reporting: second tab opens");
    const std::string id = w->tabs().front()->id();
    st.set("log.jumpType", "unsaved-test");  // make the close save differ
    const int before_tab = g_backend->messages;
    settings_fail_next(settings_test_failure::replace);
    check(w->close_tab(id) && g_backend->messages == before_tab + 1,
          "settings reporting: tab close reports a failed save on the surviving window");

    auto next = log_window::open(ui, first);
    if (!next) return check(false, "settings reporting: another window opens");
    st.remove("log.window.size");
    const int before_window = g_backend->messages;
    settings_fail_next(settings_test_failure::close);
    const bool closed = frame->close_request && frame->close_request();
    check(closed && g_backend->messages == before_window,
          "settings reporting: with another window open, close defers the error");
    w.reset();
    (*next)->show();
    check(g_backend->messages == before_window + 1 && st.take_save_report().message.empty(),
          "settings reporting: next shown window reports the retained close failure once");
    next->reset();

    // Exercise the actual window lifecycle and production before-quit helper.
    // Only the backend dialog and quit are held, so no GUI opens and the null
    // event loop remains usable for the following tests.
    {
        st.set("log.viewPatch", "1");
        const size_t before_build = g_backend->built.size();
        auto last = log_window::open(ui, first);
        if (!last) return check(false, "settings reporting: final window opens");
        windows::init(ui);
        log_window* last_window = windows::keep(std::move(*last));
        built_window* log_frame = built_for(last_window->frame());
        if (!log_frame) return check(false, "settings reporting: final frame exists");
        kept_window* final_frame = log_frame->window;
        check(loaded(ui, page_in(*log_frame, 0, 0)), "settings reporting: final log loads");
        settle(ui);
        check(g_backend->built.size() >= before_build + 2,
              "settings reporting: View Patch is open for the close contention check");
        st.remove("log.window.size");
        std::atomic<bool> ready{false}, held{false}, release{false};
        std::thread other([&]
                          {
                              settings_lock_holder lock(file);
                              held = lock.held();
                              ready = true;
                              while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                          });
        while (!ready) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const int said = g_backend->messages;
        const int quits = g_backend->quit_requests;
        g_backend->hold_dialogs = true;
        g_backend->suppress_quit = true;
        const auto transactions = st.transactions_for_tests();
        const auto start = std::chrono::steady_clock::now();
        const bool final_closed = final_frame->close_request && final_frame->close_request();
        const auto elapsed = std::chrono::steady_clock::now() - start;
        release = true;
        other.join();
        check(final_closed && g_backend->messages == said &&
                  st.transactions_for_tests() == transactions + 1 &&
                  (!held || (elapsed >= std::chrono::milliseconds(700) &&
                             elapsed < std::chrono::milliseconds(2500))),
              "settings reporting: Log plus View Patch close attempts one transaction within one second");
        if (!held)
            st.remember_save_report({"test close failure (filesystem has no locks)"});
        settle(ui);  // actual windows::release invokes quit_after_settings
        check(windows::ending() && g_backend->messages == said + 1 &&
                  !g_backend->last_message_detail.empty() &&
                  g_backend->held_messages.size() == 1 && g_backend->quit_requests == quits,
              "settings reporting: final close reports while running and cannot quit before dismissal");
        quit_after_settings(ui);  // a duplicate close cannot bypass the held warning
        check(g_backend->quit_requests == quits && g_backend->messages == said + 1,
              "settings reporting: repeated quit request waits for the same warning");
        auto dismissed = g_backend->held_messages.empty()
                             ? std::function<void(rap::dialog_role)>{}
                             : std::move(g_backend->held_messages.front());
        g_backend->held_messages.clear();
        g_backend->hold_dialogs = false;
        if (dismissed) dismissed(rap::dialog_role::ok);
        check(g_backend->quit_requests == quits + 1 && st.take_save_report().message.empty(),
              "settings reporting: only dismissing the close warning permits quit");
        g_backend->suppress_quit = false;
        windows::init(ui);
    }
    st.use_file(rap::sys::path_from_utf8(root) / "tortilla-settings");
}

void settings_preference_failure_tests(rap::ui& ui, const std::string& repo,
                                        const std::string& root, rap::subscription& warning_lifetime)
{
    auto& st = settings::get();
    for (bool parented : {false, true})
    {
        st.use_file(rap::sys::path_from_utf8(root) /
                    (parented ? "settings-parent-failure" : "settings-standalone-failure"));
        st.set(k_appearance_setting, "light");
        (void)st.take_save_report();
        std::unique_ptr<rap::window> parent;
        if (parented)
            parent = create_window(ui, rap::layout::window{"Settings owner", {rap::layout::label{"Owner"}}});
        bool saved = false, closed = false;
        show_settings(ui, parent.get(), repo, settings_page::general,
                      [&] { saved = true; },
                      [&] { closed = true; quit_after_settings(ui); });
        built_window& built = g_backend->built.back();
        kept_window* frame = built.window;
        kept_drop_down* appearance_choice = nullptr;
        for (auto* choice : built.choices)
            if (choice->items == std::vector<std::string>{"Follow system", "Light", "Dark"})
                appearance_choice = choice;
        check(appearance_choice && frame && frame->finished,
              "settings preferences: actual General page and finish handler are built");
        if (!appearance_choice || !frame || !frame->finished) return;
        appearance_choice->choose(2);
        const int messages = g_backend->messages, quits = g_backend->quit_requests;
        g_backend->hold_dialogs = true;
        g_backend->suppress_quit = true;
        const auto transactions = st.transactions_for_tests();
        settings_fail_next(settings_test_failure::replace);
        frame->finished(rap::dialog_role::ok);
        settle(ui);
        check(st.transactions_for_tests() == transactions + 1 && !saved && !closed &&
                  st.value(k_appearance_setting) == "light" && built.window == frame &&
                  g_backend->messages == messages + 1 && g_backend->held_messages.size() == 1 &&
                  g_backend->quit_requests == quits,
              "settings preferences: one failed geometry/preferences save retains its window and continuation until dismissal");
        if (parented)
        {
            parent.reset();  // retained dismissal must not invoke a destroyed owner's callback
            quit_after_settings(ui);  // waiting for disposal, not a response callback
        }
        else
        {
            quit_after_settings(ui);  // queue is empty, but the explicit warning is still held
            check(g_backend->quit_requests == quits,
                  "settings preferences: a direct warning blocks quit even with no deferred error");
        }
        auto dismissed = g_backend->held_messages.empty()
                             ? std::function<void(rap::dialog_role)>{}
                             : std::move(g_backend->held_messages.front());
        g_backend->held_messages.clear();
        g_backend->hold_dialogs = false;
        if (!parented && dismissed) dismissed(rap::dialog_role::ok);
        // Qt drops its callback when the parent destroys the QMessageBox,
        // without emitting finished. Drop the LAST copy without invoking it.
        dismissed = {};
        settle(ui);
        check(!saved && closed == !parented && !built.window &&
                  g_backend->quit_requests == quits + 1,
              parented ? "settings preferences: no-response disposal clears warning and quits without a dead-owner continuation"
                       : "settings preferences: response followed by disposal completes and quits exactly once");
        g_backend->suppress_quit = false;
    }
    const int quits = g_backend->quit_requests;
    bool continued = false;
    g_backend->hold_dialogs = true;
    g_backend->suppress_quit = true;
    report_unsaved_settings(ui, nullptr, "UI teardown", [&] { continued = true; });
    quit_after_settings(ui);
    warning_lifetime.reset();  // the production UI scope ends before its backend
    g_backend->held_messages.clear();
    check(!continued && g_backend->quit_requests == quits,
          "settings preferences: UI teardown cancels retained callbacks without calling a dying UI");
    warning_lifetime = track_settings_warnings(ui);
    g_backend->hold_dialogs = false;
    g_backend->suppress_quit = false;
    st.use_file(rap::sys::path_from_utf8(root) / "tortilla-settings");
}

void settings_save_outcome_tests(rap::ui& ui, const std::string& repo, const std::string& root)
{
    namespace fs = std::filesystem;
    auto& st = settings::get();
#ifdef TORTILLA_FM_PROGRAM
    // Exercise the real PCManFM file effect without touching the user's set.
    struct isolated_fm_data
    {
        std::optional<std::string> home = rap::sys::env("XDG_DATA_HOME");
        std::optional<std::string> dirs = rap::sys::env("XDG_DATA_DIRS");
        ~isolated_fm_data()
        {
            if (home) ::setenv("XDG_DATA_HOME", home->c_str(), 1);
            else ::unsetenv("XDG_DATA_HOME");
            if (dirs) ::setenv("XDG_DATA_DIRS", dirs->c_str(), 1);
            else ::unsetenv("XDG_DATA_DIRS");
        }
    } isolated;
    const fs::path fm_package = rap::sys::path_from_utf8(root) / "settings-fm-package";
    std::error_code ec;
    fs::create_directories(fm_package / "file-manager/actions", ec);
    std::ofstream(fm_package / "file-manager/actions/tortilla.desktop") << "[Desktop Entry]\n";
    ::setenv("XDG_DATA_DIRS", fm_package.c_str(), 1);
#endif
    for (const auto outcome : {save_outcome::saved, save_outcome::saved_with_cleanup_warning,
                               save_outcome::failed})
    {
        const bool committed = outcome != save_outcome::failed;
        const bool warning = outcome != save_outcome::saved;
        const std::string tag = std::to_string(int(outcome));
        st.use_file(rap::sys::path_from_utf8(root) / ("settings-outcome-" + tag));
        st.set(k_appearance_setting, "light");
        st.set("shell.top", "log");  // a non-default set PCManFM must regenerate
        save_ssh_client({.command = "ssh-before-test", .variant = "ssh"});
        (void)st.take_save_report();
#ifdef TORTILLA_FM_PROGRAM
        const fs::path fm_home = rap::sys::path_from_utf8(root) / ("settings-fm-user-" + tag);
        const fs::path fm_dir = fm_home / "file-manager/actions";
        fs::create_directories(fm_dir, ec);
        const fs::path obsolete = fm_dir / "tortilla-user-obsolete.desktop";
        std::ofstream(obsolete) << "must survive a failed save\n";
        ::setenv("XDG_DATA_HOME", fm_home.c_str(), 1);
#endif
        int saved_calls = 0, closed_calls = 0;
        const int messages = g_backend->messages, quits = g_backend->quit_requests;
        g_backend->hold_dialogs = true;
        g_backend->suppress_quit = true;
        show_settings(ui, nullptr, repo, settings_page::general,
                      [&] { ++saved_calls; },
                      [&] { ++closed_calls; quit_after_settings(ui); });
        built_window& built = g_backend->built.back();
        kept_window* frame = built.window;
        kept_drop_down* appearance_choice = nullptr;
        for (auto* choice : built.choices)
            if (choice->items == std::vector<std::string>{"Follow system", "Light", "Dark"})
                appearance_choice = choice;
        check(appearance_choice && frame && frame->finished && !built.page_tabs.empty(),
              "settings outcomes: actual Settings General page is built");
        if (!appearance_choice || !frame || !frame->finished || built.page_tabs.empty()) return;
        appearance_choice->choose(2);
        built.page_tabs.front()->set_current(int(settings_page::context_menu));
        built.page_tabs.front()->set_current(int(settings_page::network));
        kept_drop_down* ssh_choice = nullptr;
        kept_entry* ssh_command = nullptr;
        for (auto* choice : built.choices)
            if (!choice->items.empty() && choice->items.back() == "Custom...") ssh_choice = choice;
        for (auto* entry : built.entries)
            if (entry->placeholder == "ssh -i ~/.ssh/key") ssh_command = entry;
        check(ssh_choice && ssh_command, "settings outcomes: actual Network SSH controls are built");
        if (!ssh_choice || !ssh_command) return;
        ssh_choice->choose(int(ssh_choice->items.size()) - 1);
        const std::string chosen = "ssh-committed-test-" + tag;
        ssh_command->type(chosen);
        const auto transactions = st.transactions_for_tests();
        if (outcome == save_outcome::failed) settings_fail_next(settings_test_failure::replace);
        if (outcome == save_outcome::saved_with_cleanup_warning)
            settings_fail_next(settings_test_failure::cleanup_warning);
        frame->finished(rap::dialog_role::ok);
        settle(ui);
        const auto env = ssh_environment();
        const std::string expected = committed ? chosen : "ssh-before-test";
        check(st.transactions_for_tests() == transactions + 1 &&
                  st.value(k_appearance_setting) == (committed ? "dark" : "light") &&
                  st.value("ssh.command") == expected &&
                  std::ranges::find(env, std::pair<std::string, std::string>{"GIT_SSH_COMMAND", expected}) != env.end() &&
                  saved_calls == (committed ? 1 : 0),
              "settings outcomes: both committed results apply cached preferences, SSH and saved callback once; failures apply none");
#ifdef TORTILLA_FM_PROGRAM
        check(fs::exists(fm_dir / "tortilla-user.desktop") == committed &&
                  fs::exists(obsolete) == !committed,
              "settings outcomes: actual isolated PCManFM files update for both committed results, never failure");
#endif
        check(g_backend->messages == messages + (warning ? 1 : 0) &&
                  closed_calls == (warning ? 0 : 1) &&
                  g_backend->quit_requests == quits + (warning ? 0 : 1),
              "settings outcomes: report dismissal gates close and quit for failures and cleanup warnings");
        if (warning)
        {
            check(g_backend->last_message.find(committed ? "were saved" : "couldn't be saved") != std::string::npos &&
                      (!committed || g_backend->last_message.find("couldn't be saved") == std::string::npos),
                  "settings outcomes: committed cleanup copy is softer and never says choices were unsaved");
            quit_after_settings(ui);
            check(g_backend->quit_requests == quits,
                  "settings outcomes: an already shown cleanup/failure warning blocks an empty-queue quit");
            auto dismissed = g_backend->held_messages.empty()
                                 ? std::function<void(rap::dialog_role)>{}
                                 : std::move(g_backend->held_messages.front());
            g_backend->held_messages.clear();
            if (dismissed) dismissed(rap::dialog_role::ok);
            dismissed = {};
            settle(ui);
        }
        check(closed_calls == 1 && saved_calls == (committed ? 1 : 0) &&
                  g_backend->quit_requests == quits + 1 && st.take_save_report().message.empty(),
              "settings outcomes: dismissal closes once without repeating committed effects or queued diagnostics");
        g_backend->hold_dialogs = false;
        g_backend->suppress_quit = false;
    }
    // Convenience SSH saves have no dialog. Publication still updates the
    // process environment while its cleanup diagnostic remains reportable.
    st.use_file(rap::sys::path_from_utf8(root) / "settings-ssh-outcomes");
    save_ssh_client({.command = "ssh-initial", .variant = "ssh"});
    settings_fail_next(settings_test_failure::cleanup_warning);
    save_ssh_client({.command = "ssh-with-cleanup", .variant = "plink"});
    const auto committed_env = ssh_environment();
    const auto cleanup = st.take_save_report();
    check(cleanup.has_cleanup_warning() && cleanup.committed() &&
              std::ranges::find(committed_env, std::pair<std::string, std::string>{"GIT_SSH_COMMAND", "ssh-with-cleanup"}) != committed_env.end(),
          "settings outcomes: convenience SSH save applies a committed cleanup result and retains its warning");
    settings_fail_next(settings_test_failure::replace);
    save_ssh_client({.command = "ssh-not-saved", .variant = "ssh"});
    check(st.take_save_report().failed() && ssh_environment() == committed_env,
          "settings outcomes: failed convenience SSH save retains the previous environment and reports failure");
    st.remember_save_report(settings::saved::cleanup_warning("committed cleanup detail"));
    st.remember_save_report({"uncommitted failure detail"});
    const auto mixed = st.take_save_report();
    check(mixed.failed() && mixed.message.find("committed cleanup detail") != std::string::npos &&
              mixed.message.find("uncommitted failure detail") != std::string::npos,
          "settings outcomes: queued mixed diagnostics retain failure severity and committed cleanup details");
    st.use_file(rap::sys::path_from_utf8(root) / "tortilla-settings");
    use_ssh_client(saved_ssh_client());
}

// Folders shown and remembered without trailing separators, a root's own
// kept: "C:" alone is a folder on that drive, not its top.
void folder_text_tests()
{
    check(folder_text("/a/b/") == "/a/b" && folder_text("/a/b//") == "/a/b" &&
              folder_text("/a/b") == "/a/b" && folder_text("/") == "/",
          "folder text: trailing separators go, the root's stays");
#ifdef _WIN32
    check(folder_text("C:/") == "C:/" && folder_text("C:\\") == "C:\\" &&
              folder_text("C:/x/") == "C:/x" && folder_text("C:\\x\\") == "C:\\x" &&
              folder_text("D:/tmp\\") == "D:/tmp",
          "folder text: a drive's top keeps its separator");
    settings::get().set("log.recentRepositories", "");
    remember_repository("C:\\");
    const auto recent = settings::get().history("log.recentRepositories");
    check(recent.size() == 1 && recent[0] == "C:\\",
          "folder text: a drive's top is remembered as its top");
    settings::get().set("log.recentRepositories", "");
#endif
}

}  // namespace

void run_log_page_tests(const std::string& root)
{
    namespace fs = std::filesystem;
    const fs::path dir = rap::sys::path_from_utf8(root) / "page-repo";
    const std::string repo = rap::sys::path_to_utf8(dir);
    std::error_code ec;
    fs::create_directories(dir, ec);
    auto git = [&](std::vector<std::string> args)
    {
        args.insert(args.begin(),
                    {"-c", "user.name=Tortilla Test", "-c", "user.email=test@tortilla.invalid",
                     "-c", "commit.gpgsign=false"});
        return run_git(repo, args, git_output::stdout_only).ok();
    };
    auto write = [&](const std::string& name, const std::string& text)
    { std::ofstream(dir / name, std::ios::binary) << text; };
    bool made = git({"init", "-q", "-b", "main"});
    for (const char* name : {"alpha", "beta", "gamma"})
    {
        write(std::string(name) + ".txt", std::string(name) + "\n");
        made = made && git({"add", "."}) && git({"commit", "-qm", std::string("Add ") + name});
    }
    // Two files changed in the working tree, for its row's file list.
    write("alpha.txt", "alpha changed\n");
    write("beta.txt", "beta changed\n");
    check(made, "log page: a repository to show");
    if (!made) return;
    settings::get().set("log.viewPatch", "0");

    rap::register_builtin_backend("tortilla-test", &create_keeping);
    int argc = 1;
    char program[] = "tortilla-tests";
    char* argv[] = {program, nullptr};
    auto loaded = rap::ui::load(
        argc, argv, {.backend = "tortilla-test", .look = {.density = rap::density::compact}});
    check(loaded && g_backend, "log page: the test's backend starts");
    if (!loaded || !g_backend) return;
    rap::ui& ui = **loaded;
    auto warning_lifetime = track_settings_warnings(ui);

    // The remotes' poll would arm itself again at once, forever, on the
    // null backend, whose timers don't wait: held here instead.
    polls polled;
    log_tab::poll_hooks_for_tests = &polled.hooks;
    g_stale_calls = 0;

    page_tests(ui, repo, git);
    poll_tests(ui, repo, polled);
    forge_presentation_tests(ui, repo);
    build_tab_icon_tests(ui, repo);
    forge_fresh_canvas_tests(ui, repo);
    forge_download_tests(ui, repo);
    forge_tabs_tests(ui, repo);
    lower_page_view_tests(ui, repo);
    check(g_stale_calls == 0, "log page: nothing reached a widget after its window went");

    // A second repository, and a third, for a window's tabs.
    std::vector<std::string> others;
    for (const char* name : {"tab-repo", "third-repo"})
    {
        const fs::path other = rap::sys::path_from_utf8(root) / name;
        fs::create_directories(other, ec);
        const std::string at = rap::sys::path_to_utf8(other);
        auto in = [&](std::vector<std::string> args)
        {
            args.insert(args.begin(),
                        {"-c", "user.name=Tortilla Test", "-c", "user.email=test@tortilla.invalid",
                         "-c", "commit.gpgsign=false"});
            return run_git(at, args, git_output::stdout_only).ok();
        };
        bool ok = in({"init", "-q", "-b", "main"});
        for (const char* file : {"one", "two"})
        {
            std::ofstream(other / (std::string(file) + ".txt"), std::ios::binary) << file << "\n";
            ok = ok && in({"add", "."}) && in({"commit", "-qm", std::string("Make ") + file});
        }
        check(ok, "log tabs: another repository to show");
        if (!ok) return;
        others.push_back(at);
    }
    g_stale_calls = 0;
    tab_tests(ui, repo, others[0], others[1]);
    toolbar_tests(ui, repo, others[0]);
    settings_close_failure_tests(ui, repo, others[0], root);
    settings_preference_failure_tests(ui, repo, root, warning_lifetime);
    settings_save_outcome_tests(ui, repo, root);
    recent_tests(root, repo, others[0]);
    identity_tests(ui, repo, others[0]);
    teardown_tests(ui, repo, others[0]);
    trust_tests(ui, root, repo);
    patch_identity_tests(ui, repo, others[0]);
    folder_text_tests();
    check(g_stale_calls == 0, "log tabs: nothing reached a page's widgets after they went");

    g_stale_calls = 0;
    tearoff_tests(ui, repo, others[0], others[1]);
    native_dialog_tests(ui, repo, others[0], root);
    check(g_stale_calls == 0,
          "log tear-off: nothing reached a page's widgets after they went, in either window");

    g_stale_calls = 0;
    forward_tests(ui, repo, others[0], others[1]);
    check(g_stale_calls == 0, "log forward: nothing reached a page's widgets after they went");

    log_tab::poll_hooks_for_tests = nullptr;
    warning_lifetime.reset();
    loaded->reset();
    g_backend = nullptr;
}

}  // namespace test
}  // namespace tortilla
