#include "changed_files_window.h"

#include "git/repo.h"
#include "i18n.h"
#include "settings.h"
#include "ui/progress_dialog.h"
#include "ui/repo_icon.h"
#include "ui/settings_dialog.h"

#include <rapport/sys.h>

namespace tortilla
{
namespace
{

// A full hash as 8 characters, as TortoiseGit shows them; a name as it is.
std::string short_rev(const std::string& rev)
{
    const bool hex =
        rev.size() == 40 && rev.find_first_not_of("0123456789abcdef") == std::string::npos;
    return hex ? rev.substr(0, 8) : rev;
}

}  // namespace

std::expected<std::unique_ptr<changed_files_window>, std::string>
changed_files_window::open(rap::ui& ui, const std::string& path, std::string from, std::string to)
{
    auto repo = repo::open(path.empty() ? "." : path);
    if (!repo) return std::unexpected(repo.error());
    std::string dir = repo->workdir().empty() ? repo->git_dir() : repo->workdir();
    std::unique_ptr<changed_files_window> w(
        new changed_files_window(ui, std::move(dir), std::move(from), std::move(to)));
    w->build();
    w->load();
    return w;
}

changed_files_window::changed_files_window(rap::ui& ui, std::string workdir, std::string from,
                                           std::string to)
    : ui_(ui)
    , workdir_(std::move(workdir))
    , from_(std::move(from))
    , to_(std::move(to))
    , files_({.ui = ui,
              .window = [this] { return window_.get(); },
              .workdir = workdir_,
              // Revert to this revision changes the working tree: the
              // progress dialog, then the files again.
              .run =
                  [this](const std::string& title, std::vector<std::vector<std::string>> commands)
              {
                  run_progress_dialog(ui_, window_.get(), title, workdir_, std::move(commands),
                                      [this](bool) { load(); });
              },
              .refresh = [this] { load(); },
              .open_settings = [this](settings_page page)
              { show_settings(ui_, window_.get(), workdir_, page); },
              .selection_changed = [this] { update_status(); }})
    , worker_(std::make_unique<repo_worker>(ui, workdir_))
{
    files_.set_source(
        to_.empty()
            ? changes_source{.what = changes_source::revision_to_working_tree, .parent = from_}
            : changes_source{.what = changes_source::revision, .hash = to_, .parent = from_});
}

changed_files_window::~changed_files_window() = default;

void changed_files_window::build()
{
    using namespace rap::layout;
    using namespace rap::literals;
    std::string root = workdir_;
    while (root.size() > 1 && (root.back() == '/' || root.back() == '\\')) root.pop_back();
    root = rap::sys::path_to_utf8(rap::sys::path_from_utf8(root).make_preferred());
    const std::string sides = short_rev(from_) + "  \xe2\x86\x92  " +
                              (to_.empty() ? std::string(_("Working tree")) : short_rev(to_));

    window_ =
        create_window(ui_, window{rap::i18n::format(_("{0} - Changed Files - Tortilla"), root),
                                  {vbox{
                                      label{sides},
                                      files_.list("changedFiles.files").stretch(),
                                      label{}.bind(status_).ellipsize(),
                                  }}}
                               .size(110_ch, 30_lines));
    settings::get().restore_window("changedFiles.window", *window_);
    files_.restore_layout();
    window_->on_close_request(
        [this]
        {
            settings::get().save_window("changedFiles.window", *window_);
            files_.save_layout();
            if (closed_) closed_();
            return true;
        });
    window_->on_key(
        [this](const rap::key_event& e)
        {
            if (!e.press || e.key != rap::key::f5) return false;
            load();
            return true;
        });
}

// The files on the worker, as the log loads a commit's: the list first,
// then its line counts; a newer load wins.
void changed_files_window::load()
{
    status_->set_text(_("Loading…"));
    files_.clear();
    const std::string from = from_, to = to_;
    worker_->submit(
        [this, from, to](repo& r, const repo_worker::progress& progress) -> std::function<void()>
        {
            auto done = r.changes_between(
                from, to,
                {.listed =
                     [&](const std::vector<file_change>& files)
                 {
                     progress.post(
                         [this, files]
                         {
                             files_.set_files(files);
                             update_status();
                         });
                 },
                 .counted = [&](size_t first, const std::vector<file_change>& files)
                 { progress.post([this, first, files] { files_.set_counts(first, files); }); },
                 .stop = [&] { return progress.stale(); }});
            if (done) return {};
            return [this, error = done.error()] { status_->set_text(error); };
        });
}

void changed_files_window::update_status()
{
    const size_t n = files_.files().size(), selected = files_.selection().size();
    status_->set_text(
        rap::i18n::format(N_("{0} file changed", "{0} files changed", n), std::to_string(n)) +
        (selected ? "  " + rap::i18n::format(_("({0} selected)"), std::to_string(selected)) : ""));
}

}  // namespace tortilla
