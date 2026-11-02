// Explorer's Create Patch Serial and Apply Patch Serial (TortoiseGit's
// CFormatPatchDlg and CImportPatchDlg): the git format-patch and git am
// command lines their dialogs make.
#pragma once

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

// Review/apply single patch: the files a patch changes, from git apply
// -z --numstat (a rename's new name).
std::vector<std::string> numstat_paths(std::string_view numstat);

// git apply [--check] [--include=<path>...] -- <patch>: the whole patch, or
// only those files of it.
std::vector<std::string> apply_args(const std::string& patch,
                                    const std::vector<std::string>& only = {}, bool check = false);

}  // namespace tortilla
