// The paths of a git command in a file it reads them from
// (--pathspec-from-file), rather than on its command line: a big first
// commit's thousands of new files would pass Windows' 32K limit.
#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace tortilla
{

// Removed when the last copy goes: hold one until the commands have run.
// Commands that take it: add, checkout, commit, reset, restore, rm, stash
// push.
class pathspec_file
{
public:
    // nullptr if it couldn't be written.
    static std::shared_ptr<pathspec_file> write(const std::vector<std::string>& paths);
    ~pathspec_file();
    pathspec_file(const pathspec_file&) = delete;
    pathspec_file& operator=(const pathspec_file&) = delete;

    // The arguments that read it, NUL-separated (in place of "-- paths").
    std::vector<std::string> arguments() const;
    const std::filesystem::path& path() const { return path_; }

private:
    explicit pathspec_file(std::filesystem::path p)
        : path_(std::move(p))
    {
    }
    std::filesystem::path path_;
};

}  // namespace tortilla
