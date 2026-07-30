# De-JUCE campaign — handoff prompts for remaining phases

One prompt per remaining phase, each written for a fresh context window
(Opus execution sessions per [dejuce-campaign.md](dejuce-campaign.md)
§Model/token policy; prompt 10 is a Fable spec session). Snapshot date:
2026-07-30. If phase statuses have moved since, trust the tower specs'
status lines and `git log` over this file.

## State at time of writing

- Merged on `main`: hosting H1a–c (PR #114), H3 (PR #118), H4 (PR #119),
  plus everything in the campaign doc's Done list.
- In flight: H5a on `dejuce/hosting-h5a` — H5a.0–H5a.2 complete, H5a.3
  next. The H5 spec `docs/dejuce-hosting-h5-platform.md` exists only on
  that branch until H5a merges.
- Gate allowlist: 179 files. JUCE modules linked: 12.
- The memory ledger `project_dejuce_roadmap.md` exists only on the Linux
  machine; sessions on other machines fall back to repo docs + git log.

## Dependency order

```
H5a (in flight) → H5b → H5c
Donor consolidation → H1d
                   → H2-donor → H2-app
H6 requires H5a+H5b+H5c and H2-app merged
GUI spike → GUI tower spec → GUI tower execution (finale)
```

One PR open at a time (campaign ritual rule 2). H1d is order-independent
of H5/H6 but must land before the GUI tower.

---

## Prompt 1 — H5a remainder (macOS CLAP/LV2/VST3 ports)

```
You are executing one phase of the Dusk Studio de-JUCE campaign in
/Users/marckorte/projects/DuskStudio (or /home/marc/projects/DuskStudio on the
Linux box). Read, in order: docs/dejuce-campaign.md (ritual + working
agreement), then check out the existing branch dejuce/hosting-h5a from origin
and read docs/dejuce-hosting-h5-platform.md ON THAT BRANCH (it is not on main
yet). If the memory ledger project_dejuce_roadmap.md exists in this machine's
memory dir, read it; if absent, trust the spec's status line and git log.

State: H5a.0 (build gates + CI dependencies), H5a.1 (portable discovery + CLAP
bundle loading), and H5a.2 (CLAP Cocoa editor) are complete on the branch;
H5a.3's host-context split already landed with the macOS build fix, so only its
Cocoa attach remains. Execute the first incomplete increment (H5a.3 VST3
portable host context + Cocoa editor core, then H5a.4 VST3 Cocoa wrapper +
cross-platform tests, H5a.5 LV2 Cocoa editor, H5a.6 closeout) — ONE increment
per session, max five files each, exactly as the spec's per-increment file
lists and work items dictate. Pause for review after each increment.

Rules: commit locally, NEVER push without Marc's word; no attribution trailers;
Linux native hosting behaviour is an invariant; keep platform code behind the
existing format abstractions; editor handles pointer-width-safe. Verify per the
spec's per-increment bar; at H5a.6 run the full Linux bar (CCACHE_DISABLE=1
builds, ctest, tools/juce-gate.sh ≤ 179, selftest only under private Xvfb with
WAYLAND_DISPLAY unset). On the Mac, a local Release configure+build with
DUSKSTUDIO_NATIVE_CLAP/LV2/VST3=ON is the compile check (DUSK_PLUGINS_PATH=
/Users/marckorte/projects/dusk-audio-plugins-pinned); macOS CI proof and Marc's
bench sign-off happen after Marc authorises the push. End your session by
updating the spec's status line and stating the resume phrase from the spec.
```

## Prompt 2 — H5b (native Audio Unit host, macOS)

```
You are executing one phase of the Dusk Studio de-JUCE campaign. Precondition:
the H5a PR (branch dejuce/hosting-h5a) is MERGED — verify with git log before
starting; if not merged, stop and report. Read, in order:
docs/dejuce-campaign.md, docs/dejuce-hosting-plan.md,
docs/dejuce-hosting-h5-platform.md §H5b (on main once H5a merges), and the
memory ledger project_dejuce_roadmap.md if present on this machine.

Create branch dejuce/hosting-h5b. Build the macOS-only native AU layer in
src/engine/au/ — AuBundle (stable type/subtype/manufacturer identifier),
AuHost, AuInstance (implements hosting::INativeInstance: init/uninit,
bus/stream-format negotiation, render, MIDI, parameter enumeration/writes,
state property-list round-trip, latency; NO allocation, lock, Objective-C
dispatch, or property query on the audio thread), AuEditor (Cocoa view factory
embedded via the H5a NSView-container pattern), AuScanner (native
PluginDescriptor rows, existing cache/sandbox rules), NativeAuSlot. Wire AU
rungs into channel/aux slots, picker, editor-open flows, MIDI bindings,
latency, shutdown, session save/restore, clone/offline plumbing. Session keys
additive; legacy JUCE-AU descriptors migrate one way. Narrow Catch2 tests per
the spec's H5b list. Work in bounded increments (≤5 files), one PR total.

Rules: commit locally, never push without Marc's word; no attribution
trailers; Linux behaviour untouched. Verify: Linux full bar before commit
(build, ctest, juce-gate ≤ current count, Xvfb selftest), macOS compile with
all native formats ON; real stock Apple AU units are Marc's bench debt —
record it in the PR body. Update spec status line; end with the spec's resume
phrase.
```

## Prompt 3 — H5c (Windows CLAP/VST3 ports)

```
You are executing one phase of the Dusk Studio de-JUCE campaign. Precondition:
H5b PR merged — verify with git log; if not, stop. Read, in order:
docs/dejuce-campaign.md, docs/dejuce-hosting-plan.md,
docs/dejuce-hosting-h5-platform.md §H5c, and the memory ledger if present.

Create branch dejuce/hosting-h5c. Enable native CLAP/VST3 on Windows: CMake
selects the VST3 SDK's module_win32.cpp + threadchecker_win32.cpp; CLAP
loading via LoadLibraryW/GetProcAddress/FreeLibrary with UTF-16 paths and
stable UTF-8 identity/error text; Windows scan defaults + environment-list
separator per CLAP/VST3 platform conventions; CLAP editor via
CLAP_WINDOW_API_WIN32, VST3 via kPlatformTypeHWND, both owning child HWND
containers with lifecycle/resize/visibility/teardown preserved; CLAP POSIX-fd
extension NOT advertised or compiled on Windows (timers + main-thread
callbacks remain); Windows LV2 stays OFF and absent from picker rows (locked
decision — do not vendor suil). windows-tests.yml explicitly enables CLAP/VST3
and runs native-host tests; windows-build.yml explicitly enables them and is
dispatched only after Marc's push word (vcpkg 504 → gh run rerun <id>
--failed).

Rules: commit locally, never push without Marc's word; no attribution
trailers; Linux and macOS behaviour untouched; handles pointer-width-safe
(LLP64: never carry unsigned long X11 handles into shared interfaces). Verify:
Linux full bar; Windows CI after authorised push; real-plugin matrix including
ASIO playback and editor DPI is Marc's bench debt — record in PR body. Update
spec status; end with resume phrase.
```

## Prompt 4 — Donor consolidation (plugins repo; unblocks H1d + H2)

```
You are executing the donor-consolidation step of the Dusk Studio de-JUCE
hosting tower (see docs/dejuce-hosting-plan.md §Standing risks and §H1d
version-prerequisite, plus docs/dejuce-hosting-h1-tape.md §H1d, in
/home/marc/projects/DuskStudio). Work happens primarily in the DONOR plugins
repo (Linux: /home/marc/projects/plugins with the multicomp-core worktree at
/home/marc/projects/plugins-multicomp-core; Mac mirror:
/Users/marckorte/projects/dusk-audio-plugins).

Goal: end the split-donor state so H1d and H2 can proceed from ONE donor rev.
Steps: (1) merge the plugins-multicomp-core branch into donor main (its core
scope is LOCKED and excludes Multiband — nothing new rides along); (2) bump
DONOR_REV in all 8 Dusk Studio workflows together, in one commit; (3) retire
the plugins-multicomp-core worktree and repoint DUSK_PLUGINS_PATH in both
build caches (build/, build-tests/) back to the consolidated donor main —
CLAUDE.md documents the single-checkout convention; (4) confirm Dusk Studio
builds + full ctest green against the consolidated donor on Linux, and that
tests/tape_core_ab.cpp and the console_saturation A/B still pass (pinned rev
69f0431 semantics must survive the merge — if the tape A/B breaks, stop and
report the param-surface delta rather than re-baselining silently).

Rules: campaign ritual applies in both repos — branch first, commit locally,
never push without Marc's word, no attribution trailers. Record in the ledger
(or the hosting plan's status line if the ledger is absent) that donor
consolidation is done and H1d + H2 are unblocked.
```

## Prompt 5 — H2-donor (Multiband compressor DPF port, plugins repo)

```
You are executing the donor half of hosting-tower phase H2 of the Dusk Studio
de-JUCE campaign. Precondition: donor consolidation done (one donor rev, no
multicomp-core worktree) — verify; if not, stop and run that step first. Read
docs/dejuce-campaign.md and docs/dejuce-hosting-plan.md §H2 in the DuskStudio
repo, then work in the donor plugins repo.

Goal: port UniversalCompressor mode 7 (Multiband) into a JUCE-free core —
extend the multi-comp/core scope or create a sibling MultibandCompressorDSP,
following the established donor DPF-core pattern (TapeMachine/core
TapeMachineDSP is the reference: plain C++ core, atomic setters, PORT_NOTES.md
documenting semantics). Marc's direction: "Multicomp will need to be ported to
DPF". The Dusk Studio consumer surface to design against: MasteringChain
bindCompParams and MasteringView's embedded donor MultibandCompressorPanel
writing mb_* APVTS params — enumerate those params first and give the core an
equivalent setter surface. A/B parity harness vs the JUCE UniversalCompressor
Multiband mode belongs in the donor repo's test convention (null or bounded
residual, documented tolerance).

After the core lands on donor main: bump DONOR_REV in all 8 Dusk Studio
workflows. Rules: branch first, commit locally, never push without Marc's
word, no attribution trailers. The app-side flip (MasteringChain/MasteringView)
is a SEPARATE session — do not start it here. Record status in the hosting
plan and ledger.
```

## Prompt 6 — H2-app (MasteringChain + MasteringView flip)

```
You are executing the app half of hosting-tower phase H2 of the Dusk Studio
de-JUCE campaign in the DuskStudio repo. Precondition: the donor Multiband
core is merged to donor main and DONOR_REV is bumped — verify; if not, stop.
Read docs/dejuce-campaign.md, docs/dejuce-hosting-plan.md §H2, the memory
ledger if present, and the donor core's PORT_NOTES.

Create branch dejuce/hosting-h2. Flip MasteringChain off
UniversalCompressor/APVTS onto the new JUCE-free Multiband core, following the
H1a house pattern (docs/dejuce-hosting-h1-tape.md): session-level atomic
params per Session.h convention #1, UI writes atoms, chain pushes all live
params to core setters per block (lock-free stores), one-way migration from
any persisted APVTS state, latency reported from the core. Replace
MasteringView's embedded donor MultibandCompressorPanel with a native panel in
the existing embedded-modal house style (MasteringLimiterEditor /
BusCompEditorPanel are the references). Remove the donor multicomp JUCE
sources from the app target in CMakeLists. Tests: A/B vs JUCE multiband
(tolerance documented), silence-in/silence-out, latency match — follow
tests/tape_core_ab.cpp's DUSK_PLUGINS_PATH gating.

Rules: audio-thread rules in CLAUDE.md are mandatory; commit locally, never
push without Marc's word; no attribution trailers. Verify: full Linux bar
(build zero new warnings, ctest, juce-gate — expect movement as donor JUCE
sources leave, record the number; Xvfb selftest), screenshot review of the new
panel, MANUAL.md audit, multiband null-listen recorded as Marc's bench debt.
Session compatibility: test the one-way migration with a real 0.12 session.
Update spec status; end with resume phrase.
```

## Prompt 7 — H1d (TapeMachine2 DPF UI embed)

```
You are executing hosting-tower phase H1d of the Dusk Studio de-JUCE campaign
on the LINUX machine (X11 embed work; /home/marc/projects/DuskStudio).
Precondition: donor consolidation done (UI and core MUST come from one donor
rev — the pinned 69f0431 predates TM2's current param surface) — verify; if
not, stop. Read docs/dejuce-campaign.md, docs/dejuce-hosting-h1-tape.md §H1d
(the executable spec for this phase), and the memory ledger — it holds the
2026-07-26 feasibility-spike recipe and param-delta table; if the ledger is
absent on this machine, say so and reconstruct the delta by diffing
TapeMachineParams.hpp between 69f0431 and donor main.

Create a branch (dejuce/hosting-h1d). Work: extend session TapeParams + core
setters to TM2's current param surface (head width, gain link, wow/flutter
enable, advanced page, preset bar); compile the TM2 DPF UI stack
(TapeMachineUI.cpp + DuskImGuiWidgets + DGL-OpenGL) into Dusk Studio; embed
its DGL window as an X11 child using the ClapPluginEditorComponent
reparent/embed pattern. Bridges: UI→engine via DPF setParameterValue mapped
through the TapeMachineParams enum onto session TapeParams atoms
(arm-on-touch preserved); engine→UI via a 30 Hz atom-diff sync calling
parameterChanged; meters via strong definitions of the DuskAccessBridge weak
symbols (tapeMachineGetVuL/R, InVuL/R) returning the in-process core's
followers, getPluginInstancePointer supplying the core. Then DELETE the
interim JUCE TapePanel, recapture the fx-03 manual figure, and note that TM2's
own preset system reverses the earlier "presets dropped" behaviour change.

Hard constraint (Marc, 2026-07-26): the param/meter/lifecycle bridges must be
windowing-independent — no Xlib types outside the one window-shim TU; this
embed is INTERIM and re-hosts onto the Wayland/EGL surface at the GUI tower.
Rules: commit locally, never push without Marc's word; no attribution
trailers. Verify: full Linux bar, tape A/B still green, Xvfb selftest,
screenshot review of the embedded UI, gate movement recorded (TapePanel files
leave the allowlist), MANUAL.md audit. TM2-embed usability pass is Marc's
bench debt. Update both hosting docs' status lines.
```

## Prompt 8 — H6 (the drop: delete the JUCE hosting path)

```
You are executing hosting-tower phase H6 — the drop — of the Dusk Studio
de-JUCE campaign. Preconditions (verify each in git log; stop if any missing):
H5a, H5b, H5c merged (native CLAP/LV2/VST3 on Linux+macOS, AU on macOS,
CLAP/VST3 on Windows) AND H2-app merged (no JUCE donor processors left in the
app). Read docs/dejuce-campaign.md, docs/dejuce-hosting-plan.md (§End-state is
the checklist), docs/dejuce-hosting-h5-platform.md, and the ledger if present.

Create branch dejuce/hosting-h6. Delete the JUCE hosting path everywhere at
once: PluginSlot, PluginManager's JUCE half (AudioPluginFormatManager /
KnownPluginList / createPluginInstance), JuceCompat.h,
PluginHostMain's JUCE message loop + MessageManagerLock sites (the OOP child
shrinks to the native scan sandbox — shm/futex layer is already
tri-platform), JUCE_PLUGINHOST_* defines, and every JUCE-host fallback rung in
the slot ladders (ladder becomes CLAP > LV2 > VST3 > MS [> AU on mac]).
Unlink juce_audio_processors from CMake on ALL THREE platforms (module count
12 → 11 — the campaign's first global unlink; juce_audio_utils and
juce_audio_formats stay for AudioThumbnail until the GUI tower). Retire
tools/tsan_suppressions.txt entries tied to deleted primitives in the SAME PR.
Update tools/juce-allowlist.txt — expect a large ratchet drop; record
before/after. Sweep per CLAUDE.md rule 10: grep for every deleted symbol
across src/, tests/, session serializer string literals, MIDI bindings,
CMakeLists, and the OOP scan wire format.

Session compatibility: sessions saved with JUCE-host descriptors must restore
through the H4 migration onto native rungs or surface a clear offline row —
test with a real 0.12 session. Rules: commit locally, never push without
Marc's word; no attribution trailers. Verify: full Linux bar, macOS +
Windows CI after authorised push, real-plugin restore matrix on Marc's bench
(record as debt). Update campaign doc: hosting tower DONE, module count 11,
GUI tower is next. End with the campaign resume state written down.
```

## Prompt 9 — GUI tower spike (gate before lock-in)

```
You are executing the GUI-tower entry gate of the Dusk Studio de-JUCE campaign
on the LINUX machine (Wayland work). Read docs/dejuce-campaign.md §Remaining
towers item 2 — it records Marc's 2026-07-27 framework decision: app UI =
Dear ImGui + DuskImGuiWidgets on EGL over a Dusk-written Wayland backend for
pugl in the hard-forked DPF stack (pugl ships mac/win/X11 only today; the
bespoke-toolkit plan is DROPPED). The gate: a spike proving the app shell as a
native Wayland window rendering ONE channel strip in ImGui at 60 Hz with
working input.

Scope: in the DPF fork, implement enough of a pugl Wayland backend to open an
xdg-shell toplevel with an EGL context, pump input (pointer + keyboard), and
run the ImGui frame loop; then render one Dusk channel strip (fader, pan, EQ
knobs, meters — DuskImGuiWidgets where they exist) bound to a stub or live
param struct. Measure and report: sustained frame rate, input latency feel,
CPU cost, and which pugl API surfaces the backend still lacks (clipboard,
cursors, IME, DnD, multi-window — enumerate, don't build). This is a SPIKE:
throwaway-quality code on a spike branch is acceptable, but the findings
report is the deliverable — it feeds the GUI tower spec. Do not touch Dusk
Studio's shipping UI. Rules: separate branch(es) in each repo touched, commit
locally, never push without Marc's word, no attribution trailers. End with a
GO/NO-GO recommendation on the ImGui+pugl-Wayland lock-in, and file the
findings in the ledger and a docs/ spike note.
```

## Prompt 10 — GUI tower spec (Fable session, after spike GO)

```
You are writing the executable spec for the FINAL tower of the Dusk Studio
de-JUCE campaign: the GUI tower. This is a Fable-tier planning session per
docs/dejuce-campaign.md §Model/token policy (spec-writing, not execution).
Preconditions: hosting tower complete (H6 merged, module count 11), GUI spike
returned GO with a findings report — read both, plus docs/dejuce-campaign.md
§Remaining towers item 2 (the locked framework decision and its owned
consequences) and the DuskStudio.md spec sections covering the UI.

Produce docs/dejuce-gui-plan.md in the house style of
docs/dejuce-hosting-h5-platform.md (status line, locked decisions, scout
ground truth, phased increments with ≤5-file lists, per-phase verification,
bench debts, resume phrase). The spec must own, at minimum: completing the
pugl Wayland backend (xdg-shell, EGL, input, clipboard, cursors, IME, DnD,
multi-window) in the DPF fork; the MessageThread.cpp event-loop rewrite (call
sites already converged by PR #112); per-view immediate-mode rewrites of ~120
files behind the existing DuskComboBox / DuskContextMenu / LookAndFeel /
EmbeddedModal seams; an ImGui theme replicating the current LookAndFeel
(screenshot-parity bar); a deliberate accessibility bridge (current JUCE a11y
handlers must not silently regress — inventory them first); AudioThumbnail
replacement (unlinks juce_audio_utils + juce_audio_formats); re-hosting the
H1d TM2 ImGui content onto the in-process Wayland/EGL surface; XWayland
retained ONLY for third-party plugin editors; macOS/Windows window backends
(pugl already ships those) so the tower stays tri-platform; and the module
unlink order for the remaining 11, juce_core last, with the String/File/
Colour/UndoableAction/Logger remnants (see the dissolved string-floor tower
note) dying with their anchor files. Multi-release: slice phases so the app
ships between them. Scout with Explore agents before writing; do not edit
code. Deliverable: the spec plus an updated campaign doc queue.
```

---

## Machine routing

- Prompts 1–2: runnable on the Mac for compile checks; closeout needs the
  Linux full bar. Mac plugin path:
  `/Users/marckorte/projects/dusk-audio-plugins-pinned`.
- Prompts 4–9: Linux machine (donor worktree, Xvfb selftest, X11 embed,
  Wayland spike).
- Prompt 3: Linux bar locally; Windows proof is CI + Marc's bench.
