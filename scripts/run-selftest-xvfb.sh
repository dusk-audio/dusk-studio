#!/usr/bin/env bash

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="${1:-${REPO_ROOT}/build/DuskStudio_artefacts/Release/DuskStudio}"
SELFTEST_TIMEOUT="${DUSKSTUDIO_SELFTEST_TIMEOUT:-90s}"
SELFTEST_KILL_AFTER="${DUSKSTUDIO_SELFTEST_KILL_AFTER:-10s}"

if [[ ! -x "$BIN" ]]; then
    echo "error: Dusk Studio binary is not executable: $BIN" >&2
    exit 1
fi

# macOS has no Wayland session to isolate and does not ship Xvfb or GNU
# timeout. Keep the same canonical entry point for cross-platform dev.sh use.
if [[ "$(uname -s)" == "Darwin" ]]; then
    exec env DUSKSTUDIO_RUN_SELFTEST=1 "$BIN"
fi

if ! command -v Xvfb >/dev/null 2>&1; then
    echo "error: Xvfb is required for the isolated Linux self-test" >&2
    exit 1
fi
if ! command -v timeout >/dev/null 2>&1; then
    echo "error: GNU timeout is required for the isolated Linux self-test" >&2
    exit 1
fi
XVFB_START_ATTEMPTS="${DUSKSTUDIO_XVFB_START_ATTEMPTS:-100}"
if [[ ! "$XVFB_START_ATTEMPTS" =~ ^[1-9][0-9]*$ ]]; then
    echo "error: DUSKSTUDIO_XVFB_START_ATTEMPTS must be a positive integer" >&2
    exit 1
fi

DISPLAY_FILE=""
XVFB_LOG=""
XVFB_PID=""

cleanup() {
    trap - EXIT INT TERM
    if [[ -n "$XVFB_PID" ]]; then
        kill "$XVFB_PID" >/dev/null 2>&1 || true
        wait "$XVFB_PID" 2>/dev/null || true
    fi
    [[ -n "$DISPLAY_FILE" ]] && rm -f "$DISPLAY_FILE"
    [[ -n "$XVFB_LOG" ]] && rm -f "$XVFB_LOG"
    :
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

DISPLAY_FILE=$(mktemp "${TMPDIR:-/tmp}/duskstudio-xvfb-display.XXXXXX")
XVFB_LOG=$(mktemp "${TMPDIR:-/tmp}/duskstudio-xvfb-log.XXXXXX")

Xvfb -displayfd 3 -screen 0 1920x1200x24 -nolisten tcp \
    3>"$DISPLAY_FILE" 2>"$XVFB_LOG" &
XVFB_PID=$!

for ((attempt = 0; attempt < XVFB_START_ATTEMPTS; ++attempt)); do
    [[ -s "$DISPLAY_FILE" ]] && break
    if ! kill -0 "$XVFB_PID" >/dev/null 2>&1; then
        echo "error: Xvfb failed to start:" >&2
        sed 's/^/  /' "$XVFB_LOG" >&2
        exit 1
    fi
    sleep 0.1
done

DISPLAY_NUMBER=""
if ! read -r DISPLAY_NUMBER < "$DISPLAY_FILE" \
    || [[ ! "$DISPLAY_NUMBER" =~ ^[0-9]+$ ]]; then
    echo "error: Xvfb did not report a ready display:" >&2
    sed 's/^/  /' "$XVFB_LOG" >&2
    exit 1
fi
DISPLAY_NUM=":$DISPLAY_NUMBER"

env -u WAYLAND_DISPLAY \
    DISPLAY="$DISPLAY_NUM" \
    DUSKSTUDIO_RUN_SELFTEST=1 \
    timeout --kill-after="$SELFTEST_KILL_AFTER" "$SELFTEST_TIMEOUT" "$BIN"
