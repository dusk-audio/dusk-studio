#!/usr/bin/env bash
# Linux leg of the regression runner. Run from anywhere: scripts/regress.sh linux
#
# Set DUSK_REGRESS_BUILD_LOCK=/path/to/lockfile to serialise the two compile
# legs against another process building the same tree (flock, exclusive).

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck source=scripts/regress/common.sh
source "${REPO_ROOT}/scripts/regress/common.sh"

JOBS="${DUSK_JOBS:-6}"
DONOR_DIR_NAME="dusk-donor-pin"
SELFTEST_TIMEOUT="${DUSK_REGRESS_SELFTEST_TIMEOUT:-180}"
VST3_PATH=""
RUN_PERF=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --perf)
            RUN_PERF=1
            shift
            ;;
        --vst3)
            [[ $# -ge 2 ]] || regress_die "--vst3 needs a path"
            VST3_PATH="$2"
            shift 2
            ;;
        *) regress_die "unknown option '$1' for the linux target" ;;
    esac
done

regress_require cmake ctest Xvfb timeout flock
cd "$REPO_ROOT"

# JUCE places the app under DuskStudio_artefacts/<CMAKE_BUILD_TYPE>/, so a
# Debug or RelWithDebInfo tree is only found by reading the type back out of
# the cache. Resolved after configure-check, which proves the cache exists.
APP_BIN=""
resolve_app_bin() {
    local build_type
    build_type="$(sed -n 's/^CMAKE_BUILD_TYPE:[^=]*=//p' "${REPO_ROOT}/build/CMakeCache.txt" | head -1)"
    build_type="${build_type:-Release}"
    APP_BIN="${REPO_ROOT}/build/DuskStudio_artefacts/${build_type}/DuskStudio"
    regress_note "build type ${build_type}: ${APP_BIN}"
}

run_build() {
    if [[ -n "${DUSK_REGRESS_BUILD_LOCK:-}" ]]; then
        flock "${DUSK_REGRESS_BUILD_LOCK}" "$@"
    else
        "$@"
    fi
}

# Both build dirs must point at the pinned donor checkout. Building against a
# drifted ../plugins silently changes the DSP under test, and the failure then
# looks like a Dusk Studio regression.
check_configure() {
    local expected dir cached rc=0
    expected=""
    if [[ -d "${REPO_ROOT}/../${DONOR_DIR_NAME}" ]]; then
        expected="$(cd "${REPO_ROOT}/../${DONOR_DIR_NAME}" && pwd)"
    fi
    if [[ -z "$expected" ]]; then
        echo "error: donor pin checkout ${REPO_ROOT}/../${DONOR_DIR_NAME} does not exist." >&2
        echo "       Recreate it from the plugins repo at the DONOR_REV in" >&2
        echo "       .github/workflows/release.yml (git worktree add)." >&2
        return 1
    fi
    for dir in build build-tests; do
        if [[ ! -f "${REPO_ROOT}/${dir}/CMakeCache.txt" ]]; then
            local extra=""
            [[ "$dir" == build-tests ]] && extra=" -DDUSKSTUDIO_BUILD_TESTS=ON"
            echo "error: ${dir}/CMakeCache.txt missing - configure it first:" >&2
            echo "       cmake -S . -B ${dir} -DCMAKE_BUILD_TYPE=Release${extra} \\" >&2
            echo "         -DDUSK_PLUGINS_PATH=${expected}" >&2
            rc=1
            continue
        fi
        cached="$(sed -n 's/^DUSK_PLUGINS_PATH:[^=]*=//p' "${REPO_ROOT}/${dir}/CMakeCache.txt" | head -1)"
        if [[ -d "$cached" ]]; then
            cached="$(cd "$cached" && pwd)"
        fi
        if [[ "$cached" != "$expected" ]]; then
            echo "error: ${dir} is configured with DUSK_PLUGINS_PATH='${cached}'," >&2
            echo "       expected the pinned donor '${expected}'." >&2
            echo "       Reconfigure: cmake -S . -B ${dir} -DDUSK_PLUGINS_PATH=${expected}" >&2
            rc=1
        else
            regress_note "${dir}: DUSK_PLUGINS_PATH=${cached}"
        fi
    done
    return "$rc"
}

# Runs a command on a private X display with WAYLAND_DISPLAY unset. The app
# aborts against the live Wayland session, so every GUI-linked leg goes here.
xvfb_run() {
    local timeout_s="$1"
    shift
    local display_file log_file xvfb_pid display_number rc=0
    display_file="$(mktemp "${TMPDIR:-/tmp}/duskstudio-regress-display.XXXXXX")"
    log_file="$(mktemp "${TMPDIR:-/tmp}/duskstudio-regress-xvfb.XXXXXX")"
    Xvfb -displayfd 3 -screen 0 1920x1200x24 -nolisten tcp \
        3>"$display_file" 2>"$log_file" &
    xvfb_pid=$!
    local attempt
    for ((attempt = 0; attempt < 100; ++attempt)); do
        [[ -s "$display_file" ]] && break
        if ! kill -0 "$xvfb_pid" 2>/dev/null; then
            echo "error: Xvfb failed to start:" >&2
            sed 's/^/  /' "$log_file" >&2
            rm -f "$display_file" "$log_file"
            return 1
        fi
        sleep 0.1
    done
    if ! read -r display_number <"$display_file" \
        || [[ ! "$display_number" =~ ^[0-9]+$ ]]; then
        echo "error: Xvfb did not report a ready display:" >&2
        sed 's/^/  /' "$log_file" >&2
        kill "$xvfb_pid" 2>/dev/null || true
        rm -f "$display_file" "$log_file"
        return 1
    fi
    env -u WAYLAND_DISPLAY DISPLAY=":${display_number}" \
        timeout --kill-after=10 "$timeout_s" "$@" || rc=$?
    kill "$xvfb_pid" 2>/dev/null || true
    wait "$xvfb_pid" 2>/dev/null || true
    rm -f "$display_file" "$log_file"
    return "$rc"
}

leg_ipc_selftest() {
    xvfb_run "$SELFTEST_TIMEOUT" env DUSKSTUDIO_RUN_IPC_SELFTEST=1 "$APP_BIN"
}

leg_ipc_host_test() {
    xvfb_run "$SELFTEST_TIMEOUT" env "DUSKSTUDIO_IPC_HOST_TEST=${VST3_PATH}" "$APP_BIN"
}

leg_perf() {
    xvfb_run "$SELFTEST_TIMEOUT" env DUSKSTUDIO_RUN_PERF_TEST=1 "$APP_BIN"
}

echo "Dusk Studio regression - linux"
echo "repo   $REPO_ROOT"
echo "commit $(git -C "$REPO_ROOT" rev-parse --short HEAD) ($(git -C "$REPO_ROOT" rev-parse --abbrev-ref HEAD))"
echo "jobs   -j${JOBS}"

regress_leg "configure-check" check_configure
if [[ "${REGRESS_LEG_STATUS[0]}" == FAIL ]]; then
    regress_summary "regress linux" || true
    exit 1
fi
resolve_app_bin

regress_leg "build-app" run_build cmake --build build -j"${JOBS}"
regress_leg "build-tests" run_build cmake --build build-tests --target dusk-studio-tests -j"${JOBS}"
regress_leg "ctest" ctest --test-dir build-tests --output-on-failure
regress_leg "juce-gate" bash tools/juce-gate.sh

if [[ -x "$APP_BIN" ]]; then
    regress_leg "selftest-xvfb" bash scripts/run-selftest-xvfb.sh "$APP_BIN"
    regress_leg "ipc-selftest" leg_ipc_selftest

    if [[ -z "$VST3_PATH" ]]; then
        for candidate in "${HOME}"/.vst3/*.vst3; do
            [[ -e "$candidate" ]] || continue
            VST3_PATH="$candidate"
            break
        done
    fi
    if [[ -n "$VST3_PATH" && -e "$VST3_PATH" ]]; then
        regress_note "plugin: ${VST3_PATH}"
        regress_leg "ipc-host-test" leg_ipc_host_test
    else
        regress_skip "ipc-host-test" "no VST3 found (pass --vst3 <path>)"
    fi

    if ((RUN_PERF)); then
        regress_leg "perf-suite" leg_perf
    else
        regress_skip "perf-suite" "not requested (--perf)"
    fi
else
    for leg in selftest-xvfb ipc-selftest ipc-host-test perf-suite; do
        regress_skip "$leg" "app binary missing: ${APP_BIN}"
    done
fi

regress_summary "regress linux"
