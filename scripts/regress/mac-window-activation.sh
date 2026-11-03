# shellcheck shell=bash
# The macOS half of tests/window_activation_smoke_test.md, run against the app
# installed in /Applications. Sourced by mac-package.sh, whose mac_run, vnc,
# remote library and run directories it uses; leg_window_activation records
# one row per case.
#
# Observations come from the window server, read over ssh by a probe built on
# the node from scripts/regress/mac/window-probe.m: the frontmost application,
# and every window front to back with its owner, layer, bounds and whether it
# is on screen. Reading the key window, or any window title, needs a permission
# the node does not grant, so "key and frontmost" is judged as: Dusk Studio is
# the frontmost application, its one main window is the topmost normal window,
# and a capture shows that window's title-bar buttons in colour, which AppKit
# draws only on the window that has the keyboard. The load alert and the save
# prompt are drawn inside the main window, where the window server cannot see
# them, so they are read from the capture, and the prompt is answered with a
# click where it should be: the session it holds back opens only if the click
# landed on it.
#
# Everything is driven the way a user does it: keys, typing and clicks over
# Screen Sharing, and opens from outside through LaunchServices (open -a), the
# route Open With and a drop on the Dock icon take, which reaches the running
# instance whether or not the app declares a document type for session.json.
# Calculator, which opens no document, is the other application put in front.

SCREEN_PY="${REPO_ROOT}/scripts/regress/mac/screen.py"
WA_ROWS=(window-activation:launch window-activation:session-open window-activation:minimized window-activation:handoff)

# The modals centre on the content area, below a title bar of this height.
WA_TITLE_BAR="${DUSK_REGRESS_MAC_TITLE_BAR:-28}"

read -r -d '' WA_LIB <<'LIB' || true
WA="$HOME/$RUN/wa"
probe() { "$WA/probe"; }
loads() {
    local n
    n="$(grep -c 'Dusk Studio/Load\]' "$WA/app/stderr.log" 2>/dev/null)"
    echo "${n:-0}"
}
snapshot() {
    probe
    echo "pids $(pgrep -f "$BIN" | tr '\n' ' ')"
    echo "loads $(loads)"
    echo "recent $(head -1 "$WA/app/home/.config/Dusk Studio/recent.txt" 2>/dev/null)"
}
# poll <seconds> <command...>
poll() {
    local end=$(($(date +%s) + $1))
    shift
    until "$@"; do
        (($(date +%s) >= end)) && return 1
        sleep 0.25
    done
}
# Not grep -q: it closes the pipe at the first match, and under pipefail the
# probe's SIGPIPE would fail a check that matched.
front_is() { probe | grep "^front [0-9]* $1\$" >/dev/null; }
loads_reach() { (($(loads) >= $1)); }
main_shown() { probe | awk -v p="$1" '$1 == "win" && $3 == p && $4 == 0 && $5 == 1 && $9 >= 200 && $10 >= 200 { f = 1 } END { exit !f }'; }
window_hidden() { probe | awk -v id="$1" '$1 == "win" && $11 == id && $5 == 0 { f = 1 } END { exit !f }'; }
app_gone() { ! pgrep -f "$BIN" >/dev/null; }
calculator_front() {
    open -a Calculator || return 1
    poll 15 front_is com.apple.calculator || { echo "error: Calculator did not come to the front" >&2; return 1; }
    pgrep -x Calculator >>"$HOME/$RUN/other-pids"
}
LIB

wa_run() {
    local deadline="$1"
    shift
    { printf '%s\n' "$WA_LIB"; cat; } | mac_run "$deadline" "$@"
}

WA_DIR="${RUN_DIR}/window-activation"
WA_PID=""

wa_vnc() {
    vnc "$@" >/dev/null 2>>"${WA_DIR}/vnc.err"
}

wa_screen() {
    "$VNC_PYTHON" -I "$SCREEN_PY" "$@"
}

# wa_snapshot <name>: what the window server and the app's own files show,
# kept as <name>.txt and parsed into the WA_* fields below.
wa_snapshot() {
    local f="${WA_DIR}/$1.txt"
    WA_FRONT="" WA_FRONT_PID="" WA_PIDS="" WA_LOADS="" WA_RECENT=""
    WA_TOP_ID="" WA_TOP_PID="" WA_TOP_OWNER="" WA_TOP_X="" WA_TOP_Y="" WA_TOP_W="" WA_TOP_H=""
    WA_MAIN_ID="" WA_X="" WA_Y="" WA_W="" WA_H="" WA_MAINS=0
    wa_run 60 >"$f" 2>&1 <<'REMOTE' || return 1
snapshot
REMOTE
    WA_FRONT="$(sed -n 's/^front //p' "$f")"
    WA_FRONT_PID="${WA_FRONT%% *}"
    WA_PIDS="$(sed -n 's/^pids //p' "$f" | xargs)"
    WA_LOADS="$(sed -n 's/^loads //p' "$f")"
    WA_RECENT="$(sed -n 's/^recent //p' "$f")"
    # The topmost normal window: layer 0, on screen, visible, not a sliver.
    read -r WA_TOP_ID WA_TOP_PID WA_TOP_OWNER WA_TOP_X WA_TOP_Y WA_TOP_W WA_TOP_H < <(awk '
        $1 == "win" && $4 == 0 && $5 == 1 && $6 != "0.00" && $9 >= 50 && $10 >= 50 { print $11, $3, $12, $7, $8, $9, $10; exit }' "$f") || true
    # The app's main windows: its on-screen normal windows of a usable size.
    # The thin drop-shadow windows around the main one are not among them.
    WA_MAINS="$(awk -v p="$WA_PID" '$1 == "win" && $3 == p && $4 == 0 && $5 == 1 && $9 >= 200 && $10 >= 200' "$f" | wc -l)"
    read -r WA_MAIN_ID WA_X WA_Y WA_W WA_H < <(awk -v p="$WA_PID" '
        $1 == "win" && $3 == p && $4 == 0 && $5 == 1 && $9 >= 200 && $10 >= 200 { print $11, $7, $8, $9, $10; exit }' "$f") || true
}

# Why the last snapshot does not show Dusk Studio frontmost, with its one main
# window on top and one process; empty when it does.
wa_front_problems() {
    local p=""
    [[ "$WA_FRONT_PID" == "$WA_PID" ]] || p+="the frontmost app is ${WA_FRONT:-unknown}; "
    [[ "$WA_MAINS" == 1 ]] || p+="${WA_MAINS} Dusk Studio main windows on screen; "
    if [[ -z "$WA_MAIN_ID" || "$WA_TOP_ID" != "$WA_MAIN_ID" ]]; then
        p+="the topmost window is ${WA_TOP_ID:-none} of ${WA_TOP_OWNER:-?} (pid ${WA_TOP_PID:-?}), not the main window ${WA_MAIN_ID:-?}; "
    fi
    [[ "$WA_PIDS" == "$WA_PID" ]] || p+="Dusk Studio processes: ${WA_PIDS:-none}; "
    printf '%s' "${p%; }"
}

# wa_capture <name>: a capture as <name>.png, and WA_LIGHTS, what the main
# window's title-bar buttons show in it ("colour: ..." or "grey: ...").
wa_capture() {
    WA_SHOT="${WA_DIR}/$1.png"
    WA_LIGHTS="no capture"
    wa_vnc cap "$WA_SHOT" || return 1
    if [[ -n "$WA_MAIN_ID" ]]; then
        WA_LIGHTS="$(wa_screen lights "$WA_SHOT" "$WA_X" "$WA_Y" "$WA_W" "$WA_H")" || true
    else
        WA_LIGHTS="no main window on screen"
    fi
}

wa_centre() {
    WA_CX=$((WA_X + WA_W / 2))
    WA_CY=$((WA_Y + WA_TITLE_BAR + (WA_H - WA_TITLE_BAR) / 2))
}

# A click on the middle of the title bar makes the app active for a setup
# step without touching anything in the window.
wa_focus() {
    wa_vnc click "$((WA_X + WA_W / 2))" "$((WA_Y + WA_TITLE_BAR / 2))"
}

# wa_verdict <row> <start> <problems> <pass note>
wa_verdict() {
    if [[ -n "$3" ]]; then
        regress_record "$1" FAIL "$((SECONDS - $2))" "$3; evidence in ${WA_DIR}"
    else
        regress_record "$1" PASS "$((SECONDS - $2))" "$4"
    fi
}

wa_in_front_note() {
    printf 'pid %s frontmost, main window %s topmost and the only one, title-bar buttons in colour' "$WA_PID" "$WA_MAIN_ID"
}

# ---------------------------------------------------------------- cases

# Case 1: with Calculator in front, launch the app the way Finder does, which
# is LaunchServices opening the bundle; the session comes through the
# environment so the window that comes up is the main window, not the picker.
wa_launch() {
    local start=$SECONDS problems="" out
    printf '\n--- window-activation:launch ---\n'
    if ! wa_run 60 <<'REMOTE'; then
calculator_front
REMOTE
        wa_verdict window-activation:launch "$start" "setup: Calculator did not come to the front" ""
        return 1
    fi
    out="$(wa_run 120 <<'REMOTE'
pid="$(open_app "$WA/app" DUSKSTUDIO_LOAD_SESSION="$WA/sessions/session-a/session.json")" || exit 1
# The pid goes out first, so teardown can stop an app whose launch then fails.
echo "regress:pid $pid"
poll 60 loads_reach 1 || { echo "error: no load line from session-a" >&2; exit 1; }
poll 30 main_shown "$pid" || { echo "error: no main window on screen" >&2; exit 1; }
echo "regress:up"
REMOTE
)" || true
    printf '%s\n' "$out"
    WA_PID="$(sed -n 's/^regress:pid //p' <<<"$out")"
    if [[ -z "$WA_PID" ]] || ! grep -qx 'regress:up' <<<"$out"; then
        wa_verdict window-activation:launch "$start" "the app did not come up with session-a loaded" ""
        return 1
    fi
    # The window is brought forward on the tick after it is shown, and its title
    # bar repaints after that.
    wa_run 30 <<'REMOTE' || true
poll 10 front_is audio.dusk.studio
REMOTE
    sleep 1
    wa_snapshot launch || problems="no snapshot"
    wa_capture launch || true
    problems="${problems:-$(wa_front_problems)}"
    [[ "$WA_LIGHTS" == colour:* ]] || problems+="${problems:+; }title-bar buttons ${WA_LIGHTS}"
    [[ "$WA_RECENT" == "$WA_SESSIONS/session-a" ]] || problems+="${problems:+; }the open session is '${WA_RECENT}', not session-a"
    grep -E '^front|^pids|^loads|^recent' "${WA_DIR}/launch.txt" | sed 's/^/    /'
    echo "    lights: ${WA_LIGHTS}"
    wa_verdict window-activation:launch "$start" "$problems" "$(wa_in_front_note)"
}

# Case 2: File > Open (Cmd+O) in the app, the second session typed into the
# browser's name box, then Calculator brought in front over part of the main
# window before the browser's Open button is clicked. Session B's mastering mix
# is missing, so the load raises its "Missing audio files" alert.
wa_session_open() {
    local start=$SECONDS problems="" loads px py ox oy check
    printf '\n--- window-activation:session-open ---\n'
    wa_snapshot open-before || true
    if [[ -z "$WA_MAIN_ID" ]]; then
        wa_verdict window-activation:session-open "$start" "setup: no main window on screen to open from" ""
        return 0
    fi
    loads="$WA_LOADS"
    wa_centre
    px=$((WA_CX - 410))
    py=$((WA_CY - 280))
    ox=$((px + 648))
    oy=$((py + 528))
    wa_focus && sleep 1 && wa_vnc keys super-o && sleep 1.5 && wa_vnc cap "${WA_DIR}/open-browser.png" || true
    if ! check="$(wa_screen panel "${WA_DIR}/open-browser.png" "$WA_CX" "$WA_CY" 820 556 560)"; then
        wa_verdict window-activation:session-open "$start" "setup: Cmd+O showed no file browser where expected (${check})" ""
        return 0
    fi
    echo "    browser: ${check}"
    wa_vnc click "$((px + 372))" "$((py + 485))" && wa_vnc keys super-a && wa_vnc type ../session-b/session.json \
        && wa_vnc keys enter || true
    sleep 1.5
    if ! wa_run 60 <<'REMOTE'; then
calculator_front
REMOTE
        wa_verdict window-activation:session-open "$start" "setup: Calculator did not come to the front" ""
        return 0
    fi
    sleep 1
    wa_snapshot open-covered || true
    wa_capture open-covered || true
    echo "    covered: front ${WA_FRONT}; top ${WA_TOP_OWNER} ${WA_TOP_X},${WA_TOP_Y} ${WA_TOP_W}x${WA_TOP_H}; lights ${WA_LIGHTS}"
    if [[ "$WA_FRONT" != *" com.apple.calculator" || "$WA_TOP_OWNER" != Calculator ]]; then
        wa_verdict window-activation:session-open "$start" "setup: Calculator is not in front (front ${WA_FRONT}, top ${WA_TOP_OWNER})" ""
        return 0
    fi
    if ! ((WA_TOP_X < WA_X + WA_W && WA_X < WA_TOP_X + WA_TOP_W && WA_TOP_Y < WA_Y + WA_H && WA_Y < WA_TOP_Y + WA_TOP_H)); then
        wa_verdict window-activation:session-open "$start" "setup: Calculator's window does not cover any of the main window" ""
        return 0
    fi
    if ((ox >= WA_TOP_X && ox < WA_TOP_X + WA_TOP_W && oy >= WA_TOP_Y && oy < WA_TOP_Y + WA_TOP_H)); then
        wa_verdict window-activation:session-open "$start" "setup: Calculator's window covers the browser's Open button; move it and run again" ""
        return 0
    fi
    # The control for the key-window evidence: grey here, with Calculator active.
    if [[ "$WA_LIGHTS" != grey:* ]]; then
        problems="the title-bar check read '${WA_LIGHTS}' with Calculator active, so it cannot tell"
    fi
    wa_vnc click "$ox" "$oy" || true
    wa_run 60 N="$((loads + 1))" <<'REMOTE' || true
poll 15 loads_reach "$N"
REMOTE
    # The alert goes up on the tick after the load, and the window comes forward
    # with it.
    wa_run 30 <<'REMOTE' || true
poll 10 front_is audio.dusk.studio
REMOTE
    sleep 1
    wa_snapshot session-open || true
    wa_capture session-open || true
    local front
    front="$(wa_front_problems)"
    [[ -z "$front" ]] || problems+="${problems:+; }${front}"
    [[ "$WA_LIGHTS" == colour:* ]] || problems+="${problems:+; }title-bar buttons ${WA_LIGHTS}"
    if ((WA_LOADS != loads + 1)) || [[ "$WA_RECENT" != "$WA_SESSIONS/session-b" ]]; then
        problems+="${problems:+; }the Open click did not open session-b (loads ${loads} -> ${WA_LOADS}, open session '${WA_RECENT}')"
    fi
    if [[ -n "$WA_MAIN_ID" ]]; then
        wa_centre
        if ! check="$(wa_screen panel "$WA_SHOT" "$WA_CX" "$WA_CY" 460 220 620)"; then
            problems+="${problems:+; }no load alert on top of the main window (${check})"
        else
            echo "    alert: ${check}"
            wa_vnc keys esc || true
            sleep 1
            wa_vnc cap "${WA_DIR}/session-open-dismissed.png" || true
            # The control for the panel check: gone once the alert is dismissed.
            if check="$(wa_screen panel "${WA_DIR}/session-open-dismissed.png" "$WA_CX" "$WA_CY" 460 220 620)"; then
                problems+="${problems:+; }the alert check still finds a panel after Esc dismissed it (${check}), so it cannot tell"
            fi
        fi
    fi
    echo "    lights: ${WA_LIGHTS}"
    wa_verdict window-activation:session-open "$start" "$problems" \
        "Open clicked with Calculator over part of the window: session-b loaded, $(wa_in_front_note), load alert on top"
}

# Case 3: minimized from its own title bar, then session C opened from outside
# while Calculator is in front.
wa_minimized() {
    local start=$SECONDS problems="" loads id yellow
    printf '\n--- window-activation:minimized ---\n'
    wa_snapshot min-before || true
    if [[ -z "$WA_MAIN_ID" ]]; then
        wa_verdict window-activation:minimized "$start" "setup: no main window on screen to minimize" ""
        return 0
    fi
    id="$WA_MAIN_ID"
    loads="$WA_LOADS"
    wa_focus || true
    sleep 1
    wa_capture min-before || true
    yellow="$(sed -n 's/.*yellow=\([0-9]*,[0-9]*\).*/\1/p' <<<"$WA_LIGHTS")"
    if [[ -z "$yellow" ]]; then
        wa_verdict window-activation:minimized "$start" "setup: no minimize button found (${WA_LIGHTS})" ""
        return 0
    fi
    wa_vnc click "${yellow%,*}" "${yellow#*,}" || true
    if ! wa_run 60 ID="$id" <<'REMOTE'; then
poll 10 window_hidden "$ID"
REMOTE
        wa_verdict window-activation:minimized "$start" "setup: the main window ${id} is still on screen after its minimize button" ""
        return 0
    fi
    wa_snapshot minimized || true
    wa_capture minimized || true
    echo "    minimized: window ${id} off screen; front ${WA_FRONT}"
    if ! wa_run 60 <<'REMOTE'; then
calculator_front
REMOTE
        wa_verdict window-activation:minimized "$start" "setup: Calculator did not come to the front" ""
        return 0
    fi
    wa_run 60 N="$((loads + 1))" PID="$WA_PID" <<'REMOTE' || true
open -a "$APP" "$WA/sessions/session-c/session.json"
poll 20 loads_reach "$N"
poll 10 main_shown "$PID"
REMOTE
    sleep 2
    wa_snapshot minimized-open || true
    wa_capture minimized-open || true
    problems="$(wa_front_problems)"
    [[ "$WA_LIGHTS" == colour:* ]] || problems+="${problems:+; }title-bar buttons ${WA_LIGHTS}"
    [[ "$WA_MAIN_ID" == "$id" ]] || problems+="${problems:+; }the window on screen is ${WA_MAIN_ID:-none}, not the minimized ${id}"
    if ((WA_LOADS != loads + 1)) || [[ "$WA_RECENT" != "$WA_SESSIONS/session-c" ]]; then
        problems+="${problems:+; }session-c did not open (loads ${loads} -> ${WA_LOADS}, open session '${WA_RECENT}')"
    fi
    echo "    lights: ${WA_LIGHTS}"
    wa_verdict window-activation:minimized "$start" "$problems" \
        "window ${id} came back from the Dock with session-c: $(wa_in_front_note)"
}

# Case 4: an unsaved edit (M drops a marker), Calculator in front, then
# session D opened from outside. The open has to wait on the save prompt, and
# Don't Save, clicked where the prompt should be, is what lets it through.
wa_handoff() {
    local start=$SECONDS problems="" loads check dx dy
    printf '\n--- window-activation:handoff ---\n'
    wa_snapshot handoff-before || true
    if [[ -z "$WA_MAIN_ID" ]]; then
        wa_verdict window-activation:handoff "$start" "setup: no main window on screen to edit in" ""
        return 0
    fi
    loads="$WA_LOADS"
    wa_focus && sleep 1 && wa_vnc keys m || true
    sleep 1
    if ! wa_run 60 <<'REMOTE'; then
calculator_front
REMOTE
        wa_verdict window-activation:handoff "$start" "setup: Calculator did not come to the front" ""
        return 0
    fi
    wa_run 60 <<'REMOTE' || true
open -a "$APP" "$WA/sessions/session-d/session.json"
poll 15 front_is audio.dusk.studio
REMOTE
    sleep 2
    wa_snapshot handoff || true
    wa_capture handoff || true
    problems="$(wa_front_problems)"
    [[ "$WA_LIGHTS" == colour:* ]] || problems+="${problems:+; }title-bar buttons ${WA_LIGHTS}"
    if ((WA_LOADS != loads)); then
        problems+="${problems:+; }session-d opened without a save prompt (loads ${loads} -> ${WA_LOADS}): the marker edit did not land, or the app did not ask"
    elif [[ -n "$WA_MAIN_ID" ]]; then
        wa_centre
        if ! check="$(wa_screen panel "$WA_SHOT" "$WA_CX" "$WA_CY" 440 196 200)"; then
            problems+="${problems:+; }no save prompt on top of the main window (${check})"
        else
            echo "    prompt: ${check}"
        fi
        dx=$((WA_CX + 23))
        dy=$((WA_CY + 58))
        wa_vnc click "$dx" "$dy" || true
        wa_run 60 N="$((loads + 1))" <<'REMOTE' || true
poll 15 loads_reach "$N"
REMOTE
        sleep 1
        local before_main="$WA_MAIN_ID"
        wa_snapshot handoff-after || true
        wa_capture handoff-after || true
        if ((WA_LOADS != loads + 1)) || [[ "$WA_RECENT" != "$WA_SESSIONS/session-d" ]]; then
            problems+="${problems:+; }Don't Save at ${dx},${dy} did not let session-d open (loads ${loads} -> ${WA_LOADS}, open session '${WA_RECENT}')"
        fi
        [[ "$WA_PIDS" == "$WA_PID" ]] || problems+="${problems:+; }Dusk Studio processes after the open: ${WA_PIDS:-none}"
        [[ "$WA_MAINS" == 1 && "$WA_MAIN_ID" == "$before_main" ]] \
            || problems+="${problems:+; }after the open: ${WA_MAINS} main windows, ${WA_MAIN_ID:-none} on screen"
    fi
    echo "    lights: ${WA_LIGHTS}"
    wa_verdict window-activation:handoff "$start" "$problems" \
        "save prompt on top while session-d waited, Don't Save opened it; $(wa_in_front_note); no second process or window"
}

# Quits the app through Cmd+Q, so it lets go of the single-instance slot, and
# Calculator; leaves no slot behind that the leg created.
wa_teardown() {
    printf '\n--- window-activation:teardown ---\n'
    if [[ -n "$WA_PID" ]]; then
        wa_snapshot teardown || true
        if [[ -n "$WA_MAIN_ID" ]]; then
            wa_focus && sleep 1 && wa_vnc keys super-q || true
        fi
        wa_run 60 PID="$WA_PID" <<'REMOTE' || true
if poll 20 app_gone; then
    echo "Dusk Studio quit"
else
    echo "warning: Dusk Studio is still running; killing pid $PID" >&2
    kill "$PID" 2>/dev/null
    sleep 3
    kill -9 "$PID" 2>/dev/null
fi
REMOTE
    fi
    wa_run 60 <<'REMOTE' || true
for pid in $(sort -u "$HOME/$RUN/other-pids" 2>/dev/null); do
    [[ "$(ps -p "$pid" -o comm= 2>/dev/null)" == *Calculator ]] && kill "$pid" 2>/dev/null && echo "Calculator quit"
done
slot="$(getconf DARWIN_USER_TEMP_DIR)dusk-studio/instance.sock"
if [[ -f "$WA/slot-was-free" ]] && app_gone && [[ -e "$slot" ]]; then
    rm -f "$slot" && echo "removed the single-instance socket the leg left"
fi
sleep 1
probe | grep '^front'
REMOTE
}

wa_setup() {
    mkdir -p "$WA_DIR"
    scp -q "${SSH_OPTS[@]}" "${REPO_ROOT}/scripts/regress/mac/window-probe.m" "${MAC_HOST}:${REMOTE_RUN}/window-probe.m" || return 1
    wa_run 300 <<'REMOTE'
version="$(/usr/libexec/PlistBuddy -c 'Print CFBundleShortVersionString' "$APP/Contents/Info.plist" 2>/dev/null)"
[[ -x "$BIN" && -n "$version" ]] || { echo "error: no app at $APP" >&2; exit 1; }
echo "installed $APP $version"
if pgrep -f "$BIN" >/dev/null; then
    echo "error: $BIN is already running" >&2
    exit 1
fi
if pgrep -x Calculator >/dev/null; then
    echo "error: Calculator is running; the leg brings its own to the front and quits it, so quit it first" >&2
    exit 1
fi
mkdir -p "$WA/sessions" || exit 1
touch "$HOME/$RUN/other-pids"
for s in a c d; do
    mkdir -p "$WA/sessions/session-$s"
    printf '{\n    "version": 6\n}\n' >"$WA/sessions/session-$s/session.json"
done
mkdir -p "$WA/sessions/session-b"
printf '{\n    "version": 6,\n    "mastering": { "source_file": "missing-mix.wav" }\n}\n' >"$WA/sessions/session-b/session.json"
clang -fobjc-arc -framework AppKit -framework CoreGraphics "$HOME/$RUN/window-probe.m" -o "$WA/probe" || exit 1
[[ -e "$(getconf DARWIN_USER_TEMP_DIR)dusk-studio/instance.sock" ]] || touch "$WA/slot-was-free"
echo "regress:sessions $WA/sessions"
probe | grep '^front'
REMOTE
}

leg_window_activation() {
    local start=$SECONDS out rc=0 row
    printf '\n--- window-activation:setup ---\n'
    if screen_locked; then
        leg_screen >/dev/null 2>&1 || true
    fi
    out="$(wa_setup 2>&1)" || rc=$?
    printf '%s\n' "$out"
    WA_SESSIONS="$(sed -n 's/^regress:sessions //p' <<<"$out")"
    if ((rc != 0)) || [[ -z "$WA_SESSIONS" ]]; then
        regress_record window-activation:setup FAIL "$((SECONDS - start))" "$(grep -m1 '^error:' <<<"$out" || echo "exit ${rc}")"
        for row in "${WA_ROWS[@]}"; do regress_skip "$row" "setup failed"; done
        return 0
    fi
    regress_record window-activation:setup PASS "$((SECONDS - start))" "$(grep -m1 '^installed ' <<<"$out")"
    if wa_launch; then
        wa_session_open
        wa_minimized
        wa_handoff
    else
        for row in "${WA_ROWS[@]:1}"; do regress_skip "$row" "the app did not come up"; done
    fi
    wa_teardown
    wa_vnc cap "${WA_DIR}/after.png" || true
    echo "window-activation evidence: ${WA_DIR}"
}
