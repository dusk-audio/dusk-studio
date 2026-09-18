#!/usr/bin/env bash
# Linux leg of the regression runner. Run from anywhere: scripts/regress.sh linux
#
# Set DUSK_REGRESS_BUILD_LOCK=/path/to/lockfile to serialise the two compile
# legs against another process building the same tree (flock, exclusive).

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck source=scripts/regress/common.sh
source "${REPO_ROOT}/scripts/regress/common.sh"
# shellcheck source=scripts/regress/xvfb.sh
source "${REPO_ROOT}/scripts/regress/xvfb.sh"
# shellcheck source=scripts/regress/scenarios.sh
source "${REPO_ROOT}/scripts/regress/scenarios.sh"

JOBS="${DUSK_JOBS:-6}"
SELFTEST_TIMEOUT="${DUSK_REGRESS_SELFTEST_TIMEOUT:-180}"
VST3_PATH=""
RUN_PERF=0
RUN_SCENARIOS=1
GUI_SCENARIOS=0
SCENARIOS_ONLY=0
RELEASE_CHECKS=0

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
        --scenarios)
            RUN_SCENARIOS=1
            shift
            ;;
        --no-scenarios)
            RUN_SCENARIOS=0
            shift
            ;;
        --gui-scenarios)
            GUI_SCENARIOS=1
            shift
            ;;
        --scenarios-only)
            SCENARIOS_ONLY=1
            shift
            ;;
        --release-checks)
            RELEASE_CHECKS=1
            shift
            ;;
        *) regress_die "unknown option '$1' for the linux target" ;;
    esac
done

if ((SCENARIOS_ONLY)); then RUN_SCENARIOS=1; fi

regress_require cmake ctest Xvfb timeout flock pgrep
cd "$REPO_ROOT"

# JUCE places the app under DuskStudio_artefacts/<CMAKE_BUILD_TYPE>/, so a
# Debug or RelWithDebInfo tree is only found by reading the type back out of
# the cache. Resolved after configure-check, which proves the cache exists.
APP_BIN=""
resolve_app_bin() {
    local build_type=""
    if [[ -f "${REPO_ROOT}/build/CMakeCache.txt" ]]; then
        build_type="$(sed -n 's/^CMAKE_BUILD_TYPE:[^=]*=//p' "${REPO_ROOT}/build/CMakeCache.txt" | head -1)"
    fi
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
    local want dir cached head rc=0
    want="$(tr -d '[:space:]' < "${REPO_ROOT}/DONOR_REV")"
    for dir in build build-tests; do
        if [[ ! -f "${REPO_ROOT}/${dir}/CMakeCache.txt" ]]; then
            local extra=""
            [[ "$dir" == build-tests ]] && extra=" -DDUSKSTUDIO_BUILD_TESTS=ON"
            echo "error: ${dir}/CMakeCache.txt missing - configure it first:" >&2
            echo "       cmake -S . -B ${dir} -DCMAKE_BUILD_TYPE=Release${extra}" >&2
            rc=1
            continue
        fi
        # An explicit DUSK_PLUGINS_PATH is for trying donor edits: the DSP under
        # test is then not DONOR_REV, and a failure would read as a Dusk Studio
        # regression.
        cached="$(sed -n 's/^DUSK_PLUGINS_PATH:[^=]*=//p' "${REPO_ROOT}/${dir}/CMakeCache.txt" | head -1)"
        if [[ -n "$cached" ]]; then
            echo "error: ${dir} builds the donor from DUSK_PLUGINS_PATH='${cached}', not DONOR_REV." >&2
            echo "       Reconfigure without it: cmake -S . -B ${dir} -UDUSK_PLUGINS_PATH" >&2
            rc=1
            continue
        fi
        head="$(git -C "${REPO_ROOT}/${dir}/_deps/dusk-plugins" rev-parse HEAD 2>/dev/null || true)"
        if [[ "$head" != "$want" ]]; then
            echo "error: ${dir}/_deps/dusk-plugins is at '${head:-nothing}', DONOR_REV is ${want}." >&2
            echo "       Reconfigure to fetch it: cmake -S . -B ${dir}" >&2
            rc=1
        else
            regress_note "${dir}: donor at DONOR_REV ${want:0:8}"
        fi
    done
    return "$rc"
}

REPO_SLUG="dusk-audio/dusk-studio"
RULESET_ID="${DUSK_REGRESS_RULESET_ID:-17313833}"
PATREON_CONFIG="${DUSK_REGRESS_PATREON_CONFIG:-$HOME/.config/dusk-audio/patreon.json}"

leg_release_metadata() {
    scripts/release-metadata-check.sh
}

# The CI checks a merge to main has to pass. A ruleset that requires fewer
# lets a red job merge, which is what the WARN names.
RULESET_REQUIRED=(
    "Build + tests (GCC Release, Ubuntu 22.04, amd64)"
    "Build + tests (GCC Release, Ubuntu 22.04, arm64)"
    "Build + tests (clang Release, macOS arm64)"
    "Catch2 tests (MSVC x64 Release, Windows)"
    "Catch2 tests (TSan, Ubuntu 22.04)"
    "Catch2 tests (ASan + UBSan, Ubuntu 22.04)"
)

leg_github_ruleset() {
    local present check missing=""
    present="$(gh api "repos/${REPO_SLUG}/rulesets/${RULESET_ID}" \
        --jq '.rules[] | select(.type=="required_status_checks") | .parameters.required_status_checks[].context')"
    echo "ruleset ${RULESET_ID} requires:"
    while IFS= read -r check; do echo "  $check"; done <<<"$present"
    for check in "${RULESET_REQUIRED[@]}"; do
        grep -qxF -- "$check" <<<"$present" || missing="${missing}${missing:+; }${check}"
    done
    [[ -z "$missing" ]] || echo "warn: ruleset ${RULESET_ID} does not require: ${missing}"
    return 0
}

# Part 10 step 2, as the guide spells it out: no local name overrides, the
# supporter header at the donor pin the release workflow builds against, and
# the Patreon dry run. A refreshed token pair is reported, because the Actions
# secrets have to follow it before the tag.
leg_patreon_freshness() {
    local donor_rev out
    python3 - "$PATREON_CONFIG" <<'PY'
import json
import sys
from pathlib import Path

overrides = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8")).get("name_overrides", {})
if overrides:
    raise SystemExit("STOP: local Patreon name_overrides are not available to release workflows")
PY
    donor_rev="$(sed -nE 's/^[[:space:]]*DONOR_REV:[[:space:]]*([0-9a-f]{40})[[:space:]]*$/\1/p' \
        .github/workflows/release.yml | head -1)"
    [[ "$donor_rev" =~ ^[0-9a-f]{40}$ ]] || {
        echo "error: release workflow has no valid DONOR_REV" >&2
        return 1
    }
    git -C ../plugins cat-file -e "${donor_rev}^{commit}" 2>/dev/null \
        || git -C ../plugins fetch origin "$donor_rev"
    echo "supporter header at ${donor_rev}:"
    git -C ../plugins show "${donor_rev}:plugins/shared/PatreonBackers.h" | sed 's/^/  /'
    out="$(env -u DUSK_PLUGINS_PATH scripts/update-patrons.py --dry-run 2>&1)" || {
        printf '%s\n' "$out"
        return 1
    }
    printf '%s\n' "$out"
    if grep -qF 'Patreon tokens refreshed and saved.' <<<"$out"; then
        echo "warn: Patreon tokens refreshed; update the PATREON_* Actions secrets before tagging"
    fi
    return 0
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

# --scenarios-only runs against whatever binary is already in build/, so the
# compile legs are skipped rather than dropped: leg 0 stays configure-check and
# the table still says what was not run.
if ((SCENARIOS_ONLY)); then
    regress_skip "configure-check" "not run (--scenarios-only)"
else
    regress_leg "configure-check" check_configure
    if [[ "${REGRESS_LEG_STATUS[0]}" == FAIL ]]; then
        regress_summary "regress linux" || true
        exit 1
    fi
fi
resolve_app_bin

if ((SCENARIOS_ONLY)); then
    for leg in build-app build-tests ctest juce-gate; do
        regress_skip "$leg" "not run (--scenarios-only)"
    done
else
    regress_leg "build-app" run_build cmake --build build -j"${JOBS}"
    regress_leg "build-tests" run_build cmake --build build-tests --target dusk-studio-tests -j"${JOBS}"
    regress_leg "ctest" ctest --test-dir build-tests --output-on-failure
    regress_leg "juce-gate" bash tools/juce-gate.sh
fi

if [[ -x "$APP_BIN" ]]; then
    if ((SCENARIOS_ONLY)); then
        for leg in selftest-xvfb ipc-selftest ipc-host-test perf-suite; do
            regress_skip "$leg" "not run (--scenarios-only)"
        done
    else
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
    fi

    if ((RUN_SCENARIOS)); then
        if ((GUI_SCENARIOS)); then
            regress_scenarios_run "$APP_BIN" --gui
        else
            regress_scenarios_run "$APP_BIN"
        fi
    fi
else
    missing_legs=(selftest-xvfb ipc-selftest ipc-host-test perf-suite)
    if ((RUN_SCENARIOS)); then
        missing_legs+=("${SCENARIO_LEG_NAMES[@]}")
        if ((GUI_SCENARIOS)); then missing_legs+=(scenarios-gui); fi
    fi
    for leg in "${missing_legs[@]}"; do
        regress_skip "$leg" "app binary missing: ${APP_BIN}"
    done
fi

if ((RELEASE_CHECKS)); then
    regress_leg "release-metadata" leg_release_metadata
    if command -v gh >/dev/null 2>&1 && gh auth status >/dev/null 2>&1; then
        regress_leg_soft "github-ruleset" leg_github_ruleset
    else
        regress_skip "github-ruleset" "gh is not authenticated"
    fi
    if [[ -f "$PATREON_CONFIG" && -d ../plugins ]]; then
        regress_leg_soft "patreon-freshness" leg_patreon_freshness
    else
        regress_skip "patreon-freshness" "needs ${PATREON_CONFIG} and a ../plugins checkout"
    fi
else
    for leg in release-metadata github-ruleset patreon-freshness; do
        regress_skip "$leg" "not requested (--release-checks)"
    done
fi

regress_summary "regress linux"
