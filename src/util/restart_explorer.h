// Restarting Windows Explorer for the installer: it loads its shell
// extensions, and the badges' overlay handlers, only as it starts.
#pragma once

#include <string>
#include <vector>

namespace tortilla
{

// Through Windows' Restart Manager, which stops Explorer and has Windows
// start it again as the signed-in user (not as the elevated installer that
// asks). `leftovers` (files, UTF-8) are deleted while it's stopped: the old
// extension, moved aside because Explorer had it loaded. False if Explorer
// couldn't be restarted, or anywhere but Windows.
bool restart_explorer(const std::vector<std::string>& leftovers);

// The programs holding any file in `dir` (UTF-8), as Windows names them
// ("Windows Explorer"), each once: what the installer has to close.
std::vector<std::string> programs_using(const std::string& dir);

}  // namespace tortilla
