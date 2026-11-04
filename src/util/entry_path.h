// A name from a git tree or an archive as a file to write under a folder of
// Tortilla's: one checked to land there, under that name, on this system.
// Such names are anyone's (a repository made elsewhere, a bare one), and a
// name legal in git can be a drive or a device here.
#pragma once

#include <filesystem>
#include <optional>
#include <string_view>

namespace tortilla
{

enum class path_rules
{
    posix,    // '/' separates; any other byte is a name's
    windows,  // also no '\', no ':', no device names, no dots or spaces Windows drops
#ifdef _WIN32
    native = windows,
#else
    native = posix,
#endif
};

// `name`, '/'-separated, as a path relative to the folder it's written
// under; nothing if it could land anywhere else or isn't that name there:
// empty, absolute, a "..", a "." or an empty part (one trailing '/', a
// folder's entry, is kept as the folder), and by Windows' rules a drive, a
// '\', an alternate stream ("name:stream"), a character Windows refuses, a
// device name (CON, NUL, COM1...) with or without an extension, or a part
// ending in '.' or ' ' (Windows drops them: another file's name). Nothing
// is dropped or changed to make a name fit.
std::optional<std::filesystem::path> entry_path(std::string_view name,
                                                path_rules = path_rules::native);

}  // namespace tortilla
