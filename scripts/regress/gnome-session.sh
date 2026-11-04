# shellcheck shell=bash
# A private headless GNOME session for the legs that need a real compositor:
# Mutter with Xwayland, its own session bus, HOME, XDG directories and runtime
# dir, all started from an empty environment, so nothing reaches the desktop
# the runner sits in. Sourced by gnome-file-drop.sh and
# gnome-window-activation.sh, never run.
#
# DUSK_REGRESS_PIPEWIRE_DIR names the PipeWire runtime dir the app should play
# through. Without it gnome_session_setup brings up the private null graph
# itself, and falls back to the machine's graph if that fails, as the other
# legs do. Set DUSK_REGRESS_SCENARIO_KEEP=1 to keep the scratch directory.

GNOME_SESSION_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GNOME_WORK=""
GNOME_RT=""
GNOME_HOME=""
GNOME_PIPEWIRE_DIR=""
GNOME_KEEP=0
gnome_started_audio=0

# Everything the session started carries its runtime dir, which is how the
# stragglers (portals, the keyring, Xwayland) are found once Mutter is gone.
gnome_session_cleanup() {
    local pass p
    for pass in TERM KILL; do
        for p in $(pgrep -u "$(id -u)"); do
            { tr '\0' '\n' <"/proc/$p/environ"; } 2>/dev/null | grep -qxF "XDG_RUNTIME_DIR=$GNOME_RT" \
                && kill "-$pass" "$p" 2>/dev/null
        done
        sleep 0.5
    done
    awk -v rt="$GNOME_RT/" 'index($2, rt) == 1 { print $2 }' /proc/self/mounts | while read -r mount; do
        fusermount3 -u "$mount" 2>/dev/null || fusermount -u "$mount" 2>/dev/null || true
    done
    if ((gnome_started_audio)); then null_audio_stop; fi
    if [[ "${DUSK_REGRESS_SCENARIO_KEEP:-0}" == 1 || "$GNOME_KEEP" == 1 ]]; then
        echo "kept ${GNOME_WORK}"
    else
        rm -rf "$GNOME_WORK"
    fi
}

# gnome_session_setup <name>: the scratch directory, its private HOME and
# runtime dir, and a PipeWire socket; exits 77 when there is no socket to play
# through.
gnome_session_setup() {
    GNOME_WORK="$(mktemp -d "${TMPDIR:-/tmp}/duskstudio-$1.XXXXXX")" || exit 1
    GNOME_RT="$GNOME_WORK/rt"
    GNOME_HOME="$GNOME_WORK/home"
    trap gnome_session_cleanup EXIT
    mkdir -m 700 "$GNOME_RT" && mkdir -p "$GNOME_HOME/.config" "$GNOME_HOME/.local/share" \
        "$GNOME_HOME/.local/state" "$GNOME_HOME/.cache" "$GNOME_WORK/app" || exit 1

    GNOME_PIPEWIRE_DIR="${DUSK_REGRESS_PIPEWIRE_DIR:-}"
    if [[ -z "$GNOME_PIPEWIRE_DIR" ]]; then
        # shellcheck source=scripts/regress/null-audio.sh
        source "$GNOME_SESSION_DIR/null-audio.sh"
        if null_audio_start; then
            GNOME_PIPEWIRE_DIR="$NULL_AUDIO_DIR"
            gnome_started_audio=1
        else
            GNOME_PIPEWIRE_DIR="${PIPEWIRE_RUNTIME_DIR:-${XDG_RUNTIME_DIR:-}}"
        fi
    fi
    # Without a PipeWire socket the app falls back to ALSA and opens the first
    # sound card directly, which is the machine's own.
    if [[ ! -S "${GNOME_PIPEWIRE_DIR}/pipewire-0" ]]; then
        echo "no PipeWire socket to play through (looked in '${GNOME_PIPEWIRE_DIR}')"
        exit 77
    fi
}

# gnome_session_run <seconds> <driver.py> <args...>: runs
# `python3 -I <driver.py> drive <args...>` as the session command, prints its
# log and, when it failed, the app's output from $GNOME_WORK/app. Returns the
# driver's status, or 1 when the session ended before the driver did.
gnome_session_run() {
    local seconds="$1"
    shift
    # shellcheck disable=SC2016  # the session command expands its own arguments
    env -i PATH="/usr/local/bin:/usr/bin:/bin" HOME="$GNOME_HOME" USER="$(id -un)" LANG=C.UTF-8 \
        XDG_RUNTIME_DIR="$GNOME_RT" XDG_CONFIG_HOME="$GNOME_HOME/.config" XDG_DATA_HOME="$GNOME_HOME/.local/share" \
        XDG_CACHE_HOME="$GNOME_HOME/.cache" XDG_STATE_HOME="$GNOME_HOME/.local/state" \
        XDG_SESSION_TYPE=wayland XDG_CURRENT_DESKTOP=GNOME GSETTINGS_BACKEND=memory LIBGL_ALWAYS_SOFTWARE=1 \
        ${GNOME_PIPEWIRE_DIR:+PIPEWIRE_RUNTIME_DIR="$GNOME_PIPEWIRE_DIR"} \
        timeout "$seconds" dbus-run-session -- \
        mutter --wayland --headless --virtual-monitor 1920x1080 --wayland-display "dusk-regress-$$" -- \
        bash -c 'w="$1"; shift; python3 -I "$@" > "$w/driver.log" 2>&1; echo $? > "$w/driver.rc"' \
        drive "$GNOME_WORK" "$1" drive "${@:2}" \
        >"$GNOME_WORK/mutter.log" 2>&1
    local session_rc=$?

    cat "$GNOME_WORK/driver.log" 2>/dev/null
    local rc
    rc="$(cat "$GNOME_WORK/driver.rc" 2>/dev/null || echo 1)"
    if [[ ! -s "$GNOME_WORK/driver.rc" ]]; then
        echo "the private GNOME session ended before the driver did:"
        tail -20 "$GNOME_WORK/mutter.log" | sed 's/^/  /'
        rc=1
    elif [[ "$rc" == 0 && "$session_rc" != 0 ]]; then
        echo "the private GNOME session ended with status $session_rc after the driver passed:"
        tail -20 "$GNOME_WORK/mutter.log" | sed 's/^/  /'
        rc=1
    fi
    if [[ "$rc" != 0 ]]; then
        echo "app output:"
        tail -20 "$GNOME_WORK/app/app.out" 2>/dev/null | sed 's/^/  /'
        tail -20 "$GNOME_WORK/app/app.err" 2>/dev/null | sed 's/^/  /'
    fi
    return "$rc"
}
