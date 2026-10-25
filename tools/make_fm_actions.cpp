// Writes PCManFM's and PCManFM-Qt's packaged Tortilla menu, for the default
// placements (src/shell/fm_actions.h), and the names file Settings builds
// the user's own menu from. The labels are the shell menu's
// (src/shell/menu_table.h) and their translations Tortilla's catalogues, so
// neither is copied by hand.
//   tortilla-fm-actions OUTDIR NAMES TRANSLATIONS_DIR CONDITION TORTILLA
// CONDITION is the installed fm-condition script, TORTILLA the program.
#include "shell/fm_actions.h"

#include <cstdio>

int main(int argc, char** argv)
{
    using namespace tortilla;
    if (argc != 6)
    {
        std::fprintf(
            stderr,
            "usage: tortilla-fm-actions OUTDIR NAMES TRANSLATIONS_DIR CONDITION TORTILLA\n");
        return 2;
    }
    for (const std::string& id : fm_actions::ids())
    {
        bool found = false;
        for (const shell_menu::row& r : shell_menu::k_rows) found |= r.label && id == r.id;
        if (!found)
        {
            std::fprintf(stderr, "tortilla-fm-actions: no menu row \"%s\"\n", id.c_str());
            return 1;
        }
    }
    const fm_actions::names names = fm_actions::from_catalogues(argv[3]);
    std::filesystem::create_directories(std::filesystem::path(argv[2]).parent_path());
    fm_actions::write_names(argv[2], names);
    fm_actions::write(argv[1], {}, names, {argv[4], argv[5]}, false);
    return 0;
}
