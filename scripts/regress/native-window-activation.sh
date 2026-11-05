#!/usr/bin/env bash
# Real launcher tokens and session handoffs with Xwayland disabled.
set -uo pipefail

APP="${1:?usage: native-window-activation.sh <DuskStudio binary>}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP="$(cd "$(dirname "$APP")" && pwd)/$(basename "$APP")"

# shellcheck source=scripts/regress/gnome-session.sh
source "$HERE/gnome-session.sh"
gnome_session_setup native-activation
GNOME_NATIVE_ONLY=1
rc=0
gnome_session_run 300 "$HERE/native_window_activation.py" "$APP" "$GNOME_WORK/app" || rc=$?
((rc == 0)) || GNOME_KEEP=1
exit "$rc"
