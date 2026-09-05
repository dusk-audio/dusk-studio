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

  linux    (default) build, tests, ctest, JUCE gate, self-test, IPC legs
             --perf              also run the headless engine perf suite
             --vst3 <path>       plugin for the out-of-process host test
                                 (default: the first ~/.vst3/*.vst3)
  mac      drive the M3 Air over ssh: push HEAD, configure, build, ctest,
           headless self-test
             --host <user@host>  default marc@macbook-air.local
  windows  drive the libvirt win11 guest from this box
             --msi <path>          installer to test
             --release-run <id>    download the release-windows artifact instead
  all      linux, then mac, then windows (windows needs --msi/--release-run)

  -h, --help   this text
EOF
}

TARGET="linux"
if [[ $# -gt 0 && "$1" != -* ]]; then
    TARGET="$1"
    shift
fi

case "$TARGET" in
    -h | --help)
        usage
        exit 0
        ;;
    linux | mac | windows | all) ;;
    *)
        usage >&2
        echo >&2
        echo "error: unknown target '$TARGET'" >&2
        exit 2
        ;;
esac

if [[ "$TARGET" == all ]]; then
    rc=0
    bash "${REGRESS_DIR}/linux.sh" || rc=1
    bash "${REGRESS_DIR}/mac.sh" || rc=1
    bash "${REGRESS_DIR}/windows.sh" "$@" || rc=1
    if ((rc != 0)); then
        echo
        echo "regress all: at least one platform FAILED"
    else
        echo
        echo "regress all: OK"
    fi
    exit "$rc"
fi

exec bash "${REGRESS_DIR}/${TARGET}.sh" "$@"
