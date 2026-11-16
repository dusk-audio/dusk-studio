#!/usr/bin/env bash
# macOS leg of the regression runner: drives the M3 Air build node over ssh.
# With --dmg or --release-run it hands over to mac-package.sh, which tests a
# release disk image instead of a source build; --window-activation-only goes
# there too, for those cases alone against the app already installed.
#
# The node is key-auth only and never uses sudo. Its toolchain lives in ~/bin
# and ~/tools. The configure helper supplies its build flags. There is no
# GNU timeout there, so remote deadlines
# are a poll loop over a completion marker.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

for arg in "$@"; do
    if [[ "$arg" == --dmg || "$arg" == --release-run || "$arg" == --window-activation-only ]]; then
        exec bash "${REPO_ROOT}/scripts/regress/mac-package.sh" "$@"
    fi
done

# shellcheck source=scripts/regress/common.sh
source "${REPO_ROOT}/scripts/regress/common.sh"
REGRESS_PLATFORM=mac

MAC_HOST="${DUSK_REGRESS_MAC_HOST:-marc@macbook-air.local}"
MAC_REPO="${DUSK_REGRESS_MAC_REPO:-src/dusk-studio}"
MAC_DAF="${DUSK_REGRESS_MAC_DAF:-src/DAF}"
MAC_CONFIGURE="${DUSK_REGRESS_MAC_CONFIGURE:-mac-configure.sh}"
MAC_BUILD_DIR="${DUSK_REGRESS_MAC_BUILD_DIR:-build}"
MAC_TEST_BUILD_DIR="${DUSK_REGRESS_MAC_TEST_BUILD_DIR:-build-tests}"
MAC_DEDICATED=0
if [[ -n "${DUSK_REGRESS_MAC_REPO:-}" ]]; then
    [[ -n "${DUSK_REGRESS_MAC_DAF:-}" && -n "${DUSK_REGRESS_MAC_CONFIGURE:-}" ]] \
        || regress_die "a dedicated Mac repository requires explicit DAF and configure-helper paths"
    MAC_DEDICATED=1
fi
JOBS="${DUSK_JOBS:-5}"
FREE_COLD_GB="${DUSK_REGRESS_MAC_FREE_COLD_GB:-$(regress_floor_cold "$REGRESS_MAC_TREE_GB")}"
FREE_WARM_GB="${DUSK_REGRESS_MAC_FREE_WARM_GB:-$(regress_floor_warm "$REGRESS_MAC_RUN_GB")}"
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

[[ "$FREE_COLD_GB" =~ ^[0-9]+$ && "$FREE_WARM_GB" =~ ^[0-9]+$ ]] \
    || regress_die "DUSK_REGRESS_MAC_FREE_COLD_GB and DUSK_REGRESS_MAC_FREE_WARM_GB are whole numbers of GB"
regress_require ssh git timeout flock
regress_host_lock mac shared || regress_die "could not take the mac host lock"
cd "$REPO_ROOT"

HEAD_SHA="$(git rev-parse HEAD)"
HEAD_SHORT="$(git rev-parse --short HEAD)"
REMOTE_BRANCH="regress-${HEAD_SHORT}"
MAC_PREV_BRANCH=""
MAC_PREV_COMMIT=""
MAC_PREV_SUBMODULES=""
MAC_REMOTE_REPO=""
MAC_RUN_TOKEN="regress-${HEAD_SHORT:0:8}-$$"
MAC_RUN_STATE="$(mktemp "${TMPDIR:-/tmp}/dusk-mac-run.XXXXXX")"
MAC_RUN_BLOCKED="${MAC_RUN_STATE}.blocked"
# Touched when a call finds the run's claim gone: the checkout is another
# run's then, so this one neither restores it nor keeps its folder for that.
MAC_CLAIM_LOST="${MAC_RUN_STATE}.lost"
# Set once mac-preflight has claimed the node's checkout for this run; every
# later call first checks that the claim is still this run's.
MAC_CLAIMED=0

# A disconnected non-PTY ssh session can leave its build running. Keep an
# identifiable remote process group so cancellation can acknowledge its exit.
# Everything a run writes on the node outside the build trees goes in its own
# folder, ~/.cache/dusk-regress/<token>, which the run removes when it ends: a
# pid file while a call is in flight, and t/, the TMPDIR of every call. The
# single-instance tests bind TMPDIR/dusk-si-XXXXXX/dusk-studio/instance.sock,
# 41 characters past it, and macOS takes 103, so a longer folder leaves the
# node's own TMPDIR in place.
MAC_SUPERVISOR=$(cat <<'REMOTE'
set -euo pipefail
token=$1
for key in REGRESS_SOURCE REGRESS_DAF REGRESS_CONFIGURE; do
    value=${!key}
    [[ "$value" == /* ]] || printf -v "$key" '%s' "$HOME/$value"
    export "$key"
done
for key in REGRESS_BUILD REGRESS_TEST_BUILD; do
    value=${!key}
    [[ "$value" == /* ]] || printf -v "$key" '%s' "$REGRESS_SOURCE/$value"
    export "$key"
done
umask 077
REGRESS_CLAIM="$HOME/.cache/dusk-regress/source-claim-$(printf '%s' "$REGRESS_SOURCE" | cksum | awk '{ print $1 }')"
export REGRESS_CLAIM
if [[ "$REGRESS_CLAIMED" == 1 ]]; then
    holder=""
    [[ -f "$REGRESS_CLAIM" ]] && read -r holder <"$REGRESS_CLAIM"
    [[ "$holder" == "$token" ]] || {
        echo "error: this run no longer holds $REGRESS_SOURCE (its claim names '${holder:-nobody}'); stopping before it touches the checkout" >&2
        exit 76
    }
fi
slot="$HOME/.cache/dusk-regress/$token"
mkdir -p "$slot/t"
[[ ! -e "$slot/pid" ]] || exit 75
export REGRESS_RUN="$slot" REGRESS_TOKEN="$token"
printf '%s\n' "$REGRESS_SOURCE" >"$slot/source"
if ((${#slot} + 2 <= 62)); then export TMPDIR="$slot/t"; fi
cleanup() {
    rc=$?
    if ((rc == 0)); then
        rm -f "$slot/pid"
    fi
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP
pgid=$(ps -o pgid= -p "$$")
pgid=${pgid//[[:space:]]/}
[[ "$pgid" == "$$" ]] || exit 70
printf '%s\n' "$$" >"$slot/pid"
bash -s
REMOTE
)

mac_cancel() {
    [[ -s "$MAC_RUN_STATE" ]] || return 0
    local command
    printf -v command 'exec bash -c %q dusk-cancel %q' '
set -euo pipefail
token=$1
slot="$HOME/.cache/dusk-regress/$token"
for ((attempt = 0; attempt < 30; ++attempt)); do
    [[ -s "$slot/pid" ]] && break
    sleep 0.1
done
read -r pid <"$slot/pid" || exit 75
[[ "$pid" =~ ^[0-9]+$ ]] || exit 75
group_alive() {
    ps -ax -o pgid=,stat= | awk -v group="$pid" '\''
        $1 == group && $2 !~ /^Z/ { found = 1 }
        END { exit !found }
    '\''
}
if group_alive; then
    pgid=$(ps -o pgid= -p "$pid")
    pgid=${pgid//[[:space:]]/}
    command=$(ps -ww -o command= -p "$pid")
    [[ "$pgid" == "$pid" && "$command" == *" dusk-regress $token" ]] || exit 75
    kill -TERM -- "-$pid" 2>/dev/null || true
fi
for ((attempt = 0; attempt < 100; ++attempt)); do
    if ! group_alive; then
        rm -f "$slot/pid"
        exit 0
    fi
    sleep 0.1
done
exit 75
' "$MAC_RUN_TOKEN"
    if timeout --foreground 30 ssh "${SSH_OPTS[@]}" "$MAC_HOST" "$command" </dev/null; then
        : >"$MAC_RUN_STATE"
    else
        touch "$MAC_RUN_BLOCKED"
        echo "error: remote runner cancellation was not acknowledged; leaving the Mac checkout in place" >&2
        return 1
    fi
}

mac_interrupt() {
    trap '' INT TERM
    mac_cancel || true
    exit 130
}
trap mac_interrupt INT TERM

DONOR_REV="$(tr -d '[:space:]' < DONOR_REV)"
[[ "$DONOR_REV" =~ ^[0-9a-f]{40}$ ]] || regress_die "DONOR_REV does not hold a valid commit"
MAC_DAF_REV="$(tr -d '[:space:]' < DAF_REV)"
[[ "$MAC_DAF_REV" =~ ^[0-9a-f]{40}$ ]] || regress_die "DAF_REV does not hold a valid commit"

# mac_run <deadline seconds> <<'REMOTE' ... REMOTE
mac_run() {
    [[ ! -f "$MAC_RUN_BLOCKED" ]] || return 75
    local deadline="$1" command status=0
    printf -v command 'exec env REGRESS_SOURCE=%q REGRESS_DAF=%q REGRESS_CONFIGURE=%q REGRESS_BUILD=%q REGRESS_TEST_BUILD=%q REGRESS_DAF_REV=%q REGRESS_DEDICATED=%q REGRESS_JOBS=%q REGRESS_COMMIT=%q REGRESS_DONOR=%q REGRESS_BRANCH=%q REGRESS_HOST=%q REGRESS_FREE_COLD_GB=%q REGRESS_FREE_WARM_GB=%q REGRESS_CLAIMED=%q bash -c %q dusk-regress %q' \
        "$MAC_REPO" "$MAC_DAF" "$MAC_CONFIGURE" "$MAC_BUILD_DIR" "$MAC_TEST_BUILD_DIR" \
        "$MAC_DAF_REV" "$MAC_DEDICATED" "$JOBS" "$HEAD_SHA" "$DONOR_REV" "$REMOTE_BRANCH" \
        "$MAC_HOST" "$FREE_COLD_GB" "$FREE_WARM_GB" "$MAC_CLAIMED" "$MAC_SUPERVISOR" "$MAC_RUN_TOKEN"
    printf '%s\n' "$MAC_RUN_TOKEN" >"$MAC_RUN_STATE"
    timeout --foreground "$deadline" ssh "${SSH_OPTS[@]}" "$MAC_HOST" "$command" || status=$?
    # 76: the supervisor found the claim gone before it started anything.
    if ((status == 76)); then
        : >"$MAC_RUN_STATE"
        touch "$MAC_CLAIM_LOST"
    elif ((status != 0)); then
        mac_cancel || return 75
    else
        : >"$MAC_RUN_STATE"
    fi
    return "$status"
}

# Fails when the node's checkout is not back where the run found it, so the
# mac-restore row says so; the exit path tries again until it is.
MAC_RESTORED=0
restore_branch() {
    [[ -n "$MAC_PREV_BRANCH" ]] || return 0
    ((!MAC_RESTORED)) || return 0
    if [[ -f "$MAC_CLAIM_LOST" ]]; then
        echo "error: the checkout is not put back: this run lost its claim on it, so another run has it now" >&2
        return 1
    fi
    if [[ -f "$MAC_RUN_BLOCKED" ]]; then
        echo "error: the node's checkout stays on ${REMOTE_BRANCH}: a call there was never acknowledged" >&2
        return 1
    fi
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
    if ! mac_run 600 <<REMOTE; then
set -euo pipefail
cd "\$REGRESS_SOURCE"
git checkout -q ${checkout_args}
git branch -q -D "${REMOTE_BRANCH}" 2>/dev/null || true
$(declare -f regress_drop_added_submodules)
regress_drop_added_submodules "\$REGRESS_COMMIT"
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
        echo "error: could not restore ${MAC_HOST} to ${label}" >&2
        return 1
    fi
    MAC_RESTORED=1
}
# Removes the run's folder on the node. Not while a call there is
# unacknowledged: whatever it started may still be writing to it.
MAC_TORN_DOWN=0
# mac_teardown [release]: with release, the run's claim on the checkout goes
# too, which only the exit does: a restore retried there still needs it.
mac_teardown() {
    local release="${1:-0}" command
    ((!MAC_TORN_DOWN || release)) || return 0
    if [[ -f "$MAC_RUN_BLOCKED" ]]; then
        echo "error: ~/.cache/dusk-regress/${MAC_RUN_TOKEN} stays on the node, and with it the run's claim on the checkout: a call there was never acknowledged" >&2
        return 1
    fi
    printf -v command 'exec bash -c %q dusk-teardown %q %q' '
run="$HOME/.cache/dusk-regress/$1"
if [[ -d "$run" ]]; then
    [[ ! -e "$run/pid" ]] || { echo "error: $run has a pid file, so a call there may still run" >&2; exit 75; }
    rm -rf "$run"
    [[ ! -e "$run" ]] || exit 1
fi
[[ "$2" == 1 ]] || exit 0
for claim in "$HOME"/.cache/dusk-regress/source-claim-*; do
    [[ -f "$claim" ]] || continue
    holder=""
    read -r holder <"$claim" || true
    [[ "$holder" != "$1" ]] || rm -f "$claim"
done
' "$MAC_RUN_TOKEN" "$release"
    timeout --foreground 120 ssh "${SSH_OPTS[@]}" "$MAC_HOST" "$command" </dev/null || return 1
    MAC_TORN_DOWN=1
}
# While the checkout is not back, the run keeps its folder and claim, so no
# other run takes the checkout from under the retry at exit. A run that lost
# its claim has nothing to keep them for. A restore retried here runs through
# mac_run, which makes the run's folder again, so the teardown runs again after.
restore_pending() {
    [[ -n "$MAC_PREV_BRANCH" ]] && ((!MAC_RESTORED)) && [[ ! -f "$MAC_CLAIM_LOST" ]]
}
mac_cleanup() {
    if restore_pending; then
        restore_branch || true
        MAC_TORN_DOWN=0
    fi
    if restore_pending; then
        echo "error: ~/.cache/dusk-regress/${MAC_RUN_TOKEN} and the run's claim stay on the node: its checkout is not back where the run found it. Put it back by hand, then scripts/regress.sh clean mac removes both; until then the claim keeps other runs off the checkout for ten minutes after the folder was last written" >&2
    else
        mac_teardown 1 || true
    fi
    rm -f "$MAC_RUN_STATE" "$MAC_RUN_BLOCKED" "$MAC_CLAIM_LOST"
}
trap mac_cleanup EXIT

# Two source runs in one checkout move it under each other, so a run claims the
# checkout here, before it pushes or checks anything out, and refuses one that
# another live run holds. The claim is a file naming the run's folder, written
# under a mkdir mutex, so of two runs started in the same second the second
# finds the first's claim. A holder is live while a call of it is in flight
# (its folder's pid names a live process group running that run) or its folder
# was written in the last 600 s, which covers the gap between two calls; ages
# come from stat, since BSD find -mmin rounds up. A run from an older runner
# writes no claim (nor a source file in its folder) and counts only while a call
# of it is in flight. A mutex older than a minute was left by a run that died
# inside the claim, which takes well under a second: it is renamed away, never
# removed in place, so two runs that both find it stale cannot both end up
# holding the new one.
leg_preflight() {
    mac_run 180 <<'REMOTE'
set -euo pipefail
export PATH="$HOME/bin:$PATH"
echo "macOS $(sw_vers -productVersion), $(uname -m)"
base="$HOME/.cache/dusk-regress"
mutex="$base/source-claim.lock"
group_alive() {
    ps -ax -o pgid=,stat= | awk -v group="$1" '$1 == group && $2 !~ /^Z/ { found = 1 } END { exit !found }'
}
age() {
    local m
    m="$(stat -c %Y "$1" 2>/dev/null || stat -f %m "$1" 2>/dev/null)" || return 1
    [[ "$m" =~ ^[0-9]+$ ]] || return 1
    echo $(($(date +%s) - m))
}
running_in() {
    local pid=""
    [[ -s "$1/pid" ]] && read -r pid <"$1/pid"
    [[ "$pid" =~ ^[0-9]+$ ]] && group_alive "$pid" \
        && [[ "$(ps -ww -o command= -p "$pid" 2>/dev/null)" == *" dusk-regress ${1##*/}" ]] && echo "$pid"
}
tries=0
until mkdir "$mutex" 2>/dev/null; do
    mutex_age="$(age "$mutex" || echo 0)"
    if ((mutex_age > 60)); then
        broken="$mutex.broken.$REGRESS_TOKEN"
        if mv "$mutex" "$broken" 2>/dev/null; then
            broken_age="$(age "$broken" || echo 0)"
            if ((broken_age <= 60)) && [[ ! -e "$mutex" ]]; then
                mv "$broken" "$mutex" 2>/dev/null || true
            fi
            rm -rf "$broken"
        fi
        continue
    fi
    tries=$((tries + 1))
    ((tries < 300)) || { echo "error: could not take $mutex within 30 s" >&2; exit 1; }
    sleep 0.1
done
trap 'rmdir "$mutex" 2>/dev/null || true' EXIT
for other in "$base"/regress-*; do
    [[ -d "$other" && "$other" != "$REGRESS_RUN" && ! -f "$other/source" ]] || continue
    if pid="$(running_in "$other")"; then
        echo "error: a run of an older runner is live on the node: ${other##*/} (process group $pid); this run leaves the checkout alone" >&2
        exit 1
    fi
done
holder=""
[[ -f "$REGRESS_CLAIM" ]] && read -r holder <"$REGRESS_CLAIM"
if [[ -n "$holder" && "$holder" != "$REGRESS_TOKEN" ]]; then
    holder_age="$(age "$base/$holder" || echo 999999)"
    if pid="$(running_in "$base/$holder")" || ((holder_age <= 600)); then
        echo "error: another source run holds $REGRESS_SOURCE: $holder (${pid:+process group $pid, }its folder written ${holder_age} s ago); this run leaves the checkout alone" >&2
        exit 1
    fi
    echo "taking over the claim of $holder, whose run is gone"
fi
printf '%s\n' "$REGRESS_TOKEN" >"$REGRESS_CLAIM"
rmdir "$mutex"
trap - EXIT
echo "claimed $REGRESS_SOURCE for this run"
cd "$REGRESS_SOURCE"
if [[ "$REGRESS_DEDICATED" == 1 ]]; then
    [[ "$(git config --bool dusk.validationWorktree || true)" == true ]] || {
        echo "error: dedicated Mac source is not marked as a validation worktree" >&2
        exit 1
    }
    [[ -x "$REGRESS_CONFIGURE" ]] || {
        echo "error: dedicated configure helper is not executable: $REGRESS_CONFIGURE" >&2
        exit 1
    }
    [[ -z "$(git -C "$REGRESS_DAF" status --porcelain --untracked-files=all)" &&
       "$(git -C "$REGRESS_DAF" rev-parse HEAD)" == "$REGRESS_DAF_REV" ]] || {
        echo "error: dedicated DAF is dirty or does not match $REGRESS_DAF_REV" >&2
        exit 1
    }
fi
# Submodule pointers are handled separately (they get synced to the pushed
# commit and put back afterwards), so they must not count as local dirt. Edits
# inside a submodule's own worktree would be lost by that sync, so they do.
if [[ -n "$(git status --porcelain --ignore-submodules=all)" ]]; then
    echo "error: $REGRESS_HOST:$REGRESS_SOURCE has local modifications; commit or clean it first" >&2
    git status --short --ignore-submodules=all >&2
    exit 1
fi
if [[ -n "$(git submodule foreach --recursive --quiet 'git status --porcelain' 2>/dev/null)" ]]; then
    echo "error: $REGRESS_HOST:$REGRESS_SOURCE has modified submodule worktrees; commit or clean them first" >&2
    git submodule foreach --recursive --quiet 'git status --short | sed "s|^|  $path: |"' >&2
    exit 1
fi
free_kb="$(df -k "$HOME" | awk 'NR == 2 { print $4 }')"
kind=warm
want="$REGRESS_FREE_WARM_GB"
if [[ ! -f "$REGRESS_BUILD/CMakeCache.txt" || ! -f "$REGRESS_TEST_BUILD/CMakeCache.txt" ]]; then
    kind=cold
    want="$REGRESS_FREE_COLD_GB"
fi
free_gb="$(awk -v k="$free_kb" 'BEGIN { printf "%.1f", k / 1048576 }')"
if ((free_kb < want * 1048576)); then
    echo "error: $HOME has $free_gb GB free; a $kind run needs $want GB" >&2
    exit 1
fi
echo "$HOME has $free_gb GB free; a $kind run needs $want GB"
echo "tests write to ${TMPDIR:-the default TMPDIR of the node}"
echo "regress:source $(pwd -P)"
echo "regress:branch $(git rev-parse --abbrev-ref HEAD)"
echo "regress:commit $(git rev-parse HEAD)"
git submodule status | awk '{ gsub(/^[-+U]/, "", $1); print "regress:submodule", $2, $1 }'
REMOTE
}

leg_push() {
    git push --force "ssh://${MAC_HOST}${MAC_REMOTE_REPO}" \
        "HEAD:refs/heads/${REMOTE_BRANCH}"
}

leg_checkout() {
    mac_run 900 <<'REMOTE'
set -euo pipefail
cd "$REGRESS_SOURCE"
git checkout -q "$REGRESS_BRANCH"
[[ "$(git rev-parse HEAD)" == "$REGRESS_COMMIT" ]] || {
    echo "error: mac checkout $(git rev-parse HEAD) != pushed $REGRESS_COMMIT" >&2; exit 1; }
git submodule update --init --recursive
git log --oneline -1
git submodule status
REMOTE
}

# A dedicated DAF checkout is only verified; the legacy node's is moved to
# DAF_REV and left there.
leg_daf() {
    mac_run 900 <<'REMOTE'
set -euo pipefail
cd "$REGRESS_DAF"
before="$(git rev-parse HEAD)"
if [[ "$REGRESS_DEDICATED" != 1 && "$before" != "$REGRESS_DAF_REV" ]]; then
    git cat-file -e "$REGRESS_DAF_REV^{commit}" 2>/dev/null \
        || git fetch -q origin "$REGRESS_DAF_REV"
    git -c advice.detachedHead=false checkout -q --detach "$REGRESS_DAF_REV"
fi
[[ "$(git rev-parse HEAD)" == "$REGRESS_DAF_REV" ]] || {
    echo "error: DAF is at $(git rev-parse HEAD), DAF_REV is $REGRESS_DAF_REV" >&2; exit 1; }
echo "DAF $REGRESS_DAF_REV (was $before)"
REMOTE
}

# Configure fetches the donor at DONOR_REV into each tree's _deps. A cached
# DUSK_PLUGINS_PATH would build whatever that checkout holds instead, and a
# failure would then read as a Dusk Studio regression.
leg_donor_check() {
    mac_run 120 <<'REMOTE'
set -euo pipefail
rc=0
for dir in "$REGRESS_BUILD" "$REGRESS_TEST_BUILD"; do
    cached="$(sed -n 's/^DUSK_PLUGINS_PATH:[^=]*=//p' "$dir/CMakeCache.txt" | head -1)"
    if [[ -n "$cached" ]]; then
        echo "error: $dir builds the donor from DUSK_PLUGINS_PATH='$cached', not DONOR_REV" >&2
        rc=1
        continue
    fi
    head="$(git -C "$dir/_deps/dusk-plugins" rev-parse HEAD 2>/dev/null || true)"
    if [[ "$head" != "$REGRESS_DONOR" ]]; then
        echo "error: $dir/_deps/dusk-plugins is at '${head:-nothing}', DONOR_REV is $REGRESS_DONOR" >&2
        rc=1
        continue
    fi
    echo "$dir: donor at DONOR_REV ${REGRESS_DONOR:0:8}"
done
exit "$rc"
REMOTE
}

leg_configure_app() {
    mac_run 900 <<'REMOTE'
set -euo pipefail
"$REGRESS_CONFIGURE" "$REGRESS_BUILD" "-DDAF_PATH=$REGRESS_DAF" -UDUSK_PLUGINS_PATH
REMOTE
}

leg_configure_tests() {
    mac_run 900 <<'REMOTE'
set -euo pipefail
"$REGRESS_CONFIGURE" "$REGRESS_TEST_BUILD" "-DDAF_PATH=$REGRESS_DAF" -UDUSK_PLUGINS_PATH -DDUSKSTUDIO_BUILD_TESTS=ON
REMOTE
}

leg_build_app() {
    mac_run 5400 <<'REMOTE'
set -euo pipefail
export PATH="$HOME/bin:$PATH"
cd "$REGRESS_SOURCE"
cmake --build "$REGRESS_BUILD" -j"$REGRESS_JOBS"
REMOTE
}

leg_build_tests() {
    mac_run 5400 <<'REMOTE'
set -euo pipefail
export PATH="$HOME/bin:$PATH"
cd "$REGRESS_SOURCE"
cmake --build "$REGRESS_TEST_BUILD" --target dusk-studio-tests -j"$REGRESS_JOBS"
REMOTE
}

leg_ctest() {
    mac_run 1800 <<'REMOTE'
set -euo pipefail
export PATH="$HOME/bin:$PATH"
cd "$REGRESS_SOURCE"
ctest --test-dir "$REGRESS_TEST_BUILD" --output-on-failure
REMOTE
}

# No GNU timeout on the node, so the self-test runs behind a marker-file poll.
leg_selftest() {
    mac_run 600 <<'REMOTE'
set -euo pipefail
BIN="$REGRESS_BUILD/DuskStudio_artefacts/Release/DuskStudio.app/Contents/MacOS/DuskStudio"
[[ -x "$BIN" ]] || { echo "error: self-test binary missing: $BIN" >&2; exit 1; }
RC_FILE="$(mktemp -t duskstudio-selftest-rc)"
LOG_FILE="$(mktemp -t duskstudio-selftest-log)"
PID_FILE="$(mktemp -t duskstudio-selftest-pid)"
CHILD=""
# However the remote shell ends - done, timed out, or dropped with the ssh
# session - the app goes and so do its files.
cleanup() {
    APP_PID="$(cat "$PID_FILE" 2>/dev/null || true)"
    if [[ -n "$APP_PID" && ! -f "$RC_FILE" ]]; then kill -9 "$APP_PID" 2>/dev/null || true; fi
    if [[ -n "$CHILD" ]]; then wait "$CHILD" 2>/dev/null || true; fi
    rm -f "$RC_FILE" "$LOG_FILE" "$PID_FILE"
}
trap cleanup EXIT
trap 'exit 129' HUP INT TERM
rm -f "$RC_FILE"
# The subshell records the app's own pid: killing the subshell alone would
# orphan a hung DuskStudio.
(
    set +e
    DUSKSTUDIO_RUN_SELFTEST=1 "$BIN" >"$LOG_FILE" 2>&1 &
    APP=$!
    echo "$APP" >"$PID_FILE"
    wait "$APP"
    APP_RC=$?
    echo "$APP_RC" >"$RC_FILE"
) &
CHILD=$!
WAITED=0
while [[ ! -f "$RC_FILE" ]]; do
    if (( WAITED >= 420 )); then
        APP_PID="$(cat "$PID_FILE" 2>/dev/null || true)"
        [[ -n "$APP_PID" ]] && kill -9 "$APP_PID" 2>/dev/null || true
        wait "$CHILD" 2>/dev/null || true
        echo "error: self-test still running after 420 s" >&2
        sed 's/^/  /' "$LOG_FILE" >&2
        exit 124
    fi
    sleep 2
    WAITED=$(( WAITED + 2 ))
done
wait "$CHILD" 2>/dev/null || true
RC="$(cat "$RC_FILE")"
grep -E '^\[(PASS|FAIL|SKIP)\]|^Total' "$LOG_FILE" || sed 's/^/  /' "$LOG_FILE"
exit "$RC"
REMOTE
}

# The scenario suite uses the same marker-file deadline as the self-test. The
# fixtures come out of the node's build-tests tree, which the tests build has
# already produced - every fixture target is a dependency of dusk-studio-tests.
leg_scenarios() {
    mac_run 900 <<'REMOTE'
set -euo pipefail
REPO="$REGRESS_SOURCE"
BIN="$REGRESS_BUILD/DuskStudio_artefacts/Release/DuskStudio.app/Contents/MacOS/DuskStudio"
[[ -x "$BIN" ]] || { echo "error: scenario binary missing: $BIN" >&2; exit 1; }
SANDBOX_DIR="$(mktemp -d -t duskstudio-scenarios-env)"
RC_FILE="$(mktemp -t duskstudio-scenarios-rc)"
LOG_FILE="$(mktemp -t duskstudio-scenarios-log)"
PID_FILE="$(mktemp -t duskstudio-scenarios-pid)"
CHILD=""
cleanup() {
    APP_PID="$(cat "$PID_FILE" 2>/dev/null || true)"
    if [[ -n "$APP_PID" && ! -f "$RC_FILE" ]]; then kill -9 "$APP_PID" 2>/dev/null || true; fi
    if [[ -n "$CHILD" ]]; then wait "$CHILD" 2>/dev/null || true; fi
    rm -f "$RC_FILE" "$LOG_FILE" "$PID_FILE"
    rm -rf "$SANDBOX_DIR"
}
trap cleanup EXIT
trap 'exit 129' HUP INT TERM
mkdir -m 700 "$SANDBOX_DIR"/{home,config,data,cache,state,runtime}
rm -f "$RC_FILE"
(
    set +e
    env HOME="$SANDBOX_DIR/home" \
        XDG_CONFIG_HOME="$SANDBOX_DIR/config" \
        XDG_DATA_HOME="$SANDBOX_DIR/data" \
        XDG_CACHE_HOME="$SANDBOX_DIR/cache" \
        XDG_STATE_HOME="$SANDBOX_DIR/state" \
        XDG_RUNTIME_DIR="$SANDBOX_DIR/runtime" \
        DUSKSTUDIO_RUN_SCENARIOS=all \
        DUSKSTUDIO_FIXTURE_DIR="$REGRESS_TEST_BUILD:$REPO/tests/fixtures" \
        "$BIN" >"$LOG_FILE" 2>&1 &
    APP=$!
    echo "$APP" >"$PID_FILE"
    wait "$APP"
    APP_RC=$?
    echo "$APP_RC" >"$RC_FILE"
) &
CHILD=$!
WAITED=0
while [[ ! -f "$RC_FILE" ]]; do
    if (( WAITED >= 600 )); then
        APP_PID="$(cat "$PID_FILE" 2>/dev/null || true)"
        [[ -n "$APP_PID" ]] && kill -9 "$APP_PID" 2>/dev/null || true
        wait "$CHILD" 2>/dev/null || true
        echo "error: scenario suite still running after 600 s" >&2
        sed 's/^/  /' "$LOG_FILE" >&2
        exit 124
    fi
    sleep 2
    WAITED=$(( WAITED + 2 ))
done
wait "$CHILD" 2>/dev/null || true
RC="$(cat "$RC_FILE")"
grep -E '^\[(PASS|FAIL|SKIP)\]|^=== scenarios: ' "$LOG_FILE" || sed 's/^/  /' "$LOG_FILE"
exit "$RC"
REMOTE
}

# Skips carry their reasons into the leg note - several cases need LV2, which
# this node does not build.
mac_scenarios_leg() {
    printf '\n--- scenarios ---\n'
    local start=$SECONDS log rc=0
    log="$(mktemp "${TMPDIR:-/tmp}/duskstudio-mac-scenarios.XXXXXX")"
    leg_scenarios >"$log" 2>&1 || rc=$?
    cat "$log"
    regress_scenario_leg "scenarios" "$((SECONDS - start))" "$log" "$rc"
    rm -f "$log"
    return 0
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
    MAC_CLAIMED=1
    MAC_REMOTE_REPO="$(sed -n 's/^regress:source //p' <<<"$preflight_out" | head -1)"
    MAC_PREV_BRANCH="$(sed -n 's/^regress:branch //p' <<<"$preflight_out" | head -1)"
    MAC_PREV_COMMIT="$(sed -n 's/^regress:commit //p' <<<"$preflight_out" | head -1)"
    MAC_PREV_SUBMODULES="$(sed -n 's/^regress:submodule //p' <<<"$preflight_out")"
    [[ -n "$MAC_REMOTE_REPO" && -n "$MAC_PREV_BRANCH" && -n "$MAC_PREV_COMMIT" ]] \
        || regress_die "preflight did not report the source directory, branch and commit"
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
regress_leg "daf-revision" leg_daf

regress_leg "configure-app" leg_configure_app
regress_leg "configure-tests" leg_configure_tests
regress_leg "donor-check" leg_donor_check
regress_leg "build-app" leg_build_app
regress_leg "build-tests" leg_build_tests
regress_leg "ctest" leg_ctest
regress_leg "selftest" leg_selftest
mac_scenarios_leg

regress_skip "gui-launch" "foreground desktop session required for native GUI input"
regress_skip "scenarios-gui" "foreground desktop session required for native GUI input"
regress_skip "ipc-selftest" "Linux-only code path"

regress_leg "mac-restore" restore_branch

# The warm trees stay and the run's folder goes, pass or fail, and their sizes
# and the node's free space end the table.
leg_footprint() {
    mac_run 600 <<'REMOTE'
set -euo pipefail
for dir in "$REGRESS_BUILD" "$REGRESS_TEST_BUILD"; do
    [[ -d "$dir" ]] && echo "regress:tree $(du -sk "$dir" | cut -f1) ${dir##*/}"
done
echo "regress:run $(du -sk "$REGRESS_RUN" | cut -f1)"
echo "regress:free $(df -k "$HOME" | awk 'NR == 2 { print $4 }')"
REMOTE
}
mac_footprint_leg() {
    printf '\n--- footprint ---\n'
    local start=$SECONDS out rc=0 trees="" kb name run_kb free_kb
    out="$(leg_footprint 2>&1)" || rc=$?
    grep -v '^regress:' <<<"$out" || true
    while read -r kb name; do
        trees="${trees}${trees:+, }${name} $(regress_gb "$((kb * 1024))")"
    done < <(sed -n 's/^regress:tree //p' <<<"$out")
    run_kb="$(sed -n 's/^regress:run //p' <<<"$out")"
    free_kb="$(sed -n 's/^regress:free //p' <<<"$out")"
    if restore_pending; then
        regress_record footprint FAIL "$((SECONDS - start))" "the run's folder is kept: the checkout is not back yet, and the exit tries again"
    elif ((rc != 0)) || [[ ! "$free_kb" =~ ^[0-9]+$ ]]; then
        mac_teardown || true
        regress_record footprint FAIL "$((SECONDS - start))" "exit ${rc}"
    elif ! mac_teardown; then
        regress_record footprint FAIL "$((SECONDS - start))" "the run's folder ~/.cache/dusk-regress/${MAC_RUN_TOKEN} was not removed"
    else
        regress_record footprint PASS "$((SECONDS - start))" \
            "warm trees ${trees:-none}; run folder $(regress_gb "$((${run_kb:-0} * 1024))") removed; $(regress_gb "$((free_kb * 1024))") free"
    fi
}
mac_footprint_leg

regress_summary "regress mac"
