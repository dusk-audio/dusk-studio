#!/usr/bin/env bash
# Linux leg of the regression runner. Run from anywhere: scripts/regress.sh linux
#
# Set DUSK_REGRESS_BUILD_LOCK=/path/to/lockfile to serialise the two compile
# legs against another process building the same tree (flock, exclusive).
#
# --tarball <path> or --release-run <id> switches to package mode: the legs run
# against a release tarball, extracted and installed into a scratch directory
# that goes away on exit, instead of against build/.

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
TARBALL=""
RELEASE_RUN=""

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
        --tarball)
            [[ $# -ge 2 ]] || regress_die "--tarball needs a path"
            TARBALL="$2"
            shift 2
            ;;
        --release-run)
            [[ $# -ge 2 ]] || regress_die "--release-run needs a workflow run id"
            RELEASE_RUN="$2"
            shift 2
            ;;
        *) regress_die "unknown option '$1' for the linux target" ;;
    esac
done

if ((SCENARIOS_ONLY)); then RUN_SCENARIOS=1; fi

PACKAGE_MODE=0
if [[ -n "$TARBALL" && -n "$RELEASE_RUN" ]]; then
    regress_die "--tarball and --release-run both name the package; pass one"
fi
if [[ -n "$TARBALL" || -n "$RELEASE_RUN" ]]; then
    PACKAGE_MODE=1
    GUI_SCENARIOS=1
fi
if [[ -n "$TARBALL" ]]; then
    [[ -f "$TARBALL" ]] || regress_die "no such tarball: $TARBALL"
    TARBALL="$(cd "$(dirname "$TARBALL")" && pwd)/$(basename "$TARBALL")"
fi

regress_require cmake ctest Xvfb timeout flock pgrep
if ((PACKAGE_MODE)); then
    regress_require tar xz ldd readelf cmp
    if [[ -n "$RELEASE_RUN" ]]; then regress_require gh; fi
fi
cd "$REPO_ROOT"

trap regress_run_exit_hooks EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

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
    if [[ ! "$want" =~ ^[0-9a-f]{40}$ ]]; then
        echo "error: DONOR_REV does not hold a 40-character commit: '${want}'" >&2
        return 1
    fi
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
    donor_rev="$(tr -d '[:space:]' < DONOR_REV)"
    [[ "$donor_rev" =~ ^[0-9a-f]{40}$ ]] || {
        echo "error: DONOR_REV does not hold a valid commit" >&2
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

# sandboxed <command...>: runs the command, shell functions included, with a
# private HOME, XDG base directories and runtime dir (see sandbox_env), removed
# afterwards.
sandboxed() {
    local dir rc=0
    dir="$(mktemp -d "${TMPDIR:-/tmp}/duskstudio-regress.XXXXXX")" || return 1
    if sandbox_env "$dir"; then
        (
            export "${SANDBOX_ENV[@]}"
            unset DBUS_SESSION_BUS_ADDRESS
            "$@"
        ) || rc=$?
    else
        rc=1
    fi
    rm -rf "$dir"
    return "$rc"
}

leg_selftest() {
    local -a fixture=()
    local clap="${REPO_ROOT}/build-tests/dusk-studio-multi-bus-clap-fixture.clap"
    if [[ -f "$clap" ]]; then fixture=("DUSKSTUDIO_CLAP_STATE_FIXTURE=${clap}"); fi
    sandboxed env ${fixture[@]+"${fixture[@]}"} bash scripts/run-selftest-xvfb.sh "$APP_BIN"
}

leg_ipc_selftest() {
    sandboxed xvfb_run "$SELFTEST_TIMEOUT" env DUSKSTUDIO_RUN_IPC_SELFTEST=1 "$APP_BIN"
}

leg_ipc_host_test() {
    sandboxed xvfb_run "$SELFTEST_TIMEOUT" env "DUSKSTUDIO_IPC_HOST_TEST=${VST3_PATH}" "$APP_BIN"
}

leg_perf() {
    sandboxed xvfb_run "$SELFTEST_TIMEOUT" env DUSKSTUDIO_RUN_PERF_TEST=1 "$APP_BIN"
}

# ------------------------------------------------------------ package mode

PACKAGE_DIR=""
PACKAGE_ROOT=""
PACKAGE_VERSION=""
INSTALL_HOME=""
INSTALL_ENV=()

package_cleanup() {
    [[ -n "$PACKAGE_DIR" ]] || return 0
    chmod -R u+rwX "$PACKAGE_DIR" 2>/dev/null || true
    rm -rf "$PACKAGE_DIR"
}

# The asset label package-tarball.sh gives this machine's architecture.
package_arch() {
    case "$(uname -m)" in
        x86_64) printf 'x86_64' ;;
        aarch64 | arm64) printf 'aarch64' ;;
        *)
            echo "error: no Linux release is built for $(uname -m)" >&2
            return 1
            ;;
    esac
}

leg_package_fetch() {
    local arch
    arch="$(package_arch)" || return 1
    gh run view "$RELEASE_RUN" -R "$REPO_SLUG" \
        --json workflowName,headBranch,headSha,status,conclusion \
        --template '{{.workflowName}} run on {{.headBranch}} at {{.headSha}}: {{.status}} {{.conclusion}}{{"\n"}}' \
        || return 1
    mkdir -p "${PACKAGE_DIR}/artifact"
    gh run download "$RELEASE_RUN" -R "$REPO_SLUG" -n "release-linux-${arch}" \
        -D "${PACKAGE_DIR}/artifact" || return 1
    TARBALL="$(find "${PACKAGE_DIR}/artifact" -name "dusk-studio-*-Linux-${arch}.tar.xz" | head -1)"
    [[ -n "$TARBALL" ]] || {
        echo "error: the release-linux-${arch} artifact holds no tarball" >&2
        return 1
    }
    echo "tarball: ${TARBALL}"
}

leg_package_extract() {
    local name arch want
    name="$(basename "$TARBALL")"
    if [[ ! "$name" =~ ^dusk-studio-([0-9]+\.[0-9]+\.[0-9]+)-Linux-([A-Za-z0-9_]+)\.tar\.xz$ ]]; then
        echo "error: not a release tarball name (dusk-studio-X.Y.Z-Linux-<arch>.tar.xz): ${name}" >&2
        return 1
    fi
    PACKAGE_VERSION="${BASH_REMATCH[1]}"
    arch="${BASH_REMATCH[2]}"
    want="$(package_arch)" || return 1
    if [[ "$arch" != "$want" ]]; then
        echo "error: ${name} is built for ${arch}; this machine runs ${want}" >&2
        return 1
    fi
    mkdir -p "${PACKAGE_DIR}/extract"
    tar -xf "$TARBALL" -C "${PACKAGE_DIR}/extract" || return 1
    PACKAGE_ROOT="${PACKAGE_DIR}/extract/dusk-studio-${PACKAGE_VERSION}-Linux-${arch}"
    local file
    for file in DuskStudio/DuskStudio DuskStudio/dusk-studio-plugin-host install.sh; do
        [[ -x "${PACKAGE_ROOT}/${file}" ]] || {
            echo "error: the tarball has no executable ${file} under its top directory" >&2
            return 1
        }
    done
    echo "extracted ${name} to ${PACKAGE_ROOT}"
    local tree_version
    tree_version="$(tr -d '[:space:]' <"${REPO_ROOT}/VERSION")"
    if [[ "$tree_version" != "$PACKAGE_VERSION" ]]; then
        echo "note: the package is ${PACKAGE_VERSION} but this checkout is ${tree_version};"
        echo "      fixtures and black-box expectations come from the checkout"
    fi
}

leg_package_smoke() {
    sandboxed bash scripts/release-smoke-test.sh linux "$TARBALL" "$PACKAGE_VERSION"
}

# ldd from this machine, which did not build the package: a library the build
# host had and this one lacks shows up here rather than as a launch failure.
leg_package_libs() {
    local bin out rc=0
    for bin in "${PACKAGE_ROOT}/DuskStudio/DuskStudio" \
        "${PACKAGE_ROOT}/DuskStudio/dusk-studio-plugin-host"; do
        if ! out="$(ldd "$bin" 2>&1)"; then
            printf 'error: ldd %s failed:\n%s\n' "$bin" "$out" >&2
            rc=1
            continue
        fi
        if grep -q 'not found' <<<"$out"; then
            echo "error: $(basename "$bin") has libraries that do not resolve:" >&2
            grep 'not found' <<<"$out" | sed 's/^/  /' >&2
            rc=1
        fi
        if grep -qF -- "$REPO_ROOT" <<<"$out"; then
            echo "error: $(basename "$bin") resolves libraries from this checkout:" >&2
            grep -F -- "$REPO_ROOT" <<<"$out" | sed 's/^/  /' >&2
            rc=1
        fi
        if readelf -d "$bin" | grep -qE '\((RPATH|RUNPATH)\)'; then
            echo "error: $(basename "$bin") carries a search path of its own:" >&2
            readelf -d "$bin" | grep -E '\((RPATH|RUNPATH)\)' | sed 's/^/  /' >&2
            rc=1
        fi
        echo "$(basename "$bin"): $(grep -c '=> /' <<<"$out") shared libraries resolve"
    done
    return "$rc"
}

# install.sh the way the README tells a user to run it, into a private HOME
# whose XDG directories all live under it. The launcher on PATH, not the
# program directory, is what the remaining legs start.
leg_package_install() {
    local home opt desktop exec_line reported rc=0
    mkdir -p "${PACKAGE_DIR}/installed"
    sandbox_env "${PACKAGE_DIR}/installed" || return 1
    INSTALL_ENV=("${SANDBOX_ENV[@]}")
    home="${PACKAGE_DIR}/installed/home"
    INSTALL_HOME="$home"
    package_install_run "${PACKAGE_ROOT}/install.sh" || return 1

    opt="${home}/.local/opt/dusk-studio"
    desktop="${home}/.local/share/applications/audio.dusk.studio.desktop"
    local file
    for file in "${opt}/DuskStudio" "${opt}/dusk-studio-plugin-host"; do
        [[ -x "$file" ]] || { echo "error: not installed: ${file}" >&2; rc=1; }
    done
    for file in "${opt}/QUICKSTART.md" "$desktop" \
        "${home}/.local/share/icons/hicolor/256x256/apps/DuskStudio.png" \
        "${home}/.local/share/mime/packages/DuskStudio.mime.xml" \
        "${home}/.local/share/metainfo/DuskStudio.appdata.xml"; do
        [[ -f "$file" ]] || { echo "error: not installed: ${file}" >&2; rc=1; }
    done
    if [[ "$(readlink -f "${home}/.local/bin/DuskStudio")" != "$(readlink -f "${opt}/DuskStudio")" ]]; then
        echo "error: ~/.local/bin/DuskStudio does not lead to ${opt}/DuskStudio" >&2
        rc=1
    fi
    cmp -s "${PACKAGE_ROOT}/DuskStudio/DuskStudio" "${opt}/DuskStudio" \
        || { echo "error: the installed binary differs from the tarball's" >&2; rc=1; }
    exec_line="$(grep -m1 '^Exec=' "$desktop" 2>/dev/null || true)"
    if [[ "$exec_line" != "Exec=${opt}/DuskStudio %f" ]]; then
        echo "error: the installed desktop entry says '${exec_line}'" >&2
        rc=1
    fi
    ((rc == 0)) || return 1

    reported="$(package_install_run xvfb_run 60 "${home}/.local/bin/DuskStudio" --version 2>&1)" \
        || { printf 'error: the installed launcher failed --version:\n%s\n' "$reported" >&2; return 1; }
    if [[ "$reported" != *"$PACKAGE_VERSION"* ]]; then
        echo "error: the installed launcher reports '${reported}', expected ${PACKAGE_VERSION}" >&2
        return 1
    fi
    echo "installed to ${opt}; the launcher reports ${reported%%$'\n'*}"
}

# package_install_run <command...> in the install HOME. XDG_DATA_DIRS goes too:
# install.sh only writes to its data home, but the database refreshes it runs
# read the system list.
package_install_run() {
    (
        export "${INSTALL_ENV[@]}"
        unset DBUS_SESSION_BUS_ADDRESS XDG_DATA_DIRS XDG_CONFIG_DIRS
        "$@"
    )
}

leg_package_uninstall() {
    local rc=0 path
    package_install_run "${PACKAGE_ROOT}/install.sh" --uninstall || return 1
    for path in "${INSTALL_HOME}/.local/opt/dusk-studio" \
        "${INSTALL_HOME}/.local/bin/DuskStudio" \
        "${INSTALL_HOME}/.local/share/applications/audio.dusk.studio.desktop" \
        "${INSTALL_HOME}/.local/share/icons/hicolor/256x256/apps/DuskStudio.png" \
        "${INSTALL_HOME}/.local/share/mime/packages/DuskStudio.mime.xml" \
        "${INSTALL_HOME}/.local/share/metainfo/DuskStudio.appdata.xml"; do
        if [[ -e "$path" || -L "$path" ]]; then
            echo "error: --uninstall left ${path}" >&2
            rc=1
        fi
    done
    return "$rc"
}

# The scenarios load fixture plug-ins from build-tests/ (DUSKSTUDIO_FIXTURE_DIR).
# A package brings none, so they are built here from this checkout, and only
# the fixture targets: the test binary itself says nothing about the package.
leg_fixtures() {
    local -a targets=()
    if [[ -f build-tests/CMakeCache.txt ]]; then
        echo "reusing build-tests/ for the fixture plug-ins; up-to-date ones are not rebuilt"
    else
        echo "build-tests/ is not configured; configuring it for the fixture plug-ins"
        run_build cmake -S . -B build-tests -DCMAKE_BUILD_TYPE=Release -DDUSKSTUDIO_BUILD_TESTS=ON \
            || return 1
    fi
    mapfile -t targets < <(cmake --build build-tests --target help 2>/dev/null \
        | grep -oE 'dusk-studio-[A-Za-z0-9_-]+-fixture' | sort -u)
    if ((${#targets[@]} == 0)); then
        echo "error: build-tests/ declares no dusk-studio-*-fixture targets" >&2
        return 1
    fi
    echo "fixture targets: ${targets[*]}"
    run_build cmake --build build-tests --target "${targets[@]}" -j"${JOBS}"
}

regress_last_passed() {
    [[ "${REGRESS_LEG_STATUS[${#REGRESS_LEG_STATUS[@]} - 1]}" == PASS ]]
}

echo "Dusk Studio regression - linux"
echo "repo   $REPO_ROOT"
echo "commit $(git -C "$REPO_ROOT" rev-parse --short HEAD) ($(git -C "$REPO_ROOT" rev-parse --abbrev-ref HEAD))"
echo "jobs   -j${JOBS}"

INSTALLED=0
if ((PACKAGE_MODE)); then
    PACKAGE_DIR="$(mktemp -d "${TMPDIR:-/tmp}/duskstudio-regress-package.XXXXXX")"
    regress_at_exit package_cleanup
    if [[ -n "$RELEASE_RUN" ]]; then
        echo "package release.yml run ${RELEASE_RUN}"
    else
        echo "package ${TARBALL}"
    fi
    echo "scratch ${PACKAGE_DIR} (removed on exit)"

    # The source legs build and test this checkout, which says nothing about
    # the package, so they are reported as not run rather than dropped.
    for leg in configure-check build-app build-tests ctest juce-gate; do
        regress_skip "$leg" "not run (package mode)"
    done

    if [[ -n "$RELEASE_RUN" ]]; then
        regress_leg "package-fetch" leg_package_fetch
    else
        regress_skip "package-fetch" "local tarball $(basename "$TARBALL")"
    fi

    APP_BIN=""
    if [[ -n "$TARBALL" ]]; then
        regress_leg "package-extract" leg_package_extract
    else
        regress_skip "package-extract" "no tarball"
    fi
    if [[ -n "$PACKAGE_ROOT" ]] && regress_last_passed; then
        regress_leg "package-smoke" leg_package_smoke
        regress_leg "package-libs" leg_package_libs
        regress_leg "package-install" leg_package_install
        if regress_last_passed; then
            INSTALLED=1
            APP_BIN="${INSTALL_HOME}/.local/bin/DuskStudio"
        else
            APP_BIN="${PACKAGE_ROOT}/DuskStudio/DuskStudio"
            regress_note "the install failed; the legs below run the extracted binary"
        fi
        regress_note "app under test: ${APP_BIN}"
    else
        for leg in package-smoke package-libs package-install; do
            regress_skip "$leg" "no extracted package"
        done
    fi

    if ((RUN_SCENARIOS)); then
        regress_leg "package-fixtures" leg_fixtures
    fi
else
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
fi

if [[ -x "$APP_BIN" ]]; then
    if ((SCENARIOS_ONLY)); then
        for leg in selftest-xvfb ipc-selftest ipc-host-test perf-suite; do
            regress_skip "$leg" "not run (--scenarios-only)"
        done
    else
        regress_leg "selftest-xvfb" leg_selftest
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
            # These cases need a seeded config the suite runs without: the
            # picker skips outright, and the defaults only see the empty one.
            regress_leg "gui-plugin-picker" bash tests/gui_plugin_picker.sh "$APP_BIN"
            regress_leg "gui-settings-defaults" bash tests/gui_settings_defaults.sh "$APP_BIN"
        else
            regress_scenarios_run "$APP_BIN"
        fi
    fi
else
    missing_legs=(selftest-xvfb ipc-selftest ipc-host-test perf-suite)
    if ((RUN_SCENARIOS)); then
        missing_legs+=("${SCENARIO_LEG_NAMES[@]}")
        if ((GUI_SCENARIOS)); then
            missing_legs+=(scenarios-gui gui-plugin-picker gui-settings-defaults)
        fi
    fi
    for leg in "${missing_legs[@]}"; do
        regress_skip "$leg" "app binary missing: ${APP_BIN:-no package}"
    done
fi

if ((INSTALLED)); then
    regress_leg "package-uninstall" leg_package_uninstall
elif ((PACKAGE_MODE)); then
    regress_skip "package-uninstall" "nothing installed"
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
