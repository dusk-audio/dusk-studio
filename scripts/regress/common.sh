# shellcheck shell=bash
# Leg bookkeeping shared by the three platform runners. Sourced, never run.
#
# A "leg" is one verification step. Every leg prints a single PASS/FAIL/SKIP
# line as it finishes, and regress_summary prints the table plus the exit code.
# Legs never abort the run themselves - a FAIL is recorded and the remaining
# legs still run, so one invocation reports everything that is broken.

# A gh call that opens a pager waits for a key nobody presses.
export GH_PAGER=cat

REGRESS_LEG_NAME=()
REGRESS_LEG_STATUS=()
REGRESS_LEG_SECONDS=()
REGRESS_LEG_NOTE=()

# `regress.sh all` sets DUSK_REGRESS_RECORD_FILE and prints one table from
# what every platform appends there, labelled with each runner's platform.
REGRESS_PLATFORM=""

regress_note() {
    printf '    %s\n' "$*"
}

regress_record() {
    local name="$1" status="$2" secs="$3" note="${4:-}"
    REGRESS_LEG_NAME+=("$name")
    REGRESS_LEG_STATUS+=("$status")
    REGRESS_LEG_SECONDS+=("$secs")
    REGRESS_LEG_NOTE+=("$note")
    if [[ -n "${DUSK_REGRESS_RECORD_FILE:-}" ]]; then
        local flat="${note//$'\t'/ }"
        printf '%s\t%s\t%s\t%s\t%s\n' "${REGRESS_PLATFORM:-?}" "$status" "$name" "$secs" \
            "${flat//$'\n'/ }" >>"$DUSK_REGRESS_RECORD_FILE"
    fi
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

# regress_runtime_dir: makes and prints a new private directory for a launch's
# XDG_RUNTIME_DIR. The app binds $XDG_RUNTIME_DIR/dusk-studio/instance-<16 hex>.sock,
# 43 characters past the directory, and sun_path holds 108 bytes with the NUL,
# so the directory has to stay within 64 characters however long TMPDIR is. It
# goes in the caller's own runtime dir, else /tmp, and only ever holds sockets.
regress_runtime_dir() {
    local base dir
    for base in "${XDG_RUNTIME_DIR:-}" /tmp; do
        [[ "$base" == /* && -d "$base" && -w "$base" ]] || continue
        dir="$base/dusk-rt.XXXXXX"
        ((${#dir} <= 64)) || continue
        dir="$(mktemp -d "$dir")" || continue
        chmod 700 "$dir" || { rm -rf "$dir"; return 1; }
        printf '%s' "$dir"
        return 0
    done
    printf 'error: no directory short enough for a runtime dir\n' >&2
    return 1
}

regress_die() {
    printf 'error: %s\n' "$*" >&2
    exit 1
}

# regress_host_lock <host> shared|exclusive: a run that uses the mac or the
# windows host holds that host's lock shared until it exits, and
# `scripts/regress.sh clean` takes it exclusive, so a clean never removes what a
# live run on this box is using. A shared request waits out a clean; an
# exclusive one fails at once when a run holds the lock. The descriptor is left
# in REGRESS_HOST_LOCK_FD. The folder does not follow the caller's environment:
# a clean from a shell without XDG_RUNTIME_DIR (cron, env -i, a scratch HOME)
# has to find the same lock file as the run it must not overlap.
regress_lock_dir() {
    if [[ -n "${DUSK_REGRESS_LOCK_DIR:-}" ]]; then
        printf '%s' "$DUSK_REGRESS_LOCK_DIR"
    elif [[ -d "/run/user/$(id -u)" ]]; then
        printf '%s' "/run/user/$(id -u)/dusk-regress"
    else
        printf '%s' "/tmp/dusk-regress-$(id -u)"
    fi
}
REGRESS_HOST_LOCK_FD=""
regress_host_lock() {
    local host="$1" mode="$2" dir fd
    dir="$(regress_lock_dir)"
    # The runners of one prepush start together, so a runner whose mkdir loses
    # the race to make the folder takes the one the winner made.
    mkdir -p "$(dirname "$dir")" 2>/dev/null
    mkdir -m 700 "$dir" 2>/dev/null
    if [[ -L "$dir" || ! -d "$dir" || ! -O "$dir" || "$(stat -c %a "$dir" 2>/dev/null)" != 700 ]]; then
        echo "error: $dir is not a private folder of this user's (mode 700, no link)" >&2
        return 1
    fi
    exec {fd}>>"${dir}/${host}.lock" || return 1
    if [[ "$mode" == exclusive ]]; then
        flock -x -n "$fd" && { REGRESS_HOST_LOCK_FD="$fd"; return 0; }
    else
        if flock -s -n "$fd"; then REGRESS_HOST_LOCK_FD="$fd"; return 0; fi
        echo "waiting for a clean of the ${host} host to finish (${dir}/${host}.lock)" >&2
        flock -s -w 3600 "$fd" && { REGRESS_HOST_LOCK_FD="$fd"; return 0; }
    fi
    exec {fd}>&-
    return 1
}

regress_host_unlock() {
    [[ -n "$REGRESS_HOST_LOCK_FD" ]] || return 0
    exec {REGRESS_HOST_LOCK_FD}>&-
    REGRESS_HOST_LOCK_FD=""
}

# regress_win_root_ok <path>: an absolute Windows path of plain names, none of
# them only dots. It is spliced into PowerShell and cmd command lines unquoted.
regress_win_root_ok() {
    local dots='\\[.]+(\\|$)'
    [[ "$1" =~ ^[A-Za-z]:(\\[A-Za-z0-9._-]+)+$ && ! "$1" =~ $dots ]]
}

# Free-space floors, in GB, all derived here from what was measured on the
# hosts. A cold run needs twice a freshly built tree (its intermediate files
# and the tests' output fit beside it), a warm run what one run adds to a warm
# tree (what the footprint leg removes at its end), and each the room the
# operating system keeps for itself. To re-derive: read a fresh tree's size off
# the footprint row of the first run after a clean, and a warm run's addition
# off the sizes the footprint leg removes, then change the numbers here.
# The guest's numbers come from a source tree built cold on 2026-11-16: 2.85 GB
# in all, of which tmp\ held 6 MB after a full ctest. The Air's trees measured
# 2.2 GB together.
# shellcheck disable=SC2034
REGRESS_FLOOR_ROOM_GB=5 \
    REGRESS_WIN_TREE_GB=3 REGRESS_WIN_RUN_GB=1 REGRESS_WIN_PACKAGE_GB=1 \
    REGRESS_MAC_TREE_GB=3 REGRESS_MAC_RUN_GB=2 REGRESS_MAC_PACKAGE_GB=2
regress_floor_cold() { printf '%s' "$((2 * $1 + REGRESS_FLOOR_ROOM_GB))"; }
regress_floor_warm() { printf '%s' "$(($1 + REGRESS_FLOOR_ROOM_GB))"; }

# regress_win_ps <tree root> <script> [name=value...]: a PowerShell command line
# that runs <script> in the Windows guest with $rgScript (its name), $root and
# the named values set and inline-lib.ps1 from the script's folder in front of
# it. Scripts travel this way when they must not depend on the copy of the
# scripts in the guest. Full-line comments stay behind, since the command line
# Windows takes is 32767 characters, and progress records would come back over
# the redirected stream as CLIXML.
regress_win_ps() {
    local root="$1" script="$2" encoded pair
    shift 2
    encoded="$({
        printf "\$ProgressPreference = 'SilentlyContinue'\r\n\$rgScript = '%s'\r\n\$root = '%s'\r\n" \
            "$(basename "$script" .ps1)" "$root"
        for pair in "$@"; do printf "\$%s = '%s'\r\n" "${pair%%=*}" "${pair#*=}"; done
        cat "$(dirname "$script")/inline-lib.ps1" "$script" | sed '/^[[:space:]]*#/d'
    } | iconv -f UTF-8 -t UTF-16LE | base64 -w0)"
    if ((${#encoded} > 30000)); then
        printf 'error: %s is too long to send inline (%s characters encoded)\n' "$script" "${#encoded}" >&2
        return 1
    fi
    printf 'powershell -NoProfile -NonInteractive -EncodedCommand %s; exit $LASTEXITCODE' "$encoded"
}

# regress_gb <bytes>: the size in GB with one decimal.
regress_gb() {
    awk -v b="$1" 'BEGIN { printf "%.1f GB", b / 1073741824 }'
}

regress_require() {
    local tool
    for tool in "$@"; do
        command -v "$tool" >/dev/null 2>&1 \
            || regress_die "$tool is required but not on PATH"
    done
}

# regress_require_release_run <repo> <run id>: dies unless that workflow run
# has completed with success. A run still going, failed or cancelled can carry
# a partial or stale artifact, and a package mode run against it proves nothing.
regress_require_release_run() {
    local repo="$1" id="$2" state workflow status conclusion
    [[ "$id" =~ ^[0-9]+$ ]] || regress_die "--release-run takes a workflow run id, not '$id'"
    state="$(gh run view "$id" -R "$repo" --json workflowName,status,conclusion \
        --jq '[.workflowName, .status, .conclusion] | join("\t")')" \
        || regress_die "could not read workflow run $id of $repo"
    IFS=$'\t' read -r workflow status conclusion <<<"$state"
    [[ "$status" == completed && "$conclusion" == success ]] \
        || regress_die "workflow run $id (${workflow:-unknown workflow}) has not finished successfully: status ${status:-unknown}, conclusion ${conclusion:-none}"
}

# regress_drop_added_submodules <commit>: in the repository at the current
# directory, removes the checkout of every submodule <commit> has and HEAD does
# not. Switching back from a commit that added a submodule leaves its checkout
# behind as an untracked folder, which the next clean-tree check refuses. The
# clone stays in .git/modules for the next commit that needs it. The Mac runs
# this too, under its bash 3.2 and set -euo pipefail.
regress_drop_added_submodules() {
    local commit="$1" entry path
    { git ls-tree -r -z "$commit" 2>/dev/null || true; } \
        | while IFS= read -r -d '' entry; do
            [[ "$entry" == "160000 "* ]] || continue
            path=${entry#*$'\t'}
            if [[ -z "$(git --literal-pathspecs ls-tree HEAD -- "$path")" && -f "$path/.git" ]]; then
                rm -rf -- "$path"
                echo "removed $path, a submodule only the tested commit has"
            fi
        done
}
