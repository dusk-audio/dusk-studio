#!/usr/bin/env bash
# `scripts/regress.sh clean [mac|windows|all] [--host user@host]`, and the end
# of a run given --clean: removes what the regression runner owns on the macOS
# node and in the Windows guest, and nothing it does not. Each host gets one
# row, "clean", whose note says what went and how much the host has free.
#
#   mac      run folders under ~/.cache/dusk-regress whose process group is gone,
#            regress-* branches no worktree has checked out, and the package
#            run folders under ~/dusk-regress-pkg. The node's build trees stay.
#   windows  the source tree, only with its .dusk-regress-tree marker, and the
#            package run folders, only with their .dusk-regress-run marker.
#
# A host a run on this box is using is left alone: see regress_host_lock.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck source=scripts/regress/common.sh
source "${REPO_ROOT}/scripts/regress/common.sh"
REGRESS_DIR="${REPO_ROOT}/scripts/regress"

TARGET="all"
MAC_HOST="${DUSK_REGRESS_MAC_HOST:-marc@macbook-air.local}"
if [[ $# -gt 0 && "$1" != -* ]]; then
    TARGET="$1"
    shift
fi
while [[ $# -gt 0 ]]; do
    case "$1" in
        --host)
            [[ $# -ge 2 ]] || regress_die "--host needs user@host"
            MAC_HOST="$2"
            shift 2
            ;;
        *) regress_die "unknown option '$1' for clean" ;;
    esac
done
case "$TARGET" in
    mac | windows | all) ;;
    *) regress_die "clean takes mac, windows or all, not '$TARGET'" ;;
esac
regress_require ssh timeout flock

WIN_HOST="${DUSK_REGRESS_WIN_HOST:-marc@192.168.122.85}"
WIN_KEY="${DUSK_REGRESS_WIN_KEY:-${HOME}/.ssh/dusk_winvm}"
WIN_ROOT="${DUSK_REGRESS_WIN_ROOT:-C:\\Users\\marc\\dusk-audit-win}"
regress_win_root_ok "$WIN_ROOT" \
    || regress_die "DUSK_REGRESS_WIN_ROOT must be an absolute Windows path of plain names, none only dots: '$WIN_ROOT'"

# clean_report <rc> <output> <start> <host>: prints what the host said and
# records its row. A warn: line with a clean exit is a WARN: something the
# runner made is still there, and the line says what it takes to remove it.
clean_report() {
    local rc="$1" out="$2" start="$3" host="$4" removed free note status=PASS first
    grep -v '^regress:' <<<"$out" || true
    removed="$(grep -c '^removed ' <<<"$out" || true)"
    free="$(sed -n 's/^regress:free //p' <<<"$out" | tail -1)"
    note="${host}: ${removed} removed"
    if [[ -n "$free" ]]; then note="${note}, ${free} free"; fi
    if ((rc != 0)); then
        status=FAIL
        first="$(grep -m1 '^error: ' <<<"$out" || true)"
        note="${note}; ${first:-exit ${rc}}"
    elif grep -q '^warn: ' <<<"$out"; then
        status=WARN
        note="${note}; $(grep -m1 '^warn: ' <<<"$out")"
    fi
    regress_record clean "$status" "$((SECONDS - start))" "$note"
}

clean_mac() {
    REGRESS_PLATFORM=mac
    printf '\n--- clean (%s) ---\n' "$MAC_HOST"
    local start=$SECONDS rc=0 out command repos trees kb tree_note=""
    if ! regress_host_lock mac exclusive; then
        regress_record clean FAIL 0 "a run on this box is using the mac host; clean after it ends"
        return 0
    fi
    repos="src/dusk-studio"
    trees=$'src/dusk-studio/build\nsrc/dusk-studio/build-tests\nsrc/dusk-studio-regress/build-tests'
    if [[ -n "${DUSK_REGRESS_MAC_REPO:-}" && "$DUSK_REGRESS_MAC_REPO" != src/dusk-studio ]]; then
        repos="${repos}"$'\n'"${DUSK_REGRESS_MAC_REPO}"
        local dir
        for dir in "${DUSK_REGRESS_MAC_BUILD_DIR:-build}" "${DUSK_REGRESS_MAC_TEST_BUILD_DIR:-build-tests}"; do
            [[ "$dir" == /* ]] || dir="${DUSK_REGRESS_MAC_REPO}/${dir}"
            trees="${trees}"$'\n'"${dir}"
        done
    fi
    printf -v command 'exec env REGRESS_REPOS=%q REGRESS_TREES=%q bash -s' "$repos" "$trees"
    out="$(timeout 900 ssh -o BatchMode=yes -o ConnectTimeout=15 -o ServerAliveInterval=30 \
        "$MAC_HOST" "$command" <"${REGRESS_DIR}/mac/clean-node.sh" 2>&1)" || rc=$?
    while read -r kb path; do
        tree_note="${tree_note}${tree_note:+, }${path##*/src/} $(regress_gb "$((kb * 1024))")"
    done < <(sed -n 's/^regress:tree //p' <<<"$out")
    kb="$(sed -n 's/^regress:free //p' <<<"$out" | tail -1)"
    if [[ "$kb" =~ ^[0-9]+$ ]]; then
        out="$(sed '/^regress:free /d' <<<"$out")"$'\n'"regress:free $(regress_gb "$((kb * 1024))")"
    fi
    regress_host_unlock
    clean_report "$rc" "$out" "$start" "$MAC_HOST"
    [[ -z "$tree_note" ]] || printf 'warm trees kept on the node: %s\n' "$tree_note"
}

clean_windows() {
    REGRESS_PLATFORM=windows
    printf '\n--- clean (%s) ---\n' "$WIN_HOST"
    local start=$SECONDS rc=0 out command bytes drive
    if ! regress_host_lock windows exclusive; then
        regress_record clean FAIL 0 "a run on this box is using the windows guest; clean after it ends"
        return 0
    fi
    if ! command -v iconv >/dev/null || ! command -v base64 >/dev/null; then
        regress_host_unlock
        regress_record clean FAIL 0 "iconv and base64 are required on this box to send the clean script"
        return 0
    fi
    if ! command="$(regress_win_ps "$WIN_ROOT" "${REGRESS_DIR}/windows/source/clean.ps1")"; then
        regress_host_unlock
        regress_record clean FAIL 0 "the clean script could not be prepared"
        return 0
    fi
    out="$(timeout 3600 ssh -i "$WIN_KEY" -o BatchMode=yes -o ConnectTimeout=15 \
        -o ServerAliveInterval=30 -o ServerAliveCountMax=4 "$WIN_HOST" "$command" </dev/null 2>&1)" || rc=$?
    regress_host_unlock
    out="$(tr -d '\r' <<<"$out")"
    read -r bytes drive < <(sed -n 's/^regress:free //p' <<<"$out" | tail -1) || true
    if [[ "${bytes:-}" =~ ^[0-9]+$ ]]; then
        out="$(sed '/^regress:free /d' <<<"$out")"$'\n'"regress:free ${drive} $(regress_gb "$bytes")"
    fi
    clean_report "$rc" "$out" "$start" "$WIN_HOST"
}

echo "Dusk Studio regression - clean ${TARGET}"
case "$TARGET" in
    mac) clean_mac ;;
    windows) clean_windows ;;
    all)
        clean_mac
        clean_windows
        ;;
esac
regress_summary "regress clean ${TARGET}"
