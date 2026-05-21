#!/usr/bin/env bash
# Build a signed + notarized DMG. Codesigns the .app, packages via
# cpack -G DragNDrop, codesigns the DMG, submits to Apple's notary
# service via notarytool, and staples the ticket. Run on macOS 14+
# with Xcode command-line tools.
#
# Prerequisites:
#   • build/ already configured + built (Release).
#   • An Apple Developer ID Application certificate in the login
#     keychain. Identity expressed by name (e.g. "Developer ID
#     Application: Dusk Audio (TEAMID)") via $CODESIGN_IDENTITY.
#   • A keychain profile created with `xcrun notarytool store-credentials
#     <name>` so notarytool can authenticate without an env-var
#     password on disk. Profile name expressed via $APPLE_NOTARY_PROFILE.
#   • A real Apple Developer team. The script REFUSES to run if either
#     env var is empty - notarization is not optional for 1.0
#     distribution.
#
# Usage:
#   CODESIGN_IDENTITY="Developer ID Application: ..." \
#   APPLE_NOTARY_PROFILE=duskstudio-notary \
#   scripts/package-macos.sh

set -euo pipefail

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_DIR"

: "${CODESIGN_IDENTITY:?Set CODESIGN_IDENTITY to your Developer ID identity name (security find-identity -v -p codesigning)}"
: "${APPLE_NOTARY_PROFILE:?Set APPLE_NOTARY_PROFILE to a keychain profile saved with xcrun notarytool store-credentials}"

BUILD_DIR="${BUILD_DIR:-build}"
APP_PATH="$BUILD_DIR/DuskStudio_artefacts/Release/DuskStudio.app"

if [[ ! -d "$APP_PATH" ]]; then
    echo "error: $APP_PATH missing - run: cmake -S . -B $BUILD_DIR -DCMAKE_BUILD_TYPE=Release && cmake --build $BUILD_DIR -j" >&2
    exit 1
fi

ENTITLEMENTS="packaging/macos/entitlements.plist"
if [[ ! -f "$ENTITLEMENTS" ]]; then
    echo "error: $ENTITLEMENTS missing - committed alongside this script" >&2
    exit 1
fi

# 1. Deep-codesign the .app. --options runtime enables Hardened Runtime,
#    required for notarization. --deep IS retained here (despite Apple's
#    preference for per-binary signing) because the bundle contains a
#    helper at Contents/MacOS/dusk-studio-plugin-host that must inherit
#    the same Hardened Runtime entitlements as the host. The CMake
#    post-build sign step inside CMakeLists.txt avoided --deep on
#    purpose (JUCE had already signed inner Mach-Os); the release-time
#    re-sign here intentionally re-signs everything together so the
#    distributable .app has one cohesive signature and entitlement set.
echo "Codesigning .app ..."
codesign --force --deep --timestamp \
         --options runtime \
         --entitlements "$ENTITLEMENTS" \
         --sign "$CODESIGN_IDENTITY" \
         "$APP_PATH"
codesign --verify --deep --strict --verbose=2 "$APP_PATH"

# 2. Build the DMG.
echo "Building DMG ..."
pushd "$BUILD_DIR" >/dev/null
cpack -G DragNDrop -C Release
popd >/dev/null

DMG=$(ls "$BUILD_DIR"/*.dmg | head -1)
if [[ -z "$DMG" ]]; then
    echo "error: cpack produced no .dmg" >&2
    exit 1
fi

# 3. Codesign the DMG too (notarization requires a signed container).
codesign --force --timestamp --sign "$CODESIGN_IDENTITY" "$DMG"

# 4. Notarize. xcrun notarytool blocks until the service finishes
#    (success or failure); --wait prevents the script from racing
#    ahead before the ticket is available.
echo "Submitting to notary service ..."
xcrun notarytool submit "$DMG" \
                 --keychain-profile "$APPLE_NOTARY_PROFILE" \
                 --wait

# 5. Staple the ticket so the DMG works offline.
xcrun stapler staple "$DMG"
xcrun stapler validate "$DMG"

# 6. Move to repo root + checksum.
mv "$DMG" .
LOCAL_DMG="$(basename "$DMG")"
shasum -a 256 "$LOCAL_DMG" | tee -a SHA256SUMS.macos
echo
echo "Notarized DMG: $LOCAL_DMG"
