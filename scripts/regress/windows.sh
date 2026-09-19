#!/usr/bin/env bash
# Windows leg of the regression runner: drives the libvirt win11 guest.
#
# The guest has no qemu-guest-agent and no SSH. The whole channel is
#   - virsh send-key to type into a PowerShell console,
#   - an HTTP server on the host serving the phase scripts and the payload,
#   - a collector on the host that the phase scripts POST their report to,
#   - virsh screenshot to see what the guest is actually showing.
# See docs/MAINTAINER-GUIDE.md, "Regression run across platforms".

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck source=scripts/regress/common.sh
source "${REPO_ROOT}/scripts/regress/common.sh"
WIN_DIR="${REPO_ROOT}/scripts/regress/windows"

VM="${DUSK_REGRESS_VM:-win11}"
LIBVIRT_URI="${DUSK_REGRESS_LIBVIRT_URI:-qemu:///system}"
HOST_IP="${DUSK_REGRESS_HOST_IP:-192.168.122.1}"
HTTP_PORT=8000
COLLECTOR_PORT=9000
GUEST_ROOT="DuskStudio-regress"
ZIP_NAME="dusk-studio-regress.zip"
REPO_SLUG="dusk-audio/dusk-studio"

MSI_PATH=""
RELEASE_RUN=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --msi)
            [[ $# -ge 2 ]] || regress_die "--msi needs a path"
            MSI_PATH="$2"
            shift 2
            ;;
        --release-run)
            [[ $# -ge 2 ]] || regress_die "--release-run needs a workflow run id"
            RELEASE_RUN="$2"
            shift 2
            ;;
        *) regress_die "unknown option '$1' for the windows target" ;;
    esac
done

[[ -n "$MSI_PATH" || -n "$RELEASE_RUN" ]] \
    || regress_die "the windows target needs --msi <path> or --release-run <run id>"
regress_require virsh python3 7z zip ss
if [[ -n "$RELEASE_RUN" ]]; then
    regress_require gh
fi

RUN_DIR="${TMPDIR:-/tmp}/dusk-regress-windows-$(date +%Y%m%d-%H%M%S)"
SERVE_DIR="${RUN_DIR}/www"
REPORT="${RUN_DIR}/report.log"
mkdir -p "$SERVE_DIR"
: >"$REPORT"

HTTP_PID=""
COLLECTOR_PID=""

# Runs from the EXIT trap, where set -e is still live: every kill has to be
# tolerant of finding nothing, or a clean teardown exits the script non-zero.
stop_servers() {
    if [[ -n "$HTTP_PID" ]]; then
        kill "$HTTP_PID" 2>/dev/null || true
    fi
    if [[ -n "$COLLECTOR_PID" ]]; then
        kill "$COLLECTOR_PID" 2>/dev/null || true
    fi
    # Bracketed first character so the pattern cannot match this pkill's own
    # command line and take the calling shell down with it.
    pkill -f "[c]ollector\.py --bind ${HOST_IP}" 2>/dev/null || true
    pkill -f "[h]ttp\.server ${HTTP_PORT} --bind ${HOST_IP}" 2>/dev/null || true
    HTTP_PID=""
    COLLECTOR_PID=""
    return 0
}
trap stop_servers EXIT

vkey() {
    python3 "${WIN_DIR}/vkey.py" --domain "$VM" --connect "$LIBVIRT_URI" "$@"
}

screenshot() {
    python3 "${WIN_DIR}/screen.py" --domain "$VM" --connect "$LIBVIRT_URI" --out "$1"
}

screen_mean() {
    screenshot "$1" | sed -n 's/^mean=\([0-9.]*\).*/\1/p'
}

leg_payload() {
    local msi="$MSI_PATH" work="${RUN_DIR}/msi" src base dest
    if [[ -n "$RELEASE_RUN" ]]; then
        mkdir -p "${RUN_DIR}/artifact"
        gh run download "$RELEASE_RUN" -R "$REPO_SLUG" -n release-windows \
            -D "${RUN_DIR}/artifact"
        msi="$(find "${RUN_DIR}/artifact" -name '*.msi' | head -1)"
        [[ -n "$msi" ]] || {
            echo "error: the release-windows artifact contains no .msi" >&2
            return 1
        }
    fi
    [[ -f "$msi" ]] || {
        echo "error: installer not found: $msi" >&2
        return 1
    }
    echo "installer: $msi"

    mkdir -p "${work}/x" "${work}/pkg/bin"
    7z x -y -o"${work}/x" "$msi" >/dev/null
    # The MSI's CAB flattens the install tree into CM_FP_<dir>.<file> names.
    for src in "${work}/x"/CM_FP_*; do
        [[ -f "$src" ]] || continue
        base="$(basename "$src")"
        base="${base#CM_FP_}"
        if [[ "$base" == bin.* ]]; then
            dest="${work}/pkg/bin/${base#bin.}"
        else
            dest="${work}/pkg/${base}"
        fi
        mv "$src" "$dest"
    done
    # Dusk Studio looks for the child host next to itself under its dashed name.
    [[ -f "${work}/pkg/bin/dusk_studio_plugin_host.exe" ]] \
        && mv "${work}/pkg/bin/dusk_studio_plugin_host.exe" \
            "${work}/pkg/bin/dusk-studio-plugin-host.exe"

    # Phases 2 and 3 open this through DUSKSTUDIO_LOAD_SESSION instead of
    # clicking the startup picker, so the session ships with the payload rather
    # than depending on what the guest happens to have in Recent Sessions. The
    # load line names the file, so the copy phase 2 hands over gets its own name.
    mkdir -p "${work}/pkg/regress-session"
    cp "${REPO_ROOT}/scripts/regress/sessions/minimal/session.json" \
        "${work}/pkg/regress-session/session.json"
    cp "${REPO_ROOT}/scripts/regress/sessions/minimal/session.json" \
        "${work}/pkg/regress-session/handoff.json"

    if [[ ! -f "${work}/pkg/bin/DuskStudio.exe" ]]; then
        echo "error: no bin/DuskStudio.exe after unpacking; extracted:" >&2
        find "${work}/x" "${work}/pkg" -maxdepth 2 -type f -printf '  %P\n' >&2
        return 1
    fi
    (cd "${work}/pkg" && zip -qr "${SERVE_DIR}/${ZIP_NAME}" .)
    find "${work}/pkg" -type f -printf '  %P\n' | sort
    echo "payload: ${SERVE_DIR}/${ZIP_NAME}"
}

# The phase scripts are served, not typed, so the host address and the guest
# install directory are substituted in here rather than duplicated in each one.
install_scripts() {
    local pair name short
    for pair in "probe.ps1 p0.ps1" \
        "phase1-selftest.ps1 p1.ps1" \
        "phase2-handoff.ps1 p2.ps1" \
        "phase3-session-close.ps1 p3.ps1"; do
        name="${pair% *}"
        short="${pair#* }"
        sed -e "s|@@HOSTIP@@|${HOST_IP}|g" \
            -e "s|@@ROOT@@|${GUEST_ROOT}|g" \
            -e "s|@@ZIP@@|${ZIP_NAME}|g" \
            "${WIN_DIR}/${name}" >"${SERVE_DIR}/${short}"
    done
}

leg_servers() {
    stop_servers
    install_scripts
    python3 -m http.server "$HTTP_PORT" --bind "$HOST_IP" --directory "$SERVE_DIR" \
        >"${RUN_DIR}/http.log" 2>&1 &
    HTTP_PID=$!
    python3 "${WIN_DIR}/collector.py" --bind "$HOST_IP" --port "$COLLECTOR_PORT" \
        "$REPORT" >"${RUN_DIR}/collector.log" 2>&1 &
    COLLECTOR_PID=$!
    local waited=0
    while ((waited < 20)); do
        if ss -ltn "sport = :${HTTP_PORT}" | grep -q "$HOST_IP" \
            && ss -ltn "sport = :${COLLECTOR_PORT}" | grep -q "$HOST_IP"; then
            echo "serving ${SERVE_DIR} on ${HOST_IP}:${HTTP_PORT}, collector on :${COLLECTOR_PORT}"
            return 0
        fi
        sleep 1
        waited=$((waited + 1))
    done
    echo "error: host servers did not come up:" >&2
    sed 's/^/  /' "${RUN_DIR}/http.log" "${RUN_DIR}/collector.log" >&2
    return 1
}

# A blanked display returns an all-black frame, which otherwise looks the same
# as a hung guest.
leg_wake() {
    local state mean shot="${RUN_DIR}/00-wake.png"
    state="$(virsh -c "$LIBVIRT_URI" domstate "$VM")"
    echo "domain ${VM}: ${state}"
    [[ "$state" == running ]] || {
        echo "error: ${VM} is not running" >&2
        return 1
    }
    mean="$(screen_mean "$shot")"
    if [[ -z "$mean" ]] || (($(printf '%.0f' "$mean") < 3)); then
        echo "screen is dark (mean=${mean:-unknown}); sending a wake key"
        vkey --raw KEY_LEFTSHIFT
        sleep 3
        mean="$(screen_mean "$shot")"
    fi
    echo "screenshot: ${shot} (mean=${mean:-unknown})"
    if [[ -z "$mean" ]]; then
        echo "note: brightness not measurable (python3-pillow missing); wake key sent, continuing"
        return 0
    fi
    (($(printf '%.0f' "$mean") >= 3))
}

# Focus is unreliable after a phase that called SetForegroundWindow, so every
# phase gets its own console opened from the Start menu.
open_console() {
    vkey --raw KEY_LEFTMETA
    sleep 3
    vkey powershell
    sleep 3
    vkey --raw KEY_ENTER
    sleep 8
}

wait_for_marker() {
    local marker="$1" deadline="$2" offset="$3" waited=0
    while ((waited < deadline)); do
        if tail -c "+$((offset + 1))" "$REPORT" 2>/dev/null | grep -qF "$marker"; then
            return 0
        fi
        sleep 2
        waited=$((waited + 2))
    done
    return 1
}

# run_guest_phase <phase name> <served script> <deadline seconds>
run_guest_phase() {
    local name="$1" script="$2" deadline="$3"
    local offset shot body result
    offset="$(stat -c %s "$REPORT")"
    open_console
    shot="${RUN_DIR}/${name}-console.png"
    echo "console screenshot: $(screenshot "$shot")"
    vkey "iwr -useb http://${HOST_IP}:${HTTP_PORT}/${script}|iex"
    vkey --raw KEY_ENTER
    if ! wait_for_marker "REGRESS-PHASE ${name} END" "$deadline" "$offset"; then
        echo "error: ${name} did not report within ${deadline}s" >&2
        echo "timeout screenshot: $(screenshot "${RUN_DIR}/${name}-timeout.png")" >&2
        return 1
    fi
    body="$(tail -c "+$((offset + 1))" "$REPORT")"
    printf '%s\n' "$body"
    result="$(sed -n "s/^REGRESS-PHASE ${name} RESULT //p" <<<"$body" | tail -1)"
    [[ "$result" == PASS ]]
}

leg_probe() { run_guest_phase probe p0.ps1 90; }
leg_phase1() { run_guest_phase phase1 p1.ps1 900; }
leg_phase2() { run_guest_phase phase2 p2.ps1 300; }
leg_phase3() { run_guest_phase phase3 p3.ps1 300; }

echo "Dusk Studio regression - windows"
echo "guest    ${VM} (${LIBVIRT_URI}), host ${HOST_IP}"
echo "run dir  ${RUN_DIR}"

regress_leg "payload" leg_payload
regress_leg "host-servers" leg_servers
regress_leg "guest-wake" leg_wake
regress_leg "console-probe" leg_probe
regress_leg "phase1-selftest" leg_phase1
regress_leg "phase2-handoff" leg_phase2
regress_leg "phase3-session-close" leg_phase3
regress_skip "ipc-selftest" "hangs on Windows (issue #504)"

echo
echo "report:      ${REPORT}"
echo "screenshots: ${RUN_DIR}/*.png"
regress_summary "regress windows"
