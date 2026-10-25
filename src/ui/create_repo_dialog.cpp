#include "create_repo_dialog.h"

#include "git/command.h"
#include "i18n.h"

#include <rapport/sys.h>

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <system_error>
#include <vector>

namespace tortilla
{
namespace
{

namespace fs = std::filesystem;

bool same_path(const fs::path& a, const fs::path& b)
{
    std::error_code ec;
    if (fs::exists(a, ec) && fs::exists(b, ec)) return fs::equivalent(a, b, ec);
    return a.lexically_normal() == b.lexically_normal();
}

// A drive or filesystem root, the home folder, or a system folder: not
// meant to be a repository's root [CreateRepositoryCommand CheckSpecialFolder].
bool special_folder(const fs::path& folder)
{
    const fs::path p = fs::absolute(folder).lexically_normal();
    if (p == p.root_path() || p.relative_path().empty()) return true;
    std::vector<fs::path> special;
    auto env = [&](const char* name, const char* sub = nullptr)
    {
        if (const char* v = std::getenv(name); v && *v)
            special.push_back(sub ? rap::sys::path_from_utf8(v) / sub : rap::sys::path_from_utf8(v));
    };
#ifdef _WIN32
    env("USERPROFILE");
    env("USERPROFILE", "Desktop");
    env("USERPROFILE", "Documents");
    env("SystemRoot");
    env("SystemRoot", "System32");
    env("SystemRoot", "SysWOW64");
    env("ProgramFiles");
    env("ProgramFiles(x86)");
#else
    env("HOME");
    env("HOME", "Desktop");
    env("HOME", "Documents");
#endif
    for (const fs::path& s : special)
        if (same_path(p, s)) return true;
    return false;
}

bool empty_folder(const fs::path& folder)
{
    std::error_code ec;
    return !fs::is_directory(folder, ec) || fs::is_empty(folder, ec);
}

struct state
{
    rap::ui& ui;
    std::string folder;
    std::function<void(bool)> done;
    std::unique_ptr<rap::window> window;
    rap::check* bare = nullptr;

    void finish(bool made)
    {
        ui.post([keep = std::shared_ptr<rap::window>(std::move(window))] {});  // the window goes after this returns
        if (done) std::exchange(done, {})(made);
    }

    // git init [--bare] <folder>, creating it if need be.
    void init(bool bare_repo, std::shared_ptr<state> self)
    {
        const fs::path where = rap::sys::path_from_utf8(folder);
        std::error_code ec;
        fs::create_directories(where, ec);
        std::vector<std::string> args{"init"};
        if (bare_repo) args.push_back("--bare");
        args.push_back("--");
        args.push_back(folder);
        const git_result r = run_git(folder, args);
        if (!r.ok())
        {
            ui.message(nullptr,
                       {.icon = rap::message_spec::icon::error,
                        .title = "Tortilla",
                        .text = _("Could not initialize a new repository."),
                        .detail = r.output},
                       [self](rap::dialog_role) { self->finish(false); });
            return;
        }
        ui.message(nullptr,
                   {.icon = rap::message_spec::icon::info,
                    .title = "Tortilla",
                    .text = rap::i18n::format(_("Initialized empty Git repository in {0}."), folder)},
                   [self](rap::dialog_role) { self->finish(true); });
    }

    void show(std::shared_ptr<state> self)
    {
        using namespace rap::layout;
        using namespace rap::literals;
        std::string shown = rap::sys::path_to_utf8(rap::sys::path_from_utf8(folder).make_preferred());
        // A folder named *.git is meant to be bare, by convention.
        const bool dot_git = folder.ends_with(".git") || folder.ends_with(".git/") || folder.ends_with(".git\\");
        window = ui.create(
            dialog{rap::i18n::format(_("{0} - Git Init - Tortilla"), shown),
                   {vbox{
                       check{_("Make it _Bare (No working directories)"), dot_git}.bind(bare),
                       // Broken by hand: rapport's labels don't wrap.
                       label{_("If you plan to work inside this folder, leave this unchecked.\nTypically a bare "
                               "repo can only have changes 'pushed' to it.\n(By convention, a bare repo folder "
                               "should have a name that ends with '.git')")},
                       buttons{ok, cancel},
                   }}}
                .size(60_ch, {}),
            nullptr);
        window->on_finished(
            [self](rap::dialog_role role)
            {
                if (role != rap::dialog_role::ok) return self->finish(false);
                const bool bare_repo = self->bare->checked();
                // Bare in a folder with things in it: sure? [CreateRepositoryCommand]
                if (bare_repo && !empty_folder(rap::sys::path_from_utf8(self->folder)))
                {
                    self->ui.message(
                        nullptr,
                        {.icon = rap::message_spec::icon::warning,
                         .title = "Tortilla",
                         .text = rap::i18n::format(_("The target folder\n{0}\nis not empty!\nAre you sure you want "
                                                     "to initialize a git repository inside that folder?"),
                                                   self->folder),
                         .buttons = {rap::dialog_role::yes, rap::dialog_role::no}},
                        [self](rap::dialog_role r)
                        {
                            if (r == rap::dialog_role::yes)
                                self->init(true, self);
                            else
                                self->finish(false);
                        });
                    return;
                }
                self->init(bare_repo, self);
            });
        window->show();
    }
};

}  // namespace

void create_repository(rap::ui& ui, const std::string& folder, std::function<void(bool)> done)
{
    auto st = std::make_shared<state>(state{.ui = ui, .folder = folder, .done = std::move(done)});
    if (!special_folder(rap::sys::path_from_utf8(folder))) return st->show(st);
    ui.message(nullptr,
               {.icon = rap::message_spec::icon::warning,
                .title = "Tortilla",
                .text = rap::i18n::format(_("The target folder\n{0}\nis a special folder and is not supposed to be "
                                            "a repository root!\nAre you sure you want to initialize a git "
                                            "repository inside that folder?"),
                                          folder),
                .buttons = {rap::dialog_role::yes, rap::dialog_role::no}},
               [st](rap::dialog_role r)
               {
                   if (r == rap::dialog_role::yes)
                       st->show(st);
                   else
                       st->finish(false);
               });
}

}  // namespace tortilla
