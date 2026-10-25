#include "rename_dialog.h"

#include "git/command.h"
#include "git/repo.h"
#include "i18n.h"

#include <rapport/sys.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <memory>
#include <system_error>

namespace tortilla
{
namespace
{

namespace fs = std::filesystem;

std::string lower(std::string s)
{
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

std::string trimmed(std::string s)
{
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) ++start;
    return s.substr(start);
}

// A name for a path under the folder: relative, staying inside the
// repository, and with none of the characters Windows forbids.
bool valid_name(const std::string& name)
{
    if (name.empty() || name.front() == '/' || name.front() == '\\' || name.find(':') != std::string::npos)
        return false;
    for (char c : name)
        if (c == '<' || c == '>' || c == '"' || c == '|' || c == '?' || c == '*' || static_cast<unsigned char>(c) < 32)
            return false;
    const fs::path p = rap::sys::path_from_utf8(name).lexically_normal();
    return !p.empty() && *p.begin() != "..";
}

struct state
{
    rap::ui& ui;
    std::string workdir;
    std::string folder;  // repository-relative, "" at the top
    std::string name;
    std::function<void(bool)> done;
    std::unique_ptr<rap::window> window;
    rap::entry* field = nullptr;

    std::string rel(const std::string& n) const { return folder.empty() ? n : folder + "/" + n; }

    // Once: closing the window comes back here (on_finished), and finds
    // nothing left to do.
    void finish(bool renamed)
    {
        auto callback = std::exchange(done, {});
        auto closing = std::shared_ptr<rap::window>(std::move(window));
        if (closing) closing->close();
        ui.post([closing] {});  // the window goes after this returns
        if (callback) callback(renamed);
    }
    void complain(const std::string& text)
    {
        ui.message(window.get(), {.icon = rap::message_spec::icon::error, .title = "Tortilla", .text = text},
                   [](rap::dialog_role) {});
    }

    void ok()
    {
        std::string to = trimmed(field->text());
        std::ranges::replace(to, '\\', '/');
        if (to.empty() || to == name) return complain(_("New name must not be empty or the same as the original name!"));
        if (!valid_name(to))
            return complain(_("That isn't a valid name: it can't leave the repository or contain < > : \" | ? *."));
        const std::string from_rel = rel(name), to_rel = rel(to);
        // Only the letters' case changed: the same file to the filesystem,
        // which git mv is told with -f.
        const bool case_only = lower(from_rel) == lower(to_rel);
        std::error_code ec;
        if (!case_only && fs::exists(rap::sys::path_from_utf8(workdir) / rap::sys::path_from_utf8(to_rel), ec))
            return complain(rap::i18n::format(_("“{0}” already exists."), to));
        std::vector<std::string> args{"mv"};
        if (case_only) args.push_back("-f");
        args.insert(args.end(), {"--", from_rel, to_rel});
        const git_result r = run_git(workdir, args);
        if (!r.ok())
        {
            ui.message(window.get(),
                       {.icon = rap::message_spec::icon::error,
                        .title = "Tortilla",
                        .text = _("git couldn't rename it."),
                        .detail = r.output},
                       [](rap::dialog_role) {});
            return;
        }
        finish(true);
    }
};

}  // namespace

void rename_item(rap::ui& ui, const std::string& path, std::function<void(bool)> done)
{
    auto fail = [&ui, done](const std::string& text)
    {
        ui.message(nullptr, {.icon = rap::message_spec::icon::error, .title = "Tortilla", .text = text},
                   [done](rap::dialog_role) { done(false); });
    };
    auto repo = repo::open(path);
    if (!repo) return fail(repo.error());
    if (repo->bare()) return fail(_("This needs a working tree."));
    const fs::path root = rap::sys::path_from_utf8(repo->workdir());
    std::error_code ec;
    const fs::path abs = fs::absolute(rap::sys::path_from_utf8(path), ec);
    fs::path rel = fs::relative(abs, root, ec);
    if (ec || rel.empty() || rel == "." || *rel.begin() == "..") return fail(_("Only something inside the repository can be renamed."));
    std::string r = rap::sys::path_to_utf8(rel);
    std::ranges::replace(r, '\\', '/');
    while (!r.empty() && r.back() == '/') r.pop_back();
    const size_t slash = r.rfind('/');

    auto st = std::make_shared<state>(state{.ui = ui,
                                            .workdir = repo->workdir(),
                                            .folder = slash == std::string::npos ? "" : r.substr(0, slash),
                                            .name = slash == std::string::npos ? r : r.substr(slash + 1),
                                            .done = std::move(done)});
    using namespace rap::layout;
    using namespace rap::literals;
    std::weak_ptr<state> weak = st;
    st->window = ui.create(
        dialog{_("Rename - Tortilla"),
               {vbox{
                   label{rap::i18n::format(_("Rename “{0}”:"), st->name)},
                   form{{_("New name:"), entry{st->name}
                                              .bind(st->field)
                                              .on_activate(
                                                  [weak]
                                                  {
                                                      if (auto s = weak.lock()) s->ok();
                                                  })}},
                   buttons{dialog_button{rap::dialog_role::ok, ""}.on_click(
                               [weak]
                               {
                                   if (auto s = weak.lock()) s->ok();
                               }),
                           cancel},
               }}}
            .size(50_ch, {}),
        nullptr);
    st->window->on_finished(
        [st](rap::dialog_role role)
        {
            if (role != rap::dialog_role::ok) st->finish(false);
        });
    st->window->show();
    st->field->focus();
}

}  // namespace tortilla
