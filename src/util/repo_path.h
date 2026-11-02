// A path the user gave (a command line's, a file manager's selection) as git
// names it inside a working tree.
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace tortilla
{

// `path` (absolute, or from the current folder) from the working tree's top
// `workdir`, both with links resolved: '/'-separated, without a trailing
// '/', "" for the top itself. std::nullopt if it's outside the working tree.
// UTF-8 in and out, whatever the system's code page.
std::optional<std::string> repo_relative(std::string_view workdir, std::string_view path);

}  // namespace tortilla
