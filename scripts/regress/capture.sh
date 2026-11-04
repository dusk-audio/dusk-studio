# shellcheck shell=bash
# The capture-screenshots leg: docs/capture-screenshots.sh against the binary
# under test, into a scratch folder rather than docs/images/. Sourced after
# common.sh, never run.

CAPTURE_LEG_DIR=""

capture_leg_cleanup() {
    [[ -z "$CAPTURE_LEG_DIR" ]] || rm -rf "$CAPTURE_LEG_DIR"
    CAPTURE_LEG_DIR=""
}

# regress_capture_leg <app binary>: records the capture-screenshots row. The
# scratch folder goes on PASS and on an interrupted run, and is kept and named
# on FAIL.
regress_capture_leg() {
    local app="$1" name=capture-screenshots start=$SECONDS rc=0 log missing note tool
    local -a needs=()
    printf '\n--- %s ---\n' "$name"
    for tool in magick pipewire; do
        command -v "$tool" >/dev/null 2>&1 || needs+=("$tool")
    done
    if ((${#needs[@]})); then
        regress_skip "$name" "needs ${needs[*]}"
        return 0
    fi
    if [[ "${DUSK_REGRESS_REAL_AUDIO:-0}" == 1 ]]; then
        regress_skip "$name" "DUSK_REGRESS_REAL_AUDIO=1; the captures need the private null graph"
        return 0
    fi
    regress_at_exit capture_leg_cleanup
    CAPTURE_LEG_DIR="$(mktemp -d "${TMPDIR:-/tmp}/dusk-capture.XXXXXX")" || {
        regress_record "$name" FAIL "$((SECONDS - start))" "could not create a scratch folder"
        return 0
    }
    log="${CAPTURE_LEG_DIR}/capture.log"
    DUSK_CAPTURE_OUT="${CAPTURE_LEG_DIR}/figures" DUSK_CAPTURE_BIN="$app" \
        bash "${REPO_ROOT}/docs/capture-screenshots.sh" >"$log" 2>&1 || rc=$?
    grep -E '^(error: |All [0-9]+ .* figures are in )' "$log" || true

    if ((rc == 0)) && grep -qE '^All [0-9]+ .* figures are in ' "$log"; then
        note="$(sed -nE 's/^All ([0-9]+) .*/\1 figures/p' "$log" | tail -1)"
        regress_record "$name" PASS "$((SECONDS - start))" "$note"
        capture_leg_cleanup
        return 0
    fi
    missing="$(sed -n 's/^error: missing figure: //p' "$log" | tr '\n' '|' | sed 's/|$//; s/|/, /g')"
    if [[ -n "$missing" ]]; then
        note="missing ${missing}"
    elif ((rc != 0)); then
        note="exit ${rc}: $(grep -m1 '^error: ' "$log" || tail -1 "$log")"
    else
        note="exit 0 without the figure check's summary line"
    fi
    regress_record "$name" FAIL "$((SECONDS - start))" "${note}; output in ${CAPTURE_LEG_DIR}"
    CAPTURE_LEG_DIR=""
    return 0
}
