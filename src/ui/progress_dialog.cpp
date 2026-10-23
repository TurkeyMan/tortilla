#include "progress_dialog.h"

#include "git/command.h"

#include <chrono>
#include <ctime>

namespace tortilla
{
namespace
{

struct progress : std::enable_shared_from_this<progress>
{
    rap::ui& ui;
    std::string dir;
    std::vector<git_command> commands;
    std::function<void(bool)> done;

    std::unique_ptr<rap::window> window;
    rap::text_view* output = nullptr;
    rap::progress_bar* bar = nullptr;
    rap::button* close = nullptr;
    rap::button* abort = nullptr;
    std::shared_ptr<git_process> process;
    rap::subscription pulse;
    size_t next = 0;
    bool ok = true;
    std::chrono::steady_clock::time_point started;

    progress(rap::ui& u)
        : ui(u)
    {
    }

    void run_next()
    {
        if (next == commands.size() || !ok)
        {
            finish();
            return;
        }
        const git_command& args = commands[next++];
        output->append(command_line(args) + "\n", {.bold = true});
        auto self = shared_from_this();
        process = git_process::start(
            ui, dir, args, [self](const std::string& chunk) { self->output->append(chunk); },
            [self](int code)
            {
                if (code != 0) self->ok = false;
                self->run_next();
            });
    }

    void finish()
    {
        pulse.reset();
        process.reset();
        bar->set_fraction(1);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - started)
                      .count();
        std::time_t now = std::time(nullptr);
        char when[32];
        std::strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", std::localtime(&now));
        rap::text_style s{.bold = true};
        s.fg = ok ? rap::paint{rap::theme_color::positive}
                  : rap::paint{rap::theme_color::negative};
        output->append(std::string("\n") + (ok ? "Success" : "Failed") + " (" + std::to_string(ms) +
                           " ms @ " + when + ")\n",
                       s);
        close->set_enabled(true);
        abort->set_enabled(false);
        close->focus();
    }
};

}  // namespace

void run_progress_dialog(rap::ui& ui, rap::window* parent, const std::string& title,
                       const std::string& dir, std::vector<git_command> commands,
                       std::function<void(bool)> done)
{
    auto p = std::make_shared<progress>(ui);
    p->dir = dir;
    p->commands = std::move(commands);
    p->done = std::move(done);

    using namespace rap::layout;
    p->window = ui.create(
        dialog{title + " - Tortilla",
               {vbox{
                   text_view{}.read_only().monospace().wrap(false).bind(p->output).stretch(),
                   progress_bar{}.bind(p->bar),
                   buttons{
                       // Qualified: POSIX close() is in scope through <unistd.h>.
                       rap::layout::close().bind(p->close),
                       dialog_button{rap::dialog_role::none, "_Abort"}.bind(p->abort).on_click(
                           [raw = p.get()]
                           {
                               if (raw->process) raw->process->kill();
                           }),
                   },
               }}}
            .size(90_ch, 24_lines),
        parent);

    p->close->set_enabled(false);
    // The dialog keeps itself alive until it is closed.
    p->window->on_finished(
        [p](rap::dialog_role) mutable
        {
            auto keep = std::move(p);
            if (keep->process) keep->process->kill();
            if (keep->done) keep->done(keep->ok);
            keep->ui.post([keep] {});  // destroy after this callback returns
        });
    p->window->on_close_request([raw = p.get()] { return !raw->process; });
    p->window->show();
    p->started = std::chrono::steady_clock::now();
    p->pulse = ui.timer(100, true, [raw = p.get()] { raw->bar->pulse(); });
    p->run_next();
}

}  // namespace tortilla
