// Submodules for Explorer's Submodule Add, Update and Sync (and the Log's
// Submodule Update): the ones a working tree has, read in process, and the
// git submodule commands' arguments.
#pragma once

#include "git/repo.h"

#include <expected>
#include <string>
#include <vector>

namespace tortilla
{

struct submodule_info
{
    std::string name, path, url;  // path: from the working tree's top, with '/'
    bool initialized = false;     // its URL in .git/config (git submodule init ran)
    bool checked_out = false;     // its working tree is there
};

// The working tree's submodules (.gitmodules and the index), by path.
std::expected<std::vector<submodule_info>, std::string> list_submodules(const repo&);

// Whether the working tree has any (a .gitmodules at its top): cheap, for
// whether to offer Submodule Update.
bool has_submodules(const std::string& workdir);

struct submodule_add_options
{
    std::string url, path;  // path: from the working tree's top
    std::string branch;     // "": the remote's HEAD
    bool force = false;     // even if the path is ignored
};
// git submodule add [-b <branch>] [-f] -- <url> <path>
std::vector<std::string> submodule_add_args(const submodule_add_options&);

struct submodule_update_options
{
    bool init = true;       // --init: the ones not initialized too
    bool recursive = true;  // --recursive: their submodules as well
    bool remote = false;    // --remote: the remote branch's tip, not the recorded commit
    bool no_fetch = false;  // --no-fetch
    bool force = false;     // --force: local changes thrown away
    enum class how
    {
        configured,  // submodule.<name>.update, else checkout
        checkout,
        merge,
        rebase,
    };
    how mode = how::configured;
};
// git submodule update [options] -- <paths>; no paths: every one.
std::vector<std::string> submodule_update_args(const submodule_update_options&,
                                               const std::vector<std::string>& paths);

// git submodule sync [--recursive]: the URLs in .gitmodules copied to
// .git/config (and the submodules' remotes).
std::vector<std::string> submodule_sync_args(bool recursive);

}  // namespace tortilla
