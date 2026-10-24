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
    std::vector<progress_follow_up> follow_ups;
    std::function<void()> after_close;  // the follow-up chosen

    std::unique_ptr<rap::window> window;
    rap::text_view* output = nullptr;
    rap::progress_bar* bar = nullptr;
    rap::button* close = nullptr;
    rap::button* abort = nullptr;
    rap::menu_button* more = nullptr;
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
        s.fg = ok ? rap::paint{rap::theme_color::positive} : rap::paint{rap::theme_color::negative};
        output->append(std::string("\n") + (ok ? "Success" : "Failed") + " (" + std::to_string(ms) +
                           " ms @ " + when + ")\n",
                       s);
        close->set_enabled(true);
        abort->set_enabled(false);
        if (more) more->set_visible(ok);
        close->focus();
    }

    rap::menu follow_up_menu()
    {
        rap::menu m;
        for (size_t i = 0; i < follow_ups.size(); ++i)
            m.push_back({.label = follow_ups[i].label, .action = [this, i] { follow(i); }});
        return m;
    }
    void follow(size_t i)
    {
        const progress_follow_up& f = follow_ups[i];
        if (f.after_close)
        {
            after_close = f.after_close;
            window->close();
            return;
        }
        // More commands, here.
        more->set_visible(false);
        close->set_enabled(false);
        abort->set_enabled(true);
        output->append("\n");
        for (git_command& c : f.commands()) commands.push_back(std::move(c));
        bar->set_fraction(0);
        pulse = ui.timer(100, true, [this] { bar->pulse(); });
        started = std::chrono::steady_clock::now();
        run_next();
    }
};

}  // namespace

void run_progress_dialog(rap::ui& ui, rap::window* parent, const std::string& title,
                         const std::string& dir, std::vector<git_command> commands,
                         std::function<void(bool)> done, std::vector<progress_follow_up> follow_ups)
{
    auto p = std::make_shared<progress>(ui);
    p->dir = dir;
    p->commands = std::move(commands);
    p->done = std::move(done);
    p->follow_ups = std::move(follow_ups);

    using namespace rap::layout;
    rap::nodes body;
    body.push_back(text_view{}.read_only().monospace().wrap(false).bind(p->output).stretch());
    body.push_back(progress_bar{}.bind(p->bar));
    // The follow-ups, under the output (shown after success).
    if (!p->follow_ups.empty())
        body.push_back(
            hbox{menu_button{p->follow_ups.front().label}.bind(p->more).visible(false).on_menu(
                     [raw = p.get()] { return raw->follow_up_menu(); }),
                 spacer{}});
    body.push_back(buttons{
        // Qualified: POSIX close() is in scope through <unistd.h>.
        rap::layout::close().bind(p->close),
        dialog_button{rap::dialog_role::none, "_Abort"}.bind(p->abort).on_click(
            [raw = p.get()]
            {
                if (raw->process) raw->process->kill();
            }),
    });

    p->window = ui.create(
        dialog{title + " - Tortilla", {rap::node{rap::spec::box{vertical, std::move(body)}, {}}}}
            .size(90_ch, 24_lines),
        parent);

    p->close->set_enabled(false);
    // The dialog keeps itself alive until it is closed.
    p->window->on_finished(
        [p](rap::dialog_role) mutable
        {
            auto keep = std::move(p);
            if (keep->process) keep->process->kill();
            if (keep->after_close) keep->after_close();
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
