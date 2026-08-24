#!/bin/bash
# Private headless Wayland compositor for running the GUI spike, or any future
# framework-native Dusk surface, without touching the live desktop session.
#
# The live session must never host these windows: a crash in a Dusk-owned surface takes
# the session with it. This starts a mutter of its own with its own runtime directory,
# its own dbus session and its own Wayland display name, so a client can only reach it
# by being pointed at it explicitly.
#
#   tools/gui-spike/headless-compositor.sh &
#   XDG_RUNTIME_DIR=/tmp/dusk-headless WAYLAND_DISPLAY=dusk-headless \
#     env -u DISPLAY ./build-spike/gui-spike/dusk-gui-spike --seconds 10
#
# The runtime directory is short on purpose: a wayland socket path cannot exceed 108
# bytes, which a build or scratch directory easily does.
set -euo pipefail

RUNTIME_DIR="${DUSK_HEADLESS_RUNTIME_DIR:-/tmp/dusk-headless}"
DISPLAY_NAME="${DUSK_HEADLESS_DISPLAY:-dusk-headless}"
MONITOR="${DUSK_HEADLESS_MONITOR:-1920x1080}"

if ! command -v mutter >/dev/null; then
    echo "mutter is not installed; a nested compositor (weston, cage) would also work" >&2
    exit 1
fi

mkdir -p "$RUNTIME_DIR"
chmod 700 "$RUNTIME_DIR"

export XDG_RUNTIME_DIR="$RUNTIME_DIR"
unset WAYLAND_DISPLAY DISPLAY DBUS_SESSION_BUS_ADDRESS

exec dbus-run-session -- mutter --headless --no-x11 \
    --wayland-display="$DISPLAY_NAME" --virtual-monitor "$MONITOR"
