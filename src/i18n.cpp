#include "i18n.h"

#include "settings.h"

#include <rapport/sys.h>

#include <cstdlib>
#include <system_error>

namespace tortilla
{

std::filesystem::path translations_dir()
{
    namespace fs = std::filesystem;
    const fs::path bin = rap::sys::executable_path().parent_path();
    // Installed: <prefix>/bin/tortilla beside <prefix>/translations
    // (Windows) or <prefix>/share/tortilla/translations (Linux); built: the
    // build's bin/translations.
    for (const fs::path& p : {bin.parent_path() / "translations",
                              bin.parent_path() / "share" / "tortilla" / "translations",
                              bin / "translations"})
    {
        std::error_code ec;
        if (fs::is_directory(p, ec)) return p;
    }
    return {};
}

void setup_language()
{
    // TORTILLA_LANGUAGE overrides the setting for one run (trying a
    // translation without changing Settings).
    const char* once = std::getenv("TORTILLA_LANGUAGE");
    const std::string chosen = once ? once : settings::get().value(k_language_setting);
    if (chosen.empty())
        rap::i18n::set_languages(rap::sys::ui_languages());
    else
        rap::i18n::set_languages({chosen}, true);
    const auto dir = translations_dir();
    if (dir.empty()) return;
    rap::i18n::load("rapport", dir);
    rap::i18n::load("tortilla", dir);
}

}  // namespace tortilla
