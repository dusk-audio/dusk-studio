#!/usr/bin/env bash
# macOS leg of the regression runner: drives the M3 Air build node over ssh.
#
# The node is key-auth only and never uses sudo. Its toolchain lives in ~/bin
# and ~/tools, which is why every remote step goes through ~/mac-configure.sh
# or exports PATH itself. There is no GNU timeout there, so remote deadlines
# are a poll loop over a completion marker.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck source=scripts/regress/common.sh
source "${REPO_ROOT}/scripts/regress/common.sh"

MAC_HOST="${DUSK_REGRESS_MAC_HOST:-marc@macbook-air.local}"
MAC_REPO="src/dusk-studio"
MAC_DONOR="src/plugins-main"
MAC_DAF="src/DPF"
MAC_DAF_WIDGETS="src/DPF-Widgets"
JOBS="${DUSK_JOBS:-6}"
SSH_OPTS=(-o BatchMode=yes -o ConnectTimeout=15 -o ServerAliveInterval=30)

while [[ $# -gt 0 ]]; do
    case "$1" in
        --host)
            [[ $# -ge 2 ]] || regress_die "--host needs user@host"
            MAC_HOST="$2"
            shift 2
            ;;
        *) regress_die "unknown option '$1' for the mac target" ;;
    esac
done

regress_require ssh git timeout
cd "$REPO_ROOT"

HEAD_SHA="$(git rev-parse HEAD)"
HEAD_SHORT="$(git rev-parse --short HEAD)"
REMOTE_BRANCH="regress-${HEAD_SHORT}"
MAC_PREV_BRANCH=""
MAC_PREV_COMMIT=""
MAC_PREV_SUBMODULES=""
MAC_HOME=""

DONOR_REV="$(sed -n 's/^[[:space:]]*DONOR_REV:[[:space:]]*//p' \
    .github/workflows/release.yml | head -1)"
[[ -n "$DONOR_REV" ]] || regress_die "could not read DONOR_REV from .github/workflows/release.yml"

daf_pin() { # <var name in the composite action>
    sed -n "s/^[[:space:]]*$1:[[:space:]]*//p" \
        .github/actions/clone-daf-stack/action.yml | head -1
}
DAF_REV="$(daf_pin DAF_REV)"
PUGL_REV="$(daf_pin PUGL_REV)"
DAF_WIDGETS_REV="$(daf_pin DAF_WIDGETS_REV)"

# mac_run <deadline seconds> <<'REMOTE' ... REMOTE
mac_run() {
    local deadline="$1"
    timeout "$deadline" ssh "${SSH_OPTS[@]}" "$MAC_HOST" 'bash -s'
}

restore_branch() {
    [[ -n "$MAC_PREV_BRANCH" ]] || return 0
    # The name comes from the node's own checkout and is spliced into a script
    # the remote bash interprets, so it travels as a bash-quoted word (%q): a
    # ref name may legally contain $, ;, &, | and quotes. abbrev-ref reports
    # HEAD for a detached checkout; that one goes back by commit, not by name.
    local checkout_args label
    if [[ "$MAC_PREV_BRANCH" == HEAD ]]; then
        checkout_args="--detach $(printf '%q' "$MAC_PREV_COMMIT")"
        label="detached ${MAC_PREV_COMMIT:0:12}"
    else
        checkout_args="$(printf '%q' "$MAC_PREV_BRANCH")"
        label="$MAC_PREV_BRANCH"
    fi
    printf '\n--- restoring %s to %s ---\n' "$MAC_HOST" "$label"
    mac_run 600 <<REMOTE || echo "warning: could not restore ${label}" >&2
set -euo pipefail
cd "\$HOME/${MAC_REPO}"
git checkout -q ${checkout_args}
git branch -q -D "${REMOTE_BRANCH}" 2>/dev/null || true
while read -r path sha; do
    [[ -n "\$path" ]] || continue
    git -C "\$path" checkout -q --detach "\$sha" \
        || echo "warn: could not restore submodule \$path to \$sha"
done <<'SUBMODULES'
${MAC_PREV_SUBMODULES}
SUBMODULES
git rev-parse --abbrev-ref HEAD
git submodule status
REMOTE
}
trap restore_branch EXIT

leg_preflight() {
    mac_run 180 <<REMOTE
set -euo pipefail
export PATH="\$HOME/bin:\$PATH"
# The review model pins several GB of wired memory; a -j${JOBS} build on a
# 16 GB laptop will swap with it resident.
if curl -s -m 5 localhost:11434/api/generate -d '{"model":"qwen-review","keep_alive":0}' >/dev/null 2>&1; then
    echo "ollama: unload requested"
else
    echo "ollama: not responding (nothing to unload)"
fi
echo "macOS \$(sw_vers -productVersion), \$(uname -m)"
cd "\$HOME/${MAC_REPO}"
# Submodule pointers are handled separately (they get synced to the pushed
# commit and put back afterwards), so they must not count as local dirt. Edits
# inside a submodule's own worktree would be lost by that sync, so they do.
if [[ -n "\$(git status --porcelain --ignore-submodules=all)" ]]; then
    echo "error: ${MAC_HOST}:${MAC_REPO} has local modifications; commit or clean it first" >&2
    git status --short --ignore-submodules=all >&2
    exit 1
fi
if [[ -n "\$(git submodule foreach --recursive --quiet 'git status --porcelain' 2>/dev/null)" ]]; then
    echo "error: ${MAC_HOST}:${MAC_REPO} has modified submodule worktrees; commit or clean them first" >&2
    git submodule foreach --recursive --quiet 'git status --short | sed "s|^|  \$path: |"' >&2
    exit 1
fi
echo "regress:home \$HOME"
echo "regress:branch \$(git rev-parse --abbrev-ref HEAD)"
echo "regress:commit \$(git rev-parse HEAD)"
git submodule status | awk '{ gsub(/^[-+U]/, "", \$1); print "regress:submodule", \$2, \$1 }'
REMOTE
}

leg_push() {
    git push --force "ssh://${MAC_HOST}${MAC_HOME}/${MAC_REPO}" \
        "HEAD:refs/heads/${REMOTE_BRANCH}"
}

leg_checkout() {
    mac_run 900 <<REMOTE
set -euo pipefail
cd "\$HOME/${MAC_REPO}"
git checkout -q "${REMOTE_BRANCH}"
[[ "\$(git rev-parse HEAD)" == "${HEAD_SHA}" ]] || {
    echo "error: mac checkout \$(git rev-parse HEAD) != pushed ${HEAD_SHA}" >&2; exit 1; }
git submodule update --init --recursive
git log --oneline -1
git submodule status
REMOTE
}

leg_donor() {
    mac_run 900 <<REMOTE
set -euo pipefail
cd "\$HOME/${MAC_DONOR}"
if [[ "\$(git rev-parse HEAD)" != "${DONOR_REV}" ]]; then
    git fetch --depth 1 origin "${DONOR_REV}"
    git checkout -q --detach "${DONOR_REV}"
fi
[[ "\$(git rev-parse HEAD)" == "${DONOR_REV}" ]] || {
    echo "error: donor \$(git rev-parse HEAD) != DONOR_REV ${DONOR_REV}" >&2; exit 1; }
echo "donor \$(git log --oneline -1)"
REMOTE
}

# The DAF stack is best effort: the node may be offline from GitHub, and a
# drifted DAF only matters for the native UI, not for the DSP legs below.
leg_daf() {
    mac_run 900 <<REMOTE
set -euo pipefail
sync_pin() {
    local dir="\$1" rev="\$2" name="\$3"
    cd "\$HOME/\$dir"
    local before
    before="\$(git rev-parse HEAD)"
    if [[ "\$before" != "\$rev" ]]; then
        if ! git fetch --depth 1 origin "\$rev" >/dev/null 2>&1; then
            echo "warn: \$name could not fetch \$rev (left at \${before:0:8})"
            return 0
        fi
        git checkout -q --detach "\$rev"
    fi
    echo "\$name \$(git rev-parse --short HEAD) (was \${before:0:8})"
}
sync_pin "${MAC_DAF}" "${DAF_REV}" DAF
sync_pin "${MAC_DAF_WIDGETS}" "${DAF_WIDGETS_REV}" DAF-Widgets
cd "\$HOME/${MAC_DAF}"
git submodule update --init --depth 1 >/dev/null 2>&1 || echo "warn: DAF submodule update failed"
have="\$(git -C dgl/src/pugl-upstream rev-parse HEAD 2>/dev/null || echo none)"
if [[ "\$have" == "${PUGL_REV}" ]]; then
    echo "pugl \${have:0:8} (pinned)"
else
    echo "warn: pugl \${have:0:8} != pinned ${PUGL_REV}"
fi
REMOTE
}

leg_configure_app() {
    mac_run 900 <<REMOTE
set -euo pipefail
"\$HOME/mac-configure.sh" build
REMOTE
}

leg_configure_tests() {
    mac_run 900 <<REMOTE
set -euo pipefail
"\$HOME/mac-configure.sh" build-tests -DDUSKSTUDIO_BUILD_TESTS=ON
REMOTE
}

leg_build_app() {
    mac_run 5400 <<REMOTE
set -euo pipefail
export PATH="\$HOME/bin:\$PATH"
cd "\$HOME/${MAC_REPO}"
cmake --build build -j${JOBS}
REMOTE
}

leg_build_tests() {
    mac_run 5400 <<REMOTE
set -euo pipefail
export PATH="\$HOME/bin:\$PATH"
cd "\$HOME/${MAC_REPO}"
cmake --build build-tests --target dusk-studio-tests -j${JOBS}
REMOTE
}

leg_ctest() {
    mac_run 1800 <<REMOTE
set -euo pipefail
export PATH="\$HOME/bin:\$PATH"
cd "\$HOME/${MAC_REPO}"
ctest --test-dir build-tests --output-on-failure
REMOTE
}

# No GNU timeout on the node, so the self-test runs behind a marker-file poll.
leg_selftest() {
    mac_run 600 <<REMOTE
set -euo pipefail
BIN="\$HOME/${MAC_REPO}/build/DuskStudio_artefacts/Release/DuskStudio.app/Contents/MacOS/DuskStudio"
[[ -x "\$BIN" ]] || { echo "error: self-test binary missing: \$BIN" >&2; exit 1; }
RC_FILE="\$(mktemp -t duskstudio-selftest-rc)"
LOG_FILE="\$(mktemp -t duskstudio-selftest-log)"
PID_FILE="\$(mktemp -t duskstudio-selftest-pid)"
rm -f "\$RC_FILE"
# The subshell records the app's own pid: killing the subshell alone would
# orphan a hung DuskStudio.
( DUSKSTUDIO_RUN_SELFTEST=1 "\$BIN" >"\$LOG_FILE" 2>&1 & APP=\$!; echo "\$APP" >"\$PID_FILE"; wait "\$APP"; echo \$? >"\$RC_FILE" ) &
CHILD=\$!
WAITED=0
while [[ ! -f "\$RC_FILE" ]]; do
    if (( WAITED >= 420 )); then
        APP_PID="\$(cat "\$PID_FILE" 2>/dev/null || true)"
        [[ -n "\$APP_PID" ]] && kill -9 "\$APP_PID" 2>/dev/null || true
        wait "\$CHILD" 2>/dev/null || true
        echo "error: self-test still running after 420 s" >&2
        sed 's/^/  /' "\$LOG_FILE" >&2
        rm -f "\$RC_FILE" "\$LOG_FILE" "\$PID_FILE"
        exit 124
    fi
    sleep 2
    WAITED=\$(( WAITED + 2 ))
done
wait "\$CHILD" 2>/dev/null || true
RC="\$(cat "\$RC_FILE")"
grep -E '^\[(PASS|FAIL|SKIP)\]|^Total' "\$LOG_FILE" || sed 's/^/  /' "\$LOG_FILE"
rm -f "\$RC_FILE" "\$LOG_FILE" "\$PID_FILE"
exit "\$RC"
REMOTE
}

echo "Dusk Studio regression - mac"
echo "node    $MAC_HOST"
echo "commit  ${HEAD_SHORT} -> ${REMOTE_BRANCH}"
echo "donor   ${DONOR_REV}"
echo "jobs    -j${JOBS}"

printf '\n--- mac-preflight ---\n'
preflight_start=$SECONDS
preflight_out=""
preflight_rc=0
preflight_out="$(leg_preflight)" || preflight_rc=$?
printf '%s\n' "$preflight_out"
if ((preflight_rc == 0)); then
    MAC_HOME="$(sed -n 's/^regress:home //p' <<<"$preflight_out" | head -1)"
    MAC_PREV_BRANCH="$(sed -n 's/^regress:branch //p' <<<"$preflight_out" | head -1)"
    MAC_PREV_COMMIT="$(sed -n 's/^regress:commit //p' <<<"$preflight_out" | head -1)"
    MAC_PREV_SUBMODULES="$(sed -n 's/^regress:submodule //p' <<<"$preflight_out")"
    [[ -n "$MAC_HOME" && -n "$MAC_PREV_BRANCH" && -n "$MAC_PREV_COMMIT" ]] \
        || regress_die "preflight did not report the node's home directory, branch and commit"
    regress_record "mac-preflight" "PASS" "$((SECONDS - preflight_start))" \
        "was on ${MAC_PREV_BRANCH}"
else
    regress_record "mac-preflight" "FAIL" "$((SECONDS - preflight_start))" \
        "exit ${preflight_rc}"
    regress_summary "regress mac" || true
    exit 1
fi

regress_leg "push-head" leg_push
regress_leg "mac-checkout" leg_checkout
regress_leg "donor-pin" leg_donor
regress_leg_soft "daf-pins" leg_daf

regress_leg "configure-app" leg_configure_app
regress_leg "configure-tests" leg_configure_tests
regress_leg "build-app" leg_build_app
regress_leg "build-tests" leg_build_tests
regress_leg "ctest" leg_ctest
regress_leg "selftest" leg_selftest

# Launching the GUI over ssh aborts in the main window constructor on this node
# - it reproduces on main, so it is the environment, not the build. Run those
# two by hand from a console session on the Air:
#   cd ~/src/dusk-studio && ./build/DuskStudio_artefacts/Release/DuskStudio.app/Contents/MacOS/DuskStudio
regress_skip "gui-launch" "ssh session cannot construct the main window; run from a console session"
regress_skip "ipc-selftest" "Linux-only code path"

regress_summary "regress mac"
