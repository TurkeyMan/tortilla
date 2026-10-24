#!/bin/sh
# Regenerates data/translations/tortilla.pot, the template translators start
# from, from the strings marked in src/ (src/i18n.h). Needs gettext's
# xgettext; then, for each translation:
#   msgmerge -U data/translations/tortilla.<tag>.po data/translations/tortilla.pot
set -e
cd "$(dirname "$0")/.."
find src -name '*.cpp' -o -name '*.h' | grep -v '^src/shell/' | sort | xgettext --from-code=UTF-8 \
    --language=C++ --files-from=- --output=data/translations/tortilla.pot \
    --package-name=tortilla --msgid-bugs-address=https://github.com/TurkeyMan/tortilla/issues \
    --add-comments=TRANSLATORS: --sort-by-file \
    --keyword= --keyword=_:1 --keyword=C_:1c,2 --keyword=N_:1,2 --keyword=T_:1
