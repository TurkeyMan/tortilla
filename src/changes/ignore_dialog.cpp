#include "ignore_dialog.h"

#include "git/command.h"
#include "git/ignore_patterns.h"
#include "git/pathspec_file.h"
#include "git/remotes.h"
#include "git/repo.h"
#include "i18n.h"
#include "settings.h"
#include "ui/repo_icon.h"
#include "util/repo_path.h"

#include <rapport/sys.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <system_error>

namespace tortilla
{
namespace
{

namespace fs = std::filesystem;

struct state
{
    rap::ui& ui;
    std::unique_ptr<rap::window> window;
    std::string workdir;
    fs::path exclude_file;  // .git/info/exclude (the common one, for worktrees)
    std::vector<ignore_item> items;
    std::vector<std::string> relative;  // for git rm
    bool by_extension = false, untrack = false;
    rap::drop_down* scope = nullptr;  // 0: only where it is; 1: every folder
    rap::drop_down* target = nullptr;  // where
    rap::label* preview = nullptr;
    rap::check* stop_tracking = nullptr;  // shown when git tracks some of them
    std::function<void(bool)> done;
    std::shared_ptr<state> keep_alive;

    // Each ignore file (absolute) and the lines it gets.
    std::vector<std::pair<fs::path, std::vector<std::string>>> plan() const
    {
        const bool everywhere = scope && scope->current() == 1;
        const auto target_kind = target ? ignore_target(std::max(target->current(), 0)) : ignore_target::repository;
        std::vector<std::pair<fs::path, std::vector<std::string>>> out;
        const fs::path root = rap::sys::path_from_utf8(workdir);
        for (auto& [file, lines] : ignore_patterns(items, by_extension, everywhere, target_kind))
            out.push_back({file.empty() ? exclude_file : root / rap::sys::path_from_utf8(file), std::move(lines)});
        return out;
    }

    void update_preview()
    {
        if (!preview) return;
        std::string text;
        const fs::path root = rap::sys::path_from_utf8(workdir);
        for (const auto& [file, lines] : plan())
        {
            std::error_code ec;
            fs::path shown = fs::relative(file, root, ec);
            text += (text.empty() ? "" : "\n") + rap::sys::path_to_utf8(ec ? file : shown) + ":";
            for (const std::string& l : lines) text += "\n    " + l;
        }
        preview->set_text(text.empty() ? std::string(_("(nothing: no extension to go by)")) : text);
    }

    // Appends the lines each file doesn't already have.
    std::string write() const
    {
        for (const auto& [file, lines] : plan())
            if (std::string error = append_ignore_lines(file, lines); !error.empty()) return error;
        return {};
    }

    void finish(bool ok)
    {
        if (window) window->close();
        auto keep = std::move(keep_alive);
        if (done) std::exchange(done, {})(ok);
        ui.post([keep] {});  // destroyed after this callback returns
    }
    // Says what went wrong over the dialog, then ends it.
    void fail(const std::string& text, const std::string& detail)
    {
        ui.message(window.get(),
                   {.icon = rap::message_spec::icon::error, .title = "Tortilla", .text = text, .detail = detail},
                   [this](rap::dialog_role) { finish(false); });
    }
};

}  // namespace

void show_ignore_dialog(rap::ui& ui, std::vector<std::string> paths, bool by_extension, bool untrack,
                        std::function<void(bool)> done)
{
    auto fail_now = [&ui, done](const std::string& text)
    {
        ui.message(nullptr, {.icon = rap::message_spec::icon::error, .title = "Tortilla", .text = text},
                   [done](rap::dialog_role) { done(false); });
    };
    if (paths.empty()) return done(false);
    auto repo = repo::open(paths.front());
    if (!repo) return fail_now(repo.error());

    auto st = std::make_shared<state>(state{.ui = ui});
    st->keep_alive = st;
    st->workdir = repo->workdir();
    st->by_extension = by_extension;
    st->untrack = untrack;
    st->done = std::move(done);
    // git rev-parse --git-path info/exclude: the shared one in a linked
    // worktree's main repository.
    st->exclude_file = rap::sys::path_from_utf8(repo->common_dir()) / "info" / "exclude";
    for (const std::string& p : paths)
    {
        auto rel = repo_relative(st->workdir, p);
        if (!rel || rel->empty()) continue;  // outside, or the top itself
        std::string& r = *rel;
        std::error_code ec;
        const size_t slash = r.rfind('/');
        st->items.push_back(
            {slash == std::string::npos ? "" : r.substr(0, slash),
             slash == std::string::npos ? r : r.substr(slash + 1),
             fs::is_directory(fs::symlink_status(rap::sys::path_from_utf8(p), ec))});
        st->relative.push_back(std::move(r));
    }
    // Which of them git tracks: those it can stop tracking.
    // In the index, in process (the dialog waits); git when libgit2 can't
    // read it.
    std::optional<bool> tracked =
        st->relative.empty() ? std::optional(false) : any_tracked(st->workdir, st->relative);
    if (!tracked)
    {
        std::vector<std::string> ls{"ls-files", "--"};
        ls.insert(ls.end(), st->relative.begin(), st->relative.end());
        tracked = !run_git(st->workdir, literal_paths(ls), git_output::stdout_only).output.empty();
    }
    const bool some_tracked = *tracked;
    if (st->items.empty())
    {
        st->keep_alive.reset();
        return st->done(false);
    }

    // What was chosen last time; by extension it's usually meant everywhere.
    const std::string saved_scope = settings::get().value(by_extension ? "ignore.maskScope" : "ignore.nameScope");
    const int scope = saved_scope.empty() ? (by_extension ? 1 : 0) : std::clamp(std::atoi(saved_scope.c_str()), 0, 1);
    const int target = std::clamp(std::atoi(settings::get().value("ignore.where", "0").c_str()), 0, 2);

    using namespace rap::layout;
    std::weak_ptr<state> weak = st;
    auto changed = [weak](int)
    {
        if (auto s = weak.lock()) s->update_preview();
    };
    st->window = create_window(
        ui,
        dialog{_("Ignore - Tortilla"),
               {vbox{
                   form{
                       {_("Ignore"),
                        drop_down{by_extension
                                      ? std::vector<std::string>{_("Only in its folder"),
                                                                 _("In every folder")}
                                      : std::vector<std::string>{_("Only this item"),
                                                                 _("This name in every folder")},
                                  scope}
                            .on_changed(changed)
                            .bind(st->scope)},
                       {_("Write to"), drop_down{{_("The repository's .gitignore"),
                                                  _("The .gitignore in its folder"),
                                                  _("This clone only (.git/info/exclude)")},
                                                 target}
                                           .on_changed(changed)
                                           .bind(st->target)},
                   },
                   label{_("Adds:")},
                   label{}.selectable().bind(st->preview),
                   // TortoiseGit's "Delete and add to ignore list": the
                   // tracked ones would go on being tracked otherwise.
                   check{N_("Also stop tracking it (it stays on disk)",
                            "Also stop tracking them (they stay on disk)", st->items.size()),
                         untrack || some_tracked}
                       .visible(some_tracked)
                       .bind(st->stop_tracking),
                   buttons{ok, cancel},
               }}}
            .resizable(false),
        nullptr);
    st->update_preview();
    st->window->on_finished(
        [weak, by_extension](rap::dialog_role role)
        {
            auto s = weak.lock();
            if (!s) return;
            if (role != rap::dialog_role::ok) return s->finish(false);
            settings_changes choices;
            choices.set(by_extension ? "ignore.maskScope" : "ignore.nameScope",
                        std::to_string(s->scope->current()));
            choices.set("ignore.where", std::to_string(s->target->current()));
            settings::get().apply(choices);
            if (const std::string error = s->write(); !error.empty())
                return s->fail(_("Couldn't add to the ignore list."), error);
            if (s->stop_tracking && s->stop_tracking->checked())
            {
                // The paths through a file: no command-line limit.
                const auto list = pathspec_file::write(s->relative);
                if (!list)
                    return s->fail(_("git couldn't stop tracking them."),
                                   _("Could not write the list of files to the temporary folder."));
                std::vector<std::string> args{"rm", "-r", "-q", "--cached", "--ignore-unmatch"};
                for (std::string& a : list->arguments()) args.push_back(std::move(a));
                if (const git_result rm = run_git(s->workdir, literal_paths(args)); !rm.ok())
                    return s->fail(N_("git couldn't stop tracking it.", "git couldn't stop tracking them.",
                                      s->relative.size()),
                                   rm.output);
            }
            s->finish(true);
        });
    st->window->show();
}

}  // namespace tortilla
