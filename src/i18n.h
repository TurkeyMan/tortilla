// Tortilla's translations: rapport's i18n with the "tortilla" domain. The
// markers below are what tools/update-pot.sh extracts into
// data/translations/tortilla.pot.
//
//   _("Commit")                         a string to translate
//   C_("verb", "Open")                  one that reads differently in places
//   N_("{0} file", "{0} files", n)      by count (then rap::i18n::format)
//   rap::i18n::format(_("{0} of {1}"), a, b)
//
// Not for what git or the settings file reads: arguments, config keys,
// file names.
#pragma once

#include <rapport/i18n.h>

#include <filesystem>
#include <string>

#define _(text) ::rap::i18n::tr("tortilla", text)
#define C_(context, text) ::rap::i18n::tr_ctx("tortilla", context, text)
#define N_(one, many, n) ::rap::i18n::tr_n("tortilla", one, many, n)

namespace tortilla
{

// The "language" setting: a tag, or "" to follow the system.
inline constexpr const char* k_language_setting = "language";

// Chooses the language (TORTILLA_LANGUAGE, else the setting, else the
// system's; "en" is English) and loads rapport's and Tortilla's
// translations. Once, first thing: before the ui exists and before
// anything runs git.
void setup_language();

// Where the .po files are: translations/ beside bin/ (Windows), the data
// directory's (Linux), or the build's own copy.
std::filesystem::path translations_dir();

}  // namespace tortilla
