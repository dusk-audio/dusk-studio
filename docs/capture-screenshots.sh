#!/usr/bin/env bash
# Drive Dusk Studio's built-in capture harness to (re)generate manual figures.
#
# Produces the ✅ rows in docs/screenshot-list.md into docs/images/. The
# remaining rows (MTC dropdown, OS file browser, offline-plugin slot) are
# captured by hand — see that file.
#
# Runs the app on an ISOLATED Xvfb display (X11 backend), NOT your live
# session — the harness drives heavy stage switches + modal teardown that can
# crash a real Wayland compositor. createComponentSnapshot is software raster,
# so no GPU / Wayland surface is needed.
#
# Requires: Xvfb  (openSUSE: sudo zypper install xorg-x11-server-Xvfb)

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="${REPO_ROOT}/build/DuskStudio_artefacts/Release/DuskStudio"
OUT="${REPO_ROOT}/docs/images"
DISPLAY_NUM="${CAPTURE_DISPLAY:-:99}"
SCREEN="1920x1200x24"

if ! command -v Xvfb >/dev/null 2>&1; then
  echo "error: Xvfb not found. Install it: sudo zypper install xorg-x11-server-Xvfb" >&2
  exit 1
fi

if [[ ! -x "${BIN}" ]]; then
  echo "Building Dusk Studio (binary not found)..." >&2
  cmake -S "${REPO_ROOT}" -B "${REPO_ROOT}/build" -DCMAKE_BUILD_TYPE=Release >/dev/null
  cmake --build "${REPO_ROOT}/build" -j"$(nproc)"
fi

mkdir -p "${OUT}"

# ── Start an isolated virtual display ───────────────────────────────────
echo "Starting Xvfb on ${DISPLAY_NUM} (${SCREEN}) ..." >&2
Xvfb "${DISPLAY_NUM}" -screen 0 "${SCREEN}" -nolisten tcp >/dev/null 2>&1 &
XVFB_PID=$!
cleanup() { kill "${XVFB_PID}" >/dev/null 2>&1 || true; }
trap cleanup EXIT
sleep 1   # let the server come up

echo "Capturing into ${OUT} ..." >&2
# Force the X11 backend onto the virtual display: set DISPLAY, drop the
# Wayland socket so the JUCE-wayland fork can't reach the real compositor.
env -u WAYLAND_DISPLAY \
    DISPLAY="${DISPLAY_NUM}" \
    DUSKSTUDIO_SKIP_STARTUP_DIALOG=1 \
    DUSKSTUDIO_CAPTURE_DIR="${OUT}" \
    timeout 180 "${BIN}" || true   # harness quits itself; ignore teardown status

rm -rf "${OUT}/_demo"
echo "Done. PNGs in ${OUT}:" >&2
ls -1 "${OUT}"/*.png 2>/dev/null | sed "s#${OUT}/#  #" >&2 || echo "  (none — check stderr above)" >&2
