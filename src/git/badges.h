// The badges on file manager icons: a repository's files and folders as
// Clean, Modified, Conflicted or Added, from git's status. Untracked and
// ignored files get none, nor does anything outside a repository.
#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace tortilla
{

enum class badge : uint8_t
{
    none,
    clean,
    modified,
    conflicted,
    added,
};

// One repository's badges, made from git's answers. A folder sums up what's
// inside it: conflicted if anything is, else modified if anything changed
// (an added file is a change to its folder), else clean if anything in it is
// tracked.
class repo_badges
{
public:
    // `tracked`: git ls-files -z; `status`: git status --porcelain -z, with
    // or without untracked files. Paths are repository-relative, '/'-separated.
    static repo_badges build(std::string_view tracked, std::string_view status);

    // `path` repository-relative, '/'-separated, "" for the top folder.
    badge of(std::string_view path) const;
    // Whether `path` is untracked, or a folder with something untracked in
    // it: when git status listed untracked files.
    bool untracked(std::string_view path) const;

private:
    std::unordered_map<std::string, badge> changed_;  // files, then their folders
    std::unordered_set<std::string> tracked_;         // files and the folders holding them
    std::unordered_set<std::string> untracked_;       // as git listed them
    std::unordered_set<std::string> untracked_within_;  // folders holding those
};

// Runs git in `workdir` (the top of a working tree) for its badges.
std::expected<repo_badges, std::string> load_badges(const std::string& workdir);

// Just the top folder's badge, cheaply: git status alone, without listing
// every tracked file. Clean when nothing's changed.
std::expected<badge, std::string> load_summary(const std::string& workdir);

}  // namespace tortilla
