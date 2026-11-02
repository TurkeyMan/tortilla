// Files Tortilla writes under the system's temporary folder: revisions
// opened from history, and the inputs it hands git.
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace tortilla
{

// <temp>/tortilla, made if it isn't there; empty if there's no temporary
// folder.
std::filesystem::path tortilla_temp_dir();

// A new name there, "<prefix>-<time>-<n><extension>", for a file to hand
// git (a commit message, a list of paths): the time keeps two processes'
// apart, and a count two calls within one tick. Empty if there's no
// temporary folder.
std::filesystem::path unique_temp_path(std::string_view prefix, std::string_view extension);

// Where `rev`'s copy of the repository file `path` goes:
// <temp>/tortilla/<rev>/<path>, the hash shortened to 8. It keeps its
// folders, so two files of one name in a revision don't overwrite each
// other. A name such as origin/main is made a single folder.
std::filesystem::path revision_temp_path(std::string_view rev, std::string_view path);

// `content` as the whole of `path`, byte for byte, making its folders.
bool write_file(const std::filesystem::path& path, std::string_view content);

}  // namespace tortilla
