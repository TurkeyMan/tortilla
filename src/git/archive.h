// Export (log's "Export this version...", Explorer's Export): a revision's
// tracked files written out by git archive, as a .zip, or into a folder.
// git has no command writing a tree into a folder of one's choice without
// touching the index, so a folder is git archive's tar, unpacked here.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tortilla
{

enum class archive_format
{
    zip,
    tar,
};

// git archive writing `rev`'s whole tree to `file`, run from the
// repository's top (from a subfolder, git archives only that folder).
std::vector<std::string> archive_args(archive_format, const std::string& rev,
                                      const std::string& file);

// The name suggested for the export of `rev` from the repository at
// `workdir`: the repository's folder name ("x" for a bare "x.git"), and
// the revision unless it's HEAD, a hash shortened: "tortilla-1b7bd0a2".
std::string export_name(const std::string& workdir, const std::string& rev);

// An entry's name in an archive as a path under the folder unpacked into;
// nothing if it would land outside it (absolute, a drive, or a "..").
std::optional<std::filesystem::path> archive_entry_path(std::string_view name);

struct extract_result
{
    std::string error;                // the archive couldn't be read; empty if it could
    std::vector<std::string> failed;  // entries that couldn't be written
    bool ok() const { return error.empty() && failed.empty(); }
};

// Unpacks git archive's tar into `into`, creating it, writing over files
// already there. Executable bits are kept; a symbolic link the system won't
// make (Windows without the privilege) is written as a file holding its
// target, as git does with core.symlinks off. Slow for a big tree: call it
// off the UI thread.
extract_result extract_tar(const std::filesystem::path& tar, const std::filesystem::path& into);

}  // namespace tortilla
