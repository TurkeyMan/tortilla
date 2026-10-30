// git blame for the Blame window [TortoiseGitBlame]: which commit last
// changed each line of a file, read from git blame --porcelain.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace tortilla
{

// A commit some lines come from, as git blame describes it.
struct blame_commit
{
    std::string hash;
    std::string author, author_email;
    int64_t author_time = 0;
    std::string committer;
    int64_t committer_time = 0;
    std::string summary;
    std::string previous_hash;      // the commit before it that had the file; "" when it made it
    std::string previous_filename;  // the file's name there
    bool boundary = false;          // the walk's oldest commit (or uncommitted lines' "0000...")
    bool uncommitted() const { return hash.find_first_not_of('0') == std::string::npos; }
};

struct blame_line
{
    std::shared_ptr<const blame_commit> commit;
    std::string filename;      // the file's name in that commit (it may have been renamed since)
    size_t original_line = 0;  // 1-based, in that commit's version
    std::string text;
};

struct blame_options
{
    std::string revision;  // blame the file as it is at this revision; "" for the working tree
    bool ignore_whitespace = false;  // -w
    bool detect_moves = false;       // -M -C: lines moved within the file or copied from others
};

// A revision of a file, for the Blame window's history slider: the commits
// that changed it, newest first, across renames.
struct file_revision
{
    std::string hash;
    std::string path;  // the file's name in that commit
    std::string author;
    int64_t time = 0;  // author time
    std::string summary;
};

// git log's arguments for `path`'s revisions, and its output read back.
std::vector<std::string> file_revision_args(const std::string& path, const std::string& from = {});
std::vector<file_revision> parse_file_revisions(std::string_view output);

// git blame's arguments for `path` (repository-relative).
std::vector<std::string> blame_args(const std::string& path, const blame_options&);

// The lines of git blame --porcelain's output, in the file's order.
std::vector<blame_line> parse_blame(std::string_view porcelain);

}  // namespace tortilla
