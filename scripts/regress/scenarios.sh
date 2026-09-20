# shellcheck shell=bash
# Scenario legs: the in-app scenario suite plus the black-box legs that spawn
# the real app and assert on what it prints. Sourced by linux.sh, and runnable
# on its own: bash scripts/regress/scenarios.sh [--gui] [--app <binary>]
#
# The black-box legs run several app processes at once, so each leg gets a
# throwaway HOME, XDG base directories and runtime dir (sandbox_env), as does
# every other launch of the app in this file. The runtime dir is what
# keeps the single-instance slot private: makeSocketPath in
# src/util/SingleInstance.cpp keys the socket on $XDG_RUNTIME_DIR plus a hash of
# DISPLAY, so a per-leg runtime dir can never hand a session to (or steal one
# from) the maintainer's own running copy. HOME is private for the same reason
# one level up: dusk::fs::userConfigDir() resolves $HOME/.config and ignores
# XDG_CONFIG_HOME, so only a private HOME keeps Recent Sessions, app config and
# crash logs out of the maintainer's profile. The other XDG base directories
# move with it: a desktop session exports them as absolute paths into the real
# home, and the libraries under the app (GL shader caches, fontconfig) write
# there.
#
# Every wait takes an explicit budget in seconds. Each process gets its own
# stdout and stderr file - merging them would make marker order meaningless.
# Set DUSK_REGRESS_SCENARIO_KEEP=1 to keep a leg's directory for inspection.

SCENARIOS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="${REPO_ROOT:-$(cd "${SCENARIOS_DIR}/../.." && pwd)}"
MINIMAL_SESSION="${SCENARIOS_DIR}/sessions/minimal/session.json"

# The leg rows this file registers, in order. linux.sh reuses the list for its
# missing-binary skips so the two cannot drift apart.
SCENARIO_BB_LEG_NAMES=(
    bb-handoff
    bb-crash-relaunch
    bb-no-runtime-dir
    bb-no-display
    bb-damaged-recent
    bb-clean-quit
    bb-quit-twice
    bb-oop-child-kill
    bb-oop-quit-during-load
)
# shellcheck disable=SC2034  # read by linux.sh
SCENARIO_LEG_NAMES=(scenarios-headless "${SCENARIO_BB_LEG_NAMES[@]}")

SANDBOX_ENV=()

# sandbox_env <dir>: SANDBOX_ENV becomes the env assignments for a private
# HOME, XDG base directories and runtime dir under <dir>, which must exist.
#
# PipeWire finds its socket through the runtime dir, and without it the engine
# falls back to ALSA and opens the first sound card directly. So PipeWire keeps
# the real one: the app joins the graph as an ordinary client instead of
# grabbing the maintainer's interface.
sandbox_env() {
    local dir="$1"
    local pipewire_dir="${PIPEWIRE_RUNTIME_DIR:-${XDG_RUNTIME_DIR:-}}"
    mkdir -p "$dir/home" "$dir/config" && mkdir -p -m 700 "$dir/runtime" || return 1
    SANDBOX_ENV=(
        "HOME=$dir/home"
        "XDG_CONFIG_HOME=$dir/config"
        "XDG_DATA_HOME=$dir/home/.local/share"
        "XDG_CACHE_HOME=$dir/home/.cache"
        "XDG_STATE_HOME=$dir/home/.local/state"
        "XDG_RUNTIME_DIR=$dir/runtime"
    )
    if [[ -n "$pipewire_dir" ]]; then
        SANDBOX_ENV+=("PIPEWIRE_RUNTIME_DIR=$pipewire_dir")
    fi
}

BB_SDIR=""
BB_LEG=""
BB_DEADLINE=0
BB_TAGS=()
declare -A BB_PID=()
declare -A BB_EXIT=()

# ---------------------------------------------------------------- helpers

bb_begin() {
    local name="$1" budget="${2:-180}"
    BB_LEG="$name"
    BB_TAGS=()
    BB_PID=()
    BB_EXIT=()
    BB_SDIR="$(mktemp -d "${TMPDIR:-/tmp}/duskstudio-${name}.XXXXXX")" || return 1
    sandbox_env "$BB_SDIR" || return 1
    BB_DEADLINE=$((SECONDS + budget))
    return 0
}

bb_fail() {
    printf '    %s: %s\n' "$BB_LEG" "$*" >&2
    return 1
}

# Remaining seconds of the leg budget, capped at the caller's own budget so a
# wait can never outlive the leg.
bb_budget() {
    local want="$1"
    local left=$((BB_DEADLINE - SECONDS))
    ((left > 0)) || left=0
    ((want < left)) || want="$left"
    printf '%s' "$want"
}

# bb_spawn <tag> [VAR=value ...] -- <app args...>
# An entry with an empty value (VAR=) unsets that variable for the child, which
# is how the runtime-dir leg reaches the unset-XDG_RUNTIME_DIR branch.
bb_spawn() {
    local tag="$1"
    shift
    local -A assign=()
    local -a unsets=(WAYLAND_DISPLAY DBUS_SESSION_BUS_ADDRESS)
    assign[DISPLAY]="$XVFB_DISPLAY"

    local entry key value
    for entry in "${SANDBOX_ENV[@]}"; do
        assign["${entry%%=*}"]="${entry#*=}"
    done
    while [[ $# -gt 0 && "$1" != "--" ]]; do
        entry="$1"
        shift
        [[ "$entry" == *=* ]] || { bb_fail "bb_spawn: '$entry' is not VAR=value"; return 2; }
        key="${entry%%=*}"
        value="${entry#*=}"
        if [[ -z "$value" ]]; then
            unset 'assign[$key]'
            unsets+=("$key")
        else
            assign["$key"]="$value"
        fi
    done
    [[ "${1:-}" == "--" ]] || { bb_fail "bb_spawn: missing -- before the app arguments"; return 2; }
    shift

    local -a command=(env)
    for key in "${unsets[@]}"; do command+=(-u "$key"); done
    for key in "${!assign[@]}"; do command+=("$key=${assign[$key]}"); done
    command+=("$APP_BIN" "$@")

    "${command[@]}" >"$BB_SDIR/$tag.out" 2>"$BB_SDIR/$tag.err" &
    BB_PID[$tag]=$!
    BB_TAGS+=("$tag")
    return 0
}

bb_alive() {
    local pid="${BB_PID[$1]:-}"
    [[ -n "$pid" ]] || return 1
    kill -0 "$pid" 2>/dev/null
}

# bb_wait_exit <tag> <secs>: returns the app's exit status, or 124 if it is
# still running when the budget runs out. The status is also left in
# BB_EXIT[tag] for legs that accept more than one outcome.
bb_wait_exit() {
    local tag="$1" secs="$2"
    local pid="${BB_PID[$tag]:-}"
    [[ -n "$pid" ]] || { bb_fail "bb_wait_exit: no live process for '$tag'"; return 2; }
    local deadline=$((SECONDS + $(bb_budget "$secs")))
    while kill -0 "$pid" 2>/dev/null; do
        if ((SECONDS >= deadline)); then
            bb_fail "$tag still running after ${secs}s"
            return 124
        fi
        sleep 0.1
    done
    local rc=0
    wait "$pid" || rc=$?
    BB_EXIT[$tag]=$rc
    unset 'BB_PID[$tag]'
    return "$rc"
}

bb_wait_marker() {
    local tag="$1" literal="$2" secs="$3"
    local deadline=$((SECONDS + $(bb_budget "$secs")))
    while :; do
        if grep -qF -- "$literal" "$BB_SDIR/$tag.err" 2>/dev/null; then return 0; fi
        if ((SECONDS >= deadline)); then
            bb_fail "$tag never printed within ${secs}s: $literal"
            return 1
        fi
        sleep 0.1
    done
}

# bb_wait_any_marker <tag> <secs> <literal>...: succeeds when any one appears.
bb_wait_any_marker() {
    local tag="$1" secs="$2"
    shift 2
    local deadline=$((SECONDS + $(bb_budget "$secs"))) literal
    while :; do
        for literal in "$@"; do
            if grep -qF -- "$literal" "$BB_SDIR/$tag.err" 2>/dev/null; then return 0; fi
        done
        ((SECONDS < deadline)) || return 1
        sleep 0.1
    done
}

bb_assert_marker() {
    local tag="$1" literal="$2"
    if grep -qF -- "$literal" "$BB_SDIR/$tag.err" 2>/dev/null; then return 0; fi
    bb_fail "$tag did not print: $literal"
    return 1
}

bb_assert_absent() {
    local tag="$1" literal="$2"
    grep -qF -- "$literal" "$BB_SDIR/$tag.err" 2>/dev/null || return 0
    bb_fail "$tag printed what it must not: $literal"
    return 1
}

# bb_assert_order <tag> <literal>...: each literal must appear after the one
# before it. Matching walks forward through the text, so a marker that repeats
# does not let a later one match an earlier copy.
bb_assert_order() {
    local tag="$1"
    shift
    local text rest prefix marker at=0
    text="$(cat "$BB_SDIR/$tag.err" 2>/dev/null || true)"
    for marker in "$@"; do
        rest="${text:at}"
        prefix="${rest%%"$marker"*}"
        if [[ "$prefix" == "$rest" ]]; then
            bb_fail "$tag: expected after the previous marker: $marker"
            return 1
        fi
        at=$((at + ${#prefix} + ${#marker}))
    done
    return 0
}

# bb_child_pid <tag> <name>: pids of the app's own children only. Never a bare
# pkill - the maintainer runs Dusk Studio on the same box, and a pattern kill
# would take their session down with the leg.
bb_child_pid() {
    local pid="${BB_PID[$1]:-}"
    [[ -n "$pid" ]] || return 1
    pgrep -P "$pid" -f -- "$2" 2>/dev/null | head -1
}

# bb_end <rc>: kill every survivor, show the stderr of each process when the
# leg failed, drop the directory. Returns the leg's own status.
bb_end() {
    local rc="${1:-0}" tag pid file
    for tag in ${BB_TAGS[@]+"${BB_TAGS[@]}"}; do
        pid="${BB_PID[$tag]:-}"
        [[ -n "$pid" ]] || continue
        kill -9 "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    done
    if ((rc != 0)) && [[ -n "$BB_SDIR" ]]; then
        for file in "$BB_SDIR"/*.err; do
            [[ -e "$file" ]] || continue
            printf '  --- %s ---\n' "$(basename "$file")"
            sed 's/^/  /' "$file"
        done
    fi
    if [[ -n "$BB_SDIR" ]]; then
        if [[ "${DUSK_REGRESS_SCENARIO_KEEP:-0}" == 1 ]]; then
            printf '  kept %s\n' "$BB_SDIR"
        else
            # The damaged-recent leg leaves a 0000 directory behind on purpose.
            chmod -R u+rwX "$BB_SDIR" 2>/dev/null || true
            rm -rf "$BB_SDIR"
        fi
    fi
    BB_SDIR=""
    BB_TAGS=()
    BB_PID=()
    return "$rc"
}

# bb_session <name>: a private copy of the checked-in minimal session, so a leg
# never autosaves into the working tree. Echoes the path of the session json.
bb_session() {
    local name="$1" file="${2:-session.json}"
    mkdir -p "$BB_SDIR/$name" || return 1
    cp "$MINIMAL_SESSION" "$BB_SDIR/$name/$file" || return 1
    printf '%s' "$BB_SDIR/$name/$file"
}

# ------------------------------------------------------- capability probes

SCENARIOS_LIST=""
SCENARIOS_LIST_READ=0

# Reads DUSKSTUDIO_RUN_SCENARIOS=list once. The env gate does not exist in every
# build, and a binary without it would launch the full GUI and sit there, so the
# string has to be in the binary before the probe is allowed to run it.
scenarios_list() {
    if ((SCENARIOS_LIST_READ)); then
        printf '%s' "$SCENARIOS_LIST"
        return 0
    fi
    SCENARIOS_LIST_READ=1
    if scenarios_binary_has DUSKSTUDIO_RUN_SCENARIOS; then
        SCENARIOS_LIST="$(sandboxed_app_run 60 list 2>/dev/null || true)"
    fi
    printf '%s' "$SCENARIOS_LIST"
}

# sandboxed_app_run <secs> <scenario spec>: the in-app suite on its own display,
# in a sandbox that goes away with the run.
sandboxed_app_run() {
    local secs="$1" spec="$2" dir rc=0
    dir="$(mktemp -d "${TMPDIR:-/tmp}/duskstudio-scenarios.XXXXXX")" || return 1
    if sandbox_env "$dir"; then
        xvfb_run "$secs" env -u DBUS_SESSION_BUS_ADDRESS "${SANDBOX_ENV[@]}" \
            "DUSKSTUDIO_RUN_SCENARIOS=${spec}" \
            "DUSKSTUDIO_FIXTURE_DIR=${REPO_ROOT}/build-tests:${REPO_ROOT}/tests/fixtures" \
            "$APP_BIN" || rc=$?
    else
        rc=1
    fi
    rm -rf "$dir"
    return "$rc"
}

scenarios_binary_has() {
    grep -q -a -- "$1" "$APP_BIN" 2>/dev/null
}

scenarios_has_quit_timer() { scenarios_binary_has DUSKSTUDIO_QUIT_AFTER_MS; }
scenarios_has_startup_markers() { scenarios_binary_has "Dusk Studio/startup"; }
scenarios_has_case() { scenarios_list | grep -q -F -- "$1"; }

# ------------------------------------------------------------- the legs

leg_bb_no_display() {
    bb_begin bb-no-display 60 || return 1
    local rc=0
    bb_no_display_body || rc=$?
    bb_end "$rc"
}

bb_no_display_body() {
    local tag display rc
    for tag in unset unreachable; do
        display=""
        [[ "$tag" != unreachable ]] || display=":65535"
        bb_spawn "$tag" "DISPLAY=$display" "DUSKSTUDIO_NATIVE_WAYLAND=" -- || return 1
        rc=0
        bb_wait_exit "$tag" 20 || rc=$?
        [[ "$rc" -eq 1 ]] || { bb_fail "$tag returned $rc instead of rejecting startup with status 1"; return 1; }
        bb_assert_marker "$tag" "Dusk Studio needs an X11 display and could not open one." || return 1
        if [[ "$tag" == unset ]]; then
            bb_assert_marker "$tag" "No display was found (DISPLAY is unset)." || return 1
        else
            bb_assert_marker "$tag" "DISPLAY is set (:65535) but connecting to it failed" || return 1
        fi
    done
}

leg_bb_handoff() {
    bb_begin bb-handoff 240 || return 1
    local rc=0
    bb_handoff_body || rc=$?
    bb_end "$rc"
}

bb_handoff_body() {
    local first second
    first="$(bb_session first)" || return 1
    # A distinct file name: the load line reports the file name only, so naming
    # both session.json would not prove the second launch is what reloaded.
    second="$(bb_session second handoff.json)" || return 1

    bb_spawn A "DUSKSTUDIO_LOAD_SESSION=$first" -- || return 1
    bb_wait_marker A "[Dusk Studio/Load] session.json" 120 || return 1

    bb_spawn B -- "$second" || return 1
    bb_wait_exit B 60 || return 1

    bb_alive A || { bb_fail "the first instance exited on the handoff"; return 1; }
    bb_wait_marker A "[Dusk Studio/Load] handoff.json" 60 || return 1
    return 0
}

leg_bb_crash_relaunch() {
    bb_begin bb-crash-relaunch 300 || return 1
    local rc=0
    bb_crash_relaunch_body || rc=$?
    bb_end "$rc"
}

bb_crash_relaunch_body() {
    local session
    session="$(bb_session crashed)" || return 1

    bb_spawn A "DUSKSTUDIO_LOAD_SESSION=$session" -- || return 1
    bb_wait_marker A "[Dusk Studio/Load] session.json" 120 || return 1

    kill -9 "${BB_PID[A]}" 2>/dev/null || true
    wait "${BB_PID[A]}" 2>/dev/null || true
    unset 'BB_PID[A]'

    bb_spawn B -- "$session" || return 1
    bb_spawn C -- "$session" || return 1

    local deadline=$((SECONDS + $(bb_budget 120))) live=2
    while ((SECONDS < deadline)); do
        live=0
        if bb_alive B; then live=$((live + 1)); fi
        if bb_alive C; then live=$((live + 1)); fi
        if ((live == 1)); then break; fi
        sleep 0.2
    done

    if ((live != 1)); then
        bb_fail "expected exactly one surviving instance, found ${live}"
        return 1
    fi

    local survivor departed
    if bb_alive B; then survivor=B; departed=C; else survivor=C; departed=B; fi
    bb_wait_exit "$departed" 30 || {
        bb_fail "$departed handed the session over but exited ${BB_EXIT[$departed]:-?}"
        return 1
    }
    bb_assert_absent "$survivor" \
        "the socket a crashed instance left behind could not be removed" || return 1
    return 0
}

leg_bb_no_runtime_dir() {
    bb_begin bb-no-runtime-dir 240 || return 1
    local rc=0
    bb_no_runtime_dir_body || rc=$?
    bb_end "$rc"
}

bb_no_runtime_dir_body() {
    local session
    session="$(bb_session unslotted)" || return 1

    local -a quit_timer=()
    if scenarios_has_quit_timer; then quit_timer+=("DUSKSTUDIO_QUIT_AFTER_MS=8000"); fi

    bb_spawn A "XDG_RUNTIME_DIR=" "DUSKSTUDIO_LOAD_SESSION=$session" \
        ${quit_timer[@]+"${quit_timer[@]}"} -- || return 1

    bb_wait_marker A "XDG_RUNTIME_DIR is unset or not an absolute path" 60 || return 1
    bb_assert_marker A " - starting without the single-instance slot" || return 1
    bb_wait_marker A "[Dusk Studio/Load] session.json" 120 || return 1

    if scenarios_has_quit_timer; then
        bb_wait_exit A 60 || return 1
        return 0
    fi
    # No quit timer in this build: SIGTERM has no handler, so the exit status
    # carries no information and only the markers above are the assertion.
    kill "${BB_PID[A]}" 2>/dev/null || true
    bb_wait_exit A 30 || true
    return 0
}

leg_bb_damaged_recent() {
    bb_begin bb-damaged-recent 240 || return 1
    local rc=0
    bb_damaged_recent_body || rc=$?
    bb_end "$rc"
}

bb_damaged_recent_body() {
    local session dir
    session="$(bb_session damaged)" || return 1
    dir="$(dirname "$session")"
    mkdir -p "$dir/audio" || return 1
    chmod 000 "$dir/audio" || return 1
    mkdir -p "$BB_SDIR/home/.config/Dusk Studio" || return 1
    printf '%s\n' "$dir" >"$BB_SDIR/home/.config/Dusk Studio/recent.txt" || return 1

    bb_spawn A -- || return 1
    bb_wait_marker A "[Dusk Studio/startup] picker requested recents=1" 90 || return 1

    # GLX under Xvfb is not guaranteed, so on some hosts the picker legitimately
    # declines. What it may never do is stay silent about which happened. On a
    # host where GLX does work, DUSK_REGRESS_REQUIRE_PICKER=1 demands the window.
    if [[ "${DUSK_REGRESS_REQUIRE_PICKER:-0}" == 1 ]]; then
        bb_wait_marker A "[Dusk Studio/startup] picker shown" 60 || return 1
    elif ! bb_wait_any_marker A 60 "[Dusk Studio/startup] picker shown" \
            "[Dusk Studio/startup] picker unavailable on this display"; then
        bb_fail "the picker neither showed nor explained itself"
        return 1
    fi

    sleep 8
    bb_alive A || { bb_fail "the picker took the window down with it"; return 1; }
    # The markers are the assertion; how the app then quits is the quit legs'
    # job, and SIGTERM has no handler (#507), so its status says nothing.
    kill "${BB_PID[A]}" 2>/dev/null || true
    bb_wait_exit A 30 || true
    return 0
}

leg_bb_clean_quit() {
    bb_begin bb-clean-quit 240 || return 1
    local rc=0
    bb_clean_quit_body || rc=$?
    bb_end "$rc"
}

# The tail phases run on a callAsync, so 8 is printed before 6, 7 and 7b.
# Numeric order would be the wrong assertion.
BB_SHUTDOWN_ORDER=(
    "phase 1: stop autosave timer"
    "phase 2: stop transport (commits in-flight recording)"
    "phase 3: detach audio callback"
    "phase 3b: release plugin resources (setActive(false) on each)"
    "phase 4: drop plugin editor windows"
    "phase 5: flush window operations"
    "phase 5b: clear keyboard focus from every top-level window"
    "phase 8: beginSafeShutdown returning to message loop (yield to mutter)"
    "phase 6: hide main window"
    "phase 7: defer systemRequestedQuit to next message-loop tick"
    "phase 7b: posting systemRequestedQuit"
)

bb_clean_quit_body() {
    local session
    session="$(bb_session quit)" || return 1

    bb_spawn A "DUSKSTUDIO_LOAD_SESSION=$session" "DUSKSTUDIO_QUIT_AFTER_MS=8000" -- || return 1
    bb_wait_marker A "[Dusk Studio/Load] session.json" 120 || return 1
    bb_wait_exit A 90 || return 1
    bb_assert_order A "${BB_SHUTDOWN_ORDER[@]}" || return 1
    return 0
}

leg_bb_quit_twice() {
    bb_begin bb-quit-twice 240 || return 1
    local rc=0
    bb_quit_twice_body || rc=$?
    bb_end "$rc"
}

bb_quit_twice_body() {
    local session
    session="$(bb_session quit-twice)" || return 1

    # The first quit reaches systemRequestedQuit within a few milliseconds, so
    # only a second timer firing on the same tick can meet the latch.
    bb_spawn A "DUSKSTUDIO_LOAD_SESSION=$session" "DUSKSTUDIO_QUIT_AFTER_MS=8000,8000" -- \
        || return 1
    bb_wait_marker A "[Dusk Studio/Load] session.json" 120 || return 1
    bb_wait_exit A 90 || return 1
    bb_assert_order A "${BB_SHUTDOWN_ORDER[@]}" || return 1

    local seen
    seen="$(grep -c -F -- "re-entry ignored: shutdown already in progress" \
        "$BB_SDIR/A.err" || true)"
    if [[ "$seen" != 1 ]]; then
        bb_fail "the second quit was ignored ${seen} times, expected once"
        return 1
    fi
    return 0
}

leg_bb_oop_child_kill() {
    bb_begin bb-oop-child-kill 300 || return 1
    local rc=0
    bb_oop_child_kill_body || rc=$?
    bb_end "$rc"
}

# Mints a session holding one sandboxable plugin insert. Echoes its session.json.
bb_mint_oop_session() {
    local name="$1"
    local dir="$BB_SDIR/$name"
    mkdir -p "$dir" || return 1
    # On the leg's shared display, not through xvfb_run: that would start and
    # stop a second server and leave XVFB_DISPLAY pointing at the dead one.
    if ! env -u WAYLAND_DISPLAY -u DBUS_SESSION_BUS_ADDRESS "DISPLAY=$XVFB_DISPLAY" \
        "${SANDBOX_ENV[@]}" \
        DUSKSTUDIO_RUN_SCENARIOS=session.mint_oop_fixture \
        "DUSKSTUDIO_SCENARIO_OUT=$dir/session.json" \
        "DUSKSTUDIO_FIXTURE_DIR=${REPO_ROOT}/build-tests:${REPO_ROOT}/tests/fixtures" \
        timeout --kill-after=10 120 "$APP_BIN" >"$BB_SDIR/mint.out" 2>"$BB_SDIR/mint.err"; then
        bb_fail "minting the sandboxed-plugin session failed"
        return 1
    fi
    [[ -f "$dir/session.json" ]] || { bb_fail "the mint scenario wrote no session"; return 1; }
    printf '%s' "$dir/session.json"
}

bb_oop_child_kill_body() {
    local session child
    session="$(bb_mint_oop_session oop-kill)" || return 1

    bb_spawn A DUSKSTUDIO_USE_OOP_PLUGINS=1 "DUSKSTUDIO_LOAD_SESSION=$session" \
        "DUSKSTUDIO_QUIT_AFTER_MS=25000" -- || return 1
    bb_wait_marker A "[Dusk Studio/Load] session.json" 120 || return 1

    # A silent fall back to in-process hosting would leave every assertion below
    # passing against a plugin that was never sandboxed.
    bb_assert_absent A "OOP requested but host binary not found" || return 1
    bb_assert_absent A "OOP connect failed (" || return 1

    local deadline=$((SECONDS + $(bb_budget 30)))
    while :; do
        child="$(bb_child_pid A dusk-studio-plugin-host || true)"
        if [[ -n "$child" ]]; then break; fi
        ((SECONDS < deadline)) || { bb_fail "no sandboxed child process appeared"; return 1; }
        sleep 0.2
    done

    kill -9 "$child" 2>/dev/null || true
    bb_wait_marker A "OOP child process exited; slot auto-bypassed" 5 || return 1

    sleep 2
    bb_alive A || { bb_fail "the killed child took the app with it"; return 1; }
    bb_wait_exit A 90 || return 1
    return 0
}

leg_bb_oop_quit_during_load() {
    bb_begin bb-oop-quit-during-load 300 || return 1
    local rc=0
    bb_oop_quit_during_load_body || rc=$?
    bb_end "$rc"
}

bb_oop_quit_during_load_body() {
    local session
    session="$(bb_mint_oop_session oop-quit)" || return 1

    # The timer is short enough that the quit lands while the sandboxed child is
    # still coming up, which is the case that used to hang until the IPC timeout.
    bb_spawn A DUSKSTUDIO_USE_OOP_PLUGINS=1 \
        "DUSKSTUDIO_LOAD_SESSION=$session" "DUSKSTUDIO_QUIT_AFTER_MS=1500" -- || return 1
    bb_wait_exit A 90 || return 1
    bb_assert_order A "${BB_SHUTDOWN_ORDER[@]}" || return 1
    return 0
}

# ---------------------------------------------------------------- the suite

scenarios_headless_leg() {
    local name="$1" spec="$2" budget="$3"
    printf '\n--- %s ---\n' "$name"
    local start=$SECONDS log rc=0
    log="$(mktemp "${TMPDIR:-/tmp}/duskstudio-${name}.XXXXXX")"
    sandboxed_app_run "$budget" "$spec" >"$log" 2>&1 || rc=$?
    cat "$log"
    regress_scenario_leg "$name" "$((SECONDS - start))" "$log" "$rc"
    rm -f "$log"
    return 0
}

# regress_scenarios_run <app_bin> [--gui]
regress_scenarios_run() {
    APP_BIN="$1"
    shift
    local want_gui=0
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --gui) want_gui=1; shift ;;
            *) regress_die "regress_scenarios_run: unknown option '$1'" ;;
        esac
    done

    # Both probes below spin up their own short-lived display, so they have to
    # run before the shared session the bb-* legs share.
    scenarios_list >/dev/null

    if scenarios_binary_has DUSKSTUDIO_RUN_SCENARIOS; then
        scenarios_headless_leg "scenarios-headless" all 240
    else
        regress_skip "scenarios-headless" "needs DUSKSTUDIO_RUN_SCENARIOS"
    fi

    trap 'bb_end 0 >/dev/null 2>&1 || true; xvfb_session_stop || true' EXIT
    if ! xvfb_session_start; then
        local leg
        for leg in "${SCENARIO_BB_LEG_NAMES[@]}"; do
            regress_skip "$leg" "no Xvfb display"
        done
        if ((want_gui)); then
            regress_skip "scenarios-gui" "no Xvfb display"
        fi
        trap - EXIT
        return 0
    fi

    regress_leg "bb-handoff" leg_bb_handoff
    regress_leg "bb-crash-relaunch" leg_bb_crash_relaunch
    regress_leg "bb-no-runtime-dir" leg_bb_no_runtime_dir
    if [[ "$(uname -s)" == Linux ]]; then
        regress_leg "bb-no-display" leg_bb_no_display
    else
        regress_skip "bb-no-display" "Linux X11 startup diagnostic"
    fi

    if scenarios_has_startup_markers && scenarios_has_quit_timer; then
        regress_leg "bb-damaged-recent" leg_bb_damaged_recent
    else
        regress_skip "bb-damaged-recent" "needs the [Dusk Studio/startup] markers"
    fi

    if scenarios_has_quit_timer; then
        regress_leg "bb-clean-quit" leg_bb_clean_quit
        regress_leg "bb-quit-twice" leg_bb_quit_twice
    else
        regress_skip "bb-clean-quit" "needs DUSKSTUDIO_QUIT_AFTER_MS"
        regress_skip "bb-quit-twice" "needs DUSKSTUDIO_QUIT_AFTER_MS"
    fi

    if scenarios_has_quit_timer && scenarios_has_case "session.mint_oop_fixture"; then
        regress_leg "bb-oop-child-kill" leg_bb_oop_child_kill
        regress_leg "bb-oop-quit-during-load" leg_bb_oop_quit_during_load
    else
        regress_skip "bb-oop-child-kill" "needs the session.mint_oop_fixture scenario"
        regress_skip "bb-oop-quit-during-load" "needs the session.mint_oop_fixture scenario"
    fi

    xvfb_session_stop
    trap - EXIT

    if ((want_gui)); then
        if scenarios_has_case "gui."; then
            scenarios_headless_leg "scenarios-gui" gui 300
        else
            regress_skip "scenarios-gui" "gui scenarios not in this binary"
        fi
    fi
    return 0
}

# --------------------------------------------------------------- direct run

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
    set -euo pipefail

    # shellcheck source=scripts/regress/common.sh
    source "${SCENARIOS_DIR}/common.sh"
    # shellcheck source=scripts/regress/xvfb.sh
    source "${SCENARIOS_DIR}/xvfb.sh"

    regress_require Xvfb timeout pgrep

    scenarios_app_bin=""
    scenarios_gui=()
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --gui | --gui-scenarios) scenarios_gui=(--gui); shift ;;
            --app)
                [[ $# -ge 2 ]] || regress_die "--app needs a path"
                scenarios_app_bin="$2"
                shift 2
                ;;
            *) regress_die "unknown option '$1'" ;;
        esac
    done

    if [[ -z "$scenarios_app_bin" ]]; then
        [[ -f "${REPO_ROOT}/build/CMakeCache.txt" ]] \
            || regress_die "build/ is not configured; pass --app <binary>"
        scenarios_build_type="$(sed -n 's/^CMAKE_BUILD_TYPE:[^=]*=//p' \
            "${REPO_ROOT}/build/CMakeCache.txt" | head -1)"
        scenarios_app_bin="${REPO_ROOT}/build/DuskStudio_artefacts/${scenarios_build_type:-Release}/DuskStudio"
    fi
    [[ -x "$scenarios_app_bin" ]] || regress_die "app binary missing: ${scenarios_app_bin}"

    echo "Dusk Studio regression - scenarios"
    echo "repo   $REPO_ROOT"
    echo "app    $scenarios_app_bin"

    regress_scenarios_run "$scenarios_app_bin" ${scenarios_gui[@]+"${scenarios_gui[@]}"}
    regress_summary "regress scenarios"
fi
