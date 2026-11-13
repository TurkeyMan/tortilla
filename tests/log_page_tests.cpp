// The log's page dropped and built again into another window, on the null
// backend: what the page showed (the selection, the panes, the columns,
// the filter box, the selected files, the focus) comes back, and what
// changed meanwhile shows.
//
// The null backend's lists, splitters and entries keep nothing (or, the
// entries, no callbacks), so this test's backend wraps it and builds those
// three itself, keeping what they're told and what they'd call.
#include "log_page_tests.h"

#include "changes/file_model.h"
#include "git/command.h"
#include "git/sync.h"
#include "log/log_model.h"
#include "log/log_tab.h"
#include "settings.h"
#include "test_support.h"

#include <rapport/rapport.h>
#include <rapport/sys.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <thread>

namespace tortilla::test
{
namespace
{

// A widget's common part, keeping its focus.
template <class Base>
class kept : public Base
{
public:
    void set_enabled(bool) override {}
    void set_visible(bool) override {}
    void set_tooltip(const std::string&) override {}
    void focus() override { focused = true; }
    bool has_focus() const override { return focused; }
    bool focused = false;
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

    void set_model(rap::list_model* m) override { model = m; }
    std::vector<size_t> selection() const override { return selected; }
    std::optional<size_t> current() const override { return current_row; }
    void select(const std::vector<size_t>& rows) override
    {
        selected = rows;
        std::ranges::sort(selected);
    }
    void set_current(size_t row) override { current_row = row; }
    void scroll_to(size_t, rap::scroll_align) override {}
    std::optional<float> row_offset(size_t) const override { return std::nullopt; }
    void set_row_offset(size_t, float) override {}
    void set_sort_indicator(std::optional<size_t>, rap::sort_order) override {}
    std::vector<float> column_widths() const override { return widths; }
    void set_column_widths(const std::vector<float>& w) override
    {
        for (size_t i = 0; i < w.size() && i < widths.size(); ++i)
            if (w[i] > 0) widths[i] = w[i];
    }
    bool column_visible(size_t c) const override { return c < visible.size() && visible[c]; }
    void set_column_visible(size_t c, bool on) override
    {
        if (c < visible.size()) visible[c] = on;
    }
    std::vector<size_t> column_order() const override { return order; }
    void set_column_order(const std::vector<size_t>& o) override
    {
        std::vector<size_t> sorted = o;
        std::ranges::sort(sorted);
        for (size_t i = 0; i < sorted.size(); ++i)
            if (sorted[i] != i) return;
        if (sorted.size() == order.size()) order = o;
    }
    void on_selection_changed(std::function<void()> f) override { changed = std::move(f); }
    void on_activate(std::function<void(size_t)>) override {}
    void on_sort_requested(std::function<void(size_t, rap::sort_order)>) override {}
    void on_context_menu(std::function<rap::menu(std::optional<size_t>)>) override {}
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
};

class kept_splitter : public kept<rap::splitter>
{
public:
    // Laid out: each pane its share.
    explicit kept_splitter(size_t panes)
        : sizes_(panes, 100.f)
    {
    }
    std::vector<float> sizes() const override { return sizes_; }
    void set_sizes(const std::vector<float>& s) override
    {
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
    }
    // The user's typing: the text, then the entry's word of it.
    void type(const std::string& text)
    {
        value = text;
        if (changed) changed(text);
    }
    std::string text() const override { return value; }
    void set_text(const std::string& t) override { value = t; }
    void set_placeholder(const std::string& p) override { placeholder = p; }
    void on_changed(std::function<void(const std::string&)> f) override { changed = std::move(f); }
    void on_activate(std::function<void()>) override {}
    std::string value, placeholder;
    bool clearable = false;
    std::function<void(const std::string&)> changed;
};

// What one window was built with, of those three.
struct built_window
{
    std::vector<kept_list*> lists;
    std::vector<kept_splitter*> splitters;
    std::vector<kept_entry*> entries;
};

// The null backend, with lists, splitters and entries of this test's own.
class keeping_backend : public rap::backend, rap::build_sink
{
public:
    explicit keeping_backend(std::unique_ptr<rap::ui> inner)
        : inner_(std::move(inner))
    {
    }

    std::vector<built_window> built;

    std::string_view name() const override { return "tortilla-test"; }
    std::unique_ptr<rap::window> create_window(const rap::window_spec& spec, rap::window* parent,
                                               rap::build_sink& sink) override
    {
        built.emplace_back();
        sink_ = &sink;
        return null().create_window(spec, parent, *this);
    }
    int run() override { return null().run(); }
    void quit(int code) override { null().quit(code); }
    void set_color_scheme(rap::color_scheme s) override { null().set_color_scheme(s); }
    void post(std::function<void()> f) override { null().post(std::move(f)); }
    void message(rap::window* parent, const rap::message_spec& m,
                 std::function<void(rap::dialog_role)> done) override
    {
        null().message(parent, m, std::move(done));
    }
    void choose_files(rap::window* parent, const rap::file_dialog_spec& f,
                      std::function<void(std::vector<std::string>)> done) override
    {
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
    bool save_snapshot(const std::string& path) override { return null().save_snapshot(path); }
    std::unique_ptr<rap::image> create_image(int w, int h, rap::pixel_format f, const uint8_t* p,
                                             size_t stride) override
    {
        return null().create_image(w, h, f, p, stride);
    }
    rap::subscription timer(int ms, bool repeat, std::function<void()> fn) override
    {
        return null().timer(ms, repeat, std::move(fn));
    }
    void set_busy(bool on) override { null().set_busy(on); }

private:
    rap::backend& null() { return inner_->backend(); }

    // The null backend's widgets, but for the three kept here: those are
    // made here and handed to the core in their place.
    void created(const rap::node& n, rap::widget& w) override
    {
        rap::widget* mine = nullptr;
        if (const auto* s = std::get_if<rap::spec::list>(&n.spec))
        {
            auto l = std::make_unique<kept_list>(s->columns);
            built.back().lists.push_back(l.get());
            mine = keep(std::move(l));
        }
        else if (const auto* s = std::get_if<rap::spec::splitter>(&n.spec))
        {
            auto l = std::make_unique<kept_splitter>(s->panes.size());
            built.back().splitters.push_back(l.get());
            mine = keep(std::move(l));
        }
        else if (const auto* s = std::get_if<rap::spec::entry>(&n.spec))
        {
            auto e = std::make_unique<kept_entry>(*s);
            built.back().entries.push_back(e.get());
            mine = keep(std::move(e));
        }
        sink_->created(n, mine ? *mine : w);
    }
    void created(const rap::spec::dialog_button& b, rap::button& w) override
    {
        sink_->created(b, w);
    }
    rap::widget* keep(std::unique_ptr<rap::widget> w)
    {
        widgets_.push_back(std::move(w));
        return widgets_.back().get();
    }

    std::unique_ptr<rap::ui> inner_;
    rap::build_sink* sink_ = nullptr;
    // For as long as the backend: they keep no pointer that could go stale
    // (a model is only remembered, never asked).
    std::vector<std::unique_ptr<rap::widget>> widgets_;
};

keeping_backend* g_backend = nullptr;

rap::backend* create_keeping(int abi, int* argc, char** argv, const rap::look_options&)
{
    if (abi != rap::abi_version) return nullptr;
    auto inner = rap::ui::load(*argc, argv, {.backend = "null"});
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
    rap::window* window() override { return w; }
    void tab_changed(log_tab&) override {}
    void finish(bool) override {}
    bool patch_shown() const override { return false; }
    void show_patch(bool) override {}
    void selection_moved(log_tab&) override {}
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
    // The list over the rest, the forge column beside the commit's text and
    // files, then the text over the files (log_tab_page.cpp).
    if (b.splitters.size() >= 3)
    {
        p.panes = b.splitters[0];
        p.details = b.splitters[2];
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

std::optional<size_t> row_of(const log_model& log, const std::string& summary)
{
    for (size_t r = 0; r < log.row_count(); ++r)
        if (const commit* c = log.commit(r); c && c->summary == summary) return r;
    return std::nullopt;
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

    // Nothing asked of the network unasked: the remotes' poll would arm
    // itself again at once on the null backend, whose timers don't wait.
    const sync_when was_checking = check_setting();
    set_check_setting(sync_when::never);
    settings::get().set("log.viewPatch", "0");

    rap::register_builtin_backend("tortilla-test", &create_keeping);
    int argc = 1;
    char program[] = "tortilla-tests";
    char* argv[] = {program, nullptr};
    auto loaded = rap::ui::load(argc, argv, {.backend = "tortilla-test"});
    check(loaded && g_backend, "log page: the test's backend starts");
    if (!loaded || !g_backend) return set_check_setting(was_checking);
    rap::ui& ui = **loaded;

    auto opened = log_tab::open(ui, repo);
    check(opened.has_value(), "log page: the tab opens the repository");
    if (!opened) return set_check_setting(was_checking);
    std::unique_ptr<log_tab> tab = std::move(*opened);
    test_host host;
    tab->set_host(&host);
    auto build = [&](std::optional<log_view_state> view)
    {
        auto w = ui.create(rap::layout::window{"Log", {tab->build_page()}});
        host.w = w.get();
        tab->bind_page(std::move(view));
        return w;
    };
    auto window = build(std::nullopt);
    page p = last_page();
    check(p.commits && p.files && p.panes && p.details && p.search && p.path_filter,
          "log page: its lists, panes and filter boxes are found");
    if (!p.commits || !p.files || !p.panes || !p.details || !p.search || !p.path_filter)
        return set_check_setting(was_checking);
    log_model& log = *p.log;
    tab->start(false);
    const bool ready = pump(
        ui, [&] { return !log.loading() && log.working_tree_known() && log.commit_rows() == 3; });
    check(ready && log.is_working_tree(0), "log page: the log loads, the working tree's row first");
    if (!ready) return set_check_setting(was_checking);

    // -- what the user changes, carried to a second window ------------------
    const auto beta = row_of(log, "Add beta"), gamma = row_of(log, "Add gamma");
    check(beta && gamma, "log page: the commits are there");
    if (!beta || !gamma) return set_check_setting(was_checking);
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
    pump(
        ui, [] { return false; }, 50);
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
    if (!beta_file) return set_check_setting(was_checking);
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
    pump(
        ui, [] { return false; }, 50);
    loaded->reset();
    g_backend = nullptr;
    set_check_setting(was_checking);
}

}  // namespace tortilla::test
