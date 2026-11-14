#include "review_patch.h"

#include "git/command.h"
#include "git/patch_serial.h"
#include "git/temp_files.h"
#include "i18n.h"
#include "ui/compare.h"
#include "ui/modal_dialogs.h"
#include "ui/progress_dialog.h"
#include "ui/repo_icon.h"

#include <rapport/sys.h>

#include <algorithm>
#include <cstdio>
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
    std::vector<patch_entry> entries;
    std::vector<char> applies;
    bool checked = false;  // applies is known
    void changed() { notify_reset(); }
    size_t row_count() const override { return entries.size(); }
    std::string text(size_t row, size_t column) const override
    {
        const patch_entry& e = entries[row];
        if (column == 0)
            return e.old_path == e.path
                       ? e.path
                       : rap::i18n::format(_("{0} (from {1})"), e.path, e.old_path);
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
        rap::button* apply_selected = nullptr;
        rap::button* apply_all = nullptr;
        std::shared_ptr<git_process> tool;  // the diff tool, while it runs

        std::vector<std::string> selected() const
        {
            std::vector<std::string> out;
            for (size_t row : list->selection())
                if (row < rows.entries.size()) out.push_back(rows.entries[row].path);
            return out;
        }

        // The file as it is against the file as the patch makes it: the
        // file it starts from (a rename's old one) copied into a folder of
        // its own and patched there.
        void review(std::weak_ptr<state> weak)
        {
            const auto cur = list->current();
            if (!cur || *cur >= rows.entries.size()) return;
            const patch_entry entry = rows.entries[*cur];
            std::thread(
                [weak, &ui = ui, workdir = workdir, patch = patch, entry]
                {
                    // A folder of its own for the file patched, kept while
                    // the tool may read it and removed after.
                    auto scratch = std::make_shared<temp_dir>("review-patch");
                    patch_preview p;
                    if (scratch->path().empty())
                        p.applied.output = _("Could not make a folder in the temporary folder.");
                    else
                        p = preview_patch_entry(workdir, patch, entry, scratch->path());
                    ui.post(
                        [weak, scratch, r = p.applied, base = p.base, mine = p.mine]
                        {
                            auto s = weak.lock();
                            if (!s || !s->window) return;
                            if (!r.ok())
                                return show_message(
                                    s->ui, s->window.get(),
                                    {.icon = rap::message_spec::icon::error,
                                     .title = _("Review Patch"),
                                     .text = _("This file's part of the patch doesn't apply."),
                                     .detail = r.output});
                            s->tool =
                                compare_files(s->ui, s->window.get(), base, mine, {}, [scratch] {});
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
                           dialog_button{rap::dialog_role::none, _("Apply _selected")}
                               .bind(s->apply_selected)
                               .on_click(
                                   [s]
                                   {
                                       if (auto only = s->selected(); !only.empty())
                                       {
                                           s->apply(only);
                                           s->window->close();
                                       }
                                   }),
                           dialog_button{rap::dialog_role::ok, _("_Apply all")}.bind(s->apply_all),
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
            std::string text;
            if (std::FILE* f =
                    r.ok() ? rap::sys::fopen(rap::sys::path_from_utf8(patch), "rb") : nullptr)
            {
                char buf[16384];
                while (size_t n = std::fread(buf, 1, sizeof buf, f)) text.append(buf, n);
                std::fclose(f);
            }
            std::vector<patch_entry> entries =
                r.ok() ? patch_entries(r.output, text) : std::vector<patch_entry>{};
            ui.post(
                [weak, entries, error = r.ok() ? std::string() : r.output]
                {
                    auto s = weak.lock();
                    if (!s || !s->window) return;
                    if (!error.empty()) s->status->set_text(error);
                    s->rows.entries = entries;
                    s->rows.changed();
                });
            std::vector<char> applies;
            for (const patch_entry& e : entries)
                applies.push_back(run_git(workdir, apply_args(patch, {e.path}, true)).ok());
            ui.post(
                [weak, applies = std::move(applies)]
                {
                    auto s = weak.lock();
                    if (!s || !s->window) return;
                    s->rows.applies = applies;
                    s->rows.checked = true;
                    s->rows.changed();
                    // Nothing that applies: applying would only fail.
                    if (std::ranges::none_of(s->rows.applies, [](char a) { return a != 0; }))
                    {
                        s->apply_selected->set_enabled(false);
                        s->apply_all->set_enabled(false);
                        if (!s->rows.applies.empty())
                            s->status->set_text(
                                _("None of it applies to this working tree as it is."));
                    }
                });
        })
        .detach();
    st->window->show();
}

}  // namespace tortilla
