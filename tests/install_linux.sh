#!/usr/bin/env bash
# Runs scripts/install-linux.sh from a synthetic tarball into a throwaway HOME
# and checks the installed tree carries what the running app looks for beside
# its executable. Every XDG home is pointed inside the scratch HOME: a desktop
# session exports them as absolute paths, and the installer rightly follows them.

set -euo pipefail

REPO_ROOT="${1:?usage: install_linux.sh <repo-root>}"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

fail() { echo "FAIL: $*" >&2; exit 1; }

PKG="$WORK/dusk-studio-0.0.0-Linux-x86_64"
APP="$PKG/DuskStudio"
mkdir -p "$APP/share/applications" "$APP/share/icons/hicolor/256x256/apps" \
         "$APP/share/mime/packages" "$APP/share/metainfo"
printf '#!/bin/sh\n' > "$APP/DuskStudio"
printf '#!/bin/sh\n' > "$APP/dusk-studio-plugin-host"
chmod +x "$APP/DuskStudio" "$APP/dusk-studio-plugin-host"
cp "$REPO_ROOT/packaging/audio.dusk.studio.desktop" "$APP/share/applications/"
cp "$REPO_ROOT/packaging/DuskStudio.png" "$APP/share/icons/hicolor/256x256/apps/"
cp "$REPO_ROOT/packaging/DuskStudio.mime.xml" "$APP/share/mime/packages/"
cp "$REPO_ROOT/packaging/DuskStudio.appdata.xml" "$APP/share/metainfo/"
cp "$REPO_ROOT/scripts/install-linux.sh" "$PKG/install.sh"
printf '# Quickstart\n' > "$PKG/QUICKSTART.md"

HOME_DIR="$WORK/home"
mkdir -p "$HOME_DIR"
env -u XDG_DATA_DIRS -u XDG_CONFIG_DIRS \
    HOME="$HOME_DIR" \
    XDG_DATA_HOME="$HOME_DIR/.local/share" \
    XDG_CONFIG_HOME="$HOME_DIR/.config" \
    XDG_CACHE_HOME="$HOME_DIR/.cache" \
    XDG_STATE_HOME="$HOME_DIR/.local/state" \
    sh "$PKG/install.sh" >/dev/null || fail "install.sh exited non-zero"

OPT="$HOME_DIR/.local/opt/dusk-studio"
[[ -x "$OPT/DuskStudio" ]] || fail "the program was not installed"
# Settings > Quickstart looks beside the executable; an install without it
# greys the menu item out.
[[ -f "$OPT/QUICKSTART.md" ]] || fail "the installed app has no QUICKSTART.md beside it"
[[ -f "$HOME_DIR/.local/share/applications/audio.dusk.studio.desktop" ]] \
    || fail "the desktop entry did not land in the scratch XDG data home"

echo "install-linux OK"
