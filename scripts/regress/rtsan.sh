# shellcheck shell=bash
# rtsan-* legs: the self-test and the headless scenario suite on an app built
# with Clang's RealtimeSanitizer. Linux only. Sourced by linux.sh for --rtsan,
# and runnable on its own:
#
#   bash scripts/regress/rtsan.sh [--no-build]
#
# The app is built in build-rtsan/ (DUSK_RTSAN_TREE), which the first run
# configures with CC=clang CXX=clang++, Release (the option adds -g), the
# fixture plug-ins and -DDUSKSTUDIO_ENABLE_RTSAN=ON; Release because the
# scenarios find the VST3 fixtures under VST3/Release/. It needs Clang 20 or
# later with its RTSan runtime. --no-build runs the app already there.
#
# A function declared DUSK_RT_NONBLOCKING (src/foundation/RtAttributes.h), the
# audio callback and each Multicore DSP lane, runs in a real-time context, and
# RTSan reports any allocation, lock, blocking syscall or I/O made under it.
# Reports do not stop the app (halt_on_error=false), so one run lists every
# distinct one, and a leg with any gets a :reports row that fails. The only
# reports hidden are the call stacks tools/rtsan-suppressions.txt names, each
# under its issue. Those lines match symbolized stacks, so the legs need
# llvm-symbolizer on PATH, or addr2line, which RTSan then uses; with neither
# they fail.

RTSAN_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="${REPO_ROOT:-$(cd "${RTSAN_DIR}/../.." && pwd)}"
RTSAN_TREE="${DUSK_RTSAN_TREE:-${REPO_ROOT}/build-rtsan}"
RTSAN_APP=""
RTSAN_BUILD=1
RTSAN_SUPPRESSIONS="${REPO_ROOT}/tools/rtsan-suppressions.txt"
# shellcheck disable=SC2034  # read by linux.sh
RTSAN_LEG_NAMES=(rtsan-build rtsan-selftest rtsan-scenarios)

rtsan_locked() {
    if [[ -n "${DUSK_REGRESS_BUILD_LOCK:-}" ]]; then
        flock "${DUSK_REGRESS_BUILD_LOCK}" "$@"
    else
        "$@"
    fi
}

rtsan_resolve_app() {
    local build_type=""
    if [[ -f "${RTSAN_TREE}/CMakeCache.txt" ]]; then
        build_type="$(sed -n 's/^CMAKE_BUILD_TYPE:[^=]*=//p' "${RTSAN_TREE}/CMakeCache.txt" | head -1)"
    fi
    RTSAN_APP="${RTSAN_TREE}/DuskStudio_artefacts/${build_type:-Release}/DuskStudio"
}

# rtsan_options: RTSAN_OPTIONS for a run; fails when no symbolizer is found.
rtsan_options() {
    local options="halt_on_error=false:suppress_equal_stacks=true:suppressions=${RTSAN_SUPPRESSIONS}"
    local symbolizer
    symbolizer="$(command -v llvm-symbolizer 2>/dev/null || true)"
    if [[ -n "$symbolizer" ]]; then
        printf '%s:external_symbolizer_path=%s' "$options" "$symbolizer"
    elif command -v addr2line >/dev/null 2>&1; then
        printf '%s:allow_addr2line=true' "$options"
    else
        return 1
    fi
}

leg_rtsan_build() {
    if ! command -v clang >/dev/null 2>&1 || ! command -v clang++ >/dev/null 2>&1; then
        REGRESS_SKIP_REASON="needs clang and clang++ with the RTSan runtime"
        return "$REGRESS_SKIP_RC"
    fi
    local cache="${RTSAN_TREE}/CMakeCache.txt"
    if [[ ! -f "$cache" ]]; then
        rtsan_locked env CC=clang CXX=clang++ cmake -S "$REPO_ROOT" -B "$RTSAN_TREE" \
            -DCMAKE_BUILD_TYPE=Release -DDUSKSTUDIO_BUILD_TESTS=ON \
            -DDUSKSTUDIO_ENABLE_RTSAN=ON || return 1
    elif ! grep -q '^DUSKSTUDIO_ENABLE_RTSAN:BOOL=ON$' "$cache"; then
        echo "error: ${RTSAN_TREE} is configured without -DDUSKSTUDIO_ENABLE_RTSAN=ON" >&2
        return 1
    fi
    local -a targets=()
    mapfile -t targets < <(cmake --build "$RTSAN_TREE" --target help 2>/dev/null \
        | grep -oE 'dusk-studio-[A-Za-z0-9_-]+-fixture' | sort -u)
    echo "fixture targets: ${targets[*]-none}"
    rtsan_locked cmake --build "$RTSAN_TREE" --target DuskStudio ${targets[@]+"${targets[@]}"} \
        -j"${JOBS:-${DUSK_JOBS:-6}}"
}

# rtsan_reports_row <leg> <log>: a failing <leg>:reports row when the log holds
# any report, naming how many and the first call.
rtsan_reports_row() {
    local leg="$1" log="$2" count first
    count="$(grep -c 'ERROR: RealtimeSanitizer' "$log" || true)"
    ((count > 0)) || return 0
    printf '\n--- %s RTSan reports ---\n' "$leg"
    awk '/ERROR: RealtimeSanitizer/ { on = 1 } on { print } /^SUMMARY: RealtimeSanitizer/ { on = 0 }' "$log"
    # shellcheck disable=SC2016  # the backticks are RTSan's quotes
    first="$(grep -m1 -oE '(unsafe|blocking) function `[^`]*`' "$log" || true)"
    regress_record "${leg}:reports" FAIL 0 "${count} RTSan report(s)${first:+, first: ${first}}"
}

leg_rtsan_selftest() {
    local log rc=0
    local -a fixture=()
    local clap="${RTSAN_TREE}/dusk-studio-multi-bus-clap-fixture.clap"
    [[ ! -f "$clap" ]] || fixture=("DUSKSTUDIO_CLAP_STATE_FIXTURE=${clap}")
    log="$(mktemp "${TMPDIR:-/tmp}/duskstudio-rtsan-selftest.XXXXXX")" || return 1
    sandboxed env "RTSAN_OPTIONS=$(rtsan_options)" ${fixture[@]+"${fixture[@]}"} \
        bash "${REPO_ROOT}/scripts/run-selftest-xvfb.sh" "$RTSAN_APP" >"$log" 2>&1 || rc=$?
    tail -25 "$log"
    RTSAN_SELFTEST_LOG="$log"
    return "$rc"
}

leg_rtsan_scenarios() {
    local log rc=0 start=$SECONDS
    printf '\n--- rtsan-scenarios ---\n'
    log="$(mktemp "${TMPDIR:-/tmp}/duskstudio-rtsan-scenarios.XXXXXX")" || return 1
    (
        # sandboxed_app_run launches APP_BIN with these fixture roots.
        # shellcheck disable=SC2030
        APP_BIN="$RTSAN_APP"
        SCENARIO_FIXTURE_ROOTS="${RTSAN_TREE}:${REPO_ROOT}/tests/fixtures"
        export RTSAN_OPTIONS
        RTSAN_OPTIONS="$(rtsan_options)"
        sandboxed_app_run "${DUSK_RTSAN_SCENARIO_BUDGET:-300}" all
    ) >"$log" 2>&1 || rc=$?
    grep -E '^\[(FAIL|SKIP)\]|^=== scenarios: ' "$log" || true
    regress_scenario_leg "rtsan-scenarios" "$((SECONDS - start))" "$log" "$rc"
    rtsan_reports_row rtsan-scenarios "$log"
    # The case that drives the callback and the lanes under full load can skip;
    # the leg would then check far less than it says.
    grep -q '^\[PASS\] engine\.rt_callback_full_load ' "$log" \
        || regress_record "rtsan-scenarios:full-load" FAIL 0 "engine.rt_callback_full_load did not pass"
    rm -f "$log"
}

# regress_rtsan_run: the three legs, the runs on the private null PipeWire graph
# when it starts, so the self-test's device leg drives the callback from a real
# audio thread.
regress_rtsan_run() {
    local own_audio=0 leg
    if [[ "$(uname -s)" != Linux ]]; then
        for leg in "${RTSAN_LEG_NAMES[@]}"; do regress_skip "$leg" "Linux only"; done
        return 0
    fi
    rtsan_resolve_app
    if ((RTSAN_BUILD)); then
        regress_leg "rtsan-build" leg_rtsan_build
    else
        regress_skip "rtsan-build" "not run (--no-build)"
    fi
    rtsan_resolve_app
    if [[ ! -x "$RTSAN_APP" ]]; then
        regress_skip "rtsan-selftest" "no RTSan app at ${RTSAN_APP}"
        regress_skip "rtsan-scenarios" "no RTSan app at ${RTSAN_APP}"
        return 0
    fi
    regress_note "app under RTSan: ${RTSAN_APP}"
    if ! rtsan_options >/dev/null; then
        for leg in rtsan-selftest rtsan-scenarios; do
            regress_record "$leg" FAIL 0 "no llvm-symbolizer or addr2line: the suppressions match symbolized stacks"
        done
        return 0
    fi
    if [[ -z "${NULL_AUDIO_DIR:-}" ]] && null_audio_start; then
        own_audio=1
        regress_at_exit null_audio_stop
    fi
    RTSAN_SELFTEST_LOG=""
    regress_leg "rtsan-selftest" leg_rtsan_selftest
    if [[ -n "$RTSAN_SELFTEST_LOG" ]]; then
        rtsan_reports_row rtsan-selftest "$RTSAN_SELFTEST_LOG"
        rm -f "$RTSAN_SELFTEST_LOG"
    fi
    leg_rtsan_scenarios
    ((own_audio == 0)) || null_audio_stop
    return 0
}

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
    set -euo pipefail

    # shellcheck source=scripts/regress/common.sh
    source "${RTSAN_DIR}/common.sh"
    REGRESS_PLATFORM=linux
    # shellcheck source=scripts/regress/xvfb.sh
    source "${RTSAN_DIR}/xvfb.sh"
    # shellcheck source=scripts/regress/scenarios.sh
    source "${RTSAN_DIR}/scenarios.sh"

    while [[ $# -gt 0 ]]; do
        case "$1" in
            --no-build)
                RTSAN_BUILD=0
                shift
                ;;
            *) regress_die "unknown option '$1'" ;;
        esac
    done
    regress_require cmake Xvfb timeout flock

    trap regress_run_exit_hooks EXIT
    trap 'exit 130' INT
    trap 'exit 143' TERM
    regress_rtsan_run
    regress_summary "regress rtsan"
fi
