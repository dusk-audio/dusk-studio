# shellcheck shell=bash
# A private PipeWire graph with null devices for the scenario legs. Sourced,
# never run.
#
# The suite otherwise plays through the machine's own PipeWire graph and sound
# card, and a resync on that card can fail several audio cases in a row. With
# the private graph up, sandbox_env points the app's PipeWire client at it, so
# the hardware is out of the loop. Should it fail to start, the legs keep the
# machine's graph, which is how they ran before.
#
# DUSK_REGRESS_REAL_AUDIO=1 skips it and plays through the machine's graph.

NULL_AUDIO_DIR=""
NULL_AUDIO_PID=""
NULL_AUDIO_LOG=""

# Starts the daemon and waits for its socket. On success NULL_AUDIO_DIR is the
# runtime dir to hand the app as PIPEWIRE_RUNTIME_DIR; on failure it stays
# empty and the reason is printed.
null_audio_start() {
    local conf attempt
    conf="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/pipewire-null.conf"
    null_audio_stop
    if [[ "${DUSK_REGRESS_REAL_AUDIO:-0}" == 1 ]]; then
        echo "null audio: DUSK_REGRESS_REAL_AUDIO=1, the app plays through the machine's PipeWire graph"
        return 1
    fi
    if ! command -v pipewire >/dev/null 2>&1; then
        echo "null audio: no pipewire binary, the app plays through the machine's audio"
        return 1
    fi
    # The socket path has to fit in a sockaddr_un, so this stays short
    # whatever TMPDIR is.
    NULL_AUDIO_DIR="$(mktemp -d /tmp/dusk-pw.XXXXXX)" || { NULL_AUDIO_DIR=""; return 1; }
    NULL_AUDIO_LOG="${NULL_AUDIO_DIR}/pipewire.log"
    env -u XDG_RUNTIME_DIR -u PIPEWIRE_REMOTE PIPEWIRE_RUNTIME_DIR="$NULL_AUDIO_DIR" \
        pipewire -c "$conf" >"$NULL_AUDIO_LOG" 2>&1 &
    NULL_AUDIO_PID=$!
    for ((attempt = 0; attempt < 50; ++attempt)); do
        [[ -S "${NULL_AUDIO_DIR}/pipewire-0" ]] && break
        kill -0 "$NULL_AUDIO_PID" 2>/dev/null || break
        sleep 0.1
    done
    if [[ ! -S "${NULL_AUDIO_DIR}/pipewire-0" ]] || ! kill -0 "$NULL_AUDIO_PID" 2>/dev/null; then
        echo "null audio: the private PipeWire graph did not start, the app plays through the machine's graph:"
        sed 's/^/  /' "$NULL_AUDIO_LOG"
        null_audio_stop
        return 1
    fi
    echo "null audio: private PipeWire graph at ${NULL_AUDIO_DIR}"
    return 0
}

null_audio_stop() {
    if [[ -n "$NULL_AUDIO_PID" ]]; then
        kill "$NULL_AUDIO_PID" 2>/dev/null || true
        wait "$NULL_AUDIO_PID" 2>/dev/null || true
    fi
    if [[ -n "$NULL_AUDIO_DIR" ]]; then
        rm -rf "$NULL_AUDIO_DIR"
    fi
    NULL_AUDIO_DIR=""
    NULL_AUDIO_PID=""
    NULL_AUDIO_LOG=""
    return 0
}
