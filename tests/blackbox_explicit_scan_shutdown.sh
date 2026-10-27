#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$repo_root/scripts/regress/common.sh"
source "$repo_root/scripts/regress/xvfb.sh"
source "$repo_root/scripts/regress/scenarios.sh"
APP_BIN="${1:-$repo_root/build/DuskStudio_artefacts/Release/DuskStudio}"
[[ $# -le 1 && -x "$APP_BIN" ]] || regress_die "usage: $0 [DuskStudio executable]"
if [[ "$(uname -s)" != Linux ]] || ! scenarios_binary_has "dusk-studio-audio-settings"; then
    printf 'SKIP: explicit scan shutdown requires Linux and native settings\n'
    exit "$REGRESS_SKIP_RC"
fi
regress_require Xvfb pipewire python3

bb_begin bb-explicit-scan-shutdown 90
cleanup() {
    local rc=$?
    trap - EXIT INT TERM
    [[ -z "$XVFB_LOG_FILE" || ! -f "$XVFB_LOG_FILE" ]] || cp "$XVFB_LOG_FILE" "$BB_SDIR/xvfb.log"
    [[ -z "$NULL_AUDIO_LOG" || ! -f "$NULL_AUDIO_LOG" ]] || cp "$NULL_AUDIO_LOG" "$BB_SDIR/pipewire.log"
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
export DUSK_REGRESS_REAL_AUDIO=0
null_audio_start || regress_die "private PipeWire graph required"
[[ -S "$NULL_AUDIO_DIR/pipewire-0" ]] || regress_die "private PipeWire socket missing"
sandbox_env "$BB_SDIR"
xvfb_session_start
: >"$BB_SDIR/alsa-denied.conf"
mkdir -p "$BB_SDIR/home/.clap"
: >"$BB_SDIR/home/.clap/blocked.clap"
cat >"$BB_SDIR/blocked-scan-host" <<'HOST'
#!/usr/bin/env bash
printf '%s\n' "$$" >"$DUSKSTUDIO_SCAN_SHUTDOWN_READY"
exec sleep 120
HOST
chmod +x "$BB_SDIR/blocked-scan-host"
bb_spawn A "DUSKSTUDIO_RUN_SCENARIOS=gui:gui.settings_plugin_scan_quit" \
    "DUSKSTUDIO_SCAN_SHUTDOWN_HOST=$BB_SDIR/blocked-scan-host" \
    "DUSKSTUDIO_SCAN_SHUTDOWN_READY=$BB_SDIR/scan-ready" \
    "DUSKSTUDIO_RUN_SELFTEST=" "DUSKSTUDIO_QUIT_AFTER_MS=" "DUSKSTUDIO_LOAD_SESSION=" \
    "DUSKSTUDIO_NATIVE_WAYLAND=" "PIPEWIRE_REMOTE=pipewire-0" \
    "ALSA_CONFIG_PATH=$BB_SDIR/alsa-denied.conf" --
bb_wait_marker A "[scan-shutdown] blocked explicit scan; requesting quit" 60
bb_wait_exit A 15
bb_assert_order A "[scan-shutdown] blocked explicit scan; requesting quit" \
    "[Dusk Studio] plugin-scan worker: done, " "phase 2: stop transport"
python3 - "$BB_SDIR" <<'PY'
from pathlib import Path
import re
import sys

root = Path(sys.argv[1])
stderr = (root / "A.err").read_text()
stdout = (root / "A.out").read_text()
unsafe = (r"std::bad_alloc|terminate called|uncaught exception|Aborted|Segmentation fault|"
          r"AddressSanitizer|runtime error:|\[FAIL\]|\[DIRTY\]")
assert not re.search(unsafe, stderr + stdout, re.IGNORECASE), "unsafe explicit-scan shutdown"
assert not list(root.rglob("crash-*.txt")), "explicit-scan shutdown wrote a crash report"
PY
printf 'PASS: quit cancelled and joined the blocked explicit scanner before exit\n'
