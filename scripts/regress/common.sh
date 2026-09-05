# shellcheck shell=bash
# Leg bookkeeping shared by the three platform runners. Sourced, never run.
#
# A "leg" is one verification step. Every leg prints a single PASS/FAIL/SKIP
# line as it finishes, and regress_summary prints the table plus the exit code.
# Legs never abort the run themselves - a FAIL is recorded and the remaining
# legs still run, so one invocation reports everything that is broken.

REGRESS_LEG_NAME=()
REGRESS_LEG_STATUS=()
REGRESS_LEG_SECONDS=()
REGRESS_LEG_NOTE=()

regress_note() {
    printf '    %s\n' "$*"
}

regress_record() {
    local name="$1" status="$2" secs="$3" note="${4:-}"
    REGRESS_LEG_NAME+=("$name")
    REGRESS_LEG_STATUS+=("$status")
    REGRESS_LEG_SECONDS+=("$secs")
    REGRESS_LEG_NOTE+=("$note")
    if [[ -n "$note" ]]; then
        printf '[%s] %-28s %4ss  %s\n' "$status" "$name" "$secs" "$note"
    else
        printf '[%s] %-28s %4ss\n' "$status" "$name" "$secs"
    fi
}

# regress_leg <name> <command...>
regress_leg() {
    local name="$1"
    shift
    printf '\n--- %s ---\n' "$name"
    local start=$SECONDS
    local rc=0
    "$@" || rc=$?
    local status=PASS
    ((rc == 0)) || status=FAIL
    local note=""
    ((rc == 0)) || note="exit $rc"
    regress_record "$name" "$status" "$((SECONDS - start))" "$note"
    return 0
}

regress_skip() {
    local name="$1" reason="$2"
    regress_record "$name" "SKIP" "0" "$reason"
}

regress_failed() {
    local s
    for s in ${REGRESS_LEG_STATUS+"${REGRESS_LEG_STATUS[@]}"}; do
        [[ "$s" == FAIL ]] && return 0
    done
    return 1
}

# regress_summary <title>; returns 1 when any leg failed.
regress_summary() {
    local title="$1"
    local i
    printf '\n===== %s =====\n' "$title"
    printf '%-6s %-30s %8s  %s\n' "RESULT" "LEG" "SECONDS" "NOTE"
    for i in "${!REGRESS_LEG_NAME[@]}"; do
        printf '%-6s %-30s %8s  %s\n' \
            "${REGRESS_LEG_STATUS[$i]}" "${REGRESS_LEG_NAME[$i]}" \
            "${REGRESS_LEG_SECONDS[$i]}" "${REGRESS_LEG_NOTE[$i]}"
    done
    if regress_failed; then
        printf '\n%s: FAILED\n' "$title"
        return 1
    fi
    printf '\n%s: OK\n' "$title"
    return 0
}

regress_die() {
    printf 'error: %s\n' "$*" >&2
    exit 1
}

regress_require() {
    local tool
    for tool in "$@"; do
        command -v "$tool" >/dev/null 2>&1 \
            || regress_die "$tool is required but not on PATH"
    done
}
