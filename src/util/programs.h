// Finding programs installed on this machine, for the tools the Settings
// dialog detects (diff viewers, SSH clients).
#pragma once

#include <string>
#include <vector>

namespace tortilla
{

// Where a program would run from: `name` itself if it's a path to a file,
// else the first match on PATH (with .exe, .cmd or .bat on Windows); "" if
// none.
std::string find_program(const std::string& name);

// The folders programs are installed under: Program Files (both), and
// LOCALAPPDATA\Programs for per-user installs. Empty except on Windows.
std::vector<std::string> install_roots();

// `path` with its separators as / : how git's sh takes a Windows path in a
// command line without treating \ as an escape.
std::string forward_slashes(std::string path);

}  // namespace tortilla
