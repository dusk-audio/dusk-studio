#!/usr/bin/env bash
# Make an installed DuskStudio.app self-contained and correctly signed.
#
# Two things break the bundle between the POST_BUILD signature and the DMG:
# the app links Homebrew's lame/libsndfile/lilv/suil from outside the bundle,
# and CMake's bundle install rewrites the Mach-O load commands, which
# invalidates any signature sealed before it. Both are repaired here, on the
# installed copy, so signing is the last thing that touches the binaries.

set -euo pipefail

APP="${1:?usage: finalize-macos-bundle.sh <app> <identity> [entitlements]}"
IDENTITY="${2:?missing codesign identity}"
ENTITLEMENTS="${3:-}"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

"$HERE/bundle-macos-dylibs.sh" "$APP"

# Inside-out: nested code first, then the bundle that seals it. --deep is
# deprecated and does not reliably re-sign what install_name_tool touched.
if [[ -d "$APP/Contents/Frameworks" ]]; then
    # A Frameworks directory can exist without loose dylibs in it; without
    # nullglob the unmatched pattern reaches codesign as a literal path.
    shopt -s nullglob
    for lib in "$APP/Contents/Frameworks"/*.dylib; do
        codesign --force --options runtime --sign "$IDENTITY" "$lib"
    done
fi

# A build configured for the out-of-process plugin sandbox puts
# dusk-studio-plugin-host beside the main executable. Relocation rewrote its
# load commands too, and signing the bundle seals a helper as a resource
# rather than re-signing its own code, so it needs its own pass or it exits
# killed the first time the host spawns it.
MAIN_EXE="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' \
            "$APP/Contents/Info.plist" 2>/dev/null || basename "${APP%.app}")"
# One path, not a name: the find below is recursive, and a nested helper that
# happens to share the main executable's basename still needs signing.
MAIN_EXE_PATH="$APP/Contents/MacOS/$MAIN_EXE"
# The helper needs the same entitlements as the app, not fewer: it is the
# process that loads third-party plugin binaries, so without
# disable-library-validation hardened runtime refuses them, and it cannot even
# map the bundled dylibs beside it.
while IFS= read -r helper; do
    [[ "$helper" == "$MAIN_EXE_PATH" ]] && continue
    if [[ -n "$ENTITLEMENTS" && -f "$ENTITLEMENTS" ]]; then
        codesign --force --options runtime --entitlements "$ENTITLEMENTS" \
                 --sign "$IDENTITY" "$helper"
    else
        codesign --force --options runtime --sign "$IDENTITY" "$helper"
    fi
done < <(find "$APP/Contents/MacOS" -type f -perm -u+x)

# The entitlements carry allow-jit / allow-unsigned-executable-memory /
# disable-library-validation. Re-signing without them strips them, and
# hardened runtime then refuses every third-party plugin the host loads.
if [[ -n "$ENTITLEMENTS" && -f "$ENTITLEMENTS" ]]; then
    codesign --force --options runtime --entitlements "$ENTITLEMENTS" \
             --sign "$IDENTITY" "$APP"
else
    codesign --force --options runtime --sign "$IDENTITY" "$APP"
fi

codesign --verify --strict --deep --verbose=2 "$APP"

# A bundle that verifies can still fail to launch, which is how the v0.13.0 DMG
# shipped: run it with an empty DYLD environment so a stray Homebrew path on
# the build machine cannot satisfy a dependency the bundle is missing.
env -i HOME="$HOME" "$MAIN_EXE_PATH" --version
