#!/usr/bin/env bash
# Build a portable Linux tarball: a self-contained program directory you can run
# in place (./DuskStudio/DuskStudio) plus an install.sh that does optional system
# integration (PATH symlink + .desktop / MIME / icon registration). Same model
# Reaper ships. Replaces the AppImage.
#
# Prerequisites: BUILD_DIR (default build-linux) already configured + built
# Release, with DuskStudio + dusk-studio-plugin-host artefacts present, and
# assets/ds-icon.png committed. ImageMagick (magick/convert) for the 256x256
# icon.
#
# Output: dusk-studio-<version>-Linux-<arch>.tar.xz in the repo root.

set -euo pipefail

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_DIR"

VERSION="$(tr -d '[:space:]' < VERSION)"
BUILD_DIR="${BUILD_DIR:-build-linux}"
ARTEFACTS="$BUILD_DIR/DuskStudio_artefacts/Release"
BINARY="$ARTEFACTS/DuskStudio"
HOST="$ARTEFACTS/dusk-studio-plugin-host"
ICON_SRC="assets/ds-icon.png"

# Map uname -m to the asset arch label the rest of the release flow uses.
case "$(uname -m)" in
    x86_64)          ARCH="x86_64" ;;
    aarch64|arm64)   ARCH="aarch64" ;;
    *) echo "error: unsupported arch $(uname -m)" >&2; exit 1 ;;
esac

for f in "$BINARY" "$HOST"; do
    [[ -x "$f" ]] || { echo "error: $f missing - build $BUILD_DIR (Release) first" >&2; exit 1; }
done
[[ -f "$ICON_SRC" ]] || { echo "error: $ICON_SRC missing (brand icon)" >&2; exit 1; }

ICON_TOOL=""
command -v magick  >/dev/null 2>&1 && ICON_TOOL="magick"
[[ -z "$ICON_TOOL" ]] && command -v convert >/dev/null 2>&1 && ICON_TOOL="convert"
[[ -n "$ICON_TOOL" ]] || { echo "error: ImageMagick (magick/convert) not on PATH" >&2; exit 1; }

TOPDIR="dusk-studio-${VERSION}-Linux-${ARCH}"
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
APPDIR="$STAGE/$TOPDIR/DuskStudio"
mkdir -p "$APPDIR/share/applications" \
         "$APPDIR/share/metainfo" \
         "$APPDIR/share/mime/packages" \
         "$APPDIR/share/icons/hicolor/256x256/apps"

# Program dir: the binary + the OOP plugin-host helper sit side by side, the
# layout the app resolves the host from at runtime.
install -m 0755 "$BINARY" "$APPDIR/DuskStudio"
install -m 0755 "$HOST"   "$APPDIR/dusk-studio-plugin-host"

# Integration assets (installed by install.sh; ignored for a portable run). The
# .desktop ships a relative Exec=DuskStudio; install.sh rewrites it to the
# installed absolute path.
cp packaging/audio.dusk.studio.desktop "$APPDIR/share/applications/"
cp packaging/DuskStudio.appdata.xml    "$APPDIR/share/metainfo/"
cp packaging/DuskStudio.mime.xml       "$APPDIR/share/mime/packages/"
"$ICON_TOOL" "$ICON_SRC" -resize 256x256 \
    "$APPDIR/share/icons/hicolor/256x256/apps/DuskStudio.png"

# Installer + readme live at the tarball top level, beside the program dir.
install -m 0755 scripts/install-linux.sh "$STAGE/$TOPDIR/install.sh"
cp packaging/README-linux.txt "$STAGE/$TOPDIR/README-linux.txt"

OUTPUT="${TOPDIR}.tar.xz"
rm -f "$OUTPUT"
tar -C "$STAGE" -cJf "$OUTPUT" "$TOPDIR"

echo "Built: $OUTPUT"
