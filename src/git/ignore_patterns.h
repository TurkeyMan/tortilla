// gitignore patterns for items, as the Ignore dialog writes them: by name or
// by extension, only where the item is or in every folder, in the
// repository's .gitignore, the item's folder's, or .git/info/exclude.
#pragma once

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace tortilla
{

struct ignore_item
{
    std::string dir;   // repository-relative folder, "" at the top
    std::string name;  // file or folder name
    bool folder = false;
};

enum class ignore_target
{
    repository,  // <workdir>/.gitignore
    folder,      // each item's folder's .gitignore
    exclude,     // .git/info/exclude: this clone only
};

// Each ignore file (repository-relative, as ".gitignore" or "sub/.gitignore";
// "" for the exclude file) and the lines it gets, in item order, without
// repeats. By extension skips items that have none.
std::vector<std::pair<std::string, std::vector<std::string>>>
ignore_patterns(const std::vector<ignore_item>&, bool by_extension, bool everywhere, ignore_target);

// A name as a literal pattern: wildcards, a leading # or ! and trailing
// spaces escaped.
std::string literal_pattern(const std::string& name);

// Appends the lines `file` doesn't already have (creating it and its
// folder); an error message, or "".
std::string append_ignore_lines(const std::filesystem::path& file, const std::vector<std::string>& lines);

}  // namespace tortilla
