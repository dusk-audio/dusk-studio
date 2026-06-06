#!/usr/bin/env bash
# Build a Linux AppImage from build-linux/. Wraps the procedure
# documented in packaging/README.md so a tagged release reduces to one
# command. Run on Ubuntu 22.04 (matches the existing donor CI image).
#
# Prerequisites:
#   • build-linux/ already configured + built (Release).
#   • linuxdeploy on $PATH.
#   • assets/ds-icon.png present (committed). This script downscales it to
#     256x256 with ImageMagick (magick/convert), or falls back to a straight
#     copy if ImageMagick is absent; linuxdeploy only renames + packages the
#     already-prepared icon.
#
# Output:
#   dusk-studio-<version>-Linux-x86_64.AppImage in the repo root.

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
# .desktop declares Icon=DuskStudio so the file at the hicolor path must be
# named DuskStudio.png. The source is 500x500; the 256x256 slot must hold a
# TRUE 256x256 image or icon validators (and some desktops) flag the mismatch,
# so downscale into it. ImageMagick is required — a native-size copy ships a
# malformed icon, so hard-fail rather than warn-and-continue.
ICON_256="AppDir/usr/share/icons/hicolor/256x256/apps/DuskStudio.png"
if command -v magick >/dev/null 2>&1; then
    magick "$ICON_SRC" -resize 256x256 "$ICON_256"
elif command -v convert >/dev/null 2>&1; then
    convert "$ICON_SRC" -resize 256x256 "$ICON_256"
else
    echo "error: ImageMagick (magick/convert) not on PATH - cannot produce the 256x256 icon. Install imagemagick before packaging." >&2
    exit 1
fi

OUTPUT="dusk-studio-${VERSION}-Linux-x86_64.AppImage"
export OUTPUT
# linuxdeploy derives the AppImage's root icon name from the basename of
# --icon-file; the .desktop file's Icon= field must match. Copy the
# already-resized ICON_256 to a basename of "DuskStudio.png" so linuxdeploy +
# .desktop agree AND the root icon is the 256x256 image (not the 500x500
# source), without renaming the committed asset.
TMP_ICON_DIR="$(mktemp -d)"
# Remove the temp icon dir on ANY exit. With set -e a linuxdeploy failure
# bails before a trailing rm would run, so a plain cleanup line would leak
# the dir; the trap fires on success, error, and interrupt alike.
trap 'rm -rf "$TMP_ICON_DIR"' EXIT
TMP_ICON="$TMP_ICON_DIR/DuskStudio.png"
cp "$ICON_256" "$TMP_ICON"
linuxdeploy --appdir AppDir \
            --desktop-file packaging/audio.dusk.studio.desktop \
            --icon-file    "$TMP_ICON" \
            --output       appimage

echo
echo "Built: $OUTPUT"
