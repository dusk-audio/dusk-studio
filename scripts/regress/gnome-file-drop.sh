#!/usr/bin/env bash
# Desktop file drops onto the tape strip, the way they reach the app on GNOME:
# a GTK file drag relayed by Mutter to the app on Xwayland. Runs a private
# headless Mutter with its own session bus, HOME, XDG directories and runtime
# dir, all started from an empty environment, so nothing reaches the desktop
# the runner sits in. gnome_file_drop.py drives the drags and judges them.
#
#   gnome-file-drop.sh <DuskStudio binary>
#
# The session itself, its PipeWire socket and the cleanup come from
# gnome-session.sh; DUSK_REGRESS_PIPEWIRE_DIR and DUSK_REGRESS_SCENARIO_KEEP
# work as described there.

set -uo pipefail

APP="${1:?usage: gnome-file-drop.sh <DuskStudio binary>}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP="$(cd "$(dirname "$APP")" && pwd)/$(basename "$APP")"

# shellcheck source=scripts/regress/gnome-session.sh
source "$HERE/gnome-session.sh"
gnome_session_setup gnome-drop
mkdir -p "$GNOME_WORK/files/one" "$GNOME_WORK/files/many" "$GNOME_WORK/files/midi" || exit 1

python3 -I - "$GNOME_WORK/files" <<'EOF' || exit 1
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

gnome_session_run 300 "$HERE/gnome_file_drop.py" "$APP" "$GNOME_WORK/app" "$GNOME_WORK/files"
