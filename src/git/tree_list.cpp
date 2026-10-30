#include "tree_list.h"

#include <git2.h>

#include <cstring>
#include <memory>

namespace tortilla
{
namespace
{

oid to_oid(const git_oid* o)
{
    oid out;
    std::memcpy(out.bytes.data(), o->id, out.bytes.size());
    return out;
}

git_oid to_git(const oid& o)
{
    git_oid out{};
    std::memcpy(out.id, o.bytes.data(), o.bytes.size());
    return out;
}

using object_ptr = std::unique_ptr<git_object, decltype(&git_object_free)>;
using tree_ptr = std::unique_ptr<git_tree, decltype(&git_tree_free)>;

tree_ptr lookup_tree(const repo& r, const oid& id)
{
    const git_oid g = to_git(id);
    git_tree* t = nullptr;
    if (git_tree_lookup(&t, r.raw(), &g) != 0) t = nullptr;
    return tree_ptr(t, &git_tree_free);
}

}  // namespace

std::expected<tree_revision, std::string> resolve_tree(const repo& r, const std::string& rev)
{
    git_object* found = nullptr;
    if (git_revparse_single(&found, r.raw(), rev.empty() ? "HEAD" : rev.c_str()) != 0)
        return std::unexpected(last_git_error());
    object_ptr obj(found, &git_object_free);
    tree_revision out;
    git_object* peeled = nullptr;
    // A commit (or a tag of one) by its commit; a tree's own id browses it bare.
    if (git_object_peel(&peeled, obj.get(), GIT_OBJECT_COMMIT) == 0)
    {
        object_ptr c(peeled, &git_object_free);
        const git_commit* commit = reinterpret_cast<const git_commit*>(c.get());
        out.commit = to_oid(git_commit_id(commit));
        out.tree = to_oid(git_commit_tree_id(commit));
        if (const char* s = git_commit_summary(const_cast<git_commit*>(commit))) out.subject = s;
        return out;
    }
    if (git_object_peel(&peeled, obj.get(), GIT_OBJECT_TREE) != 0)
        return std::unexpected(last_git_error());
    out.tree = to_oid(git_object_id(peeled));
    git_object_free(peeled);
    return out;
}

std::vector<std::string> tree_folders(const repo& r, const oid& tree)
{
    std::vector<std::string> out;
    tree_ptr t = lookup_tree(r, tree);
    if (!t) return out;
    // Pre-order, so each folder comes before those inside it.
    git_tree_walk(
        t.get(), GIT_TREEWALK_PRE,
        [](const char* root, const git_tree_entry* e, void* payload) -> int
        {
            if (git_tree_entry_type(e) == GIT_OBJECT_TREE)
                static_cast<std::vector<std::string>*>(payload)->push_back(std::string(root) +
                                                                           git_tree_entry_name(e));
            return 0;
        },
        &out);
    return out;
}

std::expected<std::vector<tree_entry>, std::string> tree_entries(const repo& r, const oid& tree,
                                                                 const std::string& path)
{
    tree_ptr t = lookup_tree(r, tree);
    if (!t) return std::unexpected(last_git_error());
    if (!path.empty())
    {
        git_tree_entry* e = nullptr;
        if (git_tree_entry_bypath(&e, t.get(), path.c_str()) != 0)
            return std::unexpected(last_git_error());
        const bool folder = git_tree_entry_type(e) == GIT_OBJECT_TREE;
        const oid sub = to_oid(git_tree_entry_id(e));
        git_tree_entry_free(e);
        if (!folder) return std::unexpected(path + ": not a folder");
        t = lookup_tree(r, sub);
        if (!t) return std::unexpected(last_git_error());
    }
    // Sizes from the objects' headers: no blob read whole.
    git_odb* odb = nullptr;
    if (git_repository_odb(&odb, r.raw()) != 0) odb = nullptr;
    std::unique_ptr<git_odb, decltype(&git_odb_free)> odb_owner(odb, &git_odb_free);

    std::vector<tree_entry> out;
    const size_t n = git_tree_entrycount(t.get());
    out.reserve(n);
    for (size_t i = 0; i < n; ++i)
    {
        const git_tree_entry* e = git_tree_entry_byindex(t.get(), i);
        tree_entry entry;
        entry.name = git_tree_entry_name(e);
        entry.id = to_oid(git_tree_entry_id(e));
        switch (git_tree_entry_filemode(e))
        {
        case GIT_FILEMODE_TREE: entry.type = tree_entry::kind::folder; break;
        case GIT_FILEMODE_BLOB_EXECUTABLE: entry.type = tree_entry::kind::executable; break;
        case GIT_FILEMODE_LINK: entry.type = tree_entry::kind::symlink; break;
        case GIT_FILEMODE_COMMIT: entry.type = tree_entry::kind::submodule; break;
        default: entry.type = tree_entry::kind::file; break;
        }
        if (odb && git_tree_entry_type(e) == GIT_OBJECT_BLOB)
        {
            size_t size = 0;
            git_object_t type{};
            if (git_odb_read_header(&size, &type, odb, git_tree_entry_id(e)) == 0)
                entry.size = size;
        }
        out.push_back(std::move(entry));
    }
    return out;
}

}  // namespace tortilla
