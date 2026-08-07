#!/usr/bin/env bash
# Build a portable Linux tarball: a self-contained program directory you can run
# in place (./DuskStudio/DuskStudio) plus an install.sh that does optional system
# integration (PATH symlink + .desktop / MIME / icon registration). Same model
# Reaper ships. Replaces the AppImage.
#
# Prerequisites: BUILD_DIR (default build-linux) already configured + built
# Release, with DuskStudio + dusk-studio-plugin-host artefacts present.
#
# Output: dusk-studio-<version>-Linux-<arch>.tar.xz in the repo root.

set -euo pipefail

# Directory modes come from mkdir -p, which -m cannot set on the intermediate
# components, so the staged tree would otherwise inherit the packager's umask.
umask 022

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_DIR"

VERSION="$(tr -d '[:space:]' < VERSION)"
[[ -n "$VERSION" ]] || { echo "error: VERSION file is empty or missing" >&2; exit 1; }
# Strict triple only, matching bump-version.sh: CMake's project(VERSION)
# rejects prerelease suffixes, so they can never reach the VERSION file.
[[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || {
    echo "error: VERSION must be a semver triple (e.g., 1.2.3), got: $VERSION" >&2
    exit 1
}
BUILD_DIR="${BUILD_DIR:-build-linux}"
ARTEFACTS="$BUILD_DIR/DuskStudio_artefacts/Release"
BINARY="$ARTEFACTS/DuskStudio"
HOST="$ARTEFACTS/dusk-studio-plugin-host"
ICON_SRC="packaging/DuskStudio.png"

# Map uname -m to the asset arch label the rest of the release flow uses.
case "$(uname -m)" in
    x86_64)          ARCH="x86_64" ;;
    aarch64|arm64)   ARCH="aarch64" ;;
    *) echo "error: unsupported arch $(uname -m)" >&2; exit 1 ;;
esac

for f in "$BINARY" "$HOST"; do
    [[ -x "$f" ]] || { echo "error: $f missing - build $BUILD_DIR (Release) first" >&2; exit 1; }
done
[[ -f "$ICON_SRC" ]] || { echo "error: $ICON_SRC missing (256x256 hicolor icon)" >&2; exit 1; }
for f in LICENSE LICENSES.txt; do
    [[ -f "$f" ]] || { echo "error: $f missing - GPL section 4 requires it in the tarball" >&2; exit 1; }
done

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
install -m 0644 packaging/audio.dusk.studio.desktop "$APPDIR/share/applications/"
install -m 0644 packaging/DuskStudio.appdata.xml    "$APPDIR/share/metainfo/"
install -m 0644 packaging/DuskStudio.mime.xml       "$APPDIR/share/mime/packages/"
install -m 0644 "$ICON_SRC"                         "$APPDIR/share/icons/hicolor/256x256/apps/"

# Installer, readme, and the license texts live at the tarball top level, beside
# the program dir. LICENSE + LICENSES.txt are not optional: GPL section 4 makes
# them part of any binary distribution.
install -m 0755 scripts/install-linux.sh "$STAGE/$TOPDIR/install.sh"
install -m 0644 packaging/README-linux.txt "$STAGE/$TOPDIR/README-linux.txt"
install -m 0644 LICENSE                    "$STAGE/$TOPDIR/LICENSE"
install -m 0644 LICENSES.txt               "$STAGE/$TOPDIR/LICENSES.txt"

OUTPUT="${TOPDIR}.tar.xz"
rm -f "$OUTPUT"
tar -C "$STAGE" -cJf "$OUTPUT" "$TOPDIR"

echo "Built: $OUTPUT"
