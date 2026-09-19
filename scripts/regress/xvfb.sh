# shellcheck shell=bash
# Private-X-display plumbing shared by the Linux legs. Sourced, never run.
#
# The app aborts against the live Wayland session, so every GUI-linked leg runs
# on its own Xvfb display with WAYLAND_DISPLAY unset. xvfb-run is deliberately
# not used: the maintainer host ships Xvfb without it.
#
# One session at a time. xvfb_session_start exports XVFB_DISPLAY and XVFB_PID;
# xvfb_session_stop tears the session down and is safe to call again, or from an
# EXIT trap, after the session is already gone.

XVFB_DISPLAY=""
XVFB_PID=""
XVFB_DISPLAY_FILE=""
XVFB_LOG_FILE=""
XVFB_LIBGL_SAVED=0
XVFB_LIBGL_WAS_SET=""
XVFB_LIBGL_VALUE=""

# Starts Xvfb on a display it picks itself (-displayfd) and waits for it to
# report the display back, which it only does once the display accepts clients.
xvfb_session_start() {
    local display_number attempt
    XVFB_DISPLAY_FILE="$(mktemp "${TMPDIR:-/tmp}/duskstudio-regress-display.XXXXXX")"
    XVFB_LOG_FILE="$(mktemp "${TMPDIR:-/tmp}/duskstudio-regress-xvfb.XXXXXX")"
    Xvfb -displayfd 3 -screen 0 1920x1200x24 -nolisten tcp +extension GLX +render \
        3>"$XVFB_DISPLAY_FILE" 2>"$XVFB_LOG_FILE" &
    XVFB_PID=$!
    for ((attempt = 0; attempt < 100; ++attempt)); do
        [[ -s "$XVFB_DISPLAY_FILE" ]] && break
        if ! kill -0 "$XVFB_PID" 2>/dev/null; then
            echo "error: Xvfb failed to start:" >&2
            sed 's/^/  /' "$XVFB_LOG_FILE" >&2
            xvfb_session_stop
            return 1
        fi
        sleep 0.1
    done
    if ! read -r display_number <"$XVFB_DISPLAY_FILE" \
        || [[ ! "$display_number" =~ ^[0-9]+$ ]]; then
        echo "error: Xvfb did not report a ready display:" >&2
        sed 's/^/  /' "$XVFB_LOG_FILE" >&2
        xvfb_session_stop
        return 1
    fi
    XVFB_DISPLAY=":${display_number}"
    # Xvfb has no GPU, so GL has to resolve to llvmpipe instead of probing for a
    # hardware driver it cannot open.
    XVFB_LIBGL_WAS_SET="${LIBGL_ALWAYS_SOFTWARE+x}"
    XVFB_LIBGL_VALUE="${LIBGL_ALWAYS_SOFTWARE-}"
    XVFB_LIBGL_SAVED=1
    export LIBGL_ALWAYS_SOFTWARE=1
    return 0
}

xvfb_session_stop() {
    if [[ -n "$XVFB_PID" ]]; then
        kill "$XVFB_PID" 2>/dev/null || true
        wait "$XVFB_PID" 2>/dev/null || true
    fi
    if [[ -n "$XVFB_DISPLAY_FILE" ]]; then
        rm -f "$XVFB_DISPLAY_FILE"
    fi
    if [[ -n "$XVFB_LOG_FILE" ]]; then
        rm -f "$XVFB_LOG_FILE"
    fi
    XVFB_DISPLAY=""
    XVFB_PID=""
    XVFB_DISPLAY_FILE=""
    XVFB_LOG_FILE=""
    if ((XVFB_LIBGL_SAVED)); then
        if [[ -n "$XVFB_LIBGL_WAS_SET" ]]; then
            export LIBGL_ALWAYS_SOFTWARE="$XVFB_LIBGL_VALUE"
        else
            unset LIBGL_ALWAYS_SOFTWARE
        fi
    fi
    XVFB_LIBGL_SAVED=0
    XVFB_LIBGL_WAS_SET=""
    XVFB_LIBGL_VALUE=""
    return 0
}

# xvfb_run <timeout_s> <command...>
xvfb_run() {
    local timeout_s="$1"
    shift
    local rc=0
    xvfb_session_start || return 1
    env -u WAYLAND_DISPLAY DISPLAY="$XVFB_DISPLAY" \
        timeout --kill-after=10 "$timeout_s" "$@" || rc=$?
    xvfb_session_stop
    return "$rc"
}
