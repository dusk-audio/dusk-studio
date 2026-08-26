#!/usr/bin/env bash
# Drive Dusk Studio's built-in capture harness to (re)generate manual figures.
#
# Produces the ✅ rows in docs/screenshot-list.md into docs/images/. The one
# remaining row, the notepad, is a native window this cannot reach and is
# captured by hand — see that file.
#
# Runs the app on an ISOLATED Xvfb display (X11 backend), NOT your live
# session — the harness drives heavy stage switches + modal teardown that can
# crash a real Wayland compositor. createComponentSnapshot is software raster,
# so no GPU / Wayland surface is needed.
#
# Requires: Xvfb  (openSUSE: sudo zypper install xorg-x11-server-Xvfb)
#           ImageMagick, for the native panels' own frame readback
# Set DUSK_JOBS to override the conservative six-worker build fallback.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="${REPO_ROOT}/build/DuskStudio_artefacts/Release/DuskStudio"
OUT="${REPO_ROOT}/docs/images"
DISPLAY_NUM="${CAPTURE_DISPLAY:-:99}"
SCREEN="1920x1200x24"
JOBS="${DUSK_JOBS:-6}"

if [[ ! "${JOBS}" =~ ^[1-9][0-9]*$ ]]; then
  echo "error: DUSK_JOBS must be a positive integer" >&2
  exit 2
fi

if ! command -v Xvfb >/dev/null 2>&1; then
  echo "error: Xvfb not found. Install it: sudo zypper install xorg-x11-server-Xvfb" >&2
  exit 1
fi

if [[ ! -x "${BIN}" ]]; then
  echo "Building Dusk Studio (binary not found)..." >&2
  cmake -S "${REPO_ROOT}" -B "${REPO_ROOT}/build" -DCMAKE_BUILD_TYPE=Release >/dev/null
  cmake --build "${REPO_ROOT}/build" -j"${JOBS}"
fi

mkdir -p "${OUT}"
LOG=""

# ── Start an isolated virtual display ───────────────────────────────────
echo "Starting Xvfb on ${DISPLAY_NUM} (${SCREEN}) ..." >&2
Xvfb "${DISPLAY_NUM}" -screen 0 "${SCREEN}" -nolisten tcp >/dev/null 2>&1 &
XVFB_PID=$!
cleanup() { kill "${XVFB_PID}" >/dev/null 2>&1 || true; rm -f "${LOG}"; }
trap cleanup EXIT
LOG="$(mktemp)"
sleep 1   # let the server come up

echo "Capturing into ${OUT} ..." >&2
# Force the X11 backend onto the virtual display: set DISPLAY, drop the
# Wayland socket so the JUCE-wayland fork can't reach the real compositor.
env -u WAYLAND_DISPLAY \
    DISPLAY="${DISPLAY_NUM}" \
    DUSKSTUDIO_SKIP_STARTUP_DIALOG=1 \
    DUSKSTUDIO_CAPTURE_DIR="${OUT}" \
    timeout 180 "${BIN}" 2>&1 | tee "${LOG}" >&2 || true

rm -rf "${OUT}/_demo"

# The native panels read their own frames back as PPM - a framework child is not
# reachable through JUCE's snapshot path - so convert those to the PNGs MANUAL.md
# embeds.
shopt -s nullglob
for ppm in "${OUT}"/*.ppm; do
  if ! command -v magick >/dev/null 2>&1; then
    echo "error: ImageMagick (magick) is needed to convert the native panel captures" >&2
    exit 1
  fi
  magick "${ppm}" "${ppm%.ppm}.png"
  rm -f "${ppm}"
done
shopt -u nullglob

# The harness quits the app itself and Linux teardown still returns nonzero, so
# the exit status says nothing. Its own "done" line is the only signal that
# every figure was written; without it the run died part-way, leaving the
# figures it never reached at whatever an earlier run wrote.
if ! grep -q '\[Dusk Studio/capture\] done' "${LOG}"; then
  echo "error: the capture harness did not finish — some PNGs in ${OUT} may be left over from an earlier run." >&2
  exit 1
fi

echo "Done. PNGs in ${OUT}:" >&2
ls -1 "${OUT}"/*.png 2>/dev/null | sed "s#${OUT}/#  #" >&2 || echo "  (none — check stderr above)" >&2
