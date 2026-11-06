#!/usr/bin/env bash
set -uo pipefail

APP="${1:?usage: native-gui-scenarios.sh <DuskStudio binary> [--without-x11]}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP="$(cd "$(dirname "$APP")" && pwd)/$(basename "$APP")"
# shellcheck source=scripts/regress/gnome-session.sh
source "$HERE/gnome-session.sh"
gnome_session_setup native-gui
case "${2:-}" in
    "") ;;
    --without-x11) GNOME_NATIVE_ONLY=1 ;;
    *) echo "unknown argument: $2" >&2; exit 2 ;;
esac
rc=0
gnome_session_run 180 "$HERE/native_gui_scenarios.py" "$APP" "$GNOME_WORK/app" || rc=$?
((rc == 0)) || GNOME_KEEP=1
exit "$rc"
