// Files Tortilla writes under the system's temporary folder: revisions
// opened from history, and the inputs it hands git.
#pragma once

#include <chrono>
#include <cstdint>
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

// Where `rev`'s copy of the repository file `path` goes in `dir`, a folder
// revision_temp_dir() made: <dir>/<rev>/<path>, the hash shortened to 8, so
// a tool's title still names the file and its revision. It keeps its
// folders, so two files of one name in a revision don't overwrite each
// other. A name such as origin/main is made a single folder. Empty if `dir`
// is, or if `path` can't be written there as that name (entry_path(): "..",
// a root, and on Windows a drive, a \, a stream or a device name): a name
// from a git tree is anyone's.
std::filesystem::path revision_temp_path(const std::filesystem::path& dir, std::string_view rev,
                                         std::string_view path);

// What revision files' folders are named after: one viewer's each, so one
// finishing, or another starting, in this process or another, never
// removes or rewrites a file still shown.
inline constexpr std::string_view revision_dir_prefix = "rev";

// A new folder for revision files handed to a program whose end isn't seen
// (Open, Open with): no one owns it (no lock, see temp_dir), so
// sweep_temp_dirs() removes it once it's a day old. Empty if it can't be
// made. A viewer that sees its end holds a temp_dir(revision_dir_prefix)
// instead.
std::filesystem::path revision_temp_dir();

// `content` written as `rev`'s `path` in a new revision_temp_dir(), for a
// program that opens it: the file's path, or empty (and no folder left) if
// it couldn't be written.
std::filesystem::path write_revision_file(std::string_view rev, std::string_view path,
                                          std::string_view content);

// Removes the folders revision_temp_dir() and temp_dir made that are older
// than `age` and that no one holds (a temp_dir's lock, in any process): left
// by a crash, or by a program a file was opened in. Run at startup.
void sweep_temp_dirs(std::chrono::hours age = std::chrono::hours(24));

// A new, empty folder there, made by this call, "<prefix>-<time>-<n>"; empty
// if it can't be made. Its maker removes it when done with it.
std::filesystem::path unique_temp_dir(std::string_view prefix);

// A unique_temp_dir() removed, with all in it, when this goes: a scratch
// tree (Review Patch's previews, a comparison's revision files) kept while
// something reads it. While it lives it holds a lock on "<folder>.lock"
// beside it (flock; on Windows the file open, unshared), so no
// sweep_temp_dirs() in any Tortilla removes it however old it gets.
class temp_dir
{
public:
    explicit temp_dir(std::string_view prefix);
    ~temp_dir();
    temp_dir(const temp_dir&) = delete;
    temp_dir& operator=(const temp_dir&) = delete;
    // Empty if it couldn't be made.
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
    std::intptr_t lock_ = -1;  // a descriptor, or a HANDLE on Windows
};

// `content` as the whole of `path`, byte for byte, making its folders.
bool write_file(const std::filesystem::path& path, std::string_view content);

// write_file for a file under tortilla_temp_dir(), which it must be (false
// otherwise). It follows no link on the way, so one left in the temporary
// tree (shared, on Linux, by everyone) can't send the file elsewhere; a link
// where the file goes is replaced, its target untouched.
bool write_temp_file(const std::filesystem::path& path, std::string_view content);

}  // namespace tortilla
