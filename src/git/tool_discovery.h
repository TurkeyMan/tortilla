// Finding the diff and merge tools installed here, for the Settings pages
// to offer: where installers put them, and which are there. Each tool's
// command line stays with its kind (diff_tool.h, merge_tool.h).
#pragma once

#include <initializer_list>
#include <string>
#include <vector>

namespace tortilla
{

// `relative` (e.g. "WinMerge\\WinMergeU.exe") under each of the install
// folders (install_roots()), then `on_path`, a name to look for on PATH,
// if given: the paths to try, in order.
std::vector<std::string> install_candidates(std::initializer_list<const char*> relative,
                                            const char* on_path = nullptr);

// The JetBrains IDEs installed under the install folders, each named after
// its folder ("CLion 2025.2") with its launcher (bin\clion64.exe). Their
// launchers take `diff a b` and `merge mine theirs base merged`.
struct jetbrains_ide
{
    std::string name;
    std::string launcher;
};
std::vector<jetbrains_ide> jetbrains_ides();

// The first of `programs` that's there, as a command's first word: a name
// found on PATH stays a name, a path is quoted if it has spaces ("C:\Program
// Files\..."). "" if none is.
std::string installed_program(const std::vector<std::string>& programs);

// "flatpak run <app>" if the Flatpak app is installed for the user or the
// system; "" if not (and on Windows).
std::string flatpak_command(const char* app);

}  // namespace tortilla
