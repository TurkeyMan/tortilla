// Explorer's Create Patch Serial and Apply Patch Serial (TortoiseGit's
// CFormatPatchDlg and CImportPatchDlg): the git format-patch and git am
// command lines their dialogs make.
#pragma once

#include "git/command.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace tortilla
{

struct format_patch_options
{
    std::string folder;  // where the numbered .patch files go
    enum class which
    {
        since,   // the commits since a revision (not in it, in HEAD)
        number,  // HEAD's newest `number` commits
        range,   // from..to
    } commits = which::since;
    std::string since;
    int number = 1;
    std::string from, to;  // to: "" is HEAD
};

// git format-patch -o <folder> <since> | -<number> | <from>..<to>
std::vector<std::string> format_patch_serial_args(const format_patch_options&);

struct am_options
{
    bool three_way = false;     // --3way: fall back to a 3-way merge
    bool ignore_space = false;  // --ignore-space-change
    bool keep_cr = false;       // --keep-cr
    bool signoff = false;       // --signoff: a Signed-off-by trailer
};

// git am [options] -- <patch>...: the patches applied in order, a commit each.
std::vector<std::string> am_args(const am_options&, const std::vector<std::string>& patches);

// Whether git am stopped part way (`git_dir`/rebase-apply/applying): what's
// left is to go on, skip the patch, or abort.
bool am_in_progress(const std::string& git_dir);

// Review/apply single patch: a file a patch changes.
struct patch_entry
{
    enum class change
    {
        modified,
        added,
        deleted,
        renamed,
        copied,
    };
    std::string old_path;  // where it was: a rename's or copy's source, else `path`
    std::string path;      // where it goes, the name git apply --include matches
    change kind = change::modified;
    bool binary = false;
};

// The files a patch changes, in order: git apply -z --numstat's `numstat`
// (its list and names), each with its old name and its change from the
// `patch` text itself where git's headers say (a plain diff's are modified).
std::vector<patch_entry> patch_entries(std::string_view numstat, std::string_view patch);

// git apply [--check] [--include=<path>...] -- <patch>: the whole patch, or
// only those files of it, each name taken literally (its wildcards escaped).
// The applicability check, Review's preview and Apply selected all use it.
std::vector<std::string> apply_args(const std::string& patch,
                                    const std::vector<std::string>& only = {}, bool check = false);

struct patch_preview
{
    git_result applied;  // the entry applied in the folder
    std::string base;    // the working file as it is (empty_file() for an added one)
    std::string mine;    // as the patch leaves it (empty_file() for a deleted one)
};

// Review's preview of one entry: the file it starts from (a rename's old
// one) copied from `workdir` into `folder`, an empty folder of the caller's,
// and that entry of `patch` applied there. Names that wouldn't land in the
// folder as themselves are refused.
patch_preview preview_patch_entry(const std::string& workdir, const std::string& patch,
                                  const patch_entry&, const std::filesystem::path& folder);

}  // namespace tortilla
