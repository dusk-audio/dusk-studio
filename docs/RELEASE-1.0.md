# Dusk Studio 1.0 release plan

1.0 ships on the current framework stack. The re-platforming campaign onto the
DAF, DAF-Widgets and pugl stack resumes after 1.0 as the 1.1+ H-series. No work
lands before 1.0 whose main effect is reducing framework coupling, unless it
fixes a bug that blocks the release.

This file is the plan of record for the 1.0 milestone. It carries the
definition of done, the triage of every open issue, the ordered work plan, the
release checklist, and the out-of-scope list.

## Definition of done

Each line below is a check somebody can run and get a yes or a no.

### Functional

- Every issue labelled `1.0-blocker` is closed.
- Every issue labelled `1.0` is closed or explicitly deferred with a reason
  recorded on the issue.
- The demo path runs clean on a packaged build on Linux, macOS and Windows:
  launch, pick a device, create a session from a template, arm a track, set a
  level, record, play back, overdub a second track, bounce the master.
- A fresh install with no third-party plugins installed can insert a reverb, a
  delay, a colour unit and an instrument from a built-in category.
- The session notepad edits lyrics and chords, transposes, and saves to
  `notepad.md` in the session folder.
- A local SFZ and SF2 instrument browser lists what is on disk and loads from
  it, with no network access at any point.
- Quitting through any route (window close, application quit, SIGTERM, logout)
  runs the same staged shutdown and offers the unsaved-changes prompt.

### Quality

- The full Catch2 suite passes on Linux GCC, Linux TSan, Linux ASan and UBSan,
  Linux arm64, macOS arm64 clang, and Windows MSVC x64.
- All six of those jobs are required checks on `main`.
- `tools/juce-gate.sh` passes. Counts may fall. They may not rise.
- `scripts/run-selftest-xvfb.sh` passes on a private display.
- The Windows self-test harnesses run to completion instead of hanging.
- The post-download smoke test passes against the published artifact on each
  of the three platforms.

### Packaging

- The macOS DMG is signed with a Developer ID Application certificate,
  notarized and stapled. It launches on a clean Mac by double click.
- The Windows MSI and both installed executables are Authenticode signed and
  timestamped.
- `SHA256SUMS` is published with a detached signature, and the verify command
  is documented.
- All three packages contain the documented package-contents list, verified
  in CI.
- `scripts/verify-release-assets.sh` exits 0 against the published release.

### Docs

- A one-page quickstart exists, is reachable from the app, and ships in every
  package.
- MANUAL.md describes every user-visible change since 0.13.3.
- CHANGELOG.md has a dated `## [1.0.0]` section.
- `scripts/release-metadata-check.sh` passes for the tag.

## Triage

Every open issue, one bucket each.

| # | Title | Bucket | Reason |
|---|---|---|---|
| 74 | New Feature: Downloadable SFZ | post-1.0 | Online catalog and downloads deferred to 1.1. The offline half is #535. |
| 75 | New Feature: Built In Plugins | 1.0 | Agreed 1.0 scope. A fresh install must have a reverb, delay, colour and instrument. Scoped in a comment on the issue. |
| 124 | Playlist comping (Option B) | post-1.0 | Large new feature. Take cycling already covers the 1.0 need. |
| 252 | SFZ library: cancellable download and hardened ZIP install | post-1.0 | Part of the deferred download path. Brings libcurl and libarchive with it. |
| 253 | SFZ library: portable session locator and asynchronous loading | post-1.0 | Depends on the deferred catalog. |
| 254 | SFZ library: embedded catalog UI and audited launch packs | post-1.0 | Online catalog UI and signed launch packs. The local-only browser is split out as #535. |
| 286 | Allow builds without signed SFZ catalog support | post-1.0 | Build-option convenience. Does not change what a shipped release does. |
| 294 | Consolidate and pin the reusable DSP/UI source of truth | post-1.0 | Framework-removal campaign. |
| 295 | [H2] Replace the mastering multiband processor | post-1.0 | Framework-removal campaign. |
| 296 | [H1d] Replace TapePanel with the TapeMachine 2 DAF UI | post-1.0 | Framework-removal campaign. |
| 297 | [H6] Delete plug-in hosting and unlink the audio-processors module | post-1.0 | Framework-removal campaign. |
| 298 | [Devices/macOS] Replace the CoreAudio and CoreMIDI backends | post-1.0 | Framework-removal campaign. |
| 299 | [Devices/Windows] Replace the WASAPI/ASIO and MIDI backends | post-1.0 | Framework-removal campaign. |
| 300 | [Engine] Remove remaining audio-basics and DSP primitives | post-1.0 | Framework-removal campaign. |
| 302 | [DAF fork] Complete the pugl platform contract | post-1.0 | Framework-removal campaign, and touches an out-of-bounds repo. |
| 303 | [App shell] Replace the application, window and message-thread classes | post-1.0 | Framework-removal campaign. |
| 304 | [GUI foundation] Dusk ImGui theme, widgets, layout, menus, modals | post-1.0 | Framework-removal campaign. |
| 305 | [Accessibility] Replace accessibility, keyboard nav and IME behaviour | post-1.0 | Framework-removal campaign. Screen-reader support must survive the port. |
| 306 | [Waveforms] Replace AudioThumbnail and unlink two modules | post-1.0 | Framework-removal campaign. |
| 307 | [GUI] Port console, mixer, aux, master strip and DSP editor views | post-1.0 | Framework-removal campaign. |
| 308 | [GUI] Port tape, region, transport and timeline views | post-1.0 | Framework-removal campaign. |
| 309 | [GUI] Port piano roll, virtual keyboard, bindings, controller views | post-1.0 | Framework-removal campaign. |
| 310 | [GUI] Port mastering, multisample, picker and native editor views | post-1.0 | Framework-removal campaign. |
| 311 | [GUI] Port startup, settings, dialogs, file, import and support flows | post-1.0 | Framework-removal campaign. |
| 312 | [Model] Remove framework types from session, serialization, undo | post-1.0 | Framework-removal campaign. |
| 313 | [Platform] Replace remaining core utilities and application services | post-1.0 | Framework-removal campaign. |
| 314 | [Final gate] Remove the framework from CMake, CI, packaging, tests, docs | post-1.0 | Framework-removal campaign. |
| 320 | Route macOS builds to the dusk-mac-air self-hosted runner | post-1.0 | Cost optimisation with no user-visible effect, and the runner is offline. |
| 340 | Change Tape Machine to use the Tape Machine 2 plugin | post-1.0 | Swaps donor DSP for DAF-ported V2 DSP, which reduces framework coupling. |
| 341 | Change EQ DSP to 4K-EQ-2 DSP | post-1.0 | Same. Also a tone change to a shipped signal path, which is not a 1.0 risk to take. |
| 342 | Change compressors to use Multi-Comp-2 DSP | post-1.0 | Same. |
| 442 | ASan+UBSan and Raspberry Pi jobs are not required checks | 1.0 | Two jobs can go red without blocking a merge. A 1.0 tag needs both gating. |
| 500 | CloneTrackAction native-insert clone and undo has no coverage | 1.0 | A shipped clone and undo path with no automated test. |
| 501 | Inline non-modal editor status in the aux slot area | 1.0 | A modal alert fires with no user action on every session load for DSP-only plugins. First-run polish. |
| 503 | Small residues from the milestone-6 audits | 1.0 | Four small correctness defects in shipped single-instance and hosting code. |
| 504 | Windows IPC self-test harnesses resolve the child without .exe and hang | 1.0 | Blocks manual Windows validation. Not a CI gate. See the verdict below. |
| 507 | SIGTERM bypasses the staged shutdown | 1.0-blocker | Logout or a supervisor stop skips the unsaved-changes prompt and leaves plugin children to the reaper. |
| 508 | Sandboxed slot loaded at runtime loses its child within 200 ms | 1.0-blocker | A shipped sandboxing feature drops the plugin on a real path. |
| 529 | Sign and notarize the macOS DMG | 1.0-blocker | Gatekeeper refuses an unsigned DMG. A 1.0 cannot ship a right-click-to-open install. |
| 530 | Sign the Windows MSI and its executables | 1.0-blocker | SmartScreen blocks an unsigned MSI on download. |
| 531 | Publish a signed SHA256SUMS | 1.0 | Checksums prove a download is intact but not that it is ours. |
| 532 | Post-download smoke test for published artifacts | 1.0 | Nothing today proves a published artifact runs on a machine that did not build it. |
| 533 | Quickstart page linked from the app and every package | 1.0 | The manual is a PDF. A new user needs one screen. |
| 534 | Define the 1.0 package contents and verify all three packagers | 1.0 | Three packagers grown separately, no shared contract. Matters once the plugin suite ships. |
| 535 | Local instrument browser for soundfonts on disk | 1.0 | The offline half of the SFZ work, with no network, catalog or archive code. |
| 536 | Walk the demo path on packaged builds and file what it snags on | 1.0 | Individual demo-path bugs have been fixed one at a time. Nobody has walked the whole path on a shipped build. |

Nothing was bucketed `wontfix`.

### On #504

Not a blocker. `windows-tests.yml` builds the app and the Catch2 binary and
runs the suite through `ctest`, with explicit greps that require the four
`ipc-stub` regressions and the eight `FileImporter` regressions to be present
in the inventory. `DUSKSTUDIO_RUN_IPC_SELFTEST` and `DUSKSTUDIO_IPC_HOST_TEST`
are app-level environment-gated paths in `src/DuskStudioApp.cpp`. They appear
in no workflow, no test target and no script. The Windows release gate is
therefore unaffected by the hang.

It still ships in 1.0. A maintainer validating a Windows candidate by hand
follows a documented harness that sits forever with no output, and a missing
child should exit with a failure code rather than wait.

## Work plan

PR-sized units, in order. Estimates are hours of focused work, not elapsed
time. The two columns of subsystems are disjoint within each stage, so two
people can take one item each without colliding.

### Stage 1 - Blockers

| Item | Issue | Est | Touches |
|---|---|---|---|
| SIGTERM routes through the staged shutdown | 507 | 4-6 | `src/DuskStudioApp.cpp`, `src/ui/MainComponent.cpp`, `src/foundation/MessageThread.h` |
| Runtime-enabled sandboxed slot keeps its child | 508 | 8-16 | `src/engine/PluginSlot.cpp`, `src/engine/ipc/`, `src/engine/ipc/PluginHostMain.cpp` |

508 is the uncertain one. The reproduction is known and the suspect exit path
is named in the issue, but the cause is not established. Treat the upper
estimate as the real one.

### Stage 2 - Demo path

| Item | Issue | Est | Touches |
|---|---|---|---|
| Walk the path on packaged builds, file the snags | 536 | 6-8 | No code. Produces the follow-up list. |
| Follow-up fixes from that walk | from 536 | unknown | Sized once 536 is done |
| Inline non-modal aux editor status | 501 | 4-6 | `src/ui/AuxLaneComponent.*`, `src/ui/AuxView.*`, native instance has-editor query |

536 gates the size of stage 2. Do it early even though it produces no code, so
the follow-ups are known before the schedule is committed.

### Stage 3 - Scope

| Item | Issue | Est | Touches |
|---|---|---|---|
| Built-in plugin suite: slot type, picker category, serialization | 75 | 16-24 | `src/engine/PluginSlot.*`, `src/ui/PluginPickerPanel.*`, `src/session/` |
| Built-in plugin suite: the units and their editors | 75 | 16-24 | `src/dsp/`, `src/ui/`, donor cores |
| Local SFZ and SF2 browser | 535 | 12-16 | `src/ui/multisample/`, `src/engine/sfz/`, `src/session/` |
| CloneTrackAction coverage | 500 | 3-4 | `tests/`, self-test leg |
| Milestone-6 audit residues | 503 | 4-6 | `src/util/SingleInstance.cpp`, `src/engine/PluginSlot.cpp`, `src/ui/ChannelStripComponent.cpp` |
| Windows self-test harness child resolution | 504 | 2-3 | `src/DuskStudioApp.cpp`, `src/engine/PluginManager.h` |

The two 75 items are sequential. The rest of stage 3 is independent of both.

### Stage 4 - Packaging and release

| Item | Issue | Est | Touches |
|---|---|---|---|
| Required checks on the main ruleset | 442 | 1 | Repository settings only |
| macOS Developer ID signing and notarization | 529 | 8-12 | `.github/workflows/release.yml`, repository secrets |
| Windows Authenticode signing | 530 | 8-12 | `.github/workflows/release.yml`, repository secrets |
| Signed SHA256SUMS | 531 | 3-4 | `.github/workflows/release.yml`, `scripts/verify-release-assets.sh` |
| Package contents list and CI check | 534 | 6-8 | `scripts/package-*.sh`, `CMakeLists.txt`, workflows |
| Quickstart page and in-app link | 533 | 4-6 | Docs, `src/ui/` help and startup surface, packaging |
| Post-download smoke test | 532 | 8-12 | `scripts/release-smoke-test.sh`, PowerShell sibling |

529 and 530 both need credentials procured before any code is written. Start
that procurement at the same time as stage 1, because the lead time on a
hardware-token code-signing certificate is measured in days, not hours.

Total, excluding the unknown follow-ups from 536 and the upper tail on 508:
roughly 115 to 165 hours.

## Release checklist

### Build matrix

| Platform | Workflow | Job |
|---|---|---|
| Linux x86_64 | `linux-build.yml` | Build + tests (GCC Release, Ubuntu 22.04, amd64) |
| Linux arm64 | `raspberry-pi-build.yml` | Build + tests (GCC Release, Ubuntu 22.04, arm64) |
| Linux TSan | `linux-sanitizer.yml` | Catch2 tests (TSan, Ubuntu 22.04) |
| Linux ASan + UBSan | `linux-sanitizer.yml` | Catch2 tests (ASan + UBSan, Ubuntu 22.04) |
| macOS arm64 | `macos-build.yml` | Build + tests (clang Release, macOS arm64) |
| Windows x64 | `windows-tests.yml` | Catch2 tests (MSVC x64 Release, Windows) |
| Coupling gate | `linux-build.yml` | De-JUCE ratchet |

All seven are required checks on `main` before the 1.0 tag. Two of them are
not required today; that is #442.

### The test gate, and what the numbers mean

Three different counts circulate. Only one of them is the gate.

- **1001** is the number of `TEST_CASE` declarations across the 188 files in
  `tests/`. That is a static count of the source tree. README quotes it and it
  is correct as a statement about the source.
- **979** is the number of Catch2 cases the binary actually declares when it is
  built on Linux, measured with `dusk-studio-tests --list-tests`. The gap is
  the test sources that CMake compiles only on Windows or macOS.
- **980** is what `ctest --test-dir build-tests -N` registers on Linux: those
  979 cases plus `release-mechanics-contract`, a shell test that exercises the
  real release-version script against a fixture and is added only on Unix.

**980 on Linux is the gate.** The number is platform-dependent by design, so
the release criterion is not a number at all. It is that `ctest` exits 0 on
every platform in the matrix above, with no test skipped that is not
environment-gated.

How it runs in CI:

- `linux-build.yml` configures with `-DDUSKSTUDIO_BUILD_TESTS=ON`, builds
  `dusk-studio-tests`, and runs `ctest`.
- `linux-sanitizer.yml` runs the same suite twice, once under TSan and once
  under ASan and UBSan.
- `raspberry-pi-build.yml` runs it on `ubuntu-22.04-arm`.
- `macos-build.yml` runs it on `macos-14`.
- `windows-tests.yml` runs it on `windows-2022`, and before running it greps
  the `ctest -N` inventory for four named `ipc-stub` cases and eight named
  `FileImporter` cases, so a build that silently drops those Windows
  regressions fails rather than passing with fewer tests.

Discovery is automatic. `catch_discover_tests` registers each case, in two
passes: `[alsa]` with `--allow-running-no-tests` so an all-skipped headless run
scores as a pass, and `~[alsa]` for everything else. Adding a `TEST_CASE` and
listing its `.cpp` in `tests/CMakeLists.txt` is the whole wiring step.

The release jobs in `release.yml` build and run the suite again on each
platform before packaging. No artifact is produced from a red suite.

### Signing and notarization

| OS | Artifact | Requirement | Credentials |
|---|---|---|---|
| macOS | `.app` inside the DMG, and the DMG | Developer ID Application signature, hardened runtime, secure timestamp, notarized and stapled | Developer ID Application certificate as base64 `.p12` plus password, Apple Team ID, and an App Store Connect API key (issuer ID, key ID, `.p8`) or an Apple ID with an app-specific password |
| Windows | `DuskStudio.exe`, `dusk-studio-plugin-host.exe`, and the MSI | Authenticode signature with an RFC 3161 timestamp | Code-signing certificate. Since June 2023 both OV and EV are issued on hardware tokens or through a cloud service, so this is Azure Trusted Signing, a cloud HSM through a CSP, or a self-hosted runner with the token attached |
| Linux | `SHA256SUMS` | Detached signature, public key published, verify command documented | Release signing private key and its passphrase |

Every credential is a repository secret. The release job must fail, and
publish nothing, when a secret is missing on a `v*` tag.

### Package contents

Per #534, every package contains: the app, `dusk-studio-plugin-host`, the
built-in plugin suite, the quickstart link, `LICENSE` and `LICENSES.txt`, plus
platform integration files where the platform has them. Project templates are
in the app through `src/session/SessionTemplates.h` and ship as code, not as
files. CI asserts the list per packager.

No third-party plugin is bundled in any package. That invariant comes from the
licence position in LICENSES.txt and does not change for 1.0.

### Tag procedure

Follow MAINTAINER-GUIDE Part 10 exactly. In short:

1. Finish the changelog section, run `scripts/bump-version.sh`.
2. Date the heading, run `scripts/release-metadata-check.sh`, run the Patreon
   freshness check from the primary checkout.
3. Rebuild app and tests, run the full suite, the coupling gate, and the
   headless self-test under Xvfb. Never launch a release binary on the live
   session.
4. Commit the metadata, record `RELEASE_COMMIT`.
5. Land that commit on `origin/main` and prove containment.
6. Tag that exact commit and push only the tag.

**Pushing any `v*` tag starts `release.yml`.** Preflight rejects a tag that is
not `v$(cat VERSION)` and proves the private-repo token can write before any
build starts. Linux, macOS, Windows and manual jobs then run in parallel, and
one publisher assembles the assets, creates `SHA256SUMS`, and writes the
release to the private repository `dusk-audio/dusk-studio-releases`. There is
no dry run. Do not push a `v*` tag to test anything.

### Acceptance after the tag

- `scripts/verify-release-assets.sh v1.0.0` exits 0.
- The published release carries the six existing assets plus the signature
  asset from #531.
- The smoke test from #532 passes against each downloaded artifact.
- macOS: `spctl --assess` reports `source=Notarized Developer ID`, and
  `stapler validate` on the DMG exits 0.
- Windows: `signtool verify /pa /v` exits 0 for the MSI and both executables.
- Linux: the detached signature verifies against the published public key.

Do not announce until all of the above pass. A green workflow is not a
release.

## Out of scope for 1.0

- Anything whose main effect is reducing framework coupling. The whole
  H-series (#294 through #314) is deferred. So are #340, #341 and #342,
  because swapping donor DSP for DAF-ported V2 DSP reduces coupling and
  changes the tone of shipped signal paths at the same time.
- Any change to the DAF, DAF-Widgets, pugl or DPF-Widgets repositories.
  Consuming them, which is how the native notepad UI is already built, stays.
  Modifying them does not happen before 1.0.
- Downloadable SFZ: the online catalog, downloads, archive extraction and the
  libcurl and libarchive dependencies (#74, #252, #253, #254). Only the
  offline browser (#535) is in.
- Playlist comping (#124). Take cycling already covers the 1.0 need.
- Self-hosted macOS build routing (#320).
- Any feature not named in the definition of done above. New feature requests
  arriving during the 1.0 push get `post-1.0` on sight.
