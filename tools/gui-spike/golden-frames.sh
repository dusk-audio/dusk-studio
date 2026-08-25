#!/bin/bash
# Golden frames for the widget kit: capture a set of frames, or capture them again and
# diff them against a set captured earlier.
#
#   tools/gui-spike/golden-frames.sh capture /tmp/gui-golden   # before a change
#   ...edit the kit...
#   tools/gui-spike/golden-frames.sh compare /tmp/gui-golden   # after it
#
# Golden images belong to the machine that took them: a different GPU or driver renders
# the same draw list to slightly different pixels, which is why a set is captured on the
# spot rather than committed. On one machine the frames are exact - the runs this recipe
# was written against differ by zero in every channel - so a mismatch is a change in what
# the kit draws, not noise. --tolerance raises the bar for a driver update.
#
# Everything runs under a private headless compositor started by this script. Nothing
# Dusk-owned goes on the live session's socket: a crash in one of these surfaces takes
# the desktop with it.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$HERE/../.." && pwd)"
SPIKE="${DUSK_GUI_SPIKE:-$REPO_ROOT/build-spike/gui-spike/dusk-gui-spike}"
TOLERANCE="${DUSK_GOLDEN_TOLERANCE:-0}"

MODE="${1:-}"
DIR="${2:-}"
if [[ "$MODE" != "capture" && "$MODE" != "compare" ]] || [[ -z "$DIR" ]]; then
    echo "usage: golden-frames.sh capture|compare <directory>" >&2
    exit 2
fi
if [[ ! -x "$SPIKE" ]]; then
    echo "error: the spike is not built: $SPIKE" >&2
    echo "       cmake -S . -B build-spike -G Ninja -DCMAKE_BUILD_TYPE=Release \\" >&2
    echo "             -DDUSKSTUDIO_BUILD_GUI_SPIKE=ON -DDGL_BACKEND=wayland ..." >&2
    exit 2
fi

# name:arguments. Every frame is captured with --static, so the meters hold a fixed
# value and the frame is the same every run.
VARIANTS=(
    "strip:--strips 1"
    "strip-scale2:--strips 1 --scale 2"
    "bank:--strips 8"
    "modal:--strips 1 --demo 1"
    "menu:--strips 1 --demo 2"
)

RUNTIME_DIR=$(mktemp -d /tmp/dusk-golden.XXXXXX)
COMPOSITOR=""
cleanup() {
    [[ -n "$COMPOSITOR" ]] && kill "$COMPOSITOR" >/dev/null 2>&1
    # The session bus starts gvfs, which mounts inside the runtime directory.
    fusermount -u "$RUNTIME_DIR/gvfs" >/dev/null 2>&1
    rm -rf "$RUNTIME_DIR" >/dev/null 2>&1
}
trap cleanup EXIT

DUSK_HEADLESS_RUNTIME_DIR="$RUNTIME_DIR" DUSK_HEADLESS_DISPLAY=dusk-golden \
    "$HERE/headless-compositor.sh" >/dev/null 2>&1 &
COMPOSITOR=$!
for _ in $(seq 1 100); do
    [[ -S "$RUNTIME_DIR/dusk-golden" ]] && break
    sleep 0.1
done
if [[ ! -S "$RUNTIME_DIR/dusk-golden" ]]; then
    echo "error: the headless compositor did not come up" >&2
    exit 1
fi

mkdir -p "$DIR"
STATUS=0
for variant in "${VARIANTS[@]}"; do
    name="${variant%%:*}"
    args="${variant#*:}"
    out="$DIR/$name.ppm"
    compare=()
    if [[ "$MODE" == "compare" ]]; then
        if [[ ! -f "$out" ]]; then
            echo "MISSING  $name (no golden in $DIR)"
            STATUS=1
            continue
        fi
        golden="$RUNTIME_DIR/$name-golden.ppm"
        cp "$out" "$golden"
        compare=(--compare "$golden" --tolerance "$TOLERANCE")
        out="$RUNTIME_DIR/$name.ppm"
    fi

    # shellcheck disable=SC2086 # the variant's arguments are meant to split
    line=$(XDG_RUNTIME_DIR="$RUNTIME_DIR" WAYLAND_DISPLAY=dusk-golden \
        env -u DISPLAY "$SPIKE" --seconds 3 --static --quiet --capture-frame 60 \
        --capture "$out" $args "${compare[@]}" 2>&1)
    code=$?

    if [[ "$MODE" == "capture" ]]; then
        [[ $code -eq 0 ]] && echo "captured $name" || { echo "FAILED   $name"; STATUS=1; }
        continue
    fi

    verdict=$(echo "$line" | grep -a "golden" | sed 's/.*golden [^:]*: //')
    if [[ $code -eq 0 ]]; then
        echo "match    $name"
    else
        echo "MISMATCH $name: ${verdict:-the run failed}"
        cp "$out" "$DIR/$name.actual.ppm" 2>/dev/null
        STATUS=1
    fi
done

[[ "$MODE" == "compare" && $STATUS -ne 0 ]] \
    && echo "a mismatching frame was written beside its golden as <name>.actual.ppm"
exit $STATUS
