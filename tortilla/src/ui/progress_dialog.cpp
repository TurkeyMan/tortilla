#include "progress_dialog.h"

#include "git/command.h"

#include <chrono>
#include <ctime>

namespace tortilla
{
namespace
{

struct Progress : std::enable_shared_from_this<Progress>
{
    rap::Ui& ui;
    std::string dir;
    std::vector<GitArgs> commands;
    std::function<void(bool)> done;

    std::unique_ptr<rap::Window> window;
    rap::TextView* output = nullptr;
    rap::ProgressBar* bar = nullptr;
    rap::Button* close = nullptr;
    rap::Button* abort = nullptr;
    std::shared_ptr<GitProcess> process;
    rap::Subscription pulse;
    size_t next = 0;
    bool ok = true;
    std::chrono::steady_clock::time_point started;

    Progress(rap::Ui& u)
        : ui(u)
    {
    }

    void runNext()
    {
        if (next == commands.size() || !ok)
        {
            finish();
            return;
        }
        const GitArgs& args = commands[next++];
        output->append(commandLine(args) + "\n", {.bold = true});
        auto self = shared_from_this();
        process = GitProcess::start(
            ui, dir, args, [self](const std::string& chunk) { self->output->append(chunk); },
            [self](int code)
            {
                if (code != 0) self->ok = false;
                self->runNext();
            });
    }

    void finish()
    {
        pulse.reset();
        process.reset();
        bar->setFraction(1);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - started)
                      .count();
        std::time_t now = std::time(nullptr);
        char when[32];
        std::strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", std::localtime(&now));
        rap::TextStyle s{.bold = true};
        s.fg = ok ? rap::Paint{rap::ThemeColor::Positive}
                  : rap::Paint{rap::ThemeColor::Negative};
        output->append(std::string("\n") + (ok ? "Success" : "Failed") + " (" + std::to_string(ms) +
                           " ms @ " + when + ")\n",
                       s);
        close->setEnabled(true);
        abort->setEnabled(false);
        close->focus();
    }
};

}  // namespace

void runProgressDialog(rap::Ui& ui, rap::Window* parent, const std::string& title,
                       const std::string& dir, std::vector<GitArgs> commands,
                       std::function<void(bool)> done)
{
    auto p = std::make_shared<Progress>(ui);
    p->dir = dir;
    p->commands = std::move(commands);
    p->done = std::move(done);

    using namespace rap::layout;
    p->window = ui.create(
        Dialog{title + " - Tortilla",
               {VBox{
                   TextView{}.readOnly().monospace().wrap(false).bind(p->output).stretch(),
                   ProgressBar{}.bind(p->bar),
                   Buttons{
                       Close().bind(p->close),
                       DialogButton{rap::DialogRole::None, "_Abort"}.bind(p->abort).onClick(
                           [raw = p.get()]
                           {
                               if (raw->process) raw->process->kill();
                           }),
                   },
               }}}
            .size(90_ch, 24_lines),
        parent);

    p->close->setEnabled(false);
    // The dialog keeps itself alive until it is closed.
    p->window->onFinished(
        [p](rap::DialogRole) mutable
        {
            auto keep = std::move(p);
            if (keep->process) keep->process->kill();
            if (keep->done) keep->done(keep->ok);
            keep->ui.post([keep] {});  // destroy after this callback returns
        });
    p->window->onCloseRequest([raw = p.get()] { return !raw->process; });
    p->window->show();
    p->started = std::chrono::steady_clock::now();
    p->pulse = ui.timer(100, true, [raw = p.get()] { raw->bar->pulse(); });
    p->runNext();
}

}  // namespace tortilla
