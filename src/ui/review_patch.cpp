#include "review_patch.h"

#include "git/command.h"
#include "git/diff_tool.h"
#include "git/patch_serial.h"
#include "i18n.h"
#include "ui/compare.h"
#include "ui/progress_dialog.h"
#include "ui/repo_icon.h"

#include <rapport/sys.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <thread>
#include <utility>

namespace tortilla
{
namespace
{

namespace fs = std::filesystem;

// The patch's files, and whether each applies to the working tree.
class patch_file_rows : public rap::list_model
{
public:
    std::vector<std::string> paths;
    std::vector<char> applies;
    bool checked = false;  // applies is known
    void changed() { notify_reset(); }
    size_t row_count() const override { return paths.size(); }
    std::string text(size_t row, size_t column) const override
    {
        if (column == 0) return paths[row];
        if (!checked) return _("Checking...");
        return applies[row] ? _("Applies") : _("Doesn't apply");
    }
};

std::string shown_path(const std::string& workdir)
{
    std::string root = workdir;
    while (root.size() > 1 && (root.back() == '/' || root.back() == '\\')) root.pop_back();
    return rap::sys::path_to_utf8(rap::sys::path_from_utf8(root).make_preferred());
}

// A folder of its own for one file patched, under the temp folder.
fs::path scratch_folder()
{
    static std::atomic<unsigned> count{0};
    std::error_code ec;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return fs::temp_directory_path(ec) / "tortilla" / "review-patch" /
           (std::to_string(stamp) + "-" + std::to_string(count++));
}

}  // namespace

void review_patch(rap::ui& ui, rap::window* parent, const std::string& workdir,
                  const std::string& patch, std::function<void(bool applied)> done)
{
    struct state
    {
        rap::ui& ui;
        std::string workdir, patch, title;
        std::function<void(bool)> done;
        std::unique_ptr<rap::window> window;
        patch_file_rows rows;
        rap::list* list = nullptr;
        rap::label* status = nullptr;
        std::shared_ptr<git_process> tool;  // the diff tool, while it runs

        std::vector<std::string> selected() const
        {
            std::vector<std::string> out;
            for (size_t row : list->selection())
                if (row < rows.paths.size()) out.push_back(rows.paths[row]);
            return out;
        }

        // The file as it is against the file as the patch makes it: the
        // file copied into a folder of its own and patched there (git apply
        // outside a repository works as patch does).
        void review(std::weak_ptr<state> weak)
        {
            const auto cur = list->current();
            if (!cur || *cur >= rows.paths.size()) return;
            const std::string path = rows.paths[*cur];
            std::thread(
                [weak, &ui = ui, workdir = workdir, patch = patch, path]
                {
                    const fs::path folder = scratch_folder();
                    const fs::path original =
                        rap::sys::path_from_utf8(workdir) / rap::sys::path_from_utf8(path);
                    const fs::path patched = folder / rap::sys::path_from_utf8(path);
                    std::error_code ec;
                    fs::create_directories(patched.parent_path(), ec);
                    const bool existed = fs::exists(original, ec);
                    if (existed) fs::copy_file(original, patched, ec);
                    const git_result r =
                        run_git(rap::sys::path_to_utf8(folder), apply_args(patch, {path}));
                    const std::string base =
                        existed ? rap::sys::path_to_utf8(original) : empty_file();
                    const std::string mine =
                        fs::exists(patched, ec) ? rap::sys::path_to_utf8(patched) : empty_file();
                    ui.post(
                        [weak, r, base, mine]
                        {
                            auto s = weak.lock();
                            if (!s || !s->window) return;
                            if (!r.ok())
                                return s->ui.message(
                                    s->window.get(),
                                    {.icon = rap::message_spec::icon::error,
                                     .title = _("Review Patch"),
                                     .text = _("This file's part of the patch doesn't apply."),
                                     .detail = r.output});
                            s->tool = compare_files(s->ui, s->window.get(), base, mine, {});
                        });
                })
                .detach();
        }

        // In the progress dialog, which outlives this window.
        void apply(std::vector<std::string> only)
        {
            auto finished = std::move(done);
            run_progress_dialog(ui, nullptr, title, workdir, {apply_args(patch, only)},
                                [finished](bool ok)
                                {
                                    if (finished) finished(ok);
                                });
        }
    };
    auto st = std::make_shared<state>(state{
        .ui = ui,
        .workdir = workdir,
        .patch = patch,
        .title =
            rap::i18n::format(_("{0} - Review Patch"),
                              rap::sys::path_to_utf8(rap::sys::path_from_utf8(patch).filename())),
        .done = std::move(done)});

    using namespace rap::layout;
    using namespace rap::literals;
    state* s = st.get();
    std::weak_ptr<state> weak = st;
    st->window = create_window(
        ui,
        dialog{st->title,
               {vbox{
                   label{rap::i18n::format(
                             _("The files {0} changes, in {1}:"),
                             rap::sys::path_to_utf8(rap::sys::path_from_utf8(patch).filename()),
                             shown_path(workdir))}
                       .wrap()
                       .bind(s->status),
                   list{{{_("File"), 40_ch, true}, {_("State"), 14_ch}}, &s->rows}
                       .bind(s->list)
                       .selection(rap::selection_mode::multi)
                       .on_activate([s, weak](size_t) { s->review(weak); })
                       .compact()
                       .height(12_lines)
                       .stretch(),
                   buttons{dialog_button{rap::dialog_role::none, _("_Review")}.on_click(
                               [s, weak] { s->review(weak); }),
                           dialog_button{rap::dialog_role::none, _("Apply _selected")}.on_click(
                               [s]
                               {
                                   if (auto only = s->selected(); !only.empty())
                                   {
                                       s->apply(only);
                                       s->window->close();
                                   }
                               }),
                           dialog_button{rap::dialog_role::ok, _("_Apply all")},
                           dialog_button{rap::dialog_role::close}},
               }}}
            .size(72_ch, 22_lines),
        parent);
    st->window->on_finished(
        [st](rap::dialog_role role) mutable
        {
            auto keep = std::move(st);
            keep->ui.post([keep] {});  // destroy after this callback returns
            if (!keep->done) return;   // applied from Apply selected
            if (role == rap::dialog_role::ok)
            {
                keep->apply({});
                return;
            }
            keep->done(false);
        });

    // The files, then whether each applies: off the UI thread.
    std::thread(
        [&ui, weak, workdir, patch]
        {
            const git_result r = run_git(workdir, {"apply", "-z", "--numstat", "--", patch},
                                         git_output::stdout_only);
            std::vector<std::string> paths =
                r.ok() ? numstat_paths(r.output) : std::vector<std::string>{};
            ui.post(
                [weak, paths, error = r.ok() ? std::string() : r.output]
                {
                    auto s = weak.lock();
                    if (!s || !s->window) return;
                    if (!error.empty()) s->status->set_text(error);
                    s->rows.paths = paths;
                    s->rows.changed();
                });
            std::vector<char> applies;
            for (const std::string& p : paths)
                applies.push_back(run_git(workdir, apply_args(patch, {p}, true)).ok());
            ui.post(
                [weak, applies = std::move(applies)]
                {
                    auto s = weak.lock();
                    if (!s || !s->window) return;
                    s->rows.applies = applies;
                    s->rows.checked = true;
                    s->rows.changed();
                });
        })
        .detach();
    st->window->show();
}

}  // namespace tortilla
