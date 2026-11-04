// A file's two versions handed straight to a diff tool, as TortoiseGit
// does: the blobs read through libgit2, written as a checkout would write
// them, and the tool started with them. git difftool takes git, git diff,
// its sh helper and then the tool, each slow to start on Windows.
#pragma once

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace tortilla
{

// One file that differs, as git diff pairs it (renames found).
struct file_diff
{
    std::string old_path, path;  // the same but for a rename or copy
    char status = 'M';           // A D M R C T
    std::string old_id, new_id;  // blob ids; new_id empty for the working tree's file
    bool regular = true;         // a file on both sides, not a link or a submodule
};

// The files that differ between `revisions` (one: against the working
// tree) under `paths` (names, not patterns), through libgit2, so a check
// before a tool starts costs no git. nullopt where git should answer: no
// paths (a whole tree, where git's index caches are faster) or a revision
// libgit2 can't resolve.
std::optional<std::vector<file_diff>> diff_files(const std::string& dir,
                                                 const std::vector<std::string>& revisions,
                                                 const std::vector<std::string>& paths);

// The two files a tool compares, as platform paths.
struct diff_sides
{
    std::string base, mine;
    std::vector<std::filesystem::path> written;  // the temporary files made for them
    std::filesystem::path folder;                // theirs alone (revision_temp_dir)
};

// `f`'s sides as files: a revision's written in a temporary folder of this
// comparison's own (revision_temp_path), so no other comparison or Open,
// here or in another Tortilla, touches them; through the repository's
// filters, as git difftool
// writes them (line endings, ident; an LFS file through git, which runs
// git-lfs); the working tree's is the file itself; a side that doesn't
// exist (added, deleted) is the empty file.
std::expected<diff_sides, std::string> write_diff_sides(const std::string& dir,
                                                        const std::vector<std::string>& revisions,
                                                        const file_diff& f);

// Removes the folder write_diff_sides wrote its files in, once the tool is
// done with them.
void remove_diff_sides(const diff_sides&);

// The program and arguments that run `command` (TortoiseGit's
// placeholders: %base %mine %bname %yname) on two files without a shell:
// split into words as git's tool settings are, quotes honoured, each
// placeholder replaced within its word, so no value needs quoting. Without
// %base or %mine the two files go last. The program is found on PATH.
// nullopt when the command needs sh (a variable, a pipe, an escape...), its
// program isn't found, or (Windows) it's a script git's sh would run: git
// difftool does those.
std::optional<std::vector<std::string>>
diff_tool_words(const std::string& command, const std::string& base, const std::string& mine,
                const std::string& base_name, const std::string& mine_name);

}  // namespace tortilla
