// Which SSH client git runs for ssh:// and user@host: remotes, as the
// Settings dialog's Network page sets it [TortoiseGit: Settings > Network >
// SSH client]:
//   git's own  core.sshCommand, GIT_SSH_COMMAND or GIT_SSH where tortilla was
//              started, else the ssh git finds (Git for Windows' own)
//   a client   a command line given to the git tortilla runs as
//              GIT_SSH_COMMAND (above core.sshCommand), with GIT_SSH_VARIANT
//              when it's known (plink and TortoiseGitPlink take other options)
// Kept in tortilla's settings ("ssh.command", "ssh.variant"); git's own
// config is left alone.
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace tortilla
{

struct ssh_client
{
    std::string name;
    std::string command;  // empty: git's own
    // GIT_SSH_VARIANT (ssh, plink, tortoiseplink); empty: git goes by the name.
    std::string variant;
};

ssh_client saved_ssh_client();
void save_ssh_client(const ssh_client&);

// What git uses when tortilla doesn't choose: core.sshCommand for
// `repo_dir`, else GIT_SSH_COMMAND or GIT_SSH from tortilla's environment;
// "" for git's default ssh.
std::string git_ssh_command(const std::string& repo_dir);

// SSH clients found on this machine, with command lines ready to use.
std::vector<ssh_client> detect_ssh_clients();

// For the environment of every git tortilla runs: GIT_SSH_COMMAND and
// GIT_SSH_VARIANT for the saved client, nothing for git's own. Safe from any
// thread.
std::vector<std::pair<std::string, std::string>> ssh_environment();

}  // namespace tortilla
