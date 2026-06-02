#!/usr/bin/env bash
# Build an UNSIGNED macOS DMG. The .app is ad-hoc signed (codesign -s -,
# which is free) — required for the bundle + its plugin-host helper to
# launch on Apple Silicon — then wrapped in a DMG via cpack -G DragNDrop.
# There is NO Apple Developer ID and NO notarization (by design): users
# bypass Gatekeeper on first launch (right-click -> Open).
#
# Prerequisites:
#   • build/ already configured + built (Release). Configure with
#     -DDUSKSTUDIO_CODESIGN_IDENTITY="-" so CMake ad-hoc signs the .app.
#   • Xcode command-line tools (codesign, cpack).
#
# Usage:
#   scripts/package-macos.sh

set -euo pipefail

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_DIR"

BUILD_DIR="${BUILD_DIR:-build}"
APP_PATH="$BUILD_DIR/DuskStudio_artefacts/Release/DuskStudio.app"

if [[ ! -d "$APP_PATH" ]]; then
    echo "error: $APP_PATH missing - run: cmake -S . -B $BUILD_DIR -DCMAKE_BUILD_TYPE=Release -DDUSKSTUDIO_CODESIGN_IDENTITY=- && cmake --build $BUILD_DIR -j" >&2
    exit 1
fi

ENTITLEMENTS="packaging/macos/entitlements.plist"
if [[ ! -f "$ENTITLEMENTS" ]]; then
    echo "error: $ENTITLEMENTS missing - committed alongside this script" >&2
    exit 1
fi

# Ad-hoc deep-sign the .app. --deep covers the bundled
# Contents/MacOS/dusk-studio-plugin-host helper so the whole bundle
# launches on Apple Silicon. No --options runtime / --timestamp: those
# only matter for notarization, which we don't do.
echo "Ad-hoc codesigning .app ..."
codesign --force --deep \
         --entitlements "$ENTITLEMENTS" \
         --sign - \
         "$APP_PATH"
codesign --verify --deep --strict --verbose=2 "$APP_PATH"

echo "Building DMG ..."
pushd "$BUILD_DIR" >/dev/null
cpack -G DragNDrop -C Release
popd >/dev/null

DMG=$(ls "$BUILD_DIR"/*.dmg | head -1)
if [[ -z "$DMG" ]]; then
    echo "error: cpack produced no .dmg" >&2
    exit 1
fi

mv "$DMG" .
LOCAL_DMG="$(basename "$DMG")"
shasum -a 256 "$LOCAL_DMG" | tee -a SHA256SUMS.macos
echo
echo "Unsigned DMG: $LOCAL_DMG"
