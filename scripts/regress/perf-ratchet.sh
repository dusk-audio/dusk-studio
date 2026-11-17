# shellcheck shell=bash
# perf-suite: the engine perf suite (DUSKSTUDIO_RUN_PERF_TEST=1) held to
# tools/perf-ratchet.tsv. Sourced by linux.sh for `scripts/regress.sh linux
# --perf`, and runnable on its own:
#
#   bash scripts/regress/perf-ratchet.sh [--app <binary>] [--runs <n>]
#        [--update] [--save <file>] [--ratchet <file>]
#
# The suite runs DUSK_PERF_RUNS times (default 3, --runs overrides) on a
# private Xvfb display and, when it starts, the private null PipeWire graph.
# Each run writes its rows as JSON (DUSKSTUDIO_PERF_JSON); a binary too old to
# do that is read from its text table instead. A row's value is the lowest of
# its medians over the runs, in microseconds: load on the machine only ever
# adds time.
#
# A row fails when it is more than 10% and more than 5 us over its recorded
# value, and a row the ratchet does not list fails too. A run that fails only
# on slow rows gets up to DUSK_PERF_CONFIRM more runs (default 2) before the
# leg fails: one disturbed run is noise, a regression stays slow. Wall times are only
# comparable on the machine that recorded them, so on any other host (the
# ratchet's #host line) the suite runs once to show it still completes and the
# leg is a SKIP; with --save it runs the full count and writes its values first,
# which is how a ratchet for another machine starts. CI does not run it: hosted
# runners are too noisy.
#
# --update lowers recorded values to this run and drops rows the suite no
# longer has. It never raises a value or adds a row; that is a hand edit, and
# the change has to say why. --save writes this run's values in the ratchet's
# format to <file>, for a comparison or a hand edit.

PERF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="${REPO_ROOT:-$(cd "${PERF_DIR}/../.." && pwd)}"
PERF_RATCHET="${DUSK_PERF_RATCHET:-${REPO_ROOT}/tools/perf-ratchet.tsv}"
PERF_RUNS="${DUSK_PERF_RUNS:-3}"
PERF_UPDATE=0
PERF_SAVE=""

# The CPU and thread count wall times belong to.
perf_ratchet_host() {
    local model
    model="$(sed -n 's/^model name[[:space:]]*:[[:space:]]*//p' /proc/cpuinfo 2>/dev/null | head -1)"
    printf '%s, %s threads' "${model:-unknown CPU}" "$(getconf _NPROCESSORS_ONLN)"
}

# perf_ratchet_recorded_host <ratchet>: the host its values were measured on.
perf_ratchet_recorded_host() {
    sed -n 's/^#host\t//p' "$1" | head -1
}

# perf_ratchet_judge <ratchet> <host> <check|update> <save file or ""> <run dir> <runs>
# Reads run<i>.json, or run<i>.txt when the JSON is missing, for i in 1..runs.
# Returns 0 within the ratchet, 4 when the only failures are slow rows, 1 for
# an unlisted row, 2 for a ratchet or a run that does not parse.
perf_ratchet_judge() {
    python3 -I - "$@" <<'PY'
import json
import math
import re
import sys
from pathlib import Path

ratchet_path, host, mode, save_path, run_dir, runs = sys.argv[1:7]
runs = int(runs)
TOLERANCE_RATIO = 0.10
TOLERANCE_US = 5.0


def norm(text):
    return " ".join(text.split())


def key(section, label):
    return f"{norm(section)} | {norm(label)}"


def rows_from_json(path):
    data = json.loads(path.read_text(encoding="utf-8"))
    return {key(row["section"], row["label"]): float(row["median_us"]) for row in data["rows"]}


def rows_from_text(path):
    section_line = re.compile(r"^--- (.*) ---$")
    row_line = re.compile(r"^\[(?:OK|OVER)\] (.*?)\s+median=([0-9]+(?:\.[0-9]+)?) us")
    rows, section = {}, ""
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        match = section_line.match(line)
        if match:
            section = match.group(1)
            continue
        match = row_line.match(line)
        if match:
            rows[key(section, match.group(1))] = float(match.group(2))
    return rows


medians = {}
for run in range(1, runs + 1):
    json_path = Path(run_dir) / f"run{run}.json"
    text_path = Path(run_dir) / f"run{run}.txt"
    try:
        rows = rows_from_json(json_path) if json_path.exists() else rows_from_text(text_path)
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"error: run {run} gave no readable rows: {error}")
        sys.exit(2)
    if not rows:
        print(f"error: run {run} printed no perf rows")
        sys.exit(2)
    for name, median in rows.items():
        medians.setdefault(name, []).append(median)

measured = {name: min(values) for name, values in medians.items()}
spread = {name: 100.0 * (max(values) - min(values)) / min(values) if min(values) > 0 else 0.0
          for name, values in medians.items()}


def text(value):
    return f"{math.ceil(value * 10) / 10:.1f}"


if save_path:
    with open(save_path, "w", encoding="utf-8") as out:
        out.write(f"#host\t{host}\n")
        out.write(f"#runs\t{runs}\n")
        for name in medians:
            out.write(f"{name}\t{text(measured[name])}\n")
    if mode == "save":
        print(f"saved {len(medians)} rows from {runs} run{'s' if runs != 1 else ''} to {save_path}")
        sys.exit(0)

record = re.compile(r"^([^\t#][^\t]*)\t((?:0|[1-9][0-9]*)(?:\.[0-9]+)?)$")
lines = Path(ratchet_path).read_text(encoding="utf-8").splitlines()
ratchet = {}
for number, line in enumerate(lines, 1):
    if not line.strip() or line.startswith("#"):
        continue
    match = record.match(line)
    if not match:
        print(f"error: {ratchet_path}:{number}: not a row<TAB>microseconds record: {line!r}")
        sys.exit(2)
    if match.group(1) in ratchet:
        print(f"error: {ratchet_path}:{number}: {match.group(1)!r} is listed twice")
        sys.exit(2)
    ratchet[match.group(1)] = (float(match.group(2)), number - 1)

print(f"engine perf suite, {runs} run{'s' if runs != 1 else ''}, lowest median per row, on {host}")
print(f"{'RECORDED':>9} {'MEASURED':>9} {'DELTA':>8} {'DELTA%':>8} {'SPREAD%':>8}  {'RESULT':<8} ROW")
failed = slow = False
hints, lowered = [], {}
worst = None
for name in medians:
    value = measured[name]
    if name not in ratchet:
        failed = True
        print(f"{'-':>9} {value:>9.1f} {'':>8} {'':>8} {spread[name]:>7.1f}%  {'UNLISTED':<8} {name}")
        hints.append(f"error: {name!r} is not in {ratchet_path}; add '{name}\\t{text(value)}' by hand")
        continue
    recorded = ratchet[name][0]
    delta = value - recorded
    percent = 100.0 * delta / recorded if recorded > 0 else 0.0
    slower = value > recorded * (1.0 + TOLERANCE_RATIO) and delta > TOLERANCE_US
    if slower:
        slow = True
    elif float(text(value)) < recorded:
        lowered[name] = text(value)
    if worst is None or percent > worst[0]:
        worst = (percent, name)
    print(f"{recorded:>9.1f} {value:>9.1f} {delta:>+8.1f} {percent:>+7.1f}% {spread[name]:>7.1f}%  "
          f"{'SLOWER' if slower else 'ok':<8} {name}")

gone = [name for name in ratchet if name not in medians]
for name in gone:
    print(f"note: {name!r} is recorded but the suite did not run it; --update drops it")
if worst is not None:
    print(f"slowest against the ratchet: {worst[0]:+.1f}% ({worst[1]})")
for hint in hints:
    print(hint)

if mode == "update":
    drop = {ratchet[name][1] for name in gone}
    edited = []
    for index, line in enumerate(lines):
        if index in drop:
            continue
        name = line.split("\t", 1)[0]
        if name in lowered and ratchet[name][1] == index:
            print(f"update: {name}: {line.split(chr(9), 1)[1]} -> {lowered[name]}")
            line = f"{name}\t{lowered[name]}"
        edited.append(line)
    for name in gone:
        print(f"update: dropped {name!r}")
    if edited != lines:
        Path(ratchet_path).write_text("\n".join(edited) + "\n", encoding="utf-8")
    else:
        print("update: nothing to lower")

sys.exit(1 if failed else 4 if slow else 0)
PY
}

# perf_ratchet_suite <dir> <run>: one run of the suite, its table in
# <dir>/run<run>.txt and its rows in <dir>/run<run>.json.
perf_ratchet_suite() {
    local dir="$1" run="$2" rc=0
    sandboxed xvfb_run "${SELFTEST_TIMEOUT:-${DUSK_REGRESS_SELFTEST_TIMEOUT:-180}}" \
        env DUSKSTUDIO_RUN_PERF_TEST=1 "DUSKSTUDIO_PERF_JSON=${dir}/run${run}.json" "$APP_BIN" \
        >"${dir}/run${run}.txt" 2>"${dir}/run${run}.err" || rc=$?
    if ((rc != 0)); then
        echo "error: perf run ${run} exited with status ${rc}; the end of its output:"
        tail -n 20 "${dir}/run${run}.err" "${dir}/run${run}.txt" | sed 's/^/  /'
    fi
    return "$rc"
}

leg_perf() {
    if [[ "$(uname -s)" != Linux ]]; then
        REGRESS_SKIP_REASON="wall times are judged on the Linux dev box only"
        return "$REGRESS_SKIP_RC"
    fi
    [[ -f "$PERF_RATCHET" ]] || { echo "error: no ratchet at ${PERF_RATCHET}"; return 2; }
    [[ "$PERF_RUNS" =~ ^[1-9][0-9]*$ ]] || { echo "error: runs must be a positive count, not '${PERF_RUNS}'"; return 2; }

    local dir host recorded runs="$PERF_RUNS" run start own_audio=0 rc=0 mode=check
    local confirm="${DUSK_PERF_CONFIRM:-2}"
    [[ "$confirm" =~ ^[0-9]+$ ]] || { echo "error: DUSK_PERF_CONFIRM must be a count, not '${confirm}'"; return 2; }
    host="$(perf_ratchet_host)"
    recorded="$(perf_ratchet_recorded_host "$PERF_RATCHET")"
    if [[ "$host" != "$recorded" ]]; then
        echo "    ratchet recorded on: ${recorded:-no #host line}"
        echo "    this host:           ${host}"
        [[ -n "$PERF_SAVE" ]] || runs=1
    fi
    dir="$(mktemp -d "${TMPDIR:-/tmp}/dusk-perf.XXXXXX")" || return 1
    regress_at_exit "rm -rf $(printf '%q' "$dir")"
    if [[ -z "$NULL_AUDIO_DIR" ]] && null_audio_start >"${dir}/null-audio.txt" 2>&1; then
        own_audio=1
        regress_at_exit null_audio_stop
    fi

    echo "    load average before the runs: $(cut -d' ' -f1-3 /proc/loadavg)"
    for ((run = 1; run <= runs; ++run)); do
        start=$SECONDS
        perf_ratchet_suite "$dir" "$run" || { rc=1; break; }
        echo "    run ${run} of ${runs}: $((SECONDS - start)) s"
    done
    if ((own_audio)); then null_audio_stop; fi

    if ((rc == 0)); then
        if [[ "$host" != "$recorded" ]]; then
            if [[ -n "$PERF_SAVE" ]]; then
                perf_ratchet_judge "$PERF_RATCHET" "$host" save "$PERF_SAVE" "$dir" "$runs" || rc=$?
            fi
            rm -rf "$dir"
            ((rc == 0)) || return "$rc"
            REGRESS_SKIP_REASON="the suite completes; its times are not judged off the host the ratchet was recorded on"
            return "$REGRESS_SKIP_RC"
        fi
        if ((PERF_UPDATE)); then mode=update; fi
        while :; do
            rc=0
            perf_ratchet_judge "$PERF_RATCHET" "$host" "$mode" "$PERF_SAVE" "$dir" "$runs" || rc=$?
            ((rc == 4 && confirm > 0)) || break
            confirm=$((confirm - 1))
            runs=$((runs + 1))
            echo "    slow rows: run ${runs} to confirm them, load average $(cut -d' ' -f1-3 /proc/loadavg)"
            perf_ratchet_suite "$dir" "$runs" || { rc=1; break; }
        done
        if ((rc == 4)); then rc=1; fi
    fi
    rm -rf "$dir"
    return "$rc"
}

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
    set -euo pipefail

    # shellcheck source=scripts/regress/common.sh
    source "${PERF_DIR}/common.sh"
    # shellcheck source=scripts/regress/xvfb.sh
    source "${PERF_DIR}/xvfb.sh"
    # shellcheck source=scripts/regress/scenarios.sh
    source "${PERF_DIR}/scenarios.sh"

    regress_require Xvfb timeout python3 getconf
    perf_app_bin=""
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --app)
                [[ $# -ge 2 ]] || regress_die "--app needs a path"
                perf_app_bin="$2"
                shift 2
                ;;
            --runs)
                [[ $# -ge 2 ]] || regress_die "--runs needs a count"
                PERF_RUNS="$2"
                shift 2
                ;;
            --update)
                PERF_UPDATE=1
                shift
                ;;
            --save)
                [[ $# -ge 2 ]] || regress_die "--save needs a path"
                PERF_SAVE="$2"
                shift 2
                ;;
            --ratchet)
                [[ $# -ge 2 ]] || regress_die "--ratchet needs a path"
                PERF_RATCHET="$2"
                shift 2
                ;;
            *) regress_die "unknown option '$1'" ;;
        esac
    done
    if [[ -z "$perf_app_bin" ]]; then
        [[ -f "${REPO_ROOT}/build/CMakeCache.txt" ]] \
            || regress_die "build/ is not configured; pass --app <binary>"
        perf_build_type="$(sed -n 's/^CMAKE_BUILD_TYPE:[^=]*=//p' \
            "${REPO_ROOT}/build/CMakeCache.txt" | head -1)"
        perf_app_bin="${REPO_ROOT}/build/DuskStudio_artefacts/${perf_build_type:-Release}/DuskStudio"
    fi
    [[ -x "$perf_app_bin" ]] || regress_die "app binary missing: ${perf_app_bin}"
    APP_BIN="$perf_app_bin"

    trap regress_run_exit_hooks EXIT
    regress_leg "perf-suite" leg_perf
    regress_summary "regress perf-suite"
fi
