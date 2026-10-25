#include "trust_repository.h"

#include "git/command.h"
#include "i18n.h"

#include <rapport/sys.h>

#include <filesystem>
#include <system_error>

namespace tortilla
{

std::optional<std::string> untrusted_repository(const std::string& path)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path dir = rap::sys::path_from_utf8(path.empty() ? "." : path);
    if (!fs::is_directory(dir, ec)) dir = dir.parent_path();
    if (dir.empty() || !fs::is_directory(dir, ec)) return std::nullopt;
    // git's refusal names the setting to add, in the form it wants (on
    // Windows %(prefix)///server/share/... for a network path).
    const git_result r = run_git(rap::sys::path_to_utf8(dir), {"rev-parse", "--git-dir"});
    if (r.ok()) return std::nullopt;
    const std::string key = "--add safe.directory ";
    const size_t at = r.output.find(key);
    if (at == std::string::npos) return std::nullopt;
    size_t end = r.output.find('\n', at);
    std::string value = r.output.substr(at + key.size(), end == std::string::npos ? std::string::npos : end - at - key.size());
    while (!value.empty() && (value.back() == '\r' || value.back() == ' ')) value.pop_back();
    if (value.size() >= 2 && value.front() == '\'' && value.back() == '\'') value = value.substr(1, value.size() - 2);
    if (value.empty() || value == "*") return std::nullopt;
    return value;
}

void ensure_trusted(rap::ui& ui, const std::string& path, std::function<void(bool)> done)
{
    const std::optional<std::string> value = untrusted_repository(path);
    if (!value) return done(true);
    ui.message(
        nullptr,
        {.icon = rap::message_spec::icon::warning,
         .title = "Tortilla",
         .text = rap::i18n::format(_("Git won't open the repository in\n{0}\nbecause its folder belongs to another user "
                                     "account (a network share, say, or WSL's files seen from Windows).\n\nTrust "
                                     "this repository? Only do so if you trust whoever can change its files: "
                                     "its configuration can run programs as you."),
                                   *value),
         .detail = "git config --global --add safe.directory " + *value,
         .buttons = {rap::dialog_role::yes, rap::dialog_role::no}},
        [&ui, value = *value, done](rap::dialog_role role)
        {
            if (role != rap::dialog_role::yes) return done(false);
            const git_result r = run_git(".", {"config", "--global", "--add", "safe.directory", value});
            if (r.ok()) return done(true);
            ui.message(nullptr,
                       {.icon = rap::message_spec::icon::error,
                        .title = "Tortilla",
                        .text = _("Couldn't add the repository to safe.directory."),
                        .detail = r.output},
                       [done](rap::dialog_role) { done(false); });
        });
}

}  // namespace tortilla
