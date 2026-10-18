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
#           ImageMagick and PipeWire, for native captures and a private null graph
# Set DUSK_JOBS to override the conservative six-worker build fallback.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="${REPO_ROOT}/build/DuskStudio_artefacts/Release/DuskStudio"
OUT="${REPO_ROOT}/docs/images"
JOBS="${DUSK_JOBS:-6}"

source "${REPO_ROOT}/scripts/regress/common.sh"
source "${REPO_ROOT}/scripts/regress/xvfb.sh"
source "${REPO_ROOT}/scripts/regress/scenarios.sh"

if [[ ! "${JOBS}" =~ ^[1-9][0-9]*$ ]]; then
  echo "error: DUSK_JOBS must be a positive integer" >&2
  exit 2
fi

if ! command -v Xvfb >/dev/null 2>&1; then
  echo "error: Xvfb not found. Install it: sudo zypper install xorg-x11-server-Xvfb" >&2
  exit 1
fi

if ! command -v magick >/dev/null 2>&1; then
  echo "error: ImageMagick (magick) is needed to convert the native panel captures" >&2
  exit 1
fi

if [[ ! -x "${BIN}" ]]; then
  echo "Building Dusk Studio (binary not found)..." >&2
  cmake -S "${REPO_ROOT}" -B "${REPO_ROOT}/build" -DCMAKE_BUILD_TYPE=Release >/dev/null
  cmake --build "${REPO_ROOT}/build" -j"${JOBS}"
fi

mkdir -p "${OUT}"
CAPTURE_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/dusk-manual-capture.XXXXXX")"
LOG="${CAPTURE_ROOT}/capture.log"
SFZ_LOG="${CAPTURE_ROOT}/sfz.log"
cleanup() {
  null_audio_stop
  xvfb_session_stop
  rm -rf "${CAPTURE_ROOT}"
}
trap cleanup EXIT

xvfb_session_start
if ! null_audio_start; then
  echo "error: captures require a private PipeWire null graph." >&2
  exit 1
fi
sandbox_env "${CAPTURE_ROOT}/general"
LAUNCH_UNSET=(
  -u WAYLAND_DISPLAY -u DBUS_SESSION_BUS_ADDRESS -u PIPEWIRE_REMOTE
  -u DUSKSTUDIO_RUN_SCENARIOS -u DUSKSTUDIO_LOAD_SESSION
  -u DUSKSTUDIO_QUIT_AFTER_MS -u DUSKSTUDIO_SCENARIO_CAPTURE_DIR
  -u DUSKSTUDIO_CAPTURE_DIR -u DUSKSTUDIO_RUN_SELFTEST -u DUSKSTUDIO_START_STAGE
  -u DUSKSTUDIO_INSTRUMENT_TEST -u DUSKSTUDIO_PIPELINE_TEST
  -u DUSKSTUDIO_BOUNCE_TEST -u DUSKSTUDIO_PERF_SESSION
  -u DUSKSTUDIO_CLAP_EDITOR_TEST -u DUSKSTUDIO_LV2_EDITOR_TEST
  -u DUSKSTUDIO_VST3_EDITOR_TEST -u DUSKSTUDIO_REPLACE_TEST
  -u DUSKSTUDIO_RUN_IPC_SELFTEST -u DUSKSTUDIO_IPC_HOST_TEST
  -u DUSKSTUDIO_RUN_TONE_TEST -u DUSKSTUDIO_RUN_ALSA_PERF
  -u DUSKSTUDIO_RUN_PERF_TEST -u DUSKSTUDIO_OPEN_NOTEPAD
)

echo "Capturing into ${OUT} ..." >&2
env "${LAUNCH_UNSET[@]}" \
    "${SANDBOX_ENV[@]}" DISPLAY="${XVFB_DISPLAY}" \
    DUSKSTUDIO_SKIP_STARTUP_DIALOG=1 \
    DUSKSTUDIO_CAPTURE_DIR="${OUT}" \
    timeout --kill-after=10 180 "${BIN}" 2>&1 | tee "${LOG}" >&2 || true

if ! grep -q '\[Dusk Studio/capture\] done' "${LOG}"; then
  echo "error: the capture harness did not finish; some figures may be stale." >&2
  exit 1
fi

sandbox_env "${CAPTURE_ROOT}/sfz"
rm -f "${OUT}/ms-03-sfz-installed.ppm" "${OUT}/ms-04-sfz-online.ppm"
env "${LAUNCH_UNSET[@]}" \
    "${SANDBOX_ENV[@]}" DISPLAY="${XVFB_DISPLAY}" \
    DUSKSTUDIO_RUN_SCENARIOS=gui:gui.sfz_repository_workflow \
    DUSKSTUDIO_SCENARIO_CAPTURE_DIR="${OUT}" \
    timeout --kill-after=10 300 "${BIN}" 2>&1 | tee "${SFZ_LOG}" >&2
if ! grep -qF '[PASS] gui.sfz_repository_workflow (' "${SFZ_LOG}" \
    || ! grep -q '^=== scenarios: 1 pass, 0 fail, 0 skip ===$' "${SFZ_LOG}" \
    || grep -qE '\[(FAIL|SKIP|DIRTY)\]' "${SFZ_LOG}" \
    || [[ ! -s "${OUT}/ms-03-sfz-installed.ppm" || ! -s "${OUT}/ms-04-sfz-online.ppm" ]]; then
  echo "error: fixture-driven SFZ Manager captures did not complete." >&2
  exit 1
fi

rm -rf "${OUT}/_demo"

# The native panels read their own frames back as PPM - a framework child is not
# reachable through JUCE's snapshot path - so convert those to the PNGs MANUAL.md
# embeds.
shopt -s nullglob
for ppm in "${OUT}"/*.ppm; do
  magick "${ppm}" "${ppm%.ppm}.png"
  rm -f "${ppm}"
done
shopt -u nullglob

echo "Done. PNGs in ${OUT}:" >&2
ls -1 "${OUT}"/*.png 2>/dev/null | sed "s#${OUT}/#  #" >&2 || echo "  (none — check stderr above)" >&2
