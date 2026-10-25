// git clean for Clean up (changes/cleanup_dialog.h): its arguments, and its
// dry run's listing read back.
#pragma once

#include <string>
#include <vector>

namespace tortilla
{

struct clean_options
{
    enum what
    {
        untracked,  // not ignored (git clean)
        ignored,    // only ignored (-X)
        all,        // both (-x)
    };
    what files = untracked;
    bool directories = true;  // -d
    bool submodules = false;
};

// git clean's arguments, listing what would go (-n) or removing it (-f),
// run from the repository's top. A folder is cleaned from inside it
// (-C <folder>) with no paths: given paths, git clean removes untracked
// folders too, -d or not. `files` are paths in `folder` for files chosen
// one by one. The listing's paths are relative to `folder`.
std::vector<std::string> clean_args(const clean_options&, bool dry_run, const std::string& folder,
                                    const std::vector<std::string>& files = {});
// The same in every submodule, recursively; paths are their own.
std::vector<std::string> submodule_clean_args(const clean_options&, bool dry_run);
// What a dry run would remove: the paths of git clean -n's "Would remove"
// lines, relative to where it ran. git submodule foreach's "Entering
// '<path>'" lines prefix those after them with the submodule's path.
std::vector<std::string> parse_clean_listing(const std::string& output);

}  // namespace tortilla
