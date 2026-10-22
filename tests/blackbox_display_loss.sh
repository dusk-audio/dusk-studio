#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=scripts/regress/common.sh
source "$repo_root/scripts/regress/common.sh"
# shellcheck source=scripts/regress/xvfb.sh
source "$repo_root/scripts/regress/xvfb.sh"
# shellcheck source=scripts/regress/scenarios.sh
source "$repo_root/scripts/regress/scenarios.sh"
APP_BIN="${1:-$repo_root/build/DuskStudio_artefacts/Release/DuskStudio}"
[[ $# -le 1 && -x "$APP_BIN" ]] || regress_die "usage: $0 [DuskStudio executable]"
if [[ "$(uname -s)" != Linux ]] || ! scenarios_binary_has "dusk-studio-audio-editor"; then
    printf 'SKIP: display-loss guard requires Linux and the native audio editor\n'
    exit "$REGRESS_SKIP_RC"
fi
regress_require Xvfb pipewire sha256sum cmp python3

bb_begin bb-display-loss 90
cleanup() {
    local rc=$?
    trap - EXIT INT TERM
    [[ -z "$XVFB_LOG_FILE" || ! -f "$XVFB_LOG_FILE" ]] \
        || cp "$XVFB_LOG_FILE" "$BB_SDIR/xvfb.log"
    [[ -z "$NULL_AUDIO_LOG" || ! -f "$NULL_AUDIO_LOG" ]] \
        || cp "$NULL_AUDIO_LOG" "$BB_SDIR/pipewire.log"
    if ((rc != 0)); then DUSK_REGRESS_SCENARIO_KEEP=1; fi
    bb_end "$rc" || true
    xvfb_session_stop
    null_audio_stop
    exit "$rc"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
ulimit -c 0

# This test must never fall back to the machine's audio graph.
export DUSK_REGRESS_REAL_AUDIO=0
null_audio_start || regress_die "private PipeWire graph required"
[[ -S "$NULL_AUDIO_DIR/pipewire-0" ]] || regress_die "private PipeWire socket missing"
sandbox_env "$BB_SDIR"
xvfb_session_start
printf '%s\n' "$XVFB_PID" >"$BB_SDIR/xvfb.pid"
printf '%s\n' "$NULL_AUDIO_PID" >"$BB_SDIR/pipewire.pid"
sha256sum "$APP_BIN" >"$BB_SDIR/app.sha256"
# No ALSA definitions means fallback cannot open hardware through libasound.
: >"$BB_SDIR/alsa-denied.conf"

bb_spawn A "DUSKSTUDIO_RUN_SCENARIOS=gui:gui.timeline_track_move" \
    "DUSKSTUDIO_RUN_SELFTEST=" "DUSKSTUDIO_QUIT_AFTER_MS=" \
    "DUSKSTUDIO_LOAD_SESSION=" "DUSKSTUDIO_NATIVE_WAYLAND=" \
    "PIPEWIRE_REMOTE=pipewire-0" "ALSA_CONFIG_PATH=$BB_SDIR/alsa-denied.conf" --
printf '%s\n' "${BB_PID[A]}" >"$BB_SDIR/app.pid"
bb_wait_marker A "[Dusk Studio/audio-editor] GL vendor=" 60
bb_alive A || regress_die "app exited before display loss"
kill -0 "$NULL_AUDIO_PID" || regress_die "private PipeWire graph exited"
bb_assert_marker A "[Dusk Studio/PipeWire] opened"
bb_assert_marker A "[RUN] gui.timeline_track_move"
if grep -qE '^=== scenarios:|^\[(PASS|FAIL|SKIP)\] gui\.timeline_track_move' "$BB_SDIR/A.out"; then
    regress_die "GuiRun completed before display loss"
fi

loss_start=$SECONDS
kill -KILL "$XVFB_PID"
wait "$XVFB_PID" 2>/dev/null || true
XVFB_PID=""
app_rc=0
bb_wait_exit A 10 || app_rc=$?
printf '%s\n' "${BB_EXIT[A]:-not-exited}" >"$BB_SDIR/native-exit.txt"
printf '%s\n' "$((SECONDS - loss_start))" >"$BB_SDIR/display-loss-seconds.txt"
sha256sum "$APP_BIN" >"$BB_SDIR/app-after.sha256"
cmp -s "$BB_SDIR/app.sha256" "$BB_SDIR/app-after.sha256" \
    || regress_die "app executable changed during display-loss test"
[[ "$app_rc" == 1 && "${BB_EXIT[A]:-}" == 1 ]] \
    || regress_die "display loss must exit 1 promptly; observed ${BB_EXIT[A]:-no native exit}"
kill -0 "$NULL_AUDIO_PID" || regress_die "private PipeWire graph exited during display loss"

python3 - "$BB_SDIR" <<'PY'
from pathlib import Path
import re
import sys

root = Path(sys.argv[1])
stderr = (root / "A.err").read_bytes()
stdout = (root / "A.out").read_bytes()
unsafe = (rb"std::bad_alloc|terminate called|uncaught exception|Aborted|Segmentation fault|"
          rb"AddressSanitizer|runtime error:|phase 1: stop autosave timer|"
          rb"interrupted save cleanup")
assert not re.search(unsafe, stderr + stdout, re.IGNORECASE), "unsafe cleanup or exception after display loss"
assert not re.search(rb"^=== scenarios:|^\[(PASS|FAIL|SKIP)\] gui\.timeline_track_move",
                     stdout, re.MULTILINE), "interrupted GuiRun reported completion"
assert not list(root.rglob("crash-*.txt")), "display loss wrote a crash report"
PY
printf 'PASS: interrupted track move exited 1 after losing its private X11 display\n'
