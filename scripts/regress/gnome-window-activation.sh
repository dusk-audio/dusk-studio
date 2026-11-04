#!/usr/bin/env bash
# Whether the main window comes forward on GNOME when the app starts and when a
# second launch hands it a session: with another window in front, with the
# main window minimized, and over an unsaved edit. Runs in a private headless
# GNOME session (gnome-session.sh) where Mutter manages the app's Xwayland
# window; gnome_window_activation.py drives the cases and prints one PASS or
# FAIL line for each.
#
#   gnome-window-activation.sh <DuskStudio binary>
#
# DUSK_REGRESS_PIPEWIRE_DIR and DUSK_REGRESS_SCENARIO_KEEP work as described in
# gnome-session.sh. A failed run keeps its scratch directory, which holds a
# grab of the app window for every case.

set -uo pipefail

APP="${1:?usage: gnome-window-activation.sh <DuskStudio binary>}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP="$(cd "$(dirname "$APP")" && pwd)/$(basename "$APP")"

# shellcheck source=scripts/regress/gnome-session.sh
source "$HERE/gnome-session.sh"
gnome_session_setup gnome-activation

rc=0
gnome_session_run 400 "$HERE/gnome_window_activation.py" "$APP" "$GNOME_WORK/app" || rc=$?
((rc == 0)) || GNOME_KEEP=1
exit "$rc"
