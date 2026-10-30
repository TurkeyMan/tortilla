// A revision's files as its tree holds them, for the Repository Browser:
// read in process through libgit2, nothing checked out and no git started
// (that costs near a second on Windows over \\wsl.localhost, a click).
#pragma once

#include "git/repo.h"

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <vector>

namespace tortilla
{

// What a revision ("HEAD", a branch, a tag, a hash) names, peeled to its tree.
struct tree_revision
{
    oid tree;
    std::optional<oid> commit;  // none when the revision is a tree itself
    std::string subject;        // the commit's
};

struct tree_entry
{
    enum class kind
    {
        folder,
        file,
        executable,
        symlink,
        submodule,  // a commit of another repository, with nothing here to read
    };
    std::string name;
    kind type = kind::file;
    oid id;             // the blob, the tree, or a submodule's commit
    uint64_t size = 0;  // a blob's size as stored (an LFS file's is its pointer's)
};

std::expected<tree_revision, std::string> resolve_tree(const repo&, const std::string& rev);

// Every folder in `tree` at every depth, by path ("a/b"), each before its
// own folders; the top ("") isn't listed.
std::vector<std::string> tree_folders(const repo&, const oid& tree);

// The entries of the folder at `path` in `tree` ("" the top), in the tree's
// order; an error if there's no such folder.
std::expected<std::vector<tree_entry>, std::string> tree_entries(const repo&, const oid& tree,
                                                                 const std::string& path);

}  // namespace tortilla
