#include "git_missing.h"

#include "git/git_install.h"
#include "i18n.h"
#include "settings.h"

#include <rapport/sys.h>

#include <charconv>
#include <chrono>
#include <string>

namespace tortilla
{

void show_git_missing(rap::ui& ui, rap::window* parent, std::function<void()> done)
{
    const git_install_help help = git_install_help_here();
    std::string text =
        _("Tortilla couldn't find git. It can still show your repositories read-only: the "
          "log, each commit's changes and diffs, blame, the repository browser and the "
          "references. Committing, fetching, pulling, pushing, merging, rebasing, stashing and "
          "switching branches need git, and stay greyed until it's installed.") +
        std::string("\n\n") + help.text;
    if (!help.command.empty()) text += "\n" + help.command;
    // A command to copy, or a page to open: the question's Yes does it.
    const bool open = !help.url.empty(), copy = !open && !help.command.empty();
    if (open) text += "\n\n" + std::string(_("Open the download page?"));
    if (copy) text += "\n\n" + std::string(_("Copy the command to the clipboard?"));
    ui.message(parent,
               {.icon = rap::message_spec::icon::warning,
                .title = _("git isn't installed"),
                .text = std::move(text),
                .detail = help.command,
                .buttons = open || copy ? std::vector<rap::dialog_role>{rap::dialog_role::yes,
                                                                        rap::dialog_role::no}
                                        : std::vector<rap::dialog_role>{rap::dialog_role::ok}},
               [&ui, help, open, copy, done = std::move(done)](rap::dialog_role role)
               {
                   if (role == rap::dialog_role::yes && open) rap::sys::open_url(help.url);
                   if (role == rap::dialog_role::yes && copy) ui.set_clipboard(help.command);
                   if (done) done();
               });
}

void notice_if_git_missing(rap::ui& ui, rap::window* parent)
{
    if (git_installed()) return;
    // Once a session: each window is a process of its own, so the last
    // showing is kept in the settings.
    using namespace std::chrono;
    const auto now = duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
    const std::string shown = settings::get().value("git.missing_noticed");
    long long last = 0;
    std::from_chars(shown.data(), shown.data() + shown.size(), last);
    if (now - last < 8 * 60 * 60) return;
    settings::get().set("git.missing_noticed", std::to_string(now));
    show_git_missing(ui, parent);
}

}  // namespace tortilla
