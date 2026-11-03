#include "git_install.h"

#include "i18n.h"
#include "util/programs.h"

#include <rapport/sys.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>

namespace tortilla
{
namespace
{

namespace fs = std::filesystem;

std::atomic<int> known{-1};  // -1: not looked for yet

bool look()
{
    const bool found = !find_program("git").empty();
    known = found ? 1 : 0;
    return found;
}

// A key's value in os-release text: KEY=value, KEY="value" or KEY='value'.
std::string_view os_release_value(std::string_view text, std::string_view key)
{
    while (!text.empty())
    {
        const size_t end = text.find('\n');
        std::string_view line = text.substr(0, end);
        text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.size() <= key.size() || !line.starts_with(key) || line[key.size()] != '=')
            continue;
        std::string_view value = line.substr(key.size() + 1);
        if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') &&
            value.back() == value.front())
            value = value.substr(1, value.size() - 2);
        return value;
    }
    return {};
}

}  // namespace

bool git_installed()
{
    const int k = known;
    return k < 0 ? look() : k == 1;
}

bool recheck_git()
{
    return look();
}

std::string git_missing_reason()
{
    return _("git isn't installed");
}

std::string find_git_program(const std::string& name)
{
    if (std::string on_path = find_program(name); !on_path.empty()) return on_path;
    const std::string git = find_program("git");
    if (git.empty()) return {};
    std::error_code ec;
    fs::path at = fs::canonical(rap::sys::path_from_utf8(git), ec);
    if (ec) return {};
    const fs::path bin = at.parent_path(), prefix = bin.parent_path();
#ifdef _WIN32
    const std::string file = name + ".exe";
#else
    const std::string& file = name;
#endif
    for (const fs::path& dir : {prefix / "libexec/git-core", prefix / "lib/git-core",
                                prefix / "mingw64/libexec/git-core", prefix / "mingw64/bin", bin})
        if (fs::is_regular_file(dir / rap::sys::path_from_utf8(file), ec))
            return rap::sys::path_to_utf8(dir / rap::sys::path_from_utf8(file));
    return {};
}

git_install_help linux_git_install_help(std::string_view os_release)
{
    struct distro
    {
        std::string_view id, command;
    };
    static constexpr distro k_distros[] = {
        {"debian", "sudo apt install git"},  {"ubuntu", "sudo apt install git"},
        {"fedora", "sudo dnf install git"},  {"rhel", "sudo dnf install git"},
        {"centos", "sudo dnf install git"},  {"arch", "sudo pacman -S git"},
        {"manjaro", "sudo pacman -S git"},   {"opensuse", "sudo zypper install git"},
        {"suse", "sudo zypper install git"}, {"alpine", "sudo apk add git"},
        {"void", "sudo xbps-install git"},   {"gentoo", "sudo emerge dev-vcs/git"},
    };
    const std::string text = _("Install it with your distribution's package manager:");
    auto match = [&](std::string_view id) -> std::optional<git_install_help>
    {
        if (id == "nixos")
            return git_install_help{
                .text = _("Add git to environment.systemPackages in /etc/nixos/configuration.nix, "
                          "then run “sudo nixos-rebuild switch”.")};
        // opensuse-leap, opensuse-tumbleweed: opensuse.
        for (const distro& d : k_distros)
            if (id == d.id ||
                (id.starts_with(d.id) && id.size() > d.id.size() && id[d.id.size()] == '-'))
            {
                std::string command(d.command);
                // CentOS and RHEL before 8 have yum, not dnf.
                if (d.id == "rhel" || d.id == "centos")
                {
                    const std::string_view version = os_release_value(os_release, "VERSION_ID");
                    if (!version.empty() && version.front() >= '1' && version.front() <= '7' &&
                        (version.size() == 1 || version[1] == '.'))
                        command = "sudo yum install git";
                }
                return git_install_help{.text = text, .command = std::move(command)};
            }
        return std::nullopt;
    };
    if (auto h = match(os_release_value(os_release, "ID"))) return *h;
    for (std::string_view like = os_release_value(os_release, "ID_LIKE"); !like.empty();)
    {
        const size_t space = like.find(' ');
        if (auto h = match(like.substr(0, space))) return *h;
        like.remove_prefix(space == std::string_view::npos ? like.size() : space + 1);
    }
    return {.text = _("Install the “git” package with your distribution's package manager (apt, "
                      "dnf, pacman, zypper...).")};
}

git_install_help git_install_help_here()
{
#if defined(_WIN32)
    return {.text = _("Install Git for Windows from git-scm.com, or with winget:"),
            .command = "winget install --id Git.Git",
            .url = "https://git-scm.com/download/win"};
#elif defined(__APPLE__)
    return {.text = _("Install the Xcode command line tools (or, with Homebrew, “brew install "
                      "git”):"),
            .command = "xcode-select --install"};
#else
    std::string text;
    for (const char* file : {"/etc/os-release", "/usr/lib/os-release"})
    {
        std::ifstream in(file, std::ios::binary);
        if (!in) continue;
        text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        break;
    }
    return linux_git_install_help(text);
#endif
}

}  // namespace tortilla
