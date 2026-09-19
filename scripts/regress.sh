#!/usr/bin/env bash
# Cross-platform regression runner. Re-verifies Dusk Studio on Linux (this box),
# macOS (the M3 Air build node over ssh) and Windows (the libvirt win11 guest).
#
#   scripts/regress.sh                       linux legs only (default)
#   scripts/regress.sh linux --perf
#   scripts/regress.sh mac
#   scripts/regress.sh windows --msi <path>
#   scripts/regress.sh all --msi <path>
#
# Per-platform prerequisites and what each leg proves: docs/MAINTAINER-GUIDE.md,
# "Part 9b - Regression run across platforms".

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REGRESS_DIR="${REPO_ROOT}/scripts/regress"

usage() {
    cat <<'EOF'
usage: scripts/regress.sh [linux|mac|windows|all] [options]

  linux    (default) build, tests, ctest, JUCE gate, self-test, IPC and
           scenario legs
             --perf              also run the headless engine perf suite
             --vst3 <path>       plugin for the out-of-process host test
                                 (default: the first ~/.vst3/*.vst3)
             --scenarios         run the scenario legs (the default)
             --no-scenarios      leave the scenario legs out
             --gui-scenarios     also run the GUI scenario suite
             --scenarios-only    scenario legs only, against the binary
                                 already in build/
             --release-checks    also the pre-tag legs: release metadata,
                                 the main ruleset, Patreon freshness
  mac      drive the M3 Air over ssh: push HEAD, configure, build, ctest,
           headless self-test
             --host <user@host>  default marc@macbook-air.local
  windows  drive the libvirt win11 guest from this box
             --msi <path>          installer to test
             --release-run <id>    download the release-windows artifact instead
  all      linux, then mac, then windows (windows needs --msi/--release-run).
           Options are routed to the platform that owns them, so
           `all --perf --msi <path>` is one run with both.

  -h, --help   this text
EOF
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
    usage
    exit 0
fi

TARGET="linux"
if [[ $# -gt 0 && "$1" != -* ]]; then
    TARGET="$1"
    shift
fi

case "$TARGET" in
    linux | mac | windows | all) ;;
    *)
        usage >&2
        echo >&2
        echo "error: unknown target '$TARGET'" >&2
        exit 2
        ;;
esac

if [[ "$TARGET" != all ]]; then
    exec bash "${REGRESS_DIR}/${TARGET}.sh" "$@"
fi

# `all` runs three scripts that each reject options they do not know, so the
# shared command line has to be split by owner first. An option nobody claims is
# a typo: routing it nowhere would run the whole matrix and silently ignore it.
declare -A OPTION_OWNER=(
    [--perf]=linux
    [--vst3]=linux
    [--scenarios]=linux
    [--no-scenarios]=linux
    [--gui-scenarios]=linux
    [--scenarios-only]=linux
    [--release-checks]=linux
    [--host]=mac
    [--msi]=windows
    [--release-run]=windows
)
declare -A OPTION_TAKES_VALUE=(
    [--vst3]=1
    [--host]=1
    [--msi]=1
    [--release-run]=1
)

LINUX_ARGS=()
MAC_ARGS=()
WINDOWS_ARGS=()

route_option() {
    local platform="$1" option="$2" value="${3-}"
    case "$platform" in
        linux) LINUX_ARGS+=("$option") ;;
        mac) MAC_ARGS+=("$option") ;;
        windows) WINDOWS_ARGS+=("$option") ;;
    esac
    if [[ $# -lt 3 ]]; then return 0; fi
    case "$platform" in
        linux) LINUX_ARGS+=("$value") ;;
        mac) MAC_ARGS+=("$value") ;;
        windows) WINDOWS_ARGS+=("$value") ;;
    esac
    return 0
}

while [[ $# -gt 0 ]]; do
    owner="${OPTION_OWNER[$1]:-}"
    if [[ -z "$owner" ]]; then
        usage >&2
        echo >&2
        echo "error: no platform takes the option '$1'" >&2
        exit 2
    fi
    if [[ -n "${OPTION_TAKES_VALUE[$1]:-}" ]]; then
        [[ $# -ge 2 ]] || { echo "error: $1 needs a value" >&2; exit 2; }
        route_option "$owner" "$1" "$2"
        shift 2
    else
        route_option "$owner" "$1"
        shift
    fi
done

if [[ "${DUSK_REGRESS_DRY_RUN:-0}" == 1 ]]; then
    printf 'linux:   %s\n' "${LINUX_ARGS[*]-}"
    printf 'mac:     %s\n' "${MAC_ARGS[*]-}"
    printf 'windows: %s\n' "${WINDOWS_ARGS[*]-}"
    exit 0
fi

rc=0
bash "${REGRESS_DIR}/linux.sh" ${LINUX_ARGS[@]+"${LINUX_ARGS[@]}"} || rc=1
bash "${REGRESS_DIR}/mac.sh" ${MAC_ARGS[@]+"${MAC_ARGS[@]}"} || rc=1
bash "${REGRESS_DIR}/windows.sh" ${WINDOWS_ARGS[@]+"${WINDOWS_ARGS[@]}"} || rc=1
if ((rc != 0)); then
    echo
    echo "regress all: at least one platform FAILED"
else
    echo
    echo "regress all: OK"
fi
exit "$rc"
