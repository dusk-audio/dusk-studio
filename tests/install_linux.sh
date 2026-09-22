#!/usr/bin/env bash
# Runs scripts/install-linux.sh from a synthetic tarball into a throwaway HOME
# and checks the installed tree carries what the running app looks for beside
# its executable, that --uninstall takes every installed path back out, and that
# --system without root and an unknown option are refused as documented.

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

# One environment for every user-scope run. Every XDG home is pointed inside the
# scratch HOME: a desktop session exports them as absolute paths, and the
# installer rightly follows them.
scratch_install() {
    env -u XDG_DATA_DIRS -u XDG_CONFIG_DIRS \
        HOME="$HOME_DIR" \
        XDG_DATA_HOME="$HOME_DIR/.local/share" \
        XDG_CONFIG_HOME="$HOME_DIR/.config" \
        XDG_CACHE_HOME="$HOME_DIR/.cache" \
        XDG_STATE_HOME="$HOME_DIR/.local/state" \
        sh "$PKG/install.sh" "$@"
}

scratch_install >/dev/null || fail "install.sh exited non-zero"

OPT="$HOME_DIR/.local/opt/dusk-studio"
DATA="$HOME_DIR/.local/share"
LAUNCHER="$HOME_DIR/.local/bin/DuskStudio"
DESKTOP="$DATA/applications/audio.dusk.studio.desktop"
ICON="$DATA/icons/hicolor/256x256/apps/DuskStudio.png"
MIME="$DATA/mime/packages/DuskStudio.mime.xml"
META="$DATA/metainfo/DuskStudio.appdata.xml"

[[ -x "$OPT/DuskStudio" ]] || fail "the program was not installed"
# Settings > Quickstart looks beside the executable; an install without it
# greys the menu item out.
[[ -f "$OPT/QUICKSTART.md" ]] || fail "the installed app has no QUICKSTART.md beside it"
[[ -L "$LAUNCHER" ]] || fail "the launcher did not land in the scratch bin dir"
[[ -f "$DESKTOP" ]] || fail "the desktop entry did not land in the scratch XDG data home"
[[ -f "$ICON" ]] || fail "the icon did not land in the scratch XDG data home"
[[ -f "$MIME" ]] || fail "the MIME package did not land in the scratch XDG data home"
[[ -f "$META" ]] || fail "the AppStream metadata did not land in the scratch XDG data home"

scratch_install --uninstall >/dev/null || fail "install.sh --uninstall exited non-zero"
for path in "$OPT" "$LAUNCHER" "$DESKTOP" "$ICON" "$MIME" "$META"; do
    if [[ -e "$path" || -L "$path" ]]; then fail "--uninstall left $path behind"; fi
done

if [[ "$(id -u)" != 0 ]]; then
    out="$(scratch_install --system 2>&1 >/dev/null)" && status=0 || status=$?
    [[ "$status" -eq 1 ]] || fail "--system without root exited $status, expected 1"
    [[ "$out" == "error: --system needs root (use sudo)" ]] \
        || fail "--system without root printed: $out"
fi

out="$(scratch_install --frobnicate 2>&1 >/dev/null)" && status=0 || status=$?
[[ "$status" -eq 2 ]] || fail "an unknown option exited $status, expected 2"
[[ "$out" == "unknown option: --frobnicate" ]] || fail "an unknown option printed: $out"

# The documented `sudo ./install.sh --system` path. A user namespace reports
# id -u as 0, and tmpfs over the three system dirs keeps every write inside the
# namespace, so the real install never touches the host. A kernel that refuses
# an unprivileged user namespace, or a missing mount point, skips instead.
cat > "$WORK/system-scope.sh" <<'INNER'
set -eu
PKG=$1
for dir in /opt /usr/local/bin /usr/share; do
    mount -t tmpfs tmpfs "$dir" 2>/dev/null || exit 77
done
sh "$PKG/install.sh" --system >/dev/null
[ -x /opt/dusk-studio/DuskStudio ] || { echo "no program dir" >&2; exit 1; }
[ -L /usr/local/bin/DuskStudio ] || { echo "no launcher" >&2; exit 1; }
[ -f /usr/share/applications/audio.dusk.studio.desktop ] || { echo "no desktop entry" >&2; exit 1; }
sh "$PKG/install.sh" --system --uninstall >/dev/null
for path in /opt/dusk-studio /usr/local/bin/DuskStudio \
            /usr/share/applications/audio.dusk.studio.desktop; do
    if [ -e "$path" ] || [ -L "$path" ]; then echo "left behind: $path" >&2; exit 1; fi
done
INNER
if command -v unshare >/dev/null 2>&1 && unshare --map-root-user --mount true 2>/dev/null; then
    status=0
    unshare --map-root-user --mount sh "$WORK/system-scope.sh" "$PKG" || status=$?
    if [[ "$status" -eq 77 ]]; then
        echo "skip: the system dirs cannot be shadowed with tmpfs here"
    elif [[ "$status" -ne 0 ]]; then
        fail "the --system install round-trip exited $status"
    fi
else
    echo "skip: no usable user namespace; the --system install is walked by hand"
fi

echo "install-linux OK"
