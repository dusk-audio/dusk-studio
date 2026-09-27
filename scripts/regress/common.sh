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

# A leg that found it could not run returns REGRESS_SKIP_RC with its reason in
# REGRESS_SKIP_REASON, and is recorded as SKIP. That status without a reason is
# still a FAIL.
REGRESS_SKIP_RC=77
REGRESS_SKIP_REASON=""

# regress_leg <name> <command...>
regress_leg() {
    local name="$1"
    shift
    printf '\n--- %s ---\n' "$name"
    local start=$SECONDS
    local rc=0
    REGRESS_SKIP_REASON=""
    "$@" || rc=$?
    local status=PASS note=""
    if ((rc == REGRESS_SKIP_RC)) && [[ -n "$REGRESS_SKIP_REASON" ]]; then
        status=SKIP
        note="$REGRESS_SKIP_REASON"
    elif ((rc != 0)); then
        status=FAIL
        note="exit $rc"
    fi
    regress_record "$name" "$status" "$((SECONDS - start))" "$note"
    return 0
}

# Like regress_leg, but a "warn:" line in the leg's output downgrades it to
# WARN. Used for best-effort steps (pin syncs) that should be visible in the
# table without failing the run.
regress_leg_soft() {
    local name="$1"
    shift
    printf '\n--- %s ---\n' "$name"
    local start=$SECONDS out rc=0
    out="$("$@" 2>&1)" || rc=$?
    printf '%s\n' "$out"
    local status=PASS note=""
    if ((rc != 0)); then
        status=FAIL
        note="exit $rc"
    elif grep -q '^warn:' <<<"$out"; then
        status=WARN
        note="$(grep -m1 '^warn:' <<<"$out")"
    fi
    regress_record "$name" "$status" "$((SECONDS - start))" "$note"
    return 0
}

regress_skip() {
    local name="$1" reason="$2"
    regress_record "$name" "SKIP" "0" "$reason"
}

# The case a killed run was inside: the last one the suite announced with [RUN]
# (stderr) that has no result line (stdout). Empty when the last one finished.
regress_scenario_unfinished() {
    local log="$1" name
    name="$(sed -n 's/^\[RUN\] //p' "$log" | tail -1)"
    [[ -n "$name" ]] || return 0
    if grep -qF -e "[PASS] ${name} " -e "[FAIL] ${name}:" -e "[SKIP] ${name}:" "$log"; then
        return 0
    fi
    printf '%s' "$name"
}

# regress_scenario_leg <name> <secs> <log> <rc>: records one run of the in-app
# scenario suite. PASS needs the exit status, the absence of any [FAIL] line and
# the terminal summary line: a crash after the last case would otherwise pass on
# a lucky status. Skipped cases carry their reasons into the note.
regress_scenario_leg() {
    local name="$1" secs="$2" log="$3" rc="$4"
    local verdict=PASS note=""
    if ((rc != 0)); then
        verdict=FAIL
        note="exit $rc"
        if ((rc == 124)); then
            local stalled
            stalled="$(regress_scenario_unfinished "$log")"
            note="timed out${stalled:+ in ${stalled}}"
        fi
        if grep -q '^\[FAIL\]' "$log"; then
            note="${note}: $(grep -c '^\[FAIL\]' "$log") failed, first $(grep -m1 '^\[FAIL\]' "$log")"
        fi
    elif grep -q '^\[FAIL\]' "$log"; then
        verdict=FAIL
        note="$(grep -m1 '^\[FAIL\]' "$log")"
    elif ! grep -q '^=== scenarios: ' "$log"; then
        verdict=FAIL
        note="no terminal '=== scenarios: ' line"
    else
        local skipped skiplist
        skipped="$(grep -c '^\[SKIP\]' "$log" || true)"
        if ((skipped > 0)); then
            skiplist="$(sed -n 's/^\[SKIP\] //p' "$log" | tr '\n' '|' | sed 's/|$//; s/|/; /g')"
            note="${skipped} skipped: ${skiplist}"
        fi
    fi
    regress_record "$name" "$verdict" "$secs" "$note"

    # A case that could not find its fixture proved nothing, so it gets a row
    # of its own rather than riding in the note of a leg that reads PASS.
    local unfixtured
    unfixtured="$(sed -n 's/^\[SKIP\] \([^:]*\): missing fixture: \(.*\)$/\1 (\2)/p' "$log" \
        | tr '\n' '|' | sed 's/|$//; s/|/; /g')"
    if [[ -n "$unfixtured" ]]; then
        regress_skip "${name}:fixtures" "missing fixture: ${unfixtured}"
    fi
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

# Run from the EXIT trap, whoever owns it at the time. The scenario legs set and
# clear a trap of their own, so a cleanup that has to happen on every exit
# registers here rather than setting one.
REGRESS_EXIT_HOOKS=()

regress_at_exit() {
    REGRESS_EXIT_HOOKS+=("$1")
}

regress_run_exit_hooks() {
    local hook
    for hook in ${REGRESS_EXIT_HOOKS[@]+"${REGRESS_EXIT_HOOKS[@]}"}; do
        eval "$hook" || true
    done
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
