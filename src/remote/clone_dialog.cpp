#include "clone_dialog.h"

#include "git/command.h"
#include "git/remotes.h"
#include "log/log_window.h"
#include "settings.h"
#include "ui/progress_dialog.h"
#include "ui/windows.h"

#include <rapport/sys.h>

#include <cmath>
#include <filesystem>

namespace tortilla
{
namespace
{

namespace fs = std::filesystem;

// The URLs cloned before, newest first (TortoiseGit's History\repoURLS).
std::vector<std::string> url_history()
{
    std::vector<std::string> out;
    const std::string s = settings::get().value("clone.urls");
    for (size_t at = 0; at < s.size();)
    {
        size_t end = s.find('\t', at);
        if (end == std::string::npos) end = s.size();
        if (end > at) out.push_back(s.substr(at, end - at));
        at = end + 1;
    }
    return out;
}

void remember_url(const std::string& url)
{
    std::vector<std::string> urls = url_history();
    std::erase(urls, url);
    urls.insert(urls.begin(), url);
    if (urls.size() > 25) urls.resize(25);
    std::string s;
    for (const std::string& u : urls) s += (s.empty() ? "" : "\t") + u;
    settings::get().set("clone.urls", s);
}

}  // namespace

std::unique_ptr<clone_dialog> clone_dialog::open(rap::ui& ui, const std::string& folder,
                                                 const std::string& url)
{
    std::unique_ptr<clone_dialog> d(new clone_dialog(ui));
    d->build(folder, url);
    return d;
}

clone_dialog::clone_dialog(rap::ui& ui)
    : ui_(ui)
{
}

void clone_dialog::build(const std::string& folder, const std::string& url)
{
    using namespace rap::layout;
    using namespace rap::literals;
    std::error_code ec;
    std::string dir = folder.empty() ? rap::sys::path_to_utf8(fs::current_path(ec)) : folder;
    dir = rap::sys::path_to_utf8(fs::absolute(rap::sys::path_from_utf8(dir), ec).make_preferred());
    const std::vector<std::string> history = url_history();
    const std::string first_url = !url.empty() ? url : history.empty() ? "" : history.front();

    window_ = ui_.create(dialog{
        "Git clone - Tortilla",
        {vbox{
            group{
                "Clone Existing Repository",
                vbox{
                    form{
                        {"URL", combo{history, first_url}.bind(url_).width(60_ch).on_changed(
                                    [this](const std::string& u) { url_changed(u); })},
                        {"Directory",
                         hbox{
                             entry{dir}.bind(directory_).stretch(),
                             button{"Bro_wse..."}.on_click(
                                 [this]
                                 {
                                     ui_.choose_files(window_.get(),
                                                      {.mode = rap::file_dialog_spec::mode::folder,
                                                       .title = "Clone into",
                                                       .initial_path = directory_->text()},
                                                      [this](std::vector<std::string> chosen)
                                                      {
                                                          if (chosen.empty()) return;
                                                          directory_->set_text(chosen.front());
                                                          module_.clear();
                                                          url_changed(url_->text());
                                                      });
                                 }),
                         }},
                    },
                    hbox{
                        check{"Depth"}.bind(depth_).on_toggled([this](bool) { update_state(); }),
                        spin{1, 1e9, 1}.bind(depth_value_),
                        check{"Recursive", settings::get().value("clone.recursive") == "1"}
                            .bind(recursive_)
                            .on_toggled([this](bool) { update_state(); }),
                        check{"Clone into Bare Repo"}.bind(bare_).on_toggled([this](bool)
                                                                             { update_state(); }),
                        check{"No Checkout"}
                            .bind(no_checkout_)
                            .on_toggled([this](bool) { update_state(); }),
                        spacer{},
                    },
                    hbox{
                        check{"Branch"}.bind(branch_).on_toggled([this](bool) { update_state(); }),
                        entry{}.bind(branch_name_).width(20_ch),
                        check{"Origin Name"}.bind(origin_).on_toggled([this](bool)
                                                                      { update_state(); }),
                        entry{}.bind(origin_name_).width(16_ch),
                        spacer{},
                    },
                }},
            buttons{dialog_button{rap::dialog_role::ok, ""}.on_click([this] { ok(); }), cancel},
        }}});
    window_->on_finished(
        [this](rap::dialog_role)
        {
            if (closed_) closed_();
        });
    if (!first_url.empty()) url_changed(first_url);
    update_state();
}

// The folder follows the URL: <directory>\<name>, replacing the name it
// last added [spec 7.1.2].
void clone_dialog::url_changed(const std::string& url)
{
    std::string dir = directory_->text();
    if (!module_.empty() && dir.size() > module_.size() &&
        dir.compare(dir.size() - module_.size(), module_.size(), module_) == 0)
        dir.resize(dir.size() - module_.size());
    while (!dir.empty() && (dir.back() == '/' || dir.back() == '\\')) dir.pop_back();
    module_ = clone_folder_name(url);
    if (module_.empty()) return;
#ifdef _WIN32
    const char sep = '\\';
#else
    const char sep = '/';
#endif
    module_ = sep + module_;
    directory_->set_text(dir + module_);
}

// Bare excludes Recursive, No Checkout and Origin, and they it [spec 7.1.2].
void clone_dialog::update_state()
{
    const bool bare = bare_->checked();
    recursive_->set_enabled(!bare);
    no_checkout_->set_enabled(!bare);
    origin_->set_enabled(!bare);
    bare_->set_enabled(!recursive_->checked() && !no_checkout_->checked() && !origin_->checked());
    depth_value_->set_enabled(depth_->checked());
    branch_name_->set_enabled(branch_->checked());
    origin_name_->set_enabled(origin_->checked() && !bare);
}

void clone_dialog::ok()
{
    const std::string url = url_->text(), dir = directory_->text();
    auto refuse = [this](const std::string& text)
    {
        ui_.message(window_.get(),
                    {.icon = rap::message_spec::icon::warning, .title = "Clone", .text = text});
    };
    if (url.empty() || dir.empty()) return refuse("URL and directory must not be empty.");
    if (branch_->checked() &&
        !run_git(".", {"check-ref-format", "--branch", branch_name_->text()}).ok())
        return refuse("“" + branch_name_->text() + "” isn't a valid branch name.");
    if (origin_->checked() && !bare_->checked() && origin_name_->text().empty())
        return refuse("Enter the origin's name.");
    remember_url(url);
    settings::get().set("clone.recursive", recursive_->checked() ? "1" : "0");

    git_command c{"clone", "--progress"};
    if (recursive_->checked() && !bare_->checked()) c.push_back("--recursive");
    if (bare_->checked()) c.push_back("--bare");
    if (no_checkout_->checked() && !bare_->checked()) c.push_back("--no-checkout");
    if (branch_->checked()) c.insert(c.end(), {"--branch", branch_name_->text()});
    if (origin_->checked() && !bare_->checked())
        c.insert(c.end(), {"--origin", origin_name_->text()});
    if (depth_->checked())
        c.push_back("--depth=" + std::to_string(int(std::lround(depth_value_->value()))));
    c.insert(c.end(), {"-v", "--", url, dir});

    // git runs in the nearest folder that exists [spec 7.1.3].
    std::error_code ec;
    fs::path where = rap::sys::path_from_utf8(dir).parent_path();
    while (!where.empty() && !fs::is_directory(where, ec) && where.has_relative_path())
        where = where.parent_path();
    if (where.empty() || !fs::is_directory(where, ec)) where = fs::temp_directory_path(ec);

    rap::ui& ui = ui_;
    std::vector<progress_follow_up> follow_ups{
        {.label = "Show _log",
         .after_close =
             [&ui, dir]
         {
             if (auto w = log_window::open(ui, dir)) windows::keep(std::move(*w));
         }},
        {.label = "E_xplore to",
         .after_close = [dir] { rap::sys::open_path(rap::sys::path_from_utf8(dir)); }},
        {.label = "_Retry",
         .commands = [c] { return std::vector<git_command>{c}; },
         .offered = [](bool ok) { return !ok; }},
    };
    const size_t running = windows::hold({});
    run_progress_dialog(
        ui_, nullptr, "Clone", rap::sys::path_to_utf8(where), {c},
        [running](bool) { windows::release(running); }, std::move(follow_ups));
    // close() only hides it: what closing means (its release) is ours.
    window_->close();
    if (closed_) closed_();
}

}  // namespace tortilla
