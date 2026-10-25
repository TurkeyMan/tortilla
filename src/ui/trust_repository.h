// A repository git won't open because another user owns its folder (a
// network share, \\wsl.localhost, another account's disk): git's
// safe.directory check. As TortoiseGit does, it's offered to trust that one
// folder, with what that means spelled out; never every folder, never
// without asking.
#pragma once

#include <rapport/rapport.h>

#include <functional>
#include <optional>
#include <string>

namespace tortilla
{

// The safe.directory value git suggests for the repository at `path`, when
// it refuses it for its owner; nullopt when git opens it (or fails for
// another reason).
std::optional<std::string> untrusted_repository(const std::string& path);

// When git refuses `path` for its owner, asks whether to trust it and adds
// it to the global git config if so. `done` gets whether opening can go on:
// true when it was trusted or nothing needed asking.
void ensure_trusted(rap::ui&, const std::string& path, std::function<void(bool go_on)> done);

}  // namespace tortilla
