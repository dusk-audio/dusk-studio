#!/usr/bin/env bash
# Desktop file drops onto the tape strip, the way they reach the app on GNOME:
# a GTK file drag relayed by Mutter to the app on Xwayland. Runs a private
# headless Mutter with its own session bus, HOME, XDG directories and runtime
# dir, all started from an empty environment, so nothing reaches the desktop
# the runner sits in. gnome_file_drop.py drives the drags and judges them.
#
#   gnome-file-drop.sh <DuskStudio binary>
#
# DUSK_DROP_PIPEWIRE_DIR names the PipeWire runtime dir the app should play
# through. Without it the script brings up the private null graph itself, and
# falls back to the machine's graph if that fails, as the other legs do.
# Set DUSK_REGRESS_SCENARIO_KEEP=1 to keep the scratch directory.

set -uo pipefail

APP="${1:?usage: gnome-file-drop.sh <DuskStudio binary>}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP="$(cd "$(dirname "$APP")" && pwd)/$(basename "$APP")"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/duskstudio-gnome-drop.XXXXXX")" || exit 1
RT="$WORK/rt"
HOME_DIR="$WORK/home"
started_audio=0

# Everything the session started carries its runtime dir, which is how the
# stragglers (portals, the keyring, Xwayland) are found once Mutter is gone.
cleanup() {
    local pass p
    for pass in TERM KILL; do
        for p in $(pgrep -u "$(id -u)"); do
            { tr '\0' '\n' <"/proc/$p/environ"; } 2>/dev/null | grep -qxF "XDG_RUNTIME_DIR=$RT" \
                && kill "-$pass" "$p" 2>/dev/null
        done
        sleep 0.5
    done
    awk -v rt="$RT/" 'index($2, rt) == 1 { print $2 }' /proc/self/mounts | while read -r mount; do
        fusermount3 -u "$mount" 2>/dev/null || fusermount -u "$mount" 2>/dev/null || true
    done
    if ((started_audio)); then null_audio_stop; fi
    if [[ "${DUSK_REGRESS_SCENARIO_KEEP:-0}" == 1 ]]; then
        echo "kept ${WORK}"
    else
        rm -rf "$WORK"
    fi
}
trap cleanup EXIT

mkdir -m 700 "$RT" && mkdir -p "$HOME_DIR/.config" "$HOME_DIR/.local/share" "$HOME_DIR/.local/state" \
    "$HOME_DIR/.cache" "$WORK/files/one" "$WORK/files/many" "$WORK/files/midi" "$WORK/app" || exit 1

pipewire_dir="${DUSK_DROP_PIPEWIRE_DIR:-}"
if [[ -z "$pipewire_dir" ]]; then
    # shellcheck source=scripts/regress/null-audio.sh
    source "$HERE/null-audio.sh"
    if null_audio_start; then
        pipewire_dir="$NULL_AUDIO_DIR"
        started_audio=1
    else
        pipewire_dir="${PIPEWIRE_RUNTIME_DIR:-${XDG_RUNTIME_DIR:-}}"
    fi
fi
# Without a PipeWire socket the app falls back to ALSA and opens the first sound
# card directly, which is the machine's own.
if [[ ! -S "${pipewire_dir}/pipewire-0" ]]; then
    echo "no PipeWire socket to play through (looked in '${pipewire_dir}')"
    exit 77
fi

python3 -I - "$WORK/files" <<'EOF' || exit 1
import math, os, struct, sys, wave
root = sys.argv[1]
def tone(path, hz):
    with wave.open(path, "wb") as out:
        out.setnchannels(1); out.setsampwidth(2); out.setframerate(48000)
        out.writeframes(b"".join(struct.pack("<h", int(8000 * math.sin(2 * math.pi * hz * i / 48000)))
                                 for i in range(96000)))
tone(os.path.join(root, "one", "solo tone.wav"), 440)
for name, hz in (("bass", 110), ("drums", 220), ("keys", 330)):
    tone(os.path.join(root, "many", name + ".wav"), hz)
events = b""
for note in (60, 62, 64, 65):
    events += b"\x00" + bytes([0x90, note, 100]) + b"\x83\x60" + bytes([0x80, note, 0])
events += b"\x00\xff\x2f\x00"
with open(os.path.join(root, "midi", "melody.mid"), "wb") as f:
    f.write(b"MThd" + struct.pack(">IHHH", 6, 0, 1, 480) + b"MTrk" + struct.pack(">I", len(events)) + events)
EOF

# shellcheck disable=SC2016  # the session command expands its own arguments
env -i PATH="/usr/local/bin:/usr/bin:/bin" HOME="$HOME_DIR" USER="$(id -un)" LANG=C.UTF-8 \
    XDG_RUNTIME_DIR="$RT" XDG_CONFIG_HOME="$HOME_DIR/.config" XDG_DATA_HOME="$HOME_DIR/.local/share" \
    XDG_CACHE_HOME="$HOME_DIR/.cache" XDG_STATE_HOME="$HOME_DIR/.local/state" \
    XDG_SESSION_TYPE=wayland XDG_CURRENT_DESKTOP=GNOME GSETTINGS_BACKEND=memory LIBGL_ALWAYS_SOFTWARE=1 \
    ${pipewire_dir:+PIPEWIRE_RUNTIME_DIR="$pipewire_dir"} \
    timeout 300 dbus-run-session -- \
    mutter --wayland --headless --virtual-monitor 1920x1080 --wayland-display "dusk-drop-$$" -- \
    bash -c 'python3 -I "$1" drive "$2" "$3" "$4" > "$5/driver.log" 2>&1; echo $? > "$5/driver.rc"' \
    drive "$HERE/gnome_file_drop.py" "$APP" "$WORK/app" "$WORK/files" "$WORK" \
    >"$WORK/mutter.log" 2>&1
session_rc=$?

cat "$WORK/driver.log" 2>/dev/null
rc="$(cat "$WORK/driver.rc" 2>/dev/null || echo 1)"
if [[ ! -s "$WORK/driver.rc" ]]; then
    echo "the private GNOME session ended before the driver did:"
    tail -20 "$WORK/mutter.log" | sed 's/^/  /'
elif [[ "$rc" == 0 && "$session_rc" != 0 ]]; then
    echo "the private GNOME session ended with status $session_rc after the driver passed:"
    tail -20 "$WORK/mutter.log" | sed 's/^/  /'
    rc=1
fi
if [[ "$rc" != 0 ]]; then
    echo "app output:"
    tail -20 "$WORK/app/app.out" 2>/dev/null | sed 's/^/  /'
    tail -20 "$WORK/app/app.err" 2>/dev/null | sed 's/^/  /'
fi
exit "$rc"
