#!/usr/bin/env bash
# Windows leg of the regression runner: drives the libvirt win11 guest.
#
# The guest has no qemu-guest-agent and no SSH. The whole channel is
#   - virsh send-key to type into a PowerShell console,
#   - an HTTP server on the host serving the phase scripts and the payload,
#   - a collector on the host that the phase scripts POST their report to,
#   - virsh screenshot to see what the guest is actually doing.
# The installer goes into Program Files through msiexec, and its UAC prompt is
# answered by a person at the VM console, never by this script.
# See docs/MAINTAINER-GUIDE.md, "Regression run across platforms".

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck source=scripts/regress/common.sh
source "${REPO_ROOT}/scripts/regress/common.sh"
REGRESS_PLATFORM=windows
# shellcheck source=scripts/regress/scenarios.sh
source "${REPO_ROOT}/scripts/regress/scenarios.sh"
WIN_DIR="${REPO_ROOT}/scripts/regress/windows"

VM="${DUSK_REGRESS_VM:-win11}"
LIBVIRT_URI="${DUSK_REGRESS_LIBVIRT_URI:-qemu:///system}"
HOST_IP="${DUSK_REGRESS_HOST_IP:-192.168.122.1}"
UAC_WAIT="${DUSK_REGRESS_UAC_WAIT:-1800}"
HTTP_PORT=8000
COLLECTOR_PORT=9000
REPO_SLUG="dusk-audio/dusk-studio"
ENGINE_TIMEOUT=900
GUI_TIMEOUT=1800

MSI_PATH=""
RELEASE_RUN=""
FIXTURES_DIR=""
FIXTURES_RUN=""
INSTALL_MODE="install"
RUN_SCENARIOS=1

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
        --fixtures)
            [[ $# -ge 2 ]] || regress_die "--fixtures needs a directory"
            FIXTURES_DIR="$2"
            shift 2
            ;;
        --fixtures-run)
            [[ $# -ge 2 ]] || regress_die "--fixtures-run needs a workflow run id"
            FIXTURES_RUN="$2"
            shift 2
            ;;
        --reinstall)
            INSTALL_MODE="reinstall"
            shift
            ;;
        --extract-only)
            INSTALL_MODE="extract"
            shift
            ;;
        --no-scenarios)
            RUN_SCENARIOS=0
            shift
            ;;
        *) regress_die "unknown option '$1' for the windows target" ;;
    esac
done

[[ -n "$MSI_PATH" || -n "$RELEASE_RUN" ]] \
    || regress_die "the windows target needs --msi <path> or --release-run <run id>"
[[ -z "$MSI_PATH" || -z "$RELEASE_RUN" ]] \
    || regress_die "--msi and --release-run both name the installer; pass one"
[[ -z "$FIXTURES_DIR" || -z "$FIXTURES_RUN" ]] \
    || regress_die "--fixtures and --fixtures-run both name the fixtures; pass one"
if [[ -n "$FIXTURES_DIR" ]]; then
    [[ -d "$FIXTURES_DIR" ]] || regress_die "no such fixture directory: $FIXTURES_DIR"
fi
[[ "$UAC_WAIT" =~ ^[0-9]+$ ]] || regress_die "DUSK_REGRESS_UAC_WAIT must be a number of seconds"
regress_require virsh python3 7z zip ss sha256sum
if [[ -n "$RELEASE_RUN" || -n "$FIXTURES_RUN" ]]; then
    regress_require gh
fi

STAMP="$(date +%Y%m%d-%H%M%S)"
RUN_DIR="${TMPDIR:-/tmp}/dusk-regress-windows-${STAMP}"
GUEST_ROOT="dusk-regress-${STAMP}"
SERVE_DIR="${RUN_DIR}/www"
POST_DIR="${RUN_DIR}/posts"
REPORT="${RUN_DIR}/report.log"
mkdir -p "$SERVE_DIR" "$POST_DIR"
: >"$REPORT"

HTTP_PID=""
COLLECTOR_PID=""
PACKAGE_VERSION=""
MSI_NAME=""
PHASE_NOTE=""
PHASE_WARN=""

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

# ---------------------------------------------------------------- payload

# The MSI's CAB holds every file under its File-table key, which is what the
# guest checks the installed files against.
write_manifest() {
    local cab_dir="$1" src
    for src in "$cab_dir"/*; do
        [[ -f "$src" ]] || continue
        printf '%s\t%s\n' "$(basename "$src")" "$(sha256sum "$src" | cut -d' ' -f1)"
    done >"${SERVE_DIR}/manifest.tsv"
    [[ -s "${SERVE_DIR}/manifest.tsv" ]] || {
        echo "error: the installer unpacked to no files" >&2
        return 1
    }
    grep -q $'^CM_FP_bin\.DuskStudio\.exe\t' "${SERVE_DIR}/manifest.tsv" || {
        echo "error: the installer carries no bin/DuskStudio.exe; it holds:" >&2
        cut -f1 "${SERVE_DIR}/manifest.tsv" | sed 's/^/  /' >&2
        return 1
    }
    sed 's/^/  /' "${SERVE_DIR}/manifest.tsv"
}

leg_payload() {
    local msi="$MSI_PATH" name tree_version
    if [[ -n "$RELEASE_RUN" ]]; then
        gh run view "$RELEASE_RUN" -R "$REPO_SLUG" \
            --json workflowName,headBranch,headSha,status,conclusion \
            --template '{{.workflowName}} run on {{.headBranch}} at {{.headSha}}: {{.status}} {{.conclusion}}{{"\n"}}' \
            || return 1
        mkdir -p "${RUN_DIR}/artifact"
        gh run download "$RELEASE_RUN" -R "$REPO_SLUG" -n release-windows \
            -D "${RUN_DIR}/artifact" || return 1
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
    name="$(basename "$msi")"
    if [[ ! "$name" =~ ^dusk-studio-([0-9]+\.[0-9]+\.[0-9]+)-Windows-x64\.msi$ ]]; then
        echo "error: not a release installer name (dusk-studio-X.Y.Z-Windows-x64.msi): ${name}" >&2
        return 1
    fi
    PACKAGE_VERSION="${BASH_REMATCH[1]}"
    MSI_NAME="$name"
    tree_version="$(tr -d '[:space:]' <"${REPO_ROOT}/VERSION")"
    if [[ "$tree_version" != "$PACKAGE_VERSION" ]]; then
        echo "note: the package is ${PACKAGE_VERSION} but this checkout is ${tree_version};"
        echo "      fixtures and scenario expectations come from the checkout"
    fi
    cp "$msi" "${SERVE_DIR}/${MSI_NAME}"

    mkdir -p "${RUN_DIR}/msi"
    7z x -y -o"${RUN_DIR}/msi" "$msi" >/dev/null || return 1
    write_manifest "${RUN_DIR}/msi" || return 1

    # Phases 2 and 3 open these through DUSKSTUDIO_LOAD_SESSION instead of
    # clicking the startup picker. The load line names the file, so the copy
    # phase 2 hands over gets its own name.
    cp "${REPO_ROOT}/scripts/regress/sessions/minimal/session.json" "${SERVE_DIR}/session.json"
    cp "${REPO_ROOT}/scripts/regress/sessions/minimal/session.json" "${SERVE_DIR}/handoff.json"
    echo "payload: ${SERVE_DIR}"
}

# The same two roots a Linux run hands the app: the test build's plug-ins,
# which a package does not ship and this box cannot build for Windows, and the
# checkout's data fixtures, which are the same on every platform.
leg_fixtures() {
    local stage="${RUN_DIR}/fixtures"
    mkdir -p "${stage}/build-tests" "${stage}/tests-fixtures"
    cp -r "${REPO_ROOT}/tests/fixtures/midi" "${stage}/tests-fixtures/"
    if [[ -n "$FIXTURES_RUN" ]]; then
        gh run download "$FIXTURES_RUN" -R "$REPO_SLUG" -n duskstudio-windows-fixtures \
            -D "${stage}/build-tests" || return 1
    elif [[ -n "$FIXTURES_DIR" ]]; then
        cp -r "${FIXTURES_DIR}/." "${stage}/build-tests/"
    else
        echo "no Windows fixture plug-ins given (--fixtures or --fixtures-run); data fixtures only"
    fi
    (cd "$stage" && zip -qr "${SERVE_DIR}/fixtures.zip" build-tests tests-fixtures) || return 1
    find "$stage" -type f -printf '  %P\n' | sort
}

# ---------------------------------------------------------------- channel

# The phase scripts are served, not typed: lib.ps1 goes in front of each, and
# the host address, run folder and per-phase values are substituted here.
serve_script() {
    local source="$1" served="$2"
    shift 2
    local -a subst=(
        -e "s|@@HOSTIP@@|${HOST_IP}|g"
        -e "s|@@ROOT@@|${GUEST_ROOT}|g"
        -e "s|@@VERSION@@|${PACKAGE_VERSION}|g"
        -e "s|@@MSI@@|${MSI_NAME}|g"
        -e "s|@@INSTALLMODE@@|${INSTALL_MODE}|g"
        -e "s|@@UACWAIT@@|${UAC_WAIT}|g"
    )
    local pair
    for pair in "$@"; do subst+=(-e "s|@@${pair%%=*}@@|${pair#*=}|g"); done
    cat "${WIN_DIR}/lib.ps1" "${WIN_DIR}/${source}" | sed "${subst[@]}" >"${SERVE_DIR}/${served}"
    if grep -q '@@[A-Z]*@@' "${SERVE_DIR}/${served}"; then
        echo "error: ${served} still has placeholders: $(grep -o '@@[A-Z]*@@' "${SERVE_DIR}/${served}" | sort -u | tr '\n' ' ')" >&2
        return 1
    fi
}

install_scripts() {
    serve_script probe.ps1 p0.ps1 &&
        serve_script install.ps1 pi.ps1 &&
        serve_script phase1-selftest.ps1 p1.ps1 &&
        serve_script phase2-handoff.ps1 p2.ps1 &&
        serve_script phase3-session-close.ps1 p3.ps1 &&
        serve_script scenarios.ps1 pe.ps1 PHASE=scenarios-headless SPEC=all TIMEOUT="$ENGINE_TIMEOUT" &&
        serve_script scenarios.ps1 pg.ps1 PHASE=scenarios-gui SPEC=gui TIMEOUT="$GUI_TIMEOUT" &&
        serve_script settings-defaults.ps1 pd.ps1 &&
        serve_script audit.ps1 pa.ps1
}

leg_servers() {
    stop_servers
    install_scripts || return 1
    python3 -m http.server "$HTTP_PORT" --bind "$HOST_IP" --directory "$SERVE_DIR" \
        >"${RUN_DIR}/http.log" 2>&1 &
    HTTP_PID=$!
    python3 "${WIN_DIR}/collector.py" --bind "$HOST_IP" --port "$COLLECTOR_PORT" \
        --save-dir "$POST_DIR" "$REPORT" >"${RUN_DIR}/collector.log" 2>&1 &
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

phase_body() {
    tail -c "+$(($1 + 1))" "$REPORT" 2>/dev/null | tr -d '\r'
}

# run_guest_phase <phase name> <served script> <deadline seconds>. Leaves the
# phase's report in PHASE_BODY, its REGRESS-NOTE lines in PHASE_NOTE and its
# REGRESS-WARN lines in PHASE_WARN. While a UAC prompt the phase reported is
# open, the guest is screenshotted every 30 s; otherwise every 2 minutes, so a
# stall leaves a picture of what the guest was showing.
PHASE_BODY=""
run_guest_phase() {
    local name="$1" script="$2" deadline="$3"
    local offset start body result
    local uac_open=0 last_shot=0 stalled=0 seen_uac="" line label
    offset="$(stat -c %s "$REPORT")"
    PHASE_NOTE=""
    PHASE_WARN=""
    PHASE_BODY=""
    open_console
    echo "console screenshot: $(screenshot "${RUN_DIR}/${name}-console.png")"
    vkey "iwr -useb http://${HOST_IP}:${HTTP_PORT}/${script}|iex"
    vkey --raw KEY_ENTER
    start=$SECONDS
    last_shot=$SECONDS
    while :; do
        body="$(phase_body "$offset")"
        grep -qF "REGRESS-PHASE ${name} END" <<<"$body" && break
        while IFS= read -r line; do
            [[ -n "$line" ]] || continue
            grep -qxF -- "$line" <<<"$seen_uac" && continue
            seen_uac+="${line}"$'\n'
            label="$(awk '{print $2}' <<<"$line")"
            case "$line" in
                *prompt-up*)
                    uac_open=1
                    echo "UAC prompt up in ${VM} for the ${PACKAGE_VERSION} MSI ${label}"
                    echo "  prompt ${line##* }: accept it at the VM console (virt-manager); the leg waits up to $((UAC_WAIT / 60)) min in all"
                    echo "  screenshot: $(screenshot "${RUN_DIR}/${name}-uac-${label}-${line##* }.png")"
                    last_shot=$SECONDS
                    ;;
                *closed*)
                    uac_open=0
                    echo "UAC prompt for the ${label} closed after ${line##* after }"
                    ;;
                *expired*)
                    echo "UAC prompt for the ${label} expired unanswered (Windows withdraws it after about 2 min); asking again"
                    ;;
                *timed-out*)
                    uac_open=0
                    echo "error: nobody answered the UAC prompt for the ${label} within $((UAC_WAIT / 60)) min" >&2
                    ;;
            esac
        done < <(grep '^REGRESS-UAC ' <<<"$body" || true)
        if ((!stalled)) && grep -q '^REGRESS-TIMEOUT ' <<<"$body"; then
            stalled=1
            grep -m1 '^REGRESS-TIMEOUT ' <<<"$body"
            echo "  stall screenshot: $(screenshot "${RUN_DIR}/${name}-stall.png")"
        fi
        if ((SECONDS - start >= deadline)); then
            echo "error: ${name} did not report within ${deadline}s" >&2
            echo "timeout screenshot: $(screenshot "${RUN_DIR}/${name}-timeout.png")" >&2
            PHASE_BODY="$body"
            return 1
        fi
        if ((uac_open && SECONDS - last_shot >= 30)); then
            screenshot "${RUN_DIR}/${name}-uac-latest.png" >/dev/null || true
            echo "  still waiting for the UAC prompt ($((SECONDS - start))s)"
            last_shot=$SECONDS
        elif ((SECONDS - last_shot >= 120)); then
            screenshot "${RUN_DIR}/${name}-progress.png" >/dev/null || true
            last_shot=$SECONDS
        fi
        sleep 2
    done
    PHASE_BODY="$(phase_body "$offset")"
    printf '%s\n' "$PHASE_BODY"
    PHASE_NOTE="$(sed -n 's/^REGRESS-NOTE //p' <<<"$PHASE_BODY" | paste -sd ';' - | sed 's/;/; /g')"
    PHASE_WARN="$(sed -n 's/^REGRESS-WARN //p' <<<"$PHASE_BODY" | paste -sd ';' - | sed 's/;/; /g')"
    result="$(sed -n "s/^REGRESS-PHASE ${name} RESULT //p" <<<"$PHASE_BODY" | tail -1)"
    [[ "$result" == PASS ]]
}

# guest_leg <leg> <served script> <deadline>: one guest phase as one row. A
# failed phase carries its first error line, and one that passes with
# REGRESS-WARN lines is a WARN carrying them.
guest_leg() {
    local name="$1" script="$2" deadline="$3"
    printf '\n--- %s ---\n' "$name"
    local start=$SECONDS rc=0 status=PASS note why
    run_guest_phase "$name" "$script" "$deadline" || rc=$?
    note="$PHASE_NOTE"
    if ((rc != 0)); then
        status=FAIL
        why="$(grep -m1 -E '^error: | failed' <<<"$PHASE_BODY" || true)"
        [[ -n "$PHASE_BODY" ]] || why="no report within ${deadline}s"
        note="${note:+${note}; }${why:-exit ${rc}}"
    elif [[ -n "$PHASE_WARN" ]]; then
        status=WARN
        note="${note:+${note}; }${PHASE_WARN}"
    fi
    regress_record "$name" "$status" "$((SECONDS - start))" "$note"
}

regress_last_passed() {
    [[ "${REGRESS_LEG_STATUS[${#REGRESS_LEG_STATUS[@]} - 1]}" == PASS ]]
}

# scenario_leg <leg> <served script> <deadline>: the guest runs the suite and
# posts its whole output; it is judged here by regress_scenario_leg, exactly
# as a Linux run's is.
scenario_leg() {
    local name="$1" script="$2" deadline="$3"
    printf '\n--- %s ---\n' "$name"
    local start=$SECONDS log="${POST_DIR}/${name}.log" code rc
    rm -f "$log"
    if ! run_guest_phase "$name" "$script" "$deadline"; then
        regress_record "$name" FAIL "$((SECONDS - start))" "the guest phase did not complete"
        return 0
    fi
    [[ -f "$log" ]] || {
        regress_record "$name" FAIL "$((SECONDS - start))" "no suite output came back"
        return 0
    }
    sed -i 's/\r$//' "$log"
    cat "$log"
    code="$(sed -n 's/^REGRESS-EXIT //p' <<<"$PHASE_BODY" | tail -1)"
    case "$code" in
        TIMEOUT) rc=124 ;;
        '' | *[!0-9-]*) rc=125 ;;
        *) rc="$code" ;;
    esac
    local dirty
    dirty="$(grep -c '^\[DIRTY\]' "$log" || true)"
    if ((dirty > 0)); then
        echo "${dirty} [DIRTY] line(s):"
        grep '^\[DIRTY\]' "$log" | sed 's/^/  /'
    fi
    regress_scenario_leg "$name" "$((SECONDS - start))" "$log" "$rc"
}

# ---------------------------------------------------------------- the run

echo "Dusk Studio regression - windows"
echo "guest    ${VM} (${LIBVIRT_URI}), host ${HOST_IP}"
echo "run dir  ${RUN_DIR}"
echo "guest    %LOCALAPPDATA%\\${GUEST_ROOT}"

APP_LEGS=(phase1-selftest phase2-handoff phase3-session-close)
SCENARIO_ROWS=(scenario-fixtures scenarios-headless scenarios-gui gui-settings-defaults gui-plugin-picker)

skip_rest() {
    local reason="$1" leg
    shift
    for leg in "$@"; do regress_skip "$leg" "$reason"; done
}

regress_leg "payload" leg_payload
payload_ok=0
regress_last_passed && payload_ok=1
if ((payload_ok && RUN_SCENARIOS)); then
    regress_leg "fixtures" leg_fixtures
elif ((payload_ok)); then
    mkdir -p "${RUN_DIR}/fixtures/build-tests" "${RUN_DIR}/fixtures/tests-fixtures"
    (cd "${RUN_DIR}/fixtures" && zip -qr "${SERVE_DIR}/fixtures.zip" build-tests tests-fixtures)
    regress_skip "fixtures" "not requested (--no-scenarios)"
fi

channel_ok=0
if ((payload_ok)); then
    regress_leg "host-servers" leg_servers
    if regress_last_passed; then
        regress_leg "guest-wake" leg_wake
        guest_leg "console-probe" p0.ps1 90
        regress_last_passed && channel_ok=1
    else
        skip_rest "no host servers" guest-wake console-probe
    fi
else
    skip_rest "no payload" host-servers guest-wake console-probe
fi

installed=0
if ((channel_ok)); then
    guest_leg "msi-install" pi.ps1 $((UAC_WAIT * 3 + 600))
    regress_last_passed && installed=1
else
    regress_skip "msi-install" "the guest channel is not up"
fi

if ((installed)); then
    guest_leg "phase1-selftest" p1.ps1 900
    guest_leg "phase2-handoff" p2.ps1 300
    guest_leg "phase3-session-close" p3.ps1 300
    regress_skip "ipc-selftest" "hangs on Windows (issue #504)"
    if ((RUN_SCENARIOS)); then
        SCENARIO_FIXTURE_ROOTS="${RUN_DIR}/fixtures/build-tests:${RUN_DIR}/fixtures/tests-fixtures"
        scenarios_fixture_leg
        scenario_leg "scenarios-headless" pe.ps1 $((ENGINE_TIMEOUT + 300))
        scenario_leg "scenarios-gui" pg.ps1 $((GUI_TIMEOUT + 300))
        guest_leg "gui-settings-defaults" pd.ps1 900
        regress_skip "gui-plugin-picker" \
            "its seeded cache names an LV2 fixture, and Windows builds no LV2 host; run tests/gui_plugin_picker.sh on Linux"
    else
        for leg in "${SCENARIO_ROWS[@]}"; do regress_skip "$leg" "not requested (--no-scenarios)"; done
    fi
    guest_leg "isolation-audit" pa.ps1 300
else
    for leg in "${APP_LEGS[@]}" ipc-selftest "${SCENARIO_ROWS[@]}" isolation-audit; do
        regress_skip "$leg" "nothing installed"
    done
fi
if [[ "$INSTALL_MODE" == extract ]]; then
    regress_skip "package-uninstall" "nothing installed (--extract-only)"
else
    regress_skip "package-uninstall" "left installed: uninstalling needs another UAC prompt"
fi

echo
echo "report:      ${REPORT}"
echo "suite logs:  ${POST_DIR}/"
echo "screenshots: ${RUN_DIR}/*.png"
regress_summary "regress windows"
