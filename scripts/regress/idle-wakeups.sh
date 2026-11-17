# shellcheck shell=bash
# bb-idle-wakeups: how many times a second each thread of an idle Dusk Studio
# wakes up, held to tools/idle-wakeup-budget.tsv. Linux only. Sourced by
# linux.sh, and runnable on its own:
#
#   bash scripts/regress/idle-wakeups.sh [--app <binary>] [--update] [--tsv <file>]
#
# The app opens the minimal session on a private Xvfb display and the private
# null PipeWire graph (null-audio.sh), with a sandboxed profile (sandbox_env in
# scenarios.sh), and is left alone with the transport stopped. Once the device
# is open, the session loaded and the startup scan declined, the app has
# settled when it starts no thread for DUSK_IDLE_SETTLE seconds (default 3): a
# thread born inside the window would bring its startup into the count. Then
# each thread's voluntary context switches are read from /proc/<pid>/task twice,
# DUSK_IDLE_WINDOW seconds apart (default 10). A thread switches out
# voluntarily when it blocks, and wakes once for each time it does, so the
# difference is its wake-up count. Counts are not times: a loaded machine
# delays the wake-ups but does not add any.
#
# Threads are grouped by name, a trailing number read as an index ("DuskDSP 3"
# counts under "DuskDSP *"), and each group is summed and checked against the
# budget. A name the budget does not list fails, as does one over its ceiling.
# A ceiling is wake-ups per second, or "<n>/block" for a thread the audio clock
# drives: n wake-ups per audio block at the rate and quantum the device opened
# with. The record "(non-device total)", a name no thread can have (Linux
# names stop at 15 characters), holds the sum of every thread whose ceiling is
# per second, so everything the audio clock does not drive. A thread that ends
# inside the window is judged by its name too: an unlisted one fails. --update lowers ceilings to this run plus headroom and
# drops names that did not run, and leaves the budget alone (status 3) when a
# thread ended inside the window. It never raises a ceiling or adds a name;
# that is a hand edit.

IDLE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="${REPO_ROOT:-$(cd "${IDLE_DIR}/../.." && pwd)}"
IDLE_BUDGET="${DUSK_IDLE_BUDGET:-${REPO_ROOT}/tools/idle-wakeup-budget.tsv}"
IDLE_UPDATE=0
IDLE_TSV=""

# idle_wakeups_snapshot <pid> <file>: one "tid<TAB>voluntary switches<TAB>name"
# line per thread, the message thread named [main] so the threads that keep the
# program's name (JUCE's timer thread, Mesa's) count apart from it. A thread
# that exits while it is read is left out.
idle_wakeups_snapshot() {
    local pid="$1" out="$2" task comm line vol
    : >"$out" || return 1
    for task in /proc/"$pid"/task/*; do
        { read -r comm <"$task/comm"; } 2>/dev/null || continue
        vol=""
        while IFS= read -r line; do
            if [[ "$line" == voluntary_ctxt_switches:* ]]; then
                vol="${line##*[[:space:]]}"
            fi
        done 2>/dev/null <"$task/status" || continue
        [[ "$vol" =~ ^[0-9]+$ ]] || continue
        [[ "${task##*/}" != "$pid" ]] || comm="[main]"
        printf '%s\t%s\t%s\n' "${task##*/}" "$vol" "$comm" >>"$out"
    done
}

# idle_wakeups_settle <pid> <quiet secs> <budget secs>: waits until the thread
# list has not changed for <quiet secs>.
idle_wakeups_settle() {
    local pid="$1" quiet="$2" deadline=$((SECONDS + $2 + $3)) list last="" since=$SECONDS
    while ((SECONDS < deadline)); do
        list="$(ls /proc/"$pid"/task 2>/dev/null)" || return 1
        if [[ "$list" != "$last" ]]; then
            last="$list"
            since=$SECONDS
        elif ((SECONDS - since >= quiet)); then
            return 0
        fi
        sleep 0.5
    done
    return 1
}

# The process's user plus system time so far, in clock ticks.
idle_wakeups_cpu_ticks() {
    local stat fields
    stat="$(<"/proc/$1/stat")" || return 1
    read -r -a fields <<<"${stat##*) }"
    printf '%s' "$((fields[11] + fields[12]))"
}

# Seconds since boot: a monotonic clock, so a wall-clock step inside the window
# cannot scale the rates.
idle_wakeups_now() {
    local uptime
    read -r uptime _ </proc/uptime || return 1
    printf '%s' "$uptime"
}

# idle_wakeups_judge <budget> <first> <second> <seconds> <rate> <quantum>
#                    <cpu ticks> <ticks per second> <check|update> [tsv]
# Prints the table and checks it against the budget, rewriting the budget in
# update mode. Returns 0 within budget, 1 over it, 2 for a budget that does not
# parse, 3 when update mode will not rewrite from this sample.
idle_wakeups_judge() {
    python3 -I - "$@" <<'PY'
import math
import re
import sys
from pathlib import Path

(budget_path, first_path, second_path, seconds, rate, quantum,
 cpu_ticks, ticks_per_second, mode) = sys.argv[1:10]
tsv_path = sys.argv[10] if len(sys.argv) > 10 else ""
seconds, rate, quantum = float(seconds), int(rate), int(quantum)
cpu_seconds = int(cpu_ticks) / int(ticks_per_second)
if seconds <= 0 or rate <= 0 or quantum <= 0:
    print(f"error: no usable window ({seconds} s) or device clock (rate {rate}, quantum {quantum})")
    sys.exit(2)
blocks = rate / quantum

record = re.compile(r"^([^\t#\s][^\t]*)\t((?:0|[1-9][0-9]*)(?:\.[0-9]+)?)(/block)?$")
lines = Path(budget_path).read_text(encoding="utf-8").splitlines()
budget = {}
for number, line in enumerate(lines, 1):
    if not line.strip() or line.startswith("#"):
        continue
    match = record.match(line)
    if not match:
        print(f"error: {budget_path}:{number}: not a thread-name<TAB>ceiling record: {line!r}")
        sys.exit(2)
    name = match.group(1)
    if name in budget:
        print(f"error: {budget_path}:{number}: {name!r} is listed twice")
        sys.exit(2)
    budget[name] = (float(match.group(2)), bool(match.group(3)), number - 1)
TOTAL = "(non-device total)"
if TOTAL in budget and budget[TOTAL][1]:
    print(f"error: {budget_path}: {TOTAL!r} is wake-ups per second, not per block")
    sys.exit(2)


def snapshot(path):
    threads = {}
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        tid, switches, name = line.split("\t", 2)
        threads[tid] = (int(switches), name)
    return threads


first, second = snapshot(first_path), snapshot(second_path)
groups = {}
for tid, (switches, name) in second.items():
    woke = switches - first[tid][0] if tid in first else switches
    group = groups.setdefault(re.sub(r"^(.*\D)\d+$", r"\1*", name), [0, 0])
    group[0] += 1
    group[1] += woke
born = sorted(second[tid][1] for tid in second if tid not in first)
gone = sorted(first[tid][1] for tid in first if tid not in second)
# A thread that ended inside the window has no count, but its name still ran:
# it must be budgeted, and it keeps a budgeted name from reading as idle.
ended = {}
for tid in first:
    if tid not in second:
        name = re.sub(r"^(.*\D)\d+$", r"\1*", first[tid][1])
        ended[name] = ended.get(name, 0) + 1


def text(value):
    return f"{value:.2f}".rstrip("0").rstrip(".")


# What --update would record: this run plus a quarter and one wake-up a second,
# or a tenth plus a little for a per-block ceiling.
def proposal(per_second, per_block):
    if per_block:
        return math.ceil((per_second / blocks * 1.1 + 0.05) * 20) / 20
    return float(math.ceil(per_second * 1.25) + 1)


print(f"idle wake-ups over {seconds:.1f} s at {rate} Hz, quantum {quantum} "
      f"({text(blocks)} blocks/s)")
print(f"{'THREAD':<17} {'N':>3} {'WAKEUPS/S':>10}  {'CEILING':<20} RESULT")
failed = False
hints, rows, lowered = [], [], {}
total_threads = total_wakeups = 0
for name, (count, woke) in sorted(groups.items(), key=lambda item: (-item[1][1], item[0])):
    per_second = woke / seconds
    total_threads += count
    total_wakeups += woke
    if name not in budget:
        failed = True
        verdict, ceiling_text = "UNLISTED", "-"
        hints.append(f"error: {name!r} is not in {budget_path}; if the thread belongs in an idle app, "
                     f"add '{name}\\t{text(proposal(per_second, False))}' by hand")
    else:
        ceiling, per_block, _ = budget[name]
        limit = ceiling * blocks if per_block else ceiling
        ceiling_text = f"{text(ceiling)}/block ({text(limit)}/s)" if per_block else text(ceiling)
        suggested = proposal(per_second, per_block)
        if per_second > limit:
            failed = True
            verdict = "OVER"
        else:
            verdict = "ok"
            if suggested < ceiling:
                lowered[name] = suggested
            if suggested <= ceiling * 0.75:
                unit = "/block" if per_block else ""
                hints.append(f"hint: {name!r} could come down from {text(ceiling)}{unit} to "
                             f"{text(suggested)}{unit} (idle-wakeups.sh --update)")
    print(f"{name:<17} {count:>3} {per_second:>10.1f}  {ceiling_text:<20} {verdict}")
    rows.append((name, count, woke, per_second, per_second / blocks, ceiling_text, verdict))
for name in sorted(set(ended) - set(groups)):
    if name in budget:
        continue
    failed = True
    print(f"{name:<17} {ended[name]:>3} {'ended':>10}  {'-':<20} UNLISTED")
    hints.append(f"error: {name!r} is not in {budget_path}; its thread ended inside the window, "
                 f"so the run cannot say what it costs")

print(f"{'total':<17} {total_threads:>3} {total_wakeups / seconds:>10.1f}")
# Everything the audio clock does not drive: unlisted names and every name with
# a per-second ceiling.
non_device = sum(woke for name, (count, woke) in groups.items()
                 if not (name in budget and budget[name][1])) / seconds
if TOTAL in budget:
    ceiling = budget[TOTAL][0]
    suggested = proposal(non_device, False)
    if non_device > ceiling:
        failed = True
        verdict = "OVER"
    else:
        verdict = "ok"
        if suggested < ceiling:
            lowered[TOTAL] = suggested
        if suggested <= ceiling * 0.75:
            hints.append(f"hint: {TOTAL!r} could come down from {text(ceiling)} to "
                         f"{text(suggested)} (idle-wakeups.sh --update)")
    print(f"{'non-device':<17} {'':>3} {non_device:>10.1f}  {text(ceiling):<20} {verdict}")
print(f"cpu time {cpu_seconds:.2f} s over {seconds:.1f} s ({100 * cpu_seconds / seconds:.1f}% of one core)")
if born:
    print(f"note: started inside the window, counted from their start: {', '.join(born)}")
if gone:
    print(f"note: ended inside the window, not counted: {', '.join(gone)}")
idle = sorted(name for name in budget if name not in groups and name not in ended and name != TOTAL)
for name in idle:
    print(f"note: {name!r} is budgeted but did not run; --update drops it")
for hint in hints:
    print(hint)

if tsv_path:
    with open(tsv_path, "w", encoding="utf-8") as out:
        out.write(f"#rate\t{rate}\n#quantum\t{quantum}\n#seconds\t{seconds:.3f}\n"
                  f"#cpu_seconds\t{cpu_seconds:.3f}\n#total_per_second\t{total_wakeups / seconds:.3f}\n"
                  f"#non_device_per_second\t{non_device:.3f}\n")
        out.write("thread\tthreads\twakeups\tper_second\tper_block\tceiling\tresult\n")
        for name, count, woke, per_second, per_block, ceiling_text, verdict in rows:
            out.write(f"{name}\t{count}\t{woke}\t{per_second:.3f}\t{per_block:.4f}\t{ceiling_text}\t{verdict}\n")

refused = mode == "update" and bool(gone)
if refused:
    print("update: not rewriting the budget: a thread ended inside the window, so this sample "
          "cannot say what its name costs or whether a budgeted name still runs; repeat the run")
elif mode == "update":
    drop = {budget[name][2] for name in idle}
    edited = []
    for index, line in enumerate(lines):
        if index in drop:
            continue
        name = line.split("\t", 1)[0]
        if name in lowered and budget[name][2] == index:
            ceiling, per_block, _ = budget[name]
            new = f"{name}\t{text(lowered[name])}{'/block' if per_block else ''}"
            print(f"update: {line.replace(chr(9), ' ')} -> {text(lowered[name])}{'/block' if per_block else ''}")
            line = new
        edited.append(line)
    for name in idle:
        print(f"update: dropped {name!r}")
    if edited != lines:
        Path(budget_path).write_text("\n".join(edited) + "\n", encoding="utf-8")
    else:
        print("update: nothing to lower")

sys.exit(1 if failed else 3 if refused else 0)
PY
}

idle_wakeups_body() {
    local session pid line rate quantum settle window
    local first="$BB_SDIR/first.tsv" second="$BB_SDIR/second.tsv"
    settle="${DUSK_IDLE_SETTLE:-3}"
    window="${DUSK_IDLE_WINDOW:-10}"
    session="$(bb_session idle)" || return 1

    bb_spawn A "DUSKSTUDIO_LOAD_SESSION=$session" -- || return 1
    bb_wait_marker A "[Dusk Studio/PipeWire] opened " 60 || return 1
    bb_wait_marker A "[Dusk Studio/Load] session.json" 60 || return 1
    bb_wait_marker A "[Dusk Studio] startup plugin scan: toggle=off - skipped" 30 || return 1
    line="$(grep -F "[Dusk Studio/PipeWire] opened " "$BB_SDIR/A.err" | tail -1)"
    rate="$(sed -n 's/.* rate=\([0-9][0-9]*\) quantum=\([0-9][0-9]*\) .*/\1/p' <<<"$line")"
    quantum="$(sed -n 's/.* rate=\([0-9][0-9]*\) quantum=\([0-9][0-9]*\) .*/\2/p' <<<"$line")"
    if [[ -z "$rate" || -z "$quantum" ]]; then
        bb_fail "no rate and quantum in the device line: $line"
        return 1
    fi
    printf '    %s\n' "${line#*\] }"
    if [[ -n "${DUSKSTUDIO_AUDIO_WORKERS:-}" ]]; then
        printf '    DUSKSTUDIO_AUDIO_WORKERS=%s\n' "$DUSKSTUDIO_AUDIO_WORKERS"
    fi

    pid="${BB_PID[A]}"
    if ! idle_wakeups_settle "$pid" "$settle" "$(bb_budget 30)"; then
        bb_fail "the app kept starting threads for 30 s after it loaded"
        return 1
    fi
    local cpu0 cpu1 start end ticks
    ticks="$(getconf CLK_TCK)"
    start="$(idle_wakeups_now)"
    cpu0="$(idle_wakeups_cpu_ticks "$pid")" || { bb_fail "the app is gone before the window"; return 1; }
    idle_wakeups_snapshot "$pid" "$first" || return 1
    sleep "$window"
    end="$(idle_wakeups_now)"
    cpu1="$(idle_wakeups_cpu_ticks "$pid")" || { bb_fail "the app exited inside the window"; return 1; }
    idle_wakeups_snapshot "$pid" "$second" || return 1

    local verdict=0 mode=check seconds
    if ((IDLE_UPDATE)); then mode=update; fi
    seconds="$(awk -v a="$start" -v b="$end" 'BEGIN { printf "%.3f", b - a }')"
    idle_wakeups_judge "$IDLE_BUDGET" "$first" "$second" "$seconds" "$rate" "$quantum" \
        "$((cpu1 - cpu0))" "$ticks" "$mode" ${IDLE_TSV:+"$IDLE_TSV"} || verdict=$?

    # SIGTERM takes the quit path a desktop logout does; an unchanged session
    # has nothing to prompt for.
    kill -TERM "$pid" 2>/dev/null || true
    local rc=0
    bb_wait_exit A 60 || rc=$?
    if ((rc != 0)); then
        bb_fail "the app returned $rc on SIGTERM instead of quitting cleanly"
        return 1
    fi
    bb_assert_marker A "phase 7b: posting systemRequestedQuit" || return 1
    return "$verdict"
}

leg_bb_idle_wakeups() {
    if [[ "$(uname -s)" != Linux ]]; then
        REGRESS_SKIP_REASON="reads /proc, so Linux only"
        return "$REGRESS_SKIP_RC"
    fi
    local own_audio=0 own_display=0 rc=0
    if [[ -z "$NULL_AUDIO_DIR" ]]; then
        if ! null_audio_start; then
            REGRESS_SKIP_REASON="needs the private null PipeWire graph, whose quantum the budget assumes (pipewire, DUSK_REGRESS_REAL_AUDIO unset)"
            return "$REGRESS_SKIP_RC"
        fi
        own_audio=1
    fi
    trap 'bb_end 0 >/dev/null 2>&1 || true; xvfb_session_stop || true; null_audio_stop || true; regress_run_exit_hooks' EXIT
    if [[ -z "$XVFB_PID" ]]; then
        xvfb_session_start || rc=1
        own_display=1
    fi
    if ((rc == 0)); then
        bb_begin bb-idle-wakeups 120 || rc=1
    fi
    if ((rc == 0)); then
        idle_wakeups_body || rc=$?
        bb_end "$rc" || rc=$?
    fi
    if ((own_display)); then xvfb_session_stop; fi
    if ((own_audio)); then null_audio_stop; fi
    trap regress_run_exit_hooks EXIT
    return "$rc"
}

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
    set -euo pipefail

    # shellcheck source=scripts/regress/common.sh
    source "${IDLE_DIR}/common.sh"
    # shellcheck source=scripts/regress/xvfb.sh
    source "${IDLE_DIR}/xvfb.sh"
    # shellcheck source=scripts/regress/scenarios.sh
    source "${IDLE_DIR}/scenarios.sh"

    regress_require Xvfb timeout pgrep python3 getconf
    idle_app_bin=""
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --app)
                [[ $# -ge 2 ]] || regress_die "--app needs a path"
                idle_app_bin="$2"
                shift 2
                ;;
            --update)
                IDLE_UPDATE=1
                shift
                ;;
            --tsv)
                [[ $# -ge 2 ]] || regress_die "--tsv needs a path"
                IDLE_TSV="$2"
                shift 2
                ;;
            *) regress_die "unknown option '$1'" ;;
        esac
    done
    if [[ -z "$idle_app_bin" ]]; then
        [[ -f "${REPO_ROOT}/build/CMakeCache.txt" ]] \
            || regress_die "build/ is not configured; pass --app <binary>"
        idle_build_type="$(sed -n 's/^CMAKE_BUILD_TYPE:[^=]*=//p' \
            "${REPO_ROOT}/build/CMakeCache.txt" | head -1)"
        idle_app_bin="${REPO_ROOT}/build/DuskStudio_artefacts/${idle_build_type:-Release}/DuskStudio"
    fi
    [[ -x "$idle_app_bin" ]] || regress_die "app binary missing: ${idle_app_bin}"
    APP_BIN="$idle_app_bin"

    trap regress_run_exit_hooks EXIT
    regress_leg "bb-idle-wakeups" leg_bb_idle_wakeups
    regress_summary "regress idle-wakeups"
fi
