// git-svn, for Explorer's SVN DCommit, SVN Rebase, SVN Fetch and Import SVN
// Ignore, and the Log's SVN DCommit: whether a repository is a git-svn one,
// whether git svn is installed, and the commands TortoiseGit runs.
#pragma once

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace tortilla
{

// Whether a repository is a git-svn one: git-svn's metadata in `git_dir`
// (TortoiseGit's test [spec 4.3 step 5]), or an svn-remote section in the
// config in `common_dir` (`git svn init` writes that before any fetch).
// Reads files only, for the file manager's extension too.
inline bool is_git_svn(const std::filesystem::path& git_dir,
                       const std::filesystem::path& common_dir)
{
    std::error_code ec;
    if (std::filesystem::exists(git_dir / "svn/.metadata", ec)) return true;
    std::ifstream in(common_dir / "config", std::ios::binary);
    for (std::string line; std::getline(in, line);)
    {
        std::string_view s = line;
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
        if (!s.starts_with('[')) continue;
        s.remove_prefix(1);
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
        // Section names ignore case.
        constexpr std::string_view k_section = "svn-remote";
        if (s.size() < k_section.size()) continue;
        bool same = true;
        for (size_t i = 0; i < k_section.size() && same; ++i) same = (s[i] | 0x20) == k_section[i];
        if (same && (s.size() == k_section.size() || s[k_section.size()] == ' ' ||
                     s[k_section.size()] == '"' || s[k_section.size()] == '.' ||
                     s[k_section.size()] == ']'))
            return true;
    }
    return false;
}

// Whether `git svn` runs here (`git svn --version`: many git installs lack
// it, or the Perl it needs). Asked once, then kept; it blocks, so not on the
// UI thread.
bool git_svn_installed(const std::string& dir);

// Said where an entry is greyed for it.
inline constexpr std::string_view k_no_git_svn = "git svn isn't installed";

// Whether a commit's message says it's in SVN: git svn adds a "git-svn-id:"
// line to every commit it sends there or fetches from there. SVN's history
// can't be rewritten, so the log doesn't offer to.
bool in_svn(std::string_view message);

// The branch git svn fetches into, from svn-remote.svn.fetch's value
// ("trunk:refs/remotes/origin/trunk" gives "remotes/origin/trunk"), as
// TortoiseGit's SVN Rebase reads it; "" if it has none.
std::string svn_upstream(std::string_view fetch);

// git svn dcommit [--rmdir]
std::vector<std::string> svn_dcommit_args(bool rmdir);
// git svn fetch
std::vector<std::string> svn_fetch_args();

// Import SVN Ignore's two ways: svn:ignore into .git/info/exclude (git svn
// show-ignore's output), or a .gitignore in each folder (git svn
// create-ignore).
enum class svn_ignore_into
{
    exclude,
    gitignore,
};
std::vector<std::string> svn_ignore_args(svn_ignore_into);

// .git/info/exclude with git svn show-ignore's patterns added: those it
// doesn't have yet, after what's there. TortoiseGit writes over the file;
// this keeps what the user put in it.
std::string merge_exclude(std::string_view existing, std::string_view ignores);

}  // namespace tortilla
