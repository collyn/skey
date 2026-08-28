#!/bin/sh
# Regenerate po/fcitx5-skey.pot from src/settings and merge into vi.po/en.po.
# Version is auto-detected from CMakeLists.txt (same pattern as
# scripts/make-ppa-source.sh).  msgmerge keeps existing translations,
# adds new msgids as untranslated and marks obsolete ones with #~.
# Run from the project root; needs gettext (xgettext, msgmerge, msgfmt).
set -e

VERSION=$(grep -oP 'project\(fcitx5-skey VERSION \K[0-9.]+' CMakeLists.txt)
if [ -z "$VERSION" ]; then
    echo "Error: could not determine version from CMakeLists.txt" >&2
    exit 1
fi

xgettext --c++ --from-code=UTF-8 -kT -kC_:2c \
    --package-name=fcitx5-skey --package-version="$VERSION" \
    --copyright-holder="Skey contributors" \
    --msgid-bugs-address=https://github.com/collyn/skey/issues \
    -o po/fcitx5-skey.pot src/settings/*.cpp src/settings/*.h

for lang in vi en; do
    if [ -f "po/$lang.po" ]; then
        msgmerge --backup=off -U "po/$lang.po" po/fcitx5-skey.pot
    else
        echo "po/$lang.po does not exist yet — seed it with:" >&2
        echo "  msginit --no-translator -i po/fcitx5-skey.pot -o po/$lang.po --locale=${lang}_" >&2
    fi
done

msgfmt -c -o /dev/null po/vi.po
msgfmt -c -o /dev/null po/en.po
echo "OK: po/fcitx5-skey.pot regenerated (version $VERSION), vi.po/en.po merged."
