// Tortilla's oid and libgit2's git_oid, one to the other: for the .cpp
// files in src/git that talk to libgit2 (not for the rest, which don't see
// git2.h).
#pragma once

#include "git/repo.h"

#include <git2.h>

#include <cstring>

namespace tortilla
{

inline oid to_oid(const git_oid* o)
{
    oid out;
    std::memcpy(out.bytes.data(), o->id, out.bytes.size());
    return out;
}

inline git_oid to_git(const oid& o)
{
    git_oid out{};
    std::memcpy(out.id, o.bytes.data(), o.bytes.size());
    return out;
}

}  // namespace tortilla
