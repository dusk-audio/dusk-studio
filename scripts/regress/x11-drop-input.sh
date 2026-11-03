#!/usr/bin/env bash
# XDND drops and a pointer device added after launch, on a bare X server: the
# protocol orders and device changes a desktop rarely produces on demand. Runs
# a private Xvfb, HOME, XDG directories and runtime dir from an empty
# environment, builds the X client in x11_drop_input.c, and lets
# x11_drop_input.py drive the drags and judge them.
#
#   x11-drop-input.sh <DuskStudio binary>
#
# DUSK_DROP_PIPEWIRE_DIR names the PipeWire runtime dir the app should play
# through. Without it the script brings up the private null graph itself, and
# falls back to the machine's graph if that fails, as the other legs do.
# Set DUSK_REGRESS_SCENARIO_KEEP=1 to keep the scratch directory.

set -uo pipefail

APP="${1:?usage: x11-drop-input.sh <DuskStudio binary>}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP="$(cd "$(dirname "$APP")" && pwd)/$(basename "$APP")"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/duskstudio-x11-drop.XXXXXX")" || exit 1
RT="$WORK/rt"
HOME_DIR="$WORK/home"
started_audio=0
xvfb_pid=""

cleanup() {
    local app_pid
    app_pid="$(cat "$WORK/app/app.pid" 2>/dev/null)"
    if [[ "$app_pid" =~ ^[0-9]+$ && "$(readlink "/proc/$app_pid/exe" 2>/dev/null)" == "$APP" ]]; then
        kill -9 "$app_pid" 2>/dev/null
    fi
    if [[ -n "$xvfb_pid" ]]; then
        kill "$xvfb_pid" 2>/dev/null
        wait "$xvfb_pid" 2>/dev/null
    fi
    if ((started_audio)); then null_audio_stop; fi
    if [[ "${DUSK_REGRESS_SCENARIO_KEEP:-0}" == 1 ]]; then
        echo "kept ${WORK}"
    else
        rm -rf "$WORK"
    fi
}
trap cleanup EXIT

mkdir -m 700 "$RT" && mkdir -p "$HOME_DIR/.config" "$HOME_DIR/.local/share" "$HOME_DIR/.local/state" \
    "$HOME_DIR/.cache" "$WORK/files" "$WORK/app" || exit 1

if ! gcc -O1 -o "$WORK/x11_drop_input" "$HERE/x11_drop_input.c" -lX11 -lXi -lXtst 2>"$WORK/gcc.log"; then
    echo "cannot build the X client:"
    sed 's/^/  /' "$WORK/gcc.log"
    exit 1
fi

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
# Imports are renamed, so each fixture has a length of its own (x11_drop_input.py
# FIXTURES) that names it in the autosave.
for name, frames in (("early", 24000), ("local host", 28800), ("plain+plus", 33600), ("late", 38400)):
    with wave.open(os.path.join(root, name + ".wav"), "wb") as out:
        out.setnchannels(1); out.setsampwidth(2); out.setframerate(48000)
        out.writeframes(b"".join(struct.pack("<h", int(8000 * math.sin(2 * math.pi * 440 * i / 48000)))
                                 for i in range(frames)))
EOF

# -noreset keeps the added pointer and the interned atoms should every client
# disconnect for a moment between the steps.
Xvfb -displayfd 3 -screen 0 1920x1080x24 -nolisten tcp -noreset +extension GLX +render \
    3>"$WORK/display" 2>"$WORK/xvfb.log" &
xvfb_pid=$!
for ((i = 0; i < 100; ++i)); do
    [[ -s "$WORK/display" ]] && break
    kill -0 "$xvfb_pid" 2>/dev/null || break
    sleep 0.1
done
display_number="$(cat "$WORK/display" 2>/dev/null)"
if [[ ! "$display_number" =~ ^[0-9]+$ ]]; then
    echo "Xvfb did not start:"
    sed 's/^/  /' "$WORK/xvfb.log"
    exit 1
fi

env -i PATH="/usr/local/bin:/usr/bin:/bin" HOME="$HOME_DIR" USER="$(id -un)" LANG=C.UTF-8 \
    DISPLAY=":$display_number" XDG_RUNTIME_DIR="$RT" XDG_CONFIG_HOME="$HOME_DIR/.config" \
    XDG_DATA_HOME="$HOME_DIR/.local/share" XDG_CACHE_HOME="$HOME_DIR/.cache" \
    XDG_STATE_HOME="$HOME_DIR/.local/state" LIBGL_ALWAYS_SOFTWARE=1 \
    ${pipewire_dir:+PIPEWIRE_RUNTIME_DIR="$pipewire_dir"} \
    timeout 240 python3 -I "$HERE/x11_drop_input.py" drive "$APP" "$WORK/x11_drop_input" "$WORK/app" "$WORK/files"
rc=$?
if [[ "$rc" != 0 ]]; then
    echo "app output:"
    tail -20 "$WORK/app/app.out" 2>/dev/null | sed 's/^/  /'
    tail -20 "$WORK/app/app.err" 2>/dev/null | sed 's/^/  /'
fi
exit "$rc"
