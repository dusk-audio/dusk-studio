#!/usr/bin/env bash
# Cross-platform regression runner. Re-verifies Dusk Studio on Linux (this box),
# macOS (the M3 Air build node over ssh) and Windows (the libvirt win11 guest).
#
#   scripts/regress.sh                       linux legs only (default)
#   scripts/regress.sh linux --perf
#   scripts/regress.sh mac
#   scripts/regress.sh mac --dmg <dusk-studio-X.Y.Z-macOS-arm64.dmg>
#   scripts/regress.sh mac --release-run <id>
#   scripts/regress.sh linux --tarball <dusk-studio-X.Y.Z-Linux-x86_64.tar.xz>
#   scripts/regress.sh windows --msi <path>
#   scripts/regress.sh windows --release-run <id> --fixtures-run <id>
#   scripts/regress.sh all --msi <path>
#   scripts/regress.sh all --release-run <id>
#   scripts/regress.sh windows-source        build + ctest HEAD in the win11 guest
#   scripts/regress.sh prepush               linux, mac and windows source legs, at once
#
# Per-platform prerequisites and what each leg proves: docs/MAINTAINER-GUIDE.md,
# "Part 9b - Regression run across platforms".

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REGRESS_DIR="${REPO_ROOT}/scripts/regress"

usage() {
    cat <<'EOF'
usage: scripts/regress.sh [linux|mac|windows|windows-source|all|prepush] [options]

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
             --tarball <path>    package mode: extract, smoke-test and
                                 install this release tarball in a scratch
                                 HOME, then run the self-test, IPC and
                                 scenario legs (GUI included) against the
                                 installed launcher instead of build/
             --release-run <id>  package mode with the release-linux-<arch>
                                 tarball of that release.yml run
  mac      drive the M3 Air over ssh: push HEAD, configure, build, ctest,
           headless self-test
             --host <user@host>  default marc@macbook-air.local
             --dmg <path>        package mode: install this disk image into
                                 /Applications on the Air, build the fixture
                                 plug-ins there from this checkout's HEAD, run
                                 the engine suite over ssh and the GUI suite in
                                 the desktop session, answering the macOS
                                 privacy prompts over Screen Sharing
             --release-run <id>  package mode with the release-macos disk
                                 image of that release.yml run, and fixtures
                                 from the commit the run built
  windows  drive the libvirt win11 guest from this box: install the MSI into
           Program Files (the runner accepts the test guest's UAC prompt),
           then the self-test, handoff, session-close, scenario and isolation
           legs against the installed app
             --msi <path>          installer to test
             --release-run <id>    download the release-windows artifact instead
             --fixtures <dir>      Windows builds of the fixture plug-ins, laid
                                   out as build-tests/ lays them out
             --fixtures-run <id>   the duskstudio-windows-fixtures artifact of
                                   that windows-tests.yml run instead
             --reinstall           uninstall this package, and any per-user
                                   install, first
             --extract-only        unpack with msiexec /a instead of installing:
                                   no UAC, for runner development only
             --no-scenarios        leave the scenario legs out
  windows-source
           drive the libvirt win11 guest over ssh: bring its CI-equivalent
           tree to HEAD, build CI's targets, run the Mesa check and ctest.
           No options; DUSK_REGRESS_WIN_HOST, DUSK_REGRESS_WIN_KEY and
           DUSK_REGRESS_WIN_ROOT override the guest, key and tree
  all      linux, then mac, then windows (windows needs --msi/--release-run),
           then one table of every platform's legs. Options are routed to the
           platform that owns them, so `all --perf --msi <path>` is one run
           with both. --release-run goes to all three, so `all --release-run
           <id>` tests that release run's tarball, disk image and installer;
           a platform given a local package (--tarball, --dmg, --msi) tests
           that one instead. DUSK_REGRESS_DRY_RUN=1 prints the routing and
           stops.

  prepush  before asking for a push: the linux, mac and windows-source legs
           at the same time (they run on three machines), then one table.
           The linux options above go to linux and --host to mac; package
           options, the windows package options among them, are refused,
           since a push is about this checkout. Each platform's full output
           goes to a log named in the table's header.

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
    linux | mac | windows | windows-source | all | prepush) ;;
    *)
        usage >&2
        echo >&2
        echo "error: unknown target '$TARGET'" >&2
        exit 2
        ;;
esac

if [[ "$TARGET" != all && "$TARGET" != prepush ]]; then
    exec bash "${REGRESS_DIR}/${TARGET}.sh" "$@"
fi

# `all` runs three scripts that each reject options they do not know, so the
# shared command line has to be split by owner first. An option nobody claims is
# a typo: routing it nowhere would run the whole matrix and silently ignore it.
# An option with several owners goes to each of them: one release run feeds
# every platform that tests its packages.
declare -A OPTION_OWNER=(
    [--perf]=linux
    [--vst3]=linux
    [--scenarios]=linux
    [--no-scenarios]="linux windows"
    [--gui-scenarios]=linux
    [--scenarios-only]=linux
    [--release-checks]=linux
    [--tarball]=linux
    [--host]=mac
    [--dmg]=mac
    [--msi]=windows
    [--fixtures]=windows
    [--fixtures-run]=windows
    [--reinstall]=windows
    [--extract-only]=windows
    [--release-run]="linux mac windows"
)
declare -A OPTION_TAKES_VALUE=(
    [--vst3]=1
    [--tarball]=1
    [--host]=1
    [--dmg]=1
    [--msi]=1
    [--fixtures]=1
    [--fixtures-run]=1
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

# A local package already names that platform's package, so a release run
# alongside it is for the platforms that have none.
declare -A LOCAL_PACKAGE=()
for arg in "$@"; do
    case "$arg" in
        --tarball) LOCAL_PACKAGE[linux]=1 ;;
        --dmg) LOCAL_PACKAGE[mac]=1 ;;
        --msi) LOCAL_PACKAGE[windows]=1 ;;
    esac
done

while [[ $# -gt 0 ]]; do
    owner="${OPTION_OWNER[$1]:-}"
    if [[ "$1" == --release-run ]]; then
        owner=""
        for platform in linux mac windows; do
            [[ -n "${LOCAL_PACKAGE[$platform]:-}" ]] || owner="${owner:+${owner} }${platform}"
        done
        if [[ -z "$owner" ]]; then
            echo "error: every platform has a local package; --release-run would test nothing" >&2
            exit 2
        fi
    fi
    if [[ "$TARGET" == prepush && -n "$owner" ]]; then
        kept=""
        for platform in $owner; do
            [[ "$platform" == windows ]] || kept="${kept:+${kept} }${platform}"
        done
        owner="$kept"
        if [[ -z "$owner" ]]; then
            echo "error: $1 is for the Windows package run; prepush runs the Windows source leg" >&2
            exit 2
        fi
    fi
    if [[ -z "$owner" ]]; then
        usage >&2
        echo >&2
        echo "error: no platform takes the option '$1'" >&2
        exit 2
    fi
    if [[ -n "${OPTION_TAKES_VALUE[$1]:-}" ]]; then
        [[ $# -ge 2 ]] || { echo "error: $1 needs a value" >&2; exit 2; }
        for platform in $owner; do route_option "$platform" "$1" "$2"; done
        shift 2
    else
        for platform in $owner; do route_option "$platform" "$1"; done
        shift
    fi
done

if [[ "$TARGET" == prepush ]]; then
    for arg in ${LINUX_ARGS[@]+"${LINUX_ARGS[@]}"} ${MAC_ARGS[@]+"${MAC_ARGS[@]}"}; do
        case "$arg" in
            --tarball | --dmg | --release-run | --scenarios-only)
                echo "error: prepush builds and tests this checkout; $arg does not belong in it" >&2
                exit 2
                ;;
        esac
    done
fi

if [[ "${DUSK_REGRESS_DRY_RUN:-0}" == 1 ]]; then
    printf 'linux:   %s\n' "${LINUX_ARGS[*]-}"
    printf 'mac:     %s\n' "${MAC_ARGS[*]-}"
    if [[ "$TARGET" == prepush ]]; then
        printf 'windows: source leg (windows-source.sh)\n'
    else
        printf 'windows: %s\n' "${WINDOWS_ARGS[*]-}"
    fi
    exit 0
fi

RECORD_FILE="$(mktemp "${TMPDIR:-/tmp}/dusk-regress-all.XXXXXX")"
trap 'rm -f "$RECORD_FILE"' EXIT
export DUSK_REGRESS_RECORD_FILE="$RECORD_FILE"

# A runner that dies before its first leg (a usage error, a missing tool)
# records nothing, so its exit status gets a row of its own.
rc=0
# run_platform <platform> <runner> [options]: the platform labels the rows,
# the runner is the script under scripts/regress/ that records them.
run_platform() {
    local platform="$1" runner="$2" status=0
    shift 2
    # Keep the wrapper alive until its runner finishes signal cleanup.
    if [[ "$TARGET" == prepush ]]; then
        trap ':' INT TERM
        printf '%s %s started\n' "$(date '+%F %T')" "$runner"
    fi
    bash "${REGRESS_DIR}/${runner}.sh" "$@" || status=$?
    if [[ "$TARGET" == prepush ]]; then
        printf '%s %s finished, exit %s\n' "$(date '+%F %T')" "$runner" "$status"
    fi
    if ((status != 0)); then
        rc=1
        if ! grep -q "^${platform}"$'\tFAIL\t' "$RECORD_FILE"; then
            printf '%s\tFAIL\trunner\t0\texit %s with no failed leg\n' "$platform" "$status" >>"$RECORD_FILE"
        fi
    fi
    return 0
}
if [[ "$TARGET" == prepush ]]; then
    # In parallel the three outputs would interleave, so each goes to a log that
    # outlives the run; run_platform's rc is lost in the background, but a
    # failing runner always leaves a FAIL row, which the check below reads.
    LOG_DIR="$(mktemp -d "${TMPDIR:-/tmp}/dusk-prepush.XXXXXX")"
    stop_runners() {
        trap '' INT TERM
        local -a runner_pids=($(jobs -pr)) targets=()
        local caller_group
        caller_group=$(ps -o pgid= -p "$$")
        # timeout creates separate groups. Snapshot the whole tree before
        # signalling, while the parent links still identify its descendants.
        targets=($(ps -eo pid=,ppid=,pgid= | awk -v roots="${runner_pids[*]}" -v caller="$caller_group" '
            BEGIN { n = split(roots, ids, " "); for (i = 1; i <= n; ++i) root[ids[i]] = 1 }
            { parent[$1] = $2; group[$1] = $3 }
            END {
                for (pid in parent) {
                    ancestor = pid
                    while (ancestor in parent && !root[ancestor]) ancestor = parent[ancestor]
                    if (root[ancestor]) {
                        target[pid] = 1
                        if (group[pid] != caller + 0) groups[group[pid]] = 1
                    }
                }
                for (pgid in groups) print "-" pgid
                for (pid in target) print pid
            }'))
        kill -TERM -- "${targets[@]}" 2>/dev/null || true
        wait
        exit 130
    }
    trap stop_runners INT TERM
    set -m
    run_platform linux linux ${LINUX_ARGS[@]+"${LINUX_ARGS[@]}"} >"${LOG_DIR}/linux.log" 2>&1 &
    run_platform mac mac ${MAC_ARGS[@]+"${MAC_ARGS[@]}"} >"${LOG_DIR}/mac.log" 2>&1 &
    run_platform windows windows-source >"${LOG_DIR}/windows.log" 2>&1 &
    echo "prepush: linux, mac and windows running; logs in ${LOG_DIR}"
    wait
    set +m
    trap - INT TERM
else
    run_platform linux linux ${LINUX_ARGS[@]+"${LINUX_ARGS[@]}"}
    run_platform mac mac ${MAC_ARGS[@]+"${MAC_ARGS[@]}"}
    run_platform windows windows ${WINDOWS_ARGS[@]+"${WINDOWS_ARGS[@]}"}
fi

printf '\n===== regress %s =====\n' "$TARGET"
[[ -z "${LOG_DIR:-}" ]] || printf 'logs: %s/{linux,mac,windows}.log\n' "$LOG_DIR"
printf '%-6s %-40s %8s  %s\n' "RESULT" "LEG" "SECONDS" "NOTE"
while IFS=$'\t' read -r platform status name secs note; do
    printf '%-6s %-40s %8s  %s\n' "$status" "${platform}/${name}" "$secs" "$note"
done <"$RECORD_FILE"
if ((rc != 0)) || grep -q $'^[a-z]*\tFAIL\t' "$RECORD_FILE"; then
    printf '\nregress %s: FAILED\n' "$TARGET"
    exit 1
fi
printf '\nregress %s: OK\n' "$TARGET"
exit 0
