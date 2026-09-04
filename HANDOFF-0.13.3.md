# Handoff: work milestone 0.13.3 to tag-ready

You are working in `/home/marc/projects/DuskStudio`. Never work on `main`: branch from a fresh `origin/main` and name the branch `issue-NNN-short-slug`, one per issue or group below. Read `AGENTS.md` in full before touching anything. It is binding: the De-JUCE gate, the audio-thread rules, the test policy, the `-j6` build limit, the Git rules. This file adds the milestone-specific context and the work order.

## Where things stand

GitHub milestone `0.13.3` (milestone #6) was audited on 2026-09-02 against main `09fc1d8`. Everything previously in the milestone is closed and merged. The audit found the release is not tag-ready and filed #440 to #472 into the milestone. #406 was found already fixed on main and was added to the milestone so it gets closed with the release.

Verified good, do not re-litigate:

- main is a strict superset of v0.13.2 (tag lives on `release/0.13`). Every 0.13.2 fix is present on main.
- CI is green on all five jobs at `09fc1d8`.
- Local pipeline on this box: app builds clean, 888/888 ctest, JUCE gate passes, Xvfb self-test 37 pass / 0 fail / 1 skip.
- Donor bump `69f04318` to `0a1b17f8` is one TapeMachine commit and is audio bit-identical.

Local build inputs on this box:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DDUSK_PLUGINS_PATH=/home/marc/projects/dusk-donor-pin
cmake --build build -j6
cmake -S . -B build-tests -DCMAKE_BUILD_TYPE=Release -DDUSKSTUDIO_BUILD_TESTS=ON -DDUSK_PLUGINS_PATH=/home/marc/projects/dusk-donor-pin
cmake --build build-tests --target dusk-studio-tests -j6
ctest --test-dir build-tests --output-on-failure
bash tools/juce-gate.sh
env -u WAYLAND_DISPLAY DUSKSTUDIO_SELFTEST_TIMEOUT=180s scripts/run-selftest-xvfb.sh
```

`../dusk-donor-pin` is a detached worktree of `../plugins` at `0a1b17f8` (the CI `DONOR_REV`). If it is missing: `git -C ../plugins worktree prune && git -C ../plugins worktree add --detach ../dusk-donor-pin 0a1b17f8e9dbecd26bf78dd45704c6c149e4b2ea`. DAF and DAF-Widgets are auto-discovered from `../DAF` and `../DAF-Widgets`. Never use bare `-j`. Never launch the DuskStudio binary outside the Xvfb script; it takes down the live Wayland session.

## Ground rules for this milestone

1. One issue (or one tightly related group, as listed below) per branch. Branch from fresh `origin/main`. Name branches `issue-NNN-short-slug`.
2. Before writing a fix, re-verify the issue's claim against the current source. Each issue body cites `file:line` from `09fc1d8`; lines drift. Some findings were confirmed by reading the code, some are reviewer reports that were not independently confirmed. The "verification status" column below tells you which. If a claim does not hold, say so in the report and do not fake a fix.
3. For engine, DSP or session changes: write the failing test first, then the fix. For everything with a code path a Catch2 test can reach, add a test. Follow the narrow-link pattern in `tests/CMakeLists.txt`.
4. Definition of done for a branch: app build with zero new warnings, test build, ctest 100%, `tools/juce-gate.sh` passes, and for audio-path changes the Xvfb self-test passes. Do not claim done without running them.
5. Do an AI-slop sweep before every commit: no change-narration comments, no restated-code comments, no dead code, no defensive checks that guard nothing. Commits read as hand-authored. Comments only where the why is non-obvious. Never write the literal `juce::` token in a comment.
6. Git: small, reviewable commits. No `Co-Authored-By` or any attribution trailer, ever. Do not push. When a branch is done, stop and report "branch ready: `<name>` at `<worktree path>`" with the summary. Marc runs CodeRabbit locally and pushes. One PR at a time.
7. Every user-visible change gets a `CHANGELOG.md` entry under the release section and a `MANUAL.md` check. Write both in plain sentences, no em dashes.
8. Windows and macOS code cannot be compiled here. For those files: keep the change minimal, keep tests platform-agnostic, and flag "needs CI on push" in the report. Do not stub or `#if` your way around a platform you cannot build.
9. The JUCE gate is a ratchet. Prefer the `src/foundation/` seams for anything new. If a count must go up, say so in the report with the reason; never edit `tools/juce-allowlist.txt` to make a build pass.

## Work order

Work top to bottom. Groups are meant to be one branch each.

### Phase 1: defects reachable on the default configuration

| Branch | Issues | Verification status | Notes |
|---|---|---|---|
| `issue-454-unreadable-state` | #454 | confirmed | Data loss. `decodeBase64Blob` in `src/engine/AudioEngine.cpp` must return "supplied but unreadable" as a distinct outcome; route it through `hosting::enforceRestorePolicy` as a rejection; keep the blob on the session. Test: corrupt blob survives a save round-trip. |
| `issue-460-stop-panic` | #460 | confirmed (muted-track skip, VST3 CC mapping); CLAP multi-port reviewer-reported | Three routes. Exempt `isMidi` from the gate skip in `ChannelStrip.cpp` the way the silent-skip already does. Synthesise note-offs in `Vst3Instance.cpp` when CC 120/123 is unmapped. One choke per CLAP note-input port. Tests for each route. |
| `issue-461-pdc-quarantine` | #461 | confirmed | Gate each native latency read in `recomputePdc` on `isProcessingOnline()` as well as bypass, and recompute PDC on quarantine and on the later successful reactivate. Test: quarantined insert contributes zero latency. |
| `issue-462-linux-clone-native` | #462 | confirmed | `src/session/RegionEditActions.cpp` native capture and replay are inside `#if DUSKSTUDIO_HAS_NATIVE_AU`. Guard per format. Gate the multisample commit on `msStateRestored`. Add a Linux clone round-trip test with the CLAP fixture CI already builds. |
| `issue-463-smf-parser` | #463 | confirmed | `src/engine/midi/MidiFileReader.cpp`: loop until `numTracks` MTrk chunks, correct data-byte counts for 0xF1 to 0xFE, advance instead of returning in `reorderGroup`. Add three fixture files under `tests/`. |
| `issue-465-midi-out-scratch` | #465 | confirmed (sizes and drop-all); sort cost reviewer-reported | Reserve `midiOutTrackScratch` at the source size. Replace the selection sort with an index sort over fixed storage. No allocation on the audio thread. |
| `issue-464-devicemanager-mutex` | #464 | confirmed | `src/engine/device/DeviceManager.cpp` fan-out takes `std::mutex` on the audio thread. Atomic snapshot of the callback list, retire-after-swap, prepare path outside any audio-visible lock. This file was written for the device seam; check `docs/dejuce-*.md` for the intended design before restructuring. |
| `issue-449-windows-activation` | #449 | confirmed by Win32 semantics, not compiled here | Second instance calls `AllowSetForegroundWindow(primaryPid)` before handing off; primary drops the self-PID call. `SingleInstance` needs the server PID (`GetNamedPipeServerProcessId`). Either implement `flushWindowOperations` on both platforms or delete the contract from `PlatformWindowing.h`. Needs CI. |
| `issue-466-scan-abort` | #466 | reviewer-reported; verify signatures first | Thread the abort flag through `scanLv2Plugins` and `scanAuPlugins`, checked per bundle. |

### Phase 2: release metadata and docs

| Branch | Issues | Verification status | Notes |
|---|---|---|---|
| `issue-443-manual` | #443 | confirmed (`MANUAL.md:62`, `tests/window_activation_smoke_test.md:20-22`); the other bullets reviewer-reported | Update the handoff sentence to cover all three platforms. Document Windows Unicode paths, LV2 editor live-state seeding, the unloaded-on-rejected-state behaviour. Drop the stale "#368 has not landed" note. Refresh `docs/screenshot-list.md` line references. |
| `issue-441-licenses-sfizz` | #441 | confirmed | Rewrite every sfizz rev string in `LICENSES.txt` to `0bb8aae364dc648c7c55438d17c7564a5d5eaef5`. Add a release-mechanics test that compares `git ls-tree HEAD external/sfizz` with the recorded rev, mirroring how the DAF pins are checked. |
| `issue-445-allowlist` | #445 | confirmed | Run `tools/juce-gate.sh --update`, commit the result on its own. Nothing else in the commit. |
| `issue-440-changelog` | #440 | confirmed | Do this last in Phase 2, after the fixes above have their entries. Rename the heading to `## [0.13.3] - Unreleased`. Add the missing entries listed in the issue plus one per Phase 1 fix. Say that a plugin whose state is rejected is now unloaded, not defaulted. Do not run `bump-version.sh`; that is Marc's step. |

### Phase 3: CI and release workflow

| Branch | Issues | Verification status | Notes |
|---|---|---|---|
| `issue-446-regression-tests` | #446 | confirmed (test anchoring, `DUSKSTUDIO_TEST_CLAP` unset in CI); LV2 test bullet reviewer-reported | Anchor the CLAP editor test after the method definition. Make the VST3 host-context test count constructions. Set `DUSKSTUDIO_TEST_CLAP` in the workflows to the fixture they already build. Make the LV2 mixed-separator case assert the refusal literally. |
| `issue-444-release-yml` | #444 | reviewer-reported | Draft-then-publish, assert `DUSKSTUDIO_ENABLE_NATIVE_UI` in each release configure, extend the macOS `otool -L` check to the helper, version the PDF, pin actions by SHA, add the suppression rationale. Cannot be exercised here; keep each change small and self-evidently correct. |
| #442 | #442 | confirmed | Repository ruleset change. Not a code change. Leave for Marc; mention it in your final report. |

### Phase 4: OOP sandbox, native hosting and platform hardening

OOP hosting is opt-in (default off in `PluginManager.h`). These do not gate the tag but belong in the milestone. Work them after Phases 1 to 3, in this order:

| Branch | Issues | Verification status | Notes |
|---|---|---|---|
| `issue-468-cloexec` | #468 | confirmed (`socketpair` flags 0, `memfd_create` flags 0, spawn closes only channel ends); spawnattr bullet reviewer-reported | `SOCK_CLOEXEC`, `MFD_CLOEXEC`, `FD_CLOEXEC` on macOS, `POSIX_SPAWN_SETSIGMASK` and `SETSIGDEF`. Test: two stub children, the second cannot see the first's descriptors. |
| `issue-455-vst3-iochanged` | #455 | confirmed | Latch `kIoChanged`, re-run bus discovery and `processData.prepare` from the drain timer under `suspendProcessing`, reconcile `activateBus` results. This is the VST3 half of #361. Needs a VST3 fixture that re-lays out buses; if none exists, add a minimal one under `tests/fixtures/`. |
| `issue-457-vst3-handler` | #457 | confirmed (handler set at `Vst3Instance.cpp:321`, never cleared) | Clear the component handler and any live view frame before `terminate()`. |
| `issue-456-vst3-latency-params` | #456 | confirmed (no latency re-read in `loadState`); param-queue bullet reviewer-reported | Set `latencyChanged` after a successful `setState`. Size both parameter queues in `activate()`. |
| `issue-447-oop-editor-thread` | #447 | confirmed (window created under `MessageManagerLock` on `sockThread`) | Marshal the show/hide/destroy handler bodies onto the child's message thread with a completion event. Needs CI on Windows and macOS. |
| `issue-448-foreign-hwnd` | #448 | confirmed | `visibilityChanged` override, no `SWP_SHOWWINDOW` in layout, re-attach on parent change, `IsWindow` guards. Needs CI. |
| `issue-453-shell-editor` | #453 | reviewer-reported; `PluginSlot.cpp` reaper nulling `currentRemote` is confirmed | Present the shell editor regardless of `HideEditor`; key embed teardown on "an embed exists". |
| `issue-470-worker-park` | #470 | confirmed (StoreLoad pair in `WorkerPark.h`); other two bullets reviewer-reported | `seq_cst` on the null store and the sequence loads. Route `handleLoadPlugin` through the park. Rotate `previousRemotes` under `processLock`. |
| `issue-471-oop-waits` | #471 | confirmed (sync fallback in `PluginSlot.cpp:858-870`); child-death wait reviewer-reported | Add a child-death channel to the wait, keep the async load path for OOP. |
| `issue-469-mac-pipe` | #469 | confirmed (drain loop, 31-char name truncation); listener-thread bullet reviewer-reported | Cannot be run here. Keep changes minimal and RT-safe; needs macOS CI. |
| `issue-472-windows-transport` | #472 | reviewer-reported | Five small items. Needs CI. |
| `issue-450-posix-single-instance` | #450 | confirmed (unlink without guard, unframed payload, silent skip) | Stat-and-compare before unlink or bind-to-temp plus rename, length-prefix the payload, log when the gate is skipped. |
| `issue-451-windows-single-instance` | #451 | confirmed (client never verifies server, `Local\` mutex vs global pipe) | Verify the server SID on the client, align the namespaces, keep the mutex when the listener fails. Needs CI. |
| `issue-452-x11-teardown` | #452 | confirmed (no RAII around the handler swap; sibling peer unfiltered) | Scope guard for the X error handler, atomic trap pointer, backend filter on the sibling peer. |
| `issue-458-lv2-edges` | #458 | reviewer-reported; the lilv feature-ordering claim is from 0.24 source and the app links 0.28, verify before acting | Non-null path return, second `mapPath` on the blob-only restore, sync and prune edges. |
| `issue-459-aux-attach-alerts` | #459 | reviewer-reported | Route aux-lane attach failures through the same policy helper the channel strip uses. |
| `issue-467-foundation` | #467 | reviewer-reported | `findSignedMinMax` returns `{0,0}` for `count <= 0`, `hasNaN` accumulator, `PlanarBuffer` stride width. |

### Close-out

- #406: already fixed on main (overlapped pipe, test compiled on Windows, CI green). No code change. Tell Marc it can be closed.
- When Phases 1 and 2 are merged, tell Marc the milestone is ready for `docs/MAINTAINER-GUIDE.md` Part 10 from step 1. Do not run the bump script, do not tag, do not push.

## Reporting format

At the end of each branch, one short report in plain sentences:

- branch name and worktree path
- what the issue claimed, what you found when you re-verified, what you changed
- tests added and the ctest line
- anything that needs CI on another platform
- CHANGELOG and MANUAL entries added
- anything you left out and why

No praise, no summaries of the process, no attribution lines.
