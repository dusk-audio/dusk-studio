#!/usr/bin/env bash
# Windows source leg of the regression runner: builds and tests this
# checkout's HEAD in the libvirt win11 guest over ssh, in a tree set up the way
# .github/workflows/windows-tests.yml sets up CI's. The guest-side scripts are
# scripts/regress/windows/source/, copied into <tree>\regress\ on every run so
# the guest never runs an old copy. windows.sh is the other Windows runner: it
# tests a release MSI and needs no ssh.
#
# The guest's ssh shell is PowerShell: every remote command is one PowerShell
# line that ends in `exit $LASTEXITCODE`, so the exit status survives it.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck source=scripts/regress/common.sh
source "${REPO_ROOT}/scripts/regress/common.sh"
REGRESS_PLATFORM=windows
SOURCE_DIR="${REPO_ROOT}/scripts/regress/windows/source"

DEFAULT_ROOT='C:\Users\marc\dusk-audit-win'
WIN_HOST="${DUSK_REGRESS_WIN_HOST:-marc@192.168.122.85}"
WIN_KEY="${DUSK_REGRESS_WIN_KEY:-${HOME}/.ssh/dusk_winvm}"
WIN_ROOT="${DUSK_REGRESS_WIN_ROOT:-${DEFAULT_ROOT}}"
FREE_COLD_GB="${DUSK_REGRESS_WIN_FREE_COLD_GB:-$(regress_floor_cold "$REGRESS_WIN_TREE_GB")}"
FREE_WARM_GB="${DUSK_REGRESS_WIN_FREE_WARM_GB:-$(regress_floor_warm "$REGRESS_WIN_RUN_GB")}"
SSH_OPTS=(-i "$WIN_KEY" -o BatchMode=yes -o ConnectTimeout=15
    -o ServerAliveInterval=30 -o ServerAliveCountMax=4)

[[ $# -eq 0 ]] || regress_die "unknown option '$1' for the windows-source target"
regress_win_root_ok "$WIN_ROOT" \
    || regress_die "DUSK_REGRESS_WIN_ROOT must be an absolute Windows path of plain names, none only dots: '$WIN_ROOT'"
[[ "$FREE_COLD_GB" =~ ^[0-9]+$ && "$FREE_WARM_GB" =~ ^[0-9]+$ ]] \
    || regress_die "DUSK_REGRESS_WIN_FREE_COLD_GB and DUSK_REGRESS_WIN_FREE_WARM_GB are whole numbers of GB"
regress_require ssh scp git timeout iconv base64 flock
regress_host_lock windows shared || regress_die "could not take the windows host lock"
cd "$REPO_ROOT"

HEAD_SHA="$(git rev-parse HEAD)"
BASE_SHA="$(git merge-base HEAD origin/main)" \
    || regress_die "HEAD shares no history with origin/main"
SCRIPTS="${WIN_ROOT}\\regress"
RUN_DIR="$(mktemp -d "${TMPDIR:-/tmp}/dusk-regress-winsrc.XXXXXX")"
WIN_ACTIVE=0

remote_cmd() {
    local script="$1"
    shift
    printf 'cmd /c %s\\%s' "$SCRIPTS" "$script"
    [[ $# -eq 0 ]] || printf ' %s' "$@"
    printf '; exit $LASTEXITCODE'
}

remote_ps() {
    regress_win_ps "$WIN_ROOT" "$@"
}

win_cancel() {
    printf '\n--- stopping the run in %s ---\n' "$WIN_HOST"
    if timeout --foreground 120 ssh "${SSH_OPTS[@]}" "$WIN_HOST" \
        "$(remote_ps "${SOURCE_DIR}/cancel.ps1")" </dev/null; then
        return 0
    fi
    echo "error: could not confirm that the run in ${WIN_HOST}:${WIN_ROOT} stopped;" \
        "check the guest before the next run" >&2
    return 1
}

# win_run <deadline seconds> <remote command>. A status from timeout (124),
# from ssh itself (255) or from a signal means the guest side may still be
# running, so it is stopped before the leg reports.
win_run() {
    local deadline="$1" status=0
    WIN_ACTIVE=1
    timeout --foreground "$deadline" ssh "${SSH_OPTS[@]}" "$WIN_HOST" "$2" </dev/null || status=$?
    if ((status == 124 || status == 255 || status > 128)); then
        ((status != 124)) || echo "error: still running after ${deadline} s" >&2
        win_cancel || true
    fi
    WIN_ACTIVE=0
    return "$status"
}

# A leg may be capturing its output to a file the EXIT trap deletes, so the
# cancel reports through the runner's own stdout and stderr.
exec {RUNNER_OUT}>&1 {RUNNER_ERR}>&2
win_interrupt() {
    trap '' INT TERM
    exec 1>&"$RUNNER_OUT" 2>&"$RUNNER_ERR"
    if ((WIN_ACTIVE)); then win_cancel || true; fi
    exit 130
}
trap win_interrupt INT TERM
cleanup() {
    rm -rf "$RUN_DIR"
}
trap cleanup EXIT

last_leg() {
    printf '%s' "${REGRESS_LEG_STATUS[${#REGRESS_LEG_STATUS[@]} - 1]}"
}

# The guest reads the cmd scripts with CRLF line ends whatever this checkout
# has them as.
leg_preflight() {
    win_run 600 "$(remote_ps "${SOURCE_DIR}/preflight.ps1" coldGb="$FREE_COLD_GB" warmGb="$FREE_WARM_GB")" \
        || return 1
    mkdir "${RUN_DIR}/stage"
    local file
    for file in "$SOURCE_DIR"/*.cmd "$SOURCE_DIR"/*.ps1; do
        sed 's/\r$//; s/$/\r/' "$file" >"${RUN_DIR}/stage/$(basename "$file")"
    done
    timeout 300 scp "${SSH_OPTS[@]}" -q "${RUN_DIR}"/stage/* "${WIN_HOST}:${SCRIPTS//\\//}/"
}

# The guest fetches the merge base (and any other commit the local ones stand
# on) from GitHub by hash and takes only the local commits as a bundle.
leg_update() {
    local bases bundle_arg=""
    if [[ "$BASE_SHA" == "$HEAD_SHA" ]]; then
        bases="$HEAD_SHA"
    else
        bases="$(git rev-list --boundary "${BASE_SHA}..HEAD" | sed -n 's/^-//p' | paste -sd+)"
        git bundle create "${RUN_DIR}/head.bundle" "${BASE_SHA}..HEAD" || return 1
        timeout 300 scp "${SSH_OPTS[@]}" -q "${RUN_DIR}/head.bundle" \
            "${WIN_HOST}:${SCRIPTS//\\//}/head.bundle" || return 1
        bundle_arg="${SCRIPTS}\\head.bundle"
    fi
    win_run 3600 "$(remote_cmd update.cmd "$HEAD_SHA" "$bases" ${bundle_arg:+"$bundle_arg"})"
}

# A failed build ends with its compile and link errors gathered from the
# ninja output, which -k 0 lets run past the first one.
build_leg() {
    printf '\n--- build ---\n'
    local start=$SECONDS log="${RUN_DIR}/build.log" rc=0 note=""
    win_run 7200 "$(remote_cmd build.cmd)" >"$log" 2>&1 || rc=$?
    cat "$log"
    if ((rc != 0)); then
        local pattern='error C[0-9]+|error LNK[0-9]+|fatal error' errors count first
        errors="$(grep -E "${pattern}|^FAILED: " "$log" | tr -d '\r' || true)"
        note="exit $rc"
        if [[ -n "$errors" ]]; then
            printf '\nerrors:\n%s\n' "$(head -40 <<<"$errors")"
            count="$(grep -cE "$pattern" <<<"$errors" || true)"
            first="$(grep -m1 -E "$pattern" <<<"$errors" || true)"
            note="${note}: ${count:-0} errors${first:+, first ${first##*\\src\\}}"
        fi
        regress_record build FAIL "$((SECONDS - start))" "$note"
    else
        regress_record build PASS "$((SECONDS - start))"
    fi
}

leg_mesa() {
    win_run 600 "$(remote_cmd mesa.cmd)"
}

# CI's "Check regression profile snapshots" step, taken from the workflow at
# this commit and run in the guest's checkout the way the step runs it: Windows
# PowerShell, $ErrorActionPreference Stop, and the step's exit code. Its TEMP is
# the tree's tmp\ rather than the shared account's. A step that is missing or
# no longer a powershell one fails rather than checking nothing. The guest's
# execution policy is Restricted, where CI's runners let a .ps1 run, so this
# process alone runs unrestricted; and an error goes out as text, since one
# left to the redirected error stream comes back serialised as CLIXML.
leg_snapshot() {
    local workflow="${REPO_ROOT}/.github/workflows/windows-tests.yml" step="${RUN_DIR}/snapshot"
    mkdir -p "$step"
    awk '
        /^[[:space:]]*- name: Check regression profile snapshots[[:space:]]*$/ { found = 1; next }
        found && !body && /^[[:space:]]*- name:/ { exit }
        found && !body && /^[[:space:]]*shell:[[:space:]]*powershell[[:space:]]*$/ { shell = 1; next }
        found && !body && /^[[:space:]]*run:[[:space:]]*\|[[:space:]]*$/ { body = 1; match($0, /^[[:space:]]*/); indent = RLENGTH; next }
        body && /^[[:space:]]*$/ { print ""; next }
        body { match($0, /^[[:space:]]*/); if (RLENGTH <= indent) exit; print substr($0, indent + 3) }
        END { if (!shell) exit 1 }
    ' "$workflow" >"${step}/block.ps1" || {
        echo "error: no powershell step 'Check regression profile snapshots' in $workflow" >&2
        return 1
    }
    grep -q 'windows_regress_snapshot\.ps1' "${step}/block.ps1" || {
        echo "error: the 'Check regression profile snapshots' step in $workflow no longer runs tests\\windows_regress_snapshot.ps1" >&2
        return 1
    }
    sed 's/^/  | /' "${step}/block.ps1"
    cp "${SOURCE_DIR}/inline-lib.ps1" "${step}/inline-lib.ps1"
    {
        printf '%s\n' "\$ErrorActionPreference = 'Stop'" \
            "Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass -Force" \
            "\$tmp = Join-Path \$root 'tmp'" \
            "New-Item -ItemType Directory -Force -Path \$tmp | Out-Null" \
            "\$env:TEMP = \$tmp" "\$env:TMP = \$tmp" \
            "Set-Location -LiteralPath (Join-Path \$root 'src')" \
            "try {"
        cat "${step}/block.ps1"
        printf '%s\n' "} catch {" \
            "  'error: ' + \$_.Exception.Message" \
            "  \$_.InvocationInfo.PositionMessage" \
            "  exit 1" \
            "}" \
            "if (Test-Path -LiteralPath variable:\\LASTEXITCODE) { exit \$LASTEXITCODE }" "exit 0"
    } >"${step}/snapshot.ps1"
    win_run 600 "$(remote_ps "${step}/snapshot.ps1")"
}

# The names CI insists on come from the workflow at this commit, so the two
# lists cannot drift apart. Finding none means the step changed shape, and
# that fails rather than passing an empty check.
leg_ctest_inventory() {
    local workflow="${REPO_ROOT}/.github/workflows/windows-tests.yml"
    local inventory="${RUN_DIR}/inventory.txt" required name missing=0
    required="$(awk '
        /for required in \\$/ { listing = 1; next }
        listing && match($0, /"[^"]+"/) { print substr($0, RSTART + 1, RLENGTH - 2) }
        listing && /; do$/ { listing = 0 }
    ' "$workflow")"
    [[ -n "$required" ]] || {
        echo "error: no required test names found in $workflow" >&2
        return 1
    }
    if ! win_run 600 "$(remote_cmd ctest.cmd list)" >"$inventory" 2>&1; then
        cat "$inventory"
        return 1
    fi
    grep -E '^Total Tests:' "$inventory" | tr -d '\r' || true
    while IFS= read -r name; do
        if ! grep -Fq -- "$name" "$inventory"; then
            echo "error: required Windows regression missing from CTest: $name" >&2
            missing=$((missing + 1))
        fi
    done <<<"$required"
    ((missing == 0)) || return 1
    echo "all $(wc -l <<<"$required") required test names are in the inventory"
}

ctest_leg() {
    printf '\n--- ctest ---\n'
    local start=$SECONDS log="${RUN_DIR}/ctest.log" rc=0 note=""
    win_run 3600 "$(remote_cmd ctest.cmd)" >"$log" 2>&1 || rc=$?
    tr -d '\r' <"$log" >"${log}.lf"
    cat "${log}.lf"
    note="$(grep -m1 -E 'tests passed, [0-9]+ tests failed out of' "${log}.lf" || true)"
    if ((rc != 0)); then
        local failed
        failed="$(sed -n '/^The following tests FAILED:/,$ s/^[[:space:]]*[0-9]* - \(.*\) (.*$/\1/p' "${log}.lf" \
            | paste -sd';' | sed 's/;/; /g')"
        note="exit $rc${note:+: ${note}}${failed:+; ${failed}}"
        regress_record ctest FAIL "$((SECONDS - start))" "$note"
    else
        regress_record ctest PASS "$((SECONDS - start))" "$note"
    fi
}

echo "Dusk Studio regression - windows source"
echo "started $(date '+%F %T')"
echo "guest   ${WIN_HOST}:${WIN_ROOT}"
echo "commit  $(git rev-parse --short HEAD) on merge base $(git rev-parse --short "$BASE_SHA")"

regress_leg "vm-preflight" leg_preflight
if [[ "$(last_leg)" != PASS ]]; then
    regress_summary "regress windows-source" || true
    exit 1
fi

regress_leg "update" leg_update
if [[ "$(last_leg)" == PASS ]]; then
    regress_leg "snapshot-check" leg_snapshot
    build_leg
    if [[ "$(last_leg)" == PASS ]]; then
        regress_leg "mesa" leg_mesa
        regress_leg "ctest-inventory" leg_ctest_inventory
        ctest_leg
    else
        for leg in mesa ctest-inventory ctest; do regress_skip "$leg" "the build failed"; done
    fi
else
    for leg in snapshot-check build mesa ctest-inventory ctest; do regress_skip "$leg" "the update failed"; done
fi

# The bundle and tmp go whatever the run's verdict; the warm tree stays, and
# its size and the drive's free space end the table.
tidy_leg() {
    printf '\n--- footprint ---\n'
    local start=$SECONDS log="${RUN_DIR}/tidy.log" out rc=0 tree="" free="" drive="" parts
    win_run 900 "$(remote_ps "${SOURCE_DIR}/tidy.ps1")" >"$log" 2>&1 || rc=$?
    out="$(tr -d '\r' <"$log")"
    [[ -z "$out" ]] || grep -v '^regress:' <<<"$out" || true
    read -r tree free drive < <(sed -n 's/^regress:footprint //p' <<<"$out" | tail -1) || true
    parts="$(sed -n 's/^regress:part \([^ ]*\) \([0-9]*\)$/\1 \2/p' <<<"$out" | sort -k2,2nr \
        | while read -r name bytes; do printf '%s %s; ' "$name" "$(regress_gb "$bytes")"; done)"
    [[ -z "$parts" ]] || printf 'tree parts: %s\n' "${parts%; }"
    if ((rc != 0)) || [[ -z "$tree" ]]; then
        regress_record footprint FAIL "$((SECONDS - start))" "exit ${rc}: the bundle and tmp may still be in ${WIN_ROOT}"
    else
        regress_record footprint PASS "$((SECONDS - start))" \
            "warm tree ${WIN_ROOT} $(regress_gb "$tree"), ${drive} $(regress_gb "$free") free"
    fi
}
tidy_leg

echo "finished $(date '+%F %T')"
regress_summary "regress windows-source"
