// Explorer's "Create repository here..." (TortoiseGit's CreateRepositoryCommand
// and CCreateRepoDlg) [spec 4.2, row 47]: a warning for folders that aren't
// meant to be a repository's root, a choice of bare or not (bare at first
// for a folder named *.git), then git init and what came of it.
#pragma once

#include <rapport/rapport.h>

#include <functional>
#include <string>

namespace tortilla
{

// `done` gets whether a repository was made, once nothing more is shown.
void create_repository(rap::ui&, const std::string& folder, std::function<void(bool made)> done);

}  // namespace tortilla
