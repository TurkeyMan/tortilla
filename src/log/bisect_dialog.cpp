#include "bisect_dialog.h"

#include "git/repo.h"
#include "i18n.h"
#include "ui/progress_dialog.h"
#include "ui/repo_icon.h"
#include "ui/windows.h"

#include <rapport/sys.h>

namespace tortilla
{
namespace
{

// The repository's folder, as the system writes it, for titles.
std::string root_of(std::string workdir)
{
    while (workdir.size() > 1 && (workdir.back() == '/' || workdir.back() == '\\'))
        workdir.pop_back();
    return rap::sys::path_to_utf8(rap::sys::path_from_utf8(workdir).make_preferred());
}

}  // namespace

void run_bisect(rap::ui& ui, rap::window* parent, const std::string& workdir, bisect_step step,
                std::vector<std::string> revs, std::function<void(bool ok)> done)
{
    run_progress_dialog(ui, parent, rap::i18n::format(_("{0} - Bisect"), root_of(workdir)), workdir,
                        {bisect_args(step, revs)}, std::move(done));
}

std::expected<std::unique_ptr<bisect_start_dialog>, std::string>
bisect_start_dialog::open(rap::ui& ui, const std::string& path, const std::string& good,
                          const std::string& bad)
{
    auto repo = repo::open(path.empty() ? "." : path);
    if (!repo) return std::unexpected(repo.error());
    if (repo->bare()) return std::unexpected(std::string(_("This needs a working tree.")));
    std::unique_ptr<bisect_start_dialog> d(new bisect_start_dialog(ui, repo->workdir()));
    d->build(good, bad);
    return d;
}

bisect_start_dialog::bisect_start_dialog(rap::ui& ui, std::string workdir)
    : ui_(ui)
    , workdir_(std::move(workdir))
    , good_(workdir_)
    , bad_(workdir_)
{
}

void bisect_start_dialog::build(const std::string& good, const std::string& bad)
{
    using namespace rap::layout;
    using namespace rap::literals;
    auto window = [this] { return window_.get(); };
    window_ = create_window(
        ui_,
        dialog{
            rap::i18n::format(_("{0} - Bisect start - Tortilla"), root_of(workdir_)),
            {vbox{
                good_.layout({.caption = _("Good"), .head = true, .ui = &ui_, .window = window}),
                bad_.layout({.caption = _("Bad"), .head = true, .ui = &ui_, .window = window}),
                buttons{dialog_button{rap::dialog_role::ok, ""}.on_click([this] { ok(); }), cancel},
            }}}
            .resizable(false));
    window_->on_finished(
        [this](rap::dialog_role)
        {
            if (closed_) closed_();
        });
    // Usually what's checked out is what's broken; its parent is a guess
    // at the last that worked, for the user to correct.
    good_.select(good.empty() ? "HEAD~1" : good);
    bad_.select(bad);
}

void bisect_start_dialog::ok()
{
    const std::string good = good_.revision(), bad = bad_.revision();
    if (good.empty() || bad.empty())
    {
        ui_.message(window_.get(), {.icon = rap::message_spec::icon::warning,
                                    .title = _("Bisect start"),
                                    .text = _("Both the good and the bad revision are needed.")});
        return;
    }
    const size_t running = windows::hold({});
    run_bisect(ui_, nullptr, workdir_, bisect_step::start, {bad, good},
               [done = done_, running](bool)
               {
                   if (done) done();
                   windows::release(running);
               });
    // close() only hides it: what closing means (its release) is ours.
    window_->close();
    if (closed_) closed_();
}

}  // namespace tortilla
