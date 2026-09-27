#!/usr/bin/env bash
# macOS package leg: installs a release DMG on the M3 Air the way a user does,
# into /Applications, and runs the scenario suites against the installed app.
# Reached through scripts/regress.sh mac --dmg <path> | --release-run <id>.
#
# The engine suite runs over ssh. The GUI suite cannot (a GUI launched over
# plain ssh aborts in the main window constructor), so it goes through `open`,
# which starts the app in the logged-in desktop session. That session raises
# the macOS privacy prompts, and nothing over ssh can see or answer them, so a
# watcher reads tccd's log over ssh and answers the microphone prompt Allow
# through Screen Sharing, once a screenshot shows that button at the point.
# Folder prompts are never clicked: the suites run in a private HOME and should
# raise none, so one fails the leg. The fixtures come from a worktree of the
# node's checkout at the commit the package was built from, so the checkout
# itself is never touched. /bin/bash on the node is 3.2: the remote scripts
# avoid mapfile, associative arrays and GNU options.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck source=scripts/regress/common.sh
source "${REPO_ROOT}/scripts/regress/common.sh"
REGRESS_PLATFORM=mac

MAC_HOST="${DUSK_REGRESS_MAC_HOST:-marc@macbook-air.local}"
MAC_REPO="src/dusk-studio"
MAC_TREE="src/dusk-studio-regress"
JOBS="${DUSK_JOBS:-5}"
REPO_SLUG="dusk-audio/dusk-studio"
ENGINE_TIMEOUT=900
GUI_TIMEOUT=1800
MIC_TIMEOUT=90
SSH_OPTS=(-o BatchMode=yes -o ConnectTimeout=15 -o ServerAliveInterval=30)

# Screen coordinates of the microphone prompt's Allow button on the node's
# 1920x1080 desktop.
MIC_ALLOW="${DUSK_REGRESS_MAC_MIC_ALLOW:-1019,408}"

DMG=""
RELEASE_RUN=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --dmg)
            [[ $# -ge 2 ]] || regress_die "--dmg needs a path"
            DMG="$2"
            shift 2
            ;;
        --release-run)
            [[ $# -ge 2 ]] || regress_die "--release-run needs a workflow run id"
            RELEASE_RUN="$2"
            shift 2
            ;;
        --host)
            [[ $# -ge 2 ]] || regress_die "--host needs user@host"
            MAC_HOST="$2"
            shift 2
            ;;
        *) regress_die "unknown option '$1' for the mac package leg" ;;
    esac
done

[[ -n "$DMG" || -n "$RELEASE_RUN" ]] || regress_die "the mac package leg needs --dmg <path> or --release-run <run id>"
[[ -z "$DMG" || -z "$RELEASE_RUN" ]] || regress_die "--dmg and --release-run both name the package; pass one"
if [[ -n "$DMG" ]]; then
    [[ -f "$DMG" ]] || regress_die "no such disk image: $DMG"
    DMG="$(cd "$(dirname "$DMG")" && pwd)/$(basename "$DMG")"
fi
regress_require ssh scp git timeout
if [[ -n "$RELEASE_RUN" ]]; then regress_require gh; fi

MAC_VNC_HOST="${DUSK_REGRESS_MAC_VNC_HOST:-${MAC_HOST#*@}}"
MAC_VNC_USER="$USER"
[[ "$MAC_HOST" == *@* ]] && MAC_VNC_USER="${MAC_HOST%@*}"
VNC_PYTHON="${DUSK_REGRESS_VNC_PYTHON:-${XDG_DATA_HOME:-$HOME/.local/share}/dusk-regress/vnc/bin/python}"
[[ -x "$VNC_PYTHON" ]] || VNC_PYTHON=python3
VNC_PY="${REPO_ROOT}/scripts/regress/mac/vnc.py"

cd "$REPO_ROOT"

STAMP="$(date +%Y%m%d-%H%M%S)"
RUN_DIR="${TMPDIR:-/tmp}/dusk-regress-mac-${STAMP}"
REMOTE_RUN="dusk-regress-pkg/${STAMP}"
mkdir -p "$RUN_DIR"

MAC_HOME=""
SOURCE_SHA=""
PACKAGE_VERSION=""
WATCH_SINCE=""
WATCH_PID=""
REMOTE_BRANCH=""

# Every remote script starts with its inputs as bash-quoted assignments, then
# this library, so the scripts themselves can be quoted heredocs.
read -r -d '' REMOTE_LIB <<'LIB' || true
export PATH="$HOME/bin:$HOME/tools/cmake/CMake.app/Contents/bin:$PATH"
APP=/Applications/DuskStudio.app
BIN="$APP/Contents/MacOS/DuskStudio"
FIXTURE_DIR="$HOME/$TREE/build-tests:$HOME/$TREE/tests/fixtures"

# sandbox <dir>: SANDBOX_ENV becomes a private HOME, XDG base directories,
# runtime dir, config dir and music dir, all under <dir>.
sandbox() {
    local d="$1"
    mkdir -p -m 700 "$d" "$d/home" "$d/config" "$d/runtime" "$d/music" || return 1
    SANDBOX_ENV=(
        "HOME=$d/home"
        "XDG_CONFIG_HOME=$d/config"
        "XDG_DATA_HOME=$d/home/.local/share"
        "XDG_CACHE_HOME=$d/home/.cache"
        "XDG_STATE_HOME=$d/home/.local/state"
        "XDG_RUNTIME_DIR=$d/runtime"
        "DUSKSTUDIO_CONFIG_DIR=$d/home/.config/Dusk Studio"
        "DUSKSTUDIO_MUSIC_DIR=$d/music"
    )
}

# open_app <dir> <VAR=value...>: starts the installed app in the desktop
# session, sandboxed under <dir>, and prints its pid.
open_app() {
    local d="$1" before pid="" i kv
    shift
    sandbox "$d" || return 1
    local -a args=(-n)
    for kv in "${SANDBOX_ENV[@]}" "$@"; do args+=(--env "$kv"); done
    before="$(pgrep -f "$BIN" | sort)"
    open "${args[@]}" --stdout "$d/stdout.log" --stderr "$d/stderr.log" "$APP" || return 1
    for i in $(seq 1 60); do
        pid="$(pgrep -f "$BIN" | sort | comm -13 <(printf '%s\n' "$before") - | head -1)"
        [[ -n "$pid" ]] && break
        sleep 0.5
    done
    [[ -n "$pid" ]] || { echo "error: open started no $BIN" >&2; return 1; }
    echo "$pid" >>"$HOME/$RUN/pids"
    echo "$pid"
}
LIB

# mac_run <deadline> [VAR=value...] <<'REMOTE' ... REMOTE
mac_run() {
    local deadline="$1" kv
    shift
    {
        printf 'set -uo pipefail\n'
        printf 'RUN=%q\nTREE=%q\n' "$REMOTE_RUN" "$MAC_TREE"
        for kv in "$@"; do printf '%s=%q\n' "${kv%%=*}" "${kv#*=}"; done
        printf '%s\n' "$REMOTE_LIB"
        cat
    } | timeout "$deadline" ssh "${SSH_OPTS[@]}" "$MAC_HOST" 'bash -s'
}

vnc() {
    "$VNC_PYTHON" "$VNC_PY" --host "$MAC_VNC_HOST" --user "$MAC_VNC_USER" "$@"
}

# ---------------------------------------------------------------- teardown

remote_cleanup() {
    stop_watcher
    mac_run 120 <<'REMOTE' || echo "warning: could not clean up ${MAC_HOST}:~/${REMOTE_RUN}" >&2
[[ -d "$HOME/$RUN" ]] || exit 0
while read -r pid; do
    [[ -n "$pid" && "$(ps -p "$pid" -o command= 2>/dev/null)" == "$BIN"* ]] && kill -9 "$pid" 2>/dev/null
done <"$HOME/$RUN/pids"
[[ -f "$HOME/$RUN/caffeinate.pid" ]] && kill "$(cat "$HOME/$RUN/caffeinate.pid")" 2>/dev/null
[[ -d "$HOME/$RUN/mnt" ]] && hdiutil detach -force -quiet "$HOME/$RUN/mnt" 2>/dev/null
rm -rf "$HOME/$RUN"
rmdir "$HOME/dusk-regress-pkg" 2>/dev/null
exit 0
REMOTE
}

stop_watcher() {
    [[ -n "$WATCH_PID" ]] || return 0
    kill "$WATCH_PID" 2>/dev/null || true
    wait "$WATCH_PID" 2>/dev/null || true
    WATCH_PID=""
}

trap regress_run_exit_hooks EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

# ---------------------------------------------------------------- legs

# The node's screen locks after about 20 minutes idle, which stalls a GUI run,
# so caffeinate runs for the whole leg (bounded, in case the cleanup never runs).
leg_preflight() {
    local out rc=0
    out="$(mac_run 120 <<'REMOTE'
if curl -s -m 5 localhost:11434/api/generate -d '{"model":"qwen-review","keep_alive":0}' >/dev/null 2>&1; then
    echo "ollama: unload requested"
fi
echo "macOS $(sw_vers -productVersion), $(uname -m)"
running="$(pgrep -f "$BIN" || true)"
if [[ -n "$running" ]]; then
    echo "error: $BIN is already running (pid $(echo $running)); quit it before replacing the app" >&2
    exit 1
fi
mkdir -p "$HOME/$RUN" || exit 1
touch "$HOME/$RUN/pids" "$HOME/$RUN/started"
nohup caffeinate -d -i -u -s -t 14400 >/dev/null 2>&1 &
echo $! >"$HOME/$RUN/caffeinate.pid"
echo "regress:home $HOME"
# A prompt left up by an earlier, interrupted launch blocks this build's
# launches without being raised again, so the watcher looks back that far.
echo "regress:since $(date -v-15M '+%Y-%m-%d %H:%M:%S')"
REMOTE
)" || rc=$?
    printf '%s\n' "$out"
    ((rc == 0)) || return "$rc"
    MAC_HOME="$(sed -n 's/^regress:home //p' <<<"$out")"
    WATCH_SINCE="$(sed -n 's/^regress:since //p' <<<"$out")"
    [[ -n "$MAC_HOME" && -n "$WATCH_SINCE" ]] || {
        echo "error: the node did not report its home directory and clock" >&2
        return 1
    }
}

screen_locked() {
    mac_run 60 <<'REMOTE'
ioreg -n Root -d1 -a | grep -A1 CGSSessionScreenIsLocked | grep -q '<true/>'
REMOTE
}

leg_screen() {
    if ! screen_locked; then
        echo "the desktop session is unlocked"
        return 0
    fi
    echo "the desktop session is locked; unlocking it over Screen Sharing"
    vnc unlock --cap "${RUN_DIR}/unlock.png" || return 1
    if screen_locked; then
        echo "error: still locked after the unlock attempt; see ${RUN_DIR}/unlock.png" >&2
        return 1
    fi
}

leg_package_fetch() {
    gh run view "$RELEASE_RUN" -R "$REPO_SLUG" \
        --json workflowName,headBranch,headSha,status,conclusion \
        --template '{{.workflowName}} run on {{.headBranch}} at {{.headSha}}: {{.status}} {{.conclusion}}{{"\n"}}' \
        || return 1
    SOURCE_SHA="$(gh run view "$RELEASE_RUN" -R "$REPO_SLUG" --json headSha --jq .headSha)" || return 1
    mkdir -p "${RUN_DIR}/artifact"
    gh run download "$RELEASE_RUN" -R "$REPO_SLUG" -n release-macos -D "${RUN_DIR}/artifact" || return 1
    DMG="$(find "${RUN_DIR}/artifact" -name 'dusk-studio-*-macOS-*.dmg' | head -1)"
    [[ -n "$DMG" ]] || {
        echo "error: the release-macos artifact holds no disk image" >&2
        return 1
    }
    echo "disk image: ${DMG}"
}

leg_dmg_install() {
    local name
    name="$(basename "$DMG")"
    if [[ ! "$name" =~ ^dusk-studio-([0-9]+\.[0-9]+\.[0-9]+)-macOS-arm64\.dmg$ ]]; then
        echo "error: not a release disk image name (dusk-studio-X.Y.Z-macOS-arm64.dmg): ${name}" >&2
        return 1
    fi
    PACKAGE_VERSION="${BASH_REMATCH[1]}"
    scp -q "${SSH_OPTS[@]}" "$DMG" "${MAC_HOST}:${REMOTE_RUN}/${name}" || return 1
    mac_run 600 DMG_NAME="$name" VERSION="$PACKAGE_VERSION" <<'REMOTE'
set -e
cd "$HOME/$RUN"
mkdir -p mnt
# The image carries a licence agreement: hdiutil waits for an answer, and its
# own status is the one that counts, not the SIGPIPEd yes's.
set +o pipefail
yes | hdiutil attach -nobrowse -readonly -noverify -noautoopen -mountpoint "$HOME/$RUN/mnt" "$DMG_NAME" >/dev/null
set -o pipefail
trap 'hdiutil detach -quiet "$HOME/$RUN/mnt" 2>/dev/null || hdiutil detach -force -quiet "$HOME/$RUN/mnt" 2>/dev/null || true' EXIT
src="$HOME/$RUN/mnt/DuskStudio.app"
[[ -x "$src/Contents/MacOS/DuskStudio" ]] || { echo "error: the image has no DuskStudio.app at its root:" >&2; ls "$HOME/$RUN/mnt" >&2; exit 1; }
got="$(/usr/libexec/PlistBuddy -c 'Print CFBundleShortVersionString' "$src/Contents/Info.plist")"
[[ "$got" == "$VERSION" ]] || { echo "error: the image's app says version $got, its name says $VERSION" >&2; exit 1; }
if [[ -e "$APP" ]]; then
    echo "replacing $APP $(/usr/libexec/PlistBuddy -c 'Print CFBundleShortVersionString' "$APP/Contents/Info.plist" 2>/dev/null || echo '(unreadable)')"
    rm -rf "$APP"
fi
ditto "$src" "$APP"
xattr -dr com.apple.quarantine "$APP" 2>/dev/null || true
cmp -s "$src/Contents/MacOS/DuskStudio" "$BIN" || { echo "error: $BIN differs from the image's copy" >&2; exit 1; }
codesign --verify --deep --strict "$APP" || { echo "error: $APP does not pass codesign --verify" >&2; exit 1; }
codesign -dv "$APP" 2>&1 | grep -E '^(Identifier|Signature|TeamIdentifier)=' | sed 's/^/  /'
echo "installed $APP $got"
REMOTE
}

leg_fixture_source() {
    local rc=0
    git cat-file -e "${SOURCE_SHA}^{commit}" 2>/dev/null \
        || git fetch -q origin "$SOURCE_SHA" || return 1
    REMOTE_BRANCH="regress-pkg-${SOURCE_SHA:0:12}"
    git push -q --force "ssh://${MAC_HOST}${MAC_HOME}/${MAC_REPO}" \
        "${SOURCE_SHA}:refs/heads/${REMOTE_BRANCH}" || return 1
    # add -f takes over the regress tree's own registration when its directory
    # has gone; a prune would reach every other worktree of the checkout too.
    mac_run 1800 REPO="$MAC_REPO" SHA="$SOURCE_SHA" <<'REMOTE' || rc=$?
set -e
repo="$HOME/$REPO"
tree="$HOME/$TREE"
if [[ -e "$tree/.git" ]]; then
    git -C "$tree" checkout -q -f --detach "$SHA"
else
    git -C "$repo" worktree add -q -f --detach "$tree" "$SHA"
fi
cd "$tree"
[[ "$(git rev-parse HEAD)" == "$SHA" ]] || { echo "error: $tree is at $(git rev-parse HEAD), not $SHA" >&2; exit 1; }
git submodule sync -q --recursive
git submodule update -q --init --recursive
git log --oneline -1
REMOTE
    delete_remote_branch || rc=1
    return "$rc"
}

delete_remote_branch() {
    [[ -n "$REMOTE_BRANCH" ]] || return 0
    if ! git push -q "ssh://${MAC_HOST}${MAC_HOME}/${MAC_REPO}" --delete "$REMOTE_BRANCH"; then
        echo "error: could not delete ${REMOTE_BRANCH} from ${MAC_HOST}:~/${MAC_REPO}" >&2
        return 1
    fi
    REMOTE_BRANCH=""
}

# ~/mac-configure.sh carries the node's toolchain flags but configures the
# main checkout, so it runs with its cd pointed at the worktree instead.
leg_configure_fixtures() {
    mac_run 1800 <<'REMOTE'
set -e
script="$(sed 's|^cd ~/src/dusk-studio$|cd "$REGRESS_TREE"|' "$HOME/mac-configure.sh")"
grep -qxF 'cd "$REGRESS_TREE"' <<<"$script" || {
    echo "error: ~/mac-configure.sh no longer has the 'cd ~/src/dusk-studio' line this leg retargets" >&2
    exit 1
}
REGRESS_TREE="$HOME/$TREE" bash -c "$script" mac-configure build-tests -DDUSKSTUDIO_BUILD_TESTS=ON
REMOTE
}

leg_donor_check() {
    mac_run 120 <<'REMOTE'
cd "$HOME/$TREE" || exit 1
want="$(tr -d '[:space:]' <DONOR_REV)"
cached="$(sed -n 's/^DUSK_PLUGINS_PATH:[^=]*=//p' build-tests/CMakeCache.txt | head -1)"
if [[ -n "$cached" ]]; then
    echo "error: build-tests builds the donor from DUSK_PLUGINS_PATH='$cached', not DONOR_REV" >&2
    exit 1
fi
head="$(git -C build-tests/_deps/dusk-plugins rev-parse HEAD 2>/dev/null || true)"
if [[ "$head" != "$want" ]]; then
    echo "error: build-tests/_deps/dusk-plugins is at '${head:-nothing}', DONOR_REV is $want" >&2
    exit 1
fi
echo "build-tests: donor at DONOR_REV ${want:0:8}"
REMOTE
}

# Only the fixture targets: the test binary says nothing about the package.
leg_build_fixtures() {
    mac_run 5400 JOBS="$JOBS" <<'REMOTE'
cd "$HOME/$TREE" || exit 1
targets="$(cmake --build build-tests --target help 2>/dev/null | grep -oE 'dusk-studio-[A-Za-z0-9_-]+-fixture' | sort -u | tr '\n' ' ')"
[[ -n "$targets" ]] || { echo "error: build-tests declares no dusk-studio-*-fixture targets" >&2; exit 1; }
echo "fixture targets: $targets"
# shellcheck disable=SC2086
cmake --build build-tests --target $targets -j"$JOBS"
REMOTE
}

# The fixture table the app resolves against, read from the worktree's source
# and resolved against the same roots the suites get.
mac_fixture_leg() {
    printf '\n--- scenario-fixtures ---\n'
    local out logical path missing="" count=0
    local -A resolved=()
    out="$(mac_run 120 <<'REMOTE'
table="$HOME/$TREE/src/engine/scenario/ScenarioFixtures.cpp"
sed -E -n 's/^[[:space:]]*\{[[:space:]]*"([^"]+)",[[:space:]]*"([^"]+)"[[:space:]]*\},.*/\1 \2/p' "$table" |
while read -r logical relative; do
    found=-
    for root in "$HOME/$TREE/build-tests" "$HOME/$TREE/tests/fixtures"; do
        if [[ -e "$root/$relative" ]]; then found="$root/$relative"; break; fi
    done
    echo "fixture $logical $found"
done
REMOTE
)" || true
    while read -r _ logical path; do
        [[ -n "$logical" ]] || continue
        if [[ -z "${resolved[$logical]:-}" || "${resolved[$logical]}" == - ]]; then
            resolved[$logical]="$path"
        fi
    done < <(grep '^fixture ' <<<"$out")
    if ((${#resolved[@]} == 0)); then
        regress_record "scenario-fixtures" FAIL 0 "no fixture table read from the worktree"
        return 0
    fi
    for logical in $(printf '%s\n' "${!resolved[@]}" | sort); do
        count=$((count + 1))
        if [[ "${resolved[$logical]}" != - ]]; then
            regress_note "${logical}: ${resolved[$logical]}"
        else
            regress_note "${logical}: not found under the worktree's build-tests or tests/fixtures"
            missing="${missing}${missing:+, }${logical}"
        fi
    done
    if [[ -n "$missing" ]]; then
        regress_skip "scenario-fixtures" "unresolved, cases using them prove nothing: ${missing}"
    else
        regress_record "scenario-fixtures" PASS 0 "all ${count} resolve"
    fi
}

# ---------------------------------------------------------------- prompts

# prompt_watch: runs in the background from the first launch to the last. Each
# prompt the installed app raises is logged "seen", and "answered <authValue>"
# once tccd records the answer (2 allowed, 0 denied). A microphone prompt is
# clicked Allow only when vnc.py finds that button at the point in a fresh
# capture, and no longer once the process that asked (the msgID's pid) has
# exited. Folder prompts and any other kind are logged and never clicked.
prompt_watch() {
    local log="${RUN_DIR}/prompts.log" since="$WATCH_SINCE" out line msg service value now pids pid shot rc
    local -A service_of=() clicks=() clicked_at=() answered=() closed=() alive=()
    while :; do
        pids=""
        for msg in "${!service_of[@]}"; do
            [[ -n "${answered[$msg]:-}${closed[$msg]:-}" ]] || pids="${pids} ${msg%%.*}"
        done
        out="$(mac_run 60 SINCE="$since" PIDS="$pids" <<'REMOTE'
lines="$(log show --start "$SINCE" --style compact \
    --predicate 'process == "tccd" AND (eventMessage CONTAINS "AUTHREQ_PROMPTING" OR eventMessage CONTAINS "AUTHREQ_RESULT")' \
    2>/dev/null | grep -E 'AUTHREQ_(PROMPTING|RESULT)')"
printf '%s\n' "$lines"
for pid in $PIDS $(sed -n 's/.*AUTHREQ_PROMPTING: msgID=\([0-9]*\)\..*/\1/p' <<<"$lines" | sort -u); do
    ps -p "$pid" >/dev/null 2>&1 && echo "regress:alive $pid"
done
echo "regress:since $(date -v-60S '+%Y-%m-%d %H:%M:%S')"
REMOTE
)" || true
        while IFS= read -r line; do
            if [[ "$line" =~ AUTHREQ_PROMPTING:\ msgID=([0-9.]+),\ service=([A-Za-z]+), ]] \
                && [[ "$line" == *"binary_path=/Applications/DuskStudio.app/"* ]]; then
                msg="${BASH_REMATCH[1]}"
                service="${BASH_REMATCH[2]}"
                if [[ -z "${service_of[$msg]:-}" ]]; then
                    service_of[$msg]="$service"
                    echo "seen ${msg} ${service}" >>"$log"
                fi
            elif [[ "$line" =~ AUTHREQ_RESULT:\ msgID=([0-9.]+),\ authValue=([0-9]+) ]]; then
                msg="${BASH_REMATCH[1]}"
                value="${BASH_REMATCH[2]}"
                if [[ -n "${service_of[$msg]:-}" && -z "${answered[$msg]:-}" ]]; then
                    answered[$msg]="$value"
                    echo "answered ${msg} ${service_of[$msg]} ${value}" >>"$log"
                fi
            fi
        done <<<"$out"
        now="$(sed -n 's/^regress:since //p' <<<"$out")"
        # Without a complete poll nothing is known about which pids are alive.
        if [[ -z "$now" ]]; then
            sleep 4
            continue
        fi
        since="$now"
        alive=()
        while read -r _ pid; do
            [[ -n "$pid" ]] && alive[$pid]=1
        done < <(grep '^regress:alive ' <<<"$out")

        for msg in "${!service_of[@]}"; do
            [[ -z "${answered[$msg]:-}${closed[$msg]:-}" ]] || continue
            service="${service_of[$msg]}"
            if [[ -z "${alive[${msg%%.*}]:-}" ]]; then
                closed[$msg]=gone
                echo "gone ${msg} ${service}: pid ${msg%%.*} has exited" >>"$log"
                continue
            fi
            if [[ "$service" != kTCCServiceMicrophone ]]; then
                closed[$msg]=refused
                vnc cap "${RUN_DIR}/prompt-${msg}.png" >/dev/null 2>>"${RUN_DIR}/vnc.err" || true
                echo "refused ${msg} ${service}: only the microphone prompt is clicked" >>"$log"
                continue
            fi
            ((${clicks[$msg]:-0} < 3)) || continue
            ((SECONDS - ${clicked_at[$msg]:-0} >= 20)) || continue
            clicks[$msg]=$((${clicks[$msg]:-0} + 1))
            clicked_at[$msg]=$SECONDS
            shot="${RUN_DIR}/prompt-${msg}-${clicks[$msg]}.png"
            rc=0
            vnc click "${MIC_ALLOW%,*}" "${MIC_ALLOW#*,}" --expect allow --pre-cap "$shot" \
                >/dev/null 2>>"${RUN_DIR}/vnc.err" || rc=$?
            case "$rc" in
                0) echo "clicked ${msg} ${service} Allow at ${MIC_ALLOW}" >>"$log" ;;
                3) echo "not-on-screen ${msg} ${service}: no Allow button at ${MIC_ALLOW} in $(basename "$shot") (reason in vnc.err)" >>"$log" ;;
                *) echo "click-failed ${msg} ${service} (see vnc.err)" >>"$log" ;;
            esac
        done
        sleep 4
    done
}

start_watcher() {
    : >"${RUN_DIR}/prompts.log"
    prompt_watch &
    WATCH_PID=$!
}

# Every new build is a new ad-hoc identity, so its first launch asks for the
# microphone, and an app started while that prompt is up comes up with no
# device. One throwaway launch gets it answered before any suite runs. The
# answer shows up either in the watcher's log, when this launch raised the
# prompt, or as the launch's own query result, when the build was answered
# before.
leg_mic_permission() {
    local pid out value="" deadline=$((SECONDS + MIC_TIMEOUT))
    pid="$(mac_run 60 <<'REMOTE'
open_app "$HOME/$RUN/mic" DUSKSTUDIO_SKIP_STARTUP_DIALOG=1
REMOTE
)" || return 1
    [[ "$pid" =~ ^[0-9]+$ ]] || { echo "error: no pid for the permission launch: ${pid}" >&2; return 1; }
    echo "permission launch: pid ${pid}"
    while ((SECONDS < deadline)); do
        value="$(awk '$1 == "answered" && $3 == "kTCCServiceMicrophone" { print $4 }' \
            "${RUN_DIR}/prompts.log" | tail -1)"
        [[ -n "$value" ]] && break
        out="$(mac_run 60 PID="$pid" SINCE="$WATCH_SINCE" <<'REMOTE'
log show --start "$SINCE" --style compact \
    --predicate "process == \"tccd\" AND eventMessage CONTAINS \"msgID=$PID.\"" 2>/dev/null |
    grep -E 'AUTHREQ_(CTX|RESULT)'
REMOTE
)" || true
        value="$(awk '
            /AUTHREQ_CTX: .*service=kTCCServiceMicrophone/ { match($0, /msgID=[0-9.]+/); mic[substr($0, RSTART + 6, RLENGTH - 6)] = 1 }
            /AUTHREQ_RESULT: / {
                match($0, /msgID=[0-9.]+/); id = substr($0, RSTART + 6, RLENGTH - 6)
                if (id in mic) { match($0, /authValue=[0-9]+/); v = substr($0, RSTART + 10, RLENGTH - 10); if (v != 1) last = v }
            }
            END { print last }' <<<"$out")"
        [[ -n "$value" ]] && break
        sleep 5
    done
    mac_run 60 PID="$pid" <<'REMOTE' || true
kill "$PID" 2>/dev/null
sleep 3
kill -9 "$PID" 2>/dev/null
exit 0
REMOTE
    case "$value" in
        2)
            echo "microphone: allowed"
            ;;
        0)
            echo "error: the microphone is denied for this build; reset it on the node with" >&2
            echo "       tccutil reset Microphone audio.dusk.studio" >&2
            return 1
            ;;
        *)
            vnc cap "${RUN_DIR}/mic-timeout.png" >/dev/null 2>&1 || true
            echo "error: no microphone decision within ${MIC_TIMEOUT}s; see ${RUN_DIR}/mic-timeout.png and prompts.log" >&2
            return 1
            ;;
    esac
}

# ---------------------------------------------------------------- suites

# mac_suite_leg <name> <log> <rc>: judged as every other platform's suite is,
# plus a row of its own for a BAILING teardown line, which the suite does not
# count as a failure.
mac_suite_leg() {
    local name="$1" secs="$2" log="$3" rc="$4" dirty bailing
    dirty="$(grep -c '^\[DIRTY\]' "$log" || true)"
    if ((dirty > 0)); then
        echo "${dirty} [DIRTY] line(s):"
        grep '^\[DIRTY\]' "$log" | sed 's/^/  /'
    fi
    regress_scenario_leg "$name" "$secs" "$log" "$rc"
    bailing="$(grep -c 'BAILING' "$log" || true)"
    if ((bailing > 0)); then
        regress_record "${name}:bailing" FAIL 0 "${bailing} BAILING line(s), first: $(grep -m1 'BAILING' "$log" | cut -c1-160)"
    fi
}

# The last line a suite script prints is its verdict: an exit status, TIMEOUT,
# or EXITED for an app started through open, whose status nothing can read.
suite_rc() {
    case "$(sed -n 's/^regress:exit //p' "$1" | tail -1)" in
        TIMEOUT) echo 124 ;;
        EXITED) echo 0 ;;
        '' | *[!0-9]*) echo 125 ;;
        *) sed -n 's/^regress:exit //p' "$1" | tail -1 ;;
    esac
}

leg_scenarios_headless() {
    printf '\n--- scenarios-headless ---\n'
    local start=$SECONDS raw="${RUN_DIR}/scenarios-headless.raw" log="${RUN_DIR}/scenarios-headless.log" rc
    mac_run $((ENGINE_TIMEOUT + 120)) BUDGET="$ENGINE_TIMEOUT" >"$raw" 2>&1 <<'REMOTE' || true
d="$HOME/$RUN/headless"
sandbox "$d" || exit 1
env "${SANDBOX_ENV[@]}" DUSKSTUDIO_RUN_SCENARIOS=all DUSKSTUDIO_EXPECT_MP3=1 \
    DUSKSTUDIO_FIXTURE_DIR="$FIXTURE_DIR" "$BIN" >"$d/suite.log" 2>&1 &
pid=$!
echo "$pid" >>"$HOME/$RUN/pids"
waited=0
while kill -0 "$pid" 2>/dev/null && ((waited < BUDGET)); do
    sleep 2
    waited=$((waited + 2))
done
if kill -0 "$pid" 2>/dev/null; then
    kill -9 "$pid" 2>/dev/null
    wait "$pid" 2>/dev/null
    cat "$d/suite.log"
    echo "regress:exit TIMEOUT"
    exit 0
fi
wait "$pid"
status=$?
cat "$d/suite.log"
echo "regress:exit $status"
REMOTE
    rc="$(suite_rc "$raw")"
    grep -v '^regress:exit ' "$raw" >"$log" || true
    cat "$log"
    mac_suite_leg "scenarios-headless" "$((SECONDS - start))" "$log" "$rc"
}

leg_scenarios_gui() {
    printf '\n--- scenarios-gui ---\n'
    local start=$SECONDS raw="${RUN_DIR}/scenarios-gui.raw" log="${RUN_DIR}/scenarios-gui.log" rc
    if screen_locked; then
        leg_screen >/dev/null 2>&1 || true
    fi
    mac_run $((GUI_TIMEOUT + 180)) BUDGET="$GUI_TIMEOUT" >"$raw" 2>&1 <<'REMOTE' || true
d="$HOME/$RUN/gui"
pid="$(open_app "$d" DUSKSTUDIO_RUN_SCENARIOS=gui DUSKSTUDIO_EXPECT_MP3=1 DUSKSTUDIO_FIXTURE_DIR="$FIXTURE_DIR")" || exit 1
waited=0
while kill -0 "$pid" 2>/dev/null && ((waited < BUDGET)); do
    sleep 5
    waited=$((waited + 5))
done
verdict=EXITED
if kill -0 "$pid" 2>/dev/null; then
    kill -9 "$pid" 2>/dev/null
    verdict=TIMEOUT
fi
sleep 1
cat "$d/stdout.log" "$d/stderr.log" 2>/dev/null
echo "regress:exit $verdict"
REMOTE
    rc="$(suite_rc "$raw")"
    grep -v '^regress:exit ' "$raw" >"$log" || true
    cat "$log"
    mac_suite_leg "scenarios-gui" "$((SECONDS - start))" "$log" "$rc"
}

# Every microphone prompt the watcher saw has to have been answered Allow, or
# dropped unanswered with the process that asked. Any other prompt fails the
# leg however it was answered: a folder prompt means a suite reached past its
# private HOME, and the watcher never answers one.
prompts_leg() {
    local log="${RUN_DIR}/prompts.log" msg service value seen=0 bad="" notes=""
    printf '\n--- privacy-prompts ---\n'
    cat "$log"
    while read -r _ msg service; do
        seen=$((seen + 1))
        value="$(awk -v m="$msg" '$1 == "answered" && $2 == m { print $4 }' "$log" | tail -1)"
        if [[ -z "$value" ]] && grep -q "^gone ${msg} " "$log"; then
            value=gone
        fi
        case "${service}:${value}" in
            kTCCServiceMicrophone:2)
                notes="${notes}${notes:+, }Microphone answered"
                ;;
            kTCCServiceMicrophone:gone)
                notes="${notes}${notes:+, }Microphone ${msg} unanswered, its process gone"
                ;;
            kTCCServiceSystemPolicy*Folder:*)
                bad="${bad}${bad:+, }${service#kTCCService} raised (${value:-unanswered}): a suite reached past its private HOME; if the prompt is still up, answer it Don't Allow on the node"
                ;;
            *:'' | *:gone) bad="${bad}${bad:+, }${service#kTCCService} unanswered" ;;
            *) bad="${bad}${bad:+, }${service#kTCCService} answered ${value}" ;;
        esac
    done < <(grep '^seen ' "$log")
    if [[ -n "$bad" ]]; then
        regress_record "privacy-prompts" FAIL 0 "${bad}; screenshots in ${RUN_DIR}"
    elif ((seen == 0)); then
        regress_record "privacy-prompts" PASS 0 "none raised"
    else
        regress_record "privacy-prompts" PASS 0 "$notes"
    fi
}

leg_crash_reports() {
    mac_run 60 <<'REMOTE'
reports="$(find "$HOME/Library/Logs/DiagnosticReports" -maxdepth 1 -name 'DuskStudio*' -newer "$HOME/$RUN/started" 2>/dev/null)"
if [[ -n "$reports" ]]; then
    echo "error: the installed app left crash reports:" >&2
    echo "$reports" | sed 's/^/  /' >&2
    exit 1
fi
echo "no DuskStudio crash reports since the leg started"
REMOTE
}

# ---------------------------------------------------------------- the run

regress_last_passed() {
    [[ "${REGRESS_LEG_STATUS[${#REGRESS_LEG_STATUS[@]} - 1]}" == PASS ]]
}

echo "Dusk Studio regression - mac package"
echo "node     ${MAC_HOST} (screen sharing ${MAC_VNC_HOST})"
if [[ -n "$RELEASE_RUN" ]]; then
    echo "package  release.yml run ${RELEASE_RUN}"
else
    echo "package  ${DMG}"
fi
echo "run dir  ${RUN_DIR}"
echo "node dir ~/${REMOTE_RUN} (removed on exit)"

SUITE_ROWS=(mic-permission scenarios-headless scenarios-gui privacy-prompts crash-reports)

# Registered before preflight, which creates the node directory and starts
# caffeinate and can still fail after that.
regress_at_exit remote_cleanup
regress_at_exit delete_remote_branch
regress_leg "mac-preflight" leg_preflight
if ! regress_last_passed; then
    regress_summary "regress mac" || true
    exit 1
fi
regress_leg "screen-unlocked" leg_screen

if [[ -n "$RELEASE_RUN" ]]; then
    regress_leg "package-fetch" leg_package_fetch
else
    SOURCE_SHA="$(git rev-parse HEAD)"
    regress_skip "package-fetch" "local disk image $(basename "$DMG")"
fi

installed=0
if [[ -n "$DMG" ]]; then
    regress_leg "dmg-install" leg_dmg_install
    regress_last_passed && installed=1
else
    regress_skip "dmg-install" "no disk image"
fi

if [[ -n "$SOURCE_SHA" ]]; then
    echo
    echo "fixtures from ${SOURCE_SHA} in ${MAC_HOST}:~/${MAC_TREE}"
    regress_leg "fixture-source" leg_fixture_source
    if regress_last_passed; then
        regress_leg "configure-fixtures" leg_configure_fixtures
        regress_leg "donor-check" leg_donor_check
        regress_leg "build-fixtures" leg_build_fixtures
    else
        for leg in configure-fixtures donor-check build-fixtures; do
            regress_skip "$leg" "no worktree at the source commit"
        done
    fi
    mac_fixture_leg
else
    for leg in fixture-source configure-fixtures donor-check build-fixtures scenario-fixtures; do
        regress_skip "$leg" "the source commit is unknown"
    done
fi

if ((installed)); then
    start_watcher
    regress_leg "mic-permission" leg_mic_permission
    leg_scenarios_headless
    leg_scenarios_gui
    stop_watcher
    prompts_leg
    regress_leg "crash-reports" leg_crash_reports
else
    for leg in "${SUITE_ROWS[@]}"; do regress_skip "$leg" "nothing installed"; done
fi
regress_skip "ipc-selftest" "Linux-only code path"
if ((installed)); then
    regress_skip "package-uninstall" "left in /Applications; the next run replaces it"
fi

echo
echo "logs:        ${RUN_DIR}/"
regress_summary "regress mac"
