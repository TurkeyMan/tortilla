#include "ssh_client.h"

#include "git/command.h"
#include "i18n.h"
#include "settings.h"
#include "util/programs.h"

#include <rapport/sys.h>

#include <filesystem>
#include <mutex>
#include <optional>
#include <system_error>

namespace fs = std::filesystem;

namespace tortilla
{
namespace
{

using environment = std::vector<std::pair<std::string, std::string>>;

// The saved client's environment, for git started on any thread; settings
// are read on the UI thread only (on first use, which main makes early, and
// on saving).
std::mutex g_mutex;
std::optional<environment> g_environment;

environment environment_for(const ssh_client& c)
{
    if (c.command.empty()) return {};
    environment env{{"GIT_SSH_COMMAND", c.command}};
    if (!c.variant.empty()) env.emplace_back("GIT_SSH_VARIANT", c.variant);
    return env;
}

// A program for a command line run by git's sh: / for \, and quoted if it
// has spaces ("C:/Program Files/...").
std::string program_word(const std::string& path)
{
#ifdef _WIN32
    const std::string p = forward_slashes(path);
#else
    const std::string& p = path;
#endif
    return p.find(' ') != std::string::npos ? "\"" + p + "\"" : p;
}

}  // namespace

ssh_client saved_ssh_client()
{
    const settings& s = settings::get();
    return {.command = s.value("ssh.command"), .variant = s.value("ssh.variant")};
}

void save_ssh_client(const ssh_client& c)
{
    settings& s = settings::get();
    s.set("ssh.command", c.command);
    s.set("ssh.variant", c.command.empty() ? "" : c.variant);
    std::lock_guard lock(g_mutex);
    g_environment = environment_for(c);
}

std::vector<std::pair<std::string, std::string>> ssh_environment()
{
    std::lock_guard lock(g_mutex);
    if (!g_environment) g_environment = environment_for(saved_ssh_client());
    return *g_environment;
}

std::string git_ssh_command(const std::string& repo_dir)
{
    git_result r = run_git(repo_dir, {"config", "core.sshCommand"}, git_output::stdout_only);
    std::string command = r.ok() ? r.output : "";
    while (!command.empty() && (command.back() == '\n' || command.back() == '\r'))
        command.pop_back();
    if (command.empty()) command = rap::sys::env("GIT_SSH_COMMAND").value_or("");
    if (command.empty()) command = rap::sys::env("GIT_SSH").value_or("");
    return command;
}

std::vector<ssh_client> detect_ssh_clients()
{
    std::vector<ssh_client> found;
    std::error_code ec;
    auto first = [&](const std::vector<std::string>& candidates) -> std::string
    {
        for (const std::string& c : candidates)
            if (std::string at = find_program(c); !at.empty()) return at;
        return {};
    };
    auto add = [&](const char* name, const std::string& program, const char* arguments,
                   const char* variant)
    {
        if (!program.empty()) found.push_back({name, program_word(program) + arguments, variant});
    };
#ifdef _WIN32
    const std::string system = rap::sys::env("SystemRoot").value_or("C:\\Windows") + "\\System32";
    std::vector<std::string> putty, tortoise;
    for (const std::string& root : install_roots())
    {
        putty.push_back(root + "\\PuTTY\\plink.exe");
        tortoise.push_back(root + "\\TortoiseGit\\bin\\TortoiseGitPlink.exe");
    }
    putty.push_back("plink");

    add("Windows OpenSSH", first({system + "\\OpenSSH\\ssh.exe"}), "", "ssh");
    // Git for Windows' ssh, in usr\bin of the installation whose git is on
    // PATH (cmd\git.exe, bin\git.exe or mingw64\bin\git.exe).
    if (const std::string git = find_program("git"); !git.empty())
        for (fs::path dir = rap::sys::path_from_utf8(git).parent_path(); dir.has_relative_path();
             dir = dir.parent_path())
            if (fs::is_regular_file(dir / "usr" / "bin" / "ssh.exe", ec))
            {
                add("Git for Windows OpenSSH",
                    rap::sys::path_to_utf8(dir / "usr" / "bin" / "ssh.exe"), "", "ssh");
                break;
            }
    // -batch: plink has no terminal to ask on, so it fails rather than
    // waiting (Pageant holds the keys). TortoiseGitPlink asks in a window.
    add("PuTTY plink", first(putty), " -batch", "plink");
    add("TortoiseGitPlink", first(tortoise), "", "tortoiseplink");
    // The ssh of the default WSL distribution, its keys and known hosts:
    // -e runs it without a shell, so git's arguments arrive as they are.
    add(_("WSL ssh (experimental)"), first({system + "\\wsl.exe"}), " -e ssh", "ssh");
#else
    add("OpenSSH", first({"ssh"}), "", "ssh");
    add("PuTTY plink", first({"plink"}), " -batch", "plink");
#endif
    return found;
}

}  // namespace tortilla
