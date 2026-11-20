#!/usr/bin/env bash
# Every MASTERING list picked from with the compositor's own pointer, under native
# Wayland and under X11 through Xwayland, in a private headless GNOME session.
set -uo pipefail

APP="${1:?usage: native-mastering-lists.sh <DuskStudio binary>}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP="$(cd "$(dirname "$APP")" && pwd)/$(basename "$APP")"
# shellcheck source=scripts/regress/gnome-session.sh
source "$HERE/gnome-session.sh"
gnome_session_setup native-mastering-lists
rc=0
gnome_session_run 600 "$HERE/native_mastering_lists.py" "$APP" "$GNOME_WORK/app" || rc=$?
((rc == 0)) || GNOME_KEEP=1
exit "$rc"
