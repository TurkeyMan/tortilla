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
// other. A name such as origin/main is made a single folder. Empty if
// `path` can't be written there as that name (entry_path(): "..", a root,
// and on Windows a drive, a \, a stream or a device name): a name from a
// git tree is anyone's.
std::filesystem::path revision_temp_path(std::string_view rev, std::string_view path);

// A new, empty folder there, made by this call, "<prefix>-<time>-<n>"; empty
// if it can't be made. Its maker removes it when done with it.
std::filesystem::path unique_temp_dir(std::string_view prefix);

// `content` as the whole of `path`, byte for byte, making its folders.
bool write_file(const std::filesystem::path& path, std::string_view content);

// write_file for a file under tortilla_temp_dir(), which it must be (false
// otherwise). It follows no link on the way, so one left in the temporary
// tree (shared, on Linux, by everyone) can't send the file elsewhere; a link
// where the file goes is replaced, its target untouched.
bool write_temp_file(const std::filesystem::path& path, std::string_view content);

}  // namespace tortilla
