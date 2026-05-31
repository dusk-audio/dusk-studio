#!/usr/bin/env bash
# Build a Linux AppImage from build-linux/. Wraps the procedure
# documented in packaging/README.md so a tagged release reduces to one
# command. Run on Ubuntu 22.04 (matches the existing donor CI image).
#
# Prerequisites:
#   • build-linux/ already configured + built (Release).
#   • linuxdeploy on $PATH.
#   • assets/ds-icon.png present (committed; downscaled by linuxdeploy
#     to the 256x256 hicolor slot).
#
# Output:
#   DuskStudio-<version>-x86_64.AppImage in the repo root.

set -euo pipefail

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_DIR"

VERSION="$(tr -d '[:space:]' < VERSION)"
BUILD_DIR="${BUILD_DIR:-build-linux}"
BINARY="$BUILD_DIR/DuskStudio_artefacts/Release/DuskStudio"

if [[ ! -x "$BINARY" ]]; then
    echo "error: $BINARY missing - run: cmake -S . -B $BUILD_DIR -G Ninja -DCMAKE_BUILD_TYPE=Release -DDUSKSTUDIO_PACKAGE_BUILD=ON && cmake --build $BUILD_DIR -j" >&2
    echo "       (DUSKSTUDIO_PACKAGE_BUILD=ON enforces packaging-asset checks at configure time.)" >&2
    exit 1
fi
if ! command -v linuxdeploy >/dev/null 2>&1; then
    echo "error: linuxdeploy not on PATH - grab from https://github.com/linuxdeploy/linuxdeploy/releases" >&2
    exit 1
fi
ICON_SRC="assets/ds-icon.png"
if [[ ! -f "$ICON_SRC" ]]; then
    echo "error: $ICON_SRC missing (brand icon)" >&2
    exit 1
fi

rm -rf AppDir
mkdir -p AppDir/usr/bin \
         AppDir/usr/share/applications \
         AppDir/usr/share/metainfo \
         AppDir/usr/share/mime/packages \
         AppDir/usr/share/icons/hicolor/256x256/apps

cp "$BINARY"                                       AppDir/usr/bin/DuskStudio
cp packaging/audio.dusk.studio.desktop             AppDir/usr/share/applications/
cp packaging/DuskStudio.appdata.xml                AppDir/usr/share/metainfo/
cp packaging/DuskStudio.mime.xml                   AppDir/usr/share/mime/packages/
# .desktop file declares Icon=DuskStudio so the file at the hicolor path
# must be named DuskStudio.png regardless of the source filename. The
# 500x500 source gets downscaled by linuxdeploy / freedesktop tooling
# when the WM picks a smaller cache size.
cp "$ICON_SRC"                                     AppDir/usr/share/icons/hicolor/256x256/apps/DuskStudio.png

OUTPUT="DuskStudio-${VERSION}-x86_64.AppImage"
export OUTPUT
# linuxdeploy derives the AppImage's root icon name from the basename of
# --icon-file; the .desktop file's Icon= field must match. Copy the
# source to a basename of "DuskStudio.png" so linuxdeploy + .desktop
# agree without renaming the committed asset.
TMP_ICON="$(mktemp -d)/DuskStudio.png"
cp "$ICON_SRC" "$TMP_ICON"
linuxdeploy --appdir AppDir \
            --desktop-file packaging/audio.dusk.studio.desktop \
            --icon-file    "$TMP_ICON" \
            --output       appimage
rm -rf "$(dirname "$TMP_ICON")"

echo
echo "Built: $OUTPUT"
sha256sum "$OUTPUT" | tee "$OUTPUT.sha256"
