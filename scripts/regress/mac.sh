#!/usr/bin/env bash
# macOS leg of the regression runner: drives the M3 Air build node over ssh.
# With --dmg or --release-run it hands over to mac-package.sh, which tests a
# release disk image instead of a source build.
#
# The node is key-auth only and never uses sudo. Its toolchain lives in ~/bin
# and ~/tools. The configure helper supplies its build flags. There is no
# GNU timeout there, so remote deadlines
# are a poll loop over a completion marker.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

for arg in "$@"; do
    if [[ "$arg" == --dmg || "$arg" == --release-run ]]; then
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
MAC_DAF_REV="${DUSK_REGRESS_MAC_DAF_REV:-}"
MAC_DEDICATED=0
if [[ -n "${DUSK_REGRESS_MAC_REPO:-}" ]]; then
    [[ -n "${DUSK_REGRESS_MAC_DAF:-}" && -n "${DUSK_REGRESS_MAC_CONFIGURE:-}" ]] \
        || regress_die "a dedicated Mac repository requires explicit DAF and configure-helper paths"
    [[ "$MAC_DAF_REV" =~ ^[0-9a-f]{40}$ ]] \
        || regress_die "a dedicated Mac repository requires a 40-character DAF revision"
    MAC_DEDICATED=1
fi
JOBS="${DUSK_JOBS:-5}"
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
MAC_REMOTE_REPO=""
MAC_RUN_TOKEN="regress-${HEAD_SHORT}-$$"
MAC_RUN_STATE="$(mktemp "${TMPDIR:-/tmp}/dusk-mac-run.XXXXXX")"
MAC_RUN_BLOCKED="${MAC_RUN_STATE}.blocked"

# A disconnected non-PTY ssh session can leave its build running. Keep an
# identifiable remote process group so cancellation can acknowledge its exit.
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
base="$HOME/.cache/dusk-regress"
mkdir -p "$base"
slot="$base/$token"
mkdir "$slot"
cleanup() {
    rc=$?
    if ((rc == 0)); then
        rm -f "$slot/pid"
        rmdir "$slot"
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
        rmdir "$slot" 2>/dev/null || true
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

# mac_run <deadline seconds> <<'REMOTE' ... REMOTE
mac_run() {
    [[ ! -f "$MAC_RUN_BLOCKED" ]] || return 75
    local deadline="$1" command status=0
    printf -v command 'exec env REGRESS_SOURCE=%q REGRESS_DAF=%q REGRESS_CONFIGURE=%q REGRESS_BUILD=%q REGRESS_TEST_BUILD=%q REGRESS_DAF_REV=%q REGRESS_DEDICATED=%q REGRESS_JOBS=%q REGRESS_COMMIT=%q REGRESS_DONOR=%q REGRESS_BRANCH=%q REGRESS_HOST=%q bash -c %q dusk-regress %q' \
        "$MAC_REPO" "$MAC_DAF" "$MAC_CONFIGURE" "$MAC_BUILD_DIR" "$MAC_TEST_BUILD_DIR" \
        "$MAC_DAF_REV" "$MAC_DEDICATED" "$JOBS" "$HEAD_SHA" "$DONOR_REV" "$REMOTE_BRANCH" \
        "$MAC_HOST" "$MAC_SUPERVISOR" "$MAC_RUN_TOKEN"
    printf '%s\n' "$MAC_RUN_TOKEN" >"$MAC_RUN_STATE"
    timeout --foreground "$deadline" ssh "${SSH_OPTS[@]}" "$MAC_HOST" "$command" || status=$?
    if ((status != 0)); then
        mac_cancel || return 75
    else
        : >"$MAC_RUN_STATE"
    fi
    return "$status"
}

restore_branch() {
    [[ ! -f "$MAC_RUN_BLOCKED" ]] || return 0
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
cd "\$REGRESS_SOURCE"
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
mac_cleanup() {
    restore_branch
    rm -f "$MAC_RUN_STATE" "$MAC_RUN_BLOCKED"
}
trap mac_cleanup EXIT

leg_preflight() {
    mac_run 180 <<'REMOTE'
set -euo pipefail
export PATH="$HOME/bin:$PATH"
echo "macOS $(sw_vers -productVersion), $(uname -m)"
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

# Dedicated builds verify their pinned DAF checkout; the legacy node follows
# main best-effort when it can reach GitHub.
leg_daf() {
    mac_run 900 <<'REMOTE'
set -euo pipefail
cd "$REGRESS_DAF"
if [[ "$REGRESS_DEDICATED" == 1 ]]; then
    [[ "$(git rev-parse HEAD)" == "$REGRESS_DAF_REV" ]]
    echo "DAF $REGRESS_DAF_REV (verified dedicated checkout)"
    exit 0
fi
before="$(git rev-parse --short HEAD)"
if ! git pull -q --ff-only >/dev/null 2>&1; then
    echo "warn: DAF could not follow main (left at ${before})"
    exit 0
fi
echo "DAF $(git rev-parse --short HEAD) (was ${before})"
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
if ((MAC_DEDICATED)); then
    regress_leg "daf-revision" leg_daf
else
    regress_leg_soft "daf-main" leg_daf
fi

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

regress_summary "regress mac"
