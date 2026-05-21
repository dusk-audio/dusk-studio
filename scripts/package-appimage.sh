#!/usr/bin/env bash
# Build a Linux AppImage from build-linux/. Wraps the procedure
# documented in packaging/README.md so a tagged release reduces to one
# command. Run on Ubuntu 22.04 (matches the existing donor CI image).
#
# Prerequisites:
#   • build-linux/ already configured + built (Release).
#   • linuxdeploy on $PATH.
#   • packaging/DuskStudio.png present (256x256 PNG, NOT in repo).
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
if [[ ! -f packaging/DuskStudio.png ]]; then
    echo "error: packaging/DuskStudio.png missing (256x256 brand icon)" >&2
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
cp packaging/DuskStudio.png                        AppDir/usr/share/icons/hicolor/256x256/apps/

OUTPUT="DuskStudio-${VERSION}-x86_64.AppImage"
export OUTPUT
linuxdeploy --appdir AppDir \
            --desktop-file packaging/audio.dusk.studio.desktop \
            --icon-file    packaging/DuskStudio.png \
            --output       appimage

echo
echo "Built: $OUTPUT"
sha256sum "$OUTPUT" | tee "$OUTPUT.sha256"
