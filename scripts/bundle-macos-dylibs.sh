#!/usr/bin/env bash
# Copy every non-system dylib an .app links against into Contents/Frameworks and
# rewrite the install names to load from there.
#
# The macOS build links Homebrew's lame/libsndfile/lilv/suil, which live under
# /opt/homebrew on the build machine and nowhere on a user's. Those four pull in
# a further transitive set (ogg, vorbis, FLAC, opus, mpg123, serd, sord, sratom,
# zix), so the closure is walked rather than enumerated.
#
# install_name_tool invalidates a code signature, so the caller must sign AFTER
# this runs, not before.

set -euo pipefail

APP="${1:?usage: bundle-macos-dylibs.sh <path to .app>}"
[[ -d "$APP" ]] || { echo "error: no such bundle: $APP" >&2; exit 2; }

MACOS_DIR="$APP/Contents/MacOS"
FRAMEWORKS="$APP/Contents/Frameworks"

# A dependency is "system" if the OS ships it; everything else has to travel
# inside the bundle. @-prefixed entries are already relocated.
is_system() {
    case "$1" in
        /usr/lib/*|/System/*|@*) return 0 ;;
        *) return 1 ;;
    esac
}

deps_of() {
    otool -L "$1" | tail -n +2 | awk '{print $1}'
}

mkdir -p "$FRAMEWORKS"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
PENDING="$WORK/pending"
SEEN="$WORK/seen"
: >"$PENDING"
: >"$SEEN"

# Breadth-first over the dependency graph, seeded with every Mach-O in MacOS/.
# macOS ships bash 3.2, so the queue and the visited set are files rather than
# associative arrays.
find "$MACOS_DIR" -type f -perm -u+x >>"$PENDING"

copied_count=0
while [[ -s "$PENDING" ]]; do
    current="$(head -1 "$PENDING")"
    tail -n +2 "$PENDING" >"$PENDING.rest" && mv "$PENDING.rest" "$PENDING"
    [[ -f "$current" ]] || continue
    while IFS= read -r dep; do
        is_system "$dep" && continue
        base="$(basename "$dep")"
        # A dylib's first otool -L line is its own ID, not a dependency.
        [[ "$base" == "$(basename "$current")" ]] && continue
        grep -qxF "$base" "$SEEN" && continue
        [[ -f "$dep" ]] || { echo "error: dependency not found: $dep" >&2; exit 1; }
        cp "$dep" "$FRAMEWORKS/$base"
        chmod u+w "$FRAMEWORKS/$base"
        printf '%s\n' "$base" >>"$SEEN"
        printf '%s\n' "$FRAMEWORKS/$base" >>"$PENDING"
        copied_count=$((copied_count + 1))
    done < <(deps_of "$current")
done

if [[ $copied_count -eq 0 ]]; then
    echo "no non-system dylibs to bundle"
    rmdir "$FRAMEWORKS" 2>/dev/null || true
    exit 0
fi

# Rewrite in two passes: the bundled dylibs reference each other by @loader_path
# (same directory), the executables reach them via @executable_path.
for lib in "$FRAMEWORKS"/*.dylib; do
    base="$(basename "$lib")"
    install_name_tool -id "@loader_path/$base" "$lib" 2>/dev/null
    while IFS= read -r dep; do
        is_system "$dep" && continue
        depbase="$(basename "$dep")"
        [[ "$depbase" == "$base" ]] && continue
        install_name_tool -change "$dep" "@loader_path/$depbase" "$lib" 2>/dev/null
    done < <(deps_of "$lib")
done

while IFS= read -r bin; do
    while IFS= read -r dep; do
        is_system "$dep" && continue
        install_name_tool -change "$dep" \
            "@executable_path/../Frameworks/$(basename "$dep")" "$bin" 2>/dev/null
    done < <(deps_of "$bin")
done < <(find "$MACOS_DIR" -type f -perm -u+x)

# Nothing may still point outside the bundle, in either the executables or the
# copies themselves; a survivor here is the bug this script exists to prevent.
leaked=0
while IFS= read -r macho; do
    while IFS= read -r dep; do
        is_system "$dep" && continue
        echo "error: unrelocated dependency in $(basename "$macho"): $dep" >&2
        leaked=1
    done < <(deps_of "$macho")
done < <(find "$MACOS_DIR" -type f -perm -u+x; find "$FRAMEWORKS" -name '*.dylib')
[[ $leaked -eq 0 ]] || exit 1

echo "bundled $copied_count dylib(s) into Contents/Frameworks:"
ls "$FRAMEWORKS"
