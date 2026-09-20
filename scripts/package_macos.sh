#!/usr/bin/env bash
set -euo pipefail
app="dist-all/pingkk.app"
plugins="$(qmake -query QT_INSTALL_PLUGINS)"
mkdir -p "$app/Contents/PlugIns/platforms" "$app/Contents/PlugIns/styles"
cp "$plugins/platforms/libqcocoa.dylib" "$app/Contents/PlugIns/platforms/"
cp "$plugins/styles/libqmacstyle.dylib" "$app/Contents/PlugIns/styles/"
# Resolve dependencies of only the plugins actually used by this Widgets app.
macdeployqt "$app" -always-overwrite -no-plugins \
    "-executable=$app/Contents/PlugIns/platforms/libqcocoa.dylib" \
    "-executable=$app/Contents/PlugIns/styles/libqmacstyle.dylib"
find "$app" -type d \( -name Headers -o -name '*.dSYM' \) -prune -exec rm -rf '{}' +
find "$app" -type f \( -name '*.prl' -o -name '*.la' \) -delete
strip -x "$app/Contents/MacOS/pingkk" "$app/Contents/MacOS/pingkk-cli" dist-all/bin/pingkk
codesign --force --deep --sign - "$app"
codesign --verify --deep --strict "$app"
"$app/Contents/MacOS/pingkk" --smoke-test
