# Hosting tower H5 — native platform ports + AU (executable spec)

Status: **SCOUTED 2026-07-29; MARC DECISIONS LOCKED; H5a.0-H5a.2 COMPLETE
LOCALLY; H5a.3 NEXT.** H4 is merged as PR #119 at `3c5c901`. The H5 scout
verified the live source/CMake/CI surface from that baseline. Windows LV2 is
deferred. H5 is three sequential PRs: H5a macOS CLAP/LV2/VST3, H5b macOS AU,
H5c Windows CLAP/VST3. No H5 branch is pushed without Marc's word.

Parent plan: [dejuce-hosting-plan.md](dejuce-hosting-plan.md). Campaign ritual:
[dejuce-campaign.md](dejuce-campaign.md). H1d and H2 remain blocked on donor
consolidation and are not part of H5.

## Goal

Make the existing native CLAP, LV2, and VST3 hosts first-class on macOS; add a
native Audio Unit host on macOS; then make CLAP and VST3 first-class on Windows.
H5 leaves Linux behaviour unchanged and gives H6 enough native coverage to
delete the JUCE hosting path without removing plugin hosting from a shipping
platform.

H5 does not unlink a JUCE module. H6 performs the deletion and globally unlinks
`juce_audio_processors`.

## Locked decisions

- PR sequence is H5a macOS ports -> H5b AU -> H5c Windows ports -> H6. One PR
  open at a time. Each PR must merge before the next branch starts.
- Windows LV2 is deferred. H5c does not vendor suil and does not ship a
  parameters-only LV2 tier. As verified at scout time, vcpkg has lilv but no
  suil port. Linux and macOS keep full LV2 hosting, including embedded UIs.
- H5a enables native CLAP, LV2, and VST3 by default on macOS. CI must request
  all three options explicitly and fail configure if a requested dependency or
  submodule is absent. A green build with the native formats silently OFF is not
  H5 verification.
- Linux native hosting is an invariant: same search paths, bundle semantics,
  X11 editors, CLAP POSIX-fd support, VST3 Linux run loop, LV2 behaviour, audio
  results, and current tests.
- Platform code stays behind the existing format abstractions. Do not fork
  `ClapInstance`, `Lv2Instance`, `Vst3Instance`, `NativeInsertSlot`, picker,
  session, or engine audio behaviour merely to select a window or loader API.
- Platform editor handles are pointer-width-safe. Do not carry the Linux
  `unsigned long` X11 handle into a cross-platform interface; Windows LLP64
  would truncate `HWND`.
- macOS CLAP and VST3 editor sizes are logical coordinates. Linux X11 remains
  physical pixels. The JUCE peer already returns `NSView*` on macOS and `HWND`
  on Windows; the editor layer owns a native child container at the component's
  bounds and gives that container to the plugin.
- macOS LV2 uses lilv for discovery/instances and suil with a Cocoa container
  for embedded UIs. Headless LV2 plugins remain loadable. A missing compatible
  UI is an editor-only failure, not a plugin-load failure.
- CLAP advertises `CLAP_EXT_POSIX_FD_SUPPORT` on Linux and macOS. H5c must not
  advertise or compile that extension on Windows; CLAP timer support remains.
- VST3 `Steinberg::Linux::IRunLoop` exists only on Linux. The macOS/Windows host
  object exposes the portable host application, component-handler, and
  plug-frame facets; `pump()` has no Linux fd/timer registry there.
- Scanning a native bundle continues through the sandbox child where the
  current scanner does so. H5 must not regress crash/timeout isolation.
- AU is a new native layer in H5b, not a JUCE wrapper. It uses
  AudioComponent/AudioUnit APIs and implements `hosting::INativeInstance`.
- Runtime sign-off for all macOS/Windows formats and native editors is owed to
  Marc's real machines. Linux-only development cannot claim it.
- Zero attribution trailers. Commit locally; never push without Marc's word.

## Scout ground truth

- `CMakeLists.txt` currently forces native CLAP/LV2/VST3 OFF outside Linux and
  compiles VST3's `threadchecker_linux.cpp` + `module_linux.cpp`.
- The VST3 SDK mirror contains `module_mac.mm`, `module_win32.cpp`,
  `threadchecker_mac.mm`, and `threadchecker_win32.cpp`.
- `ClapBundle.cpp` is a direct `dlopen`/`dlsym`/`dlclose` loader. A macOS
  `.clap` is normally a directory bundle whose executable must be resolved
  before loading while the original bundle path is retained for identity and
  `clap_entry::init`.
- `ClapScanner` only collects regular `*.clap` files today, so it misses macOS
  bundle directories. Its defaults are Linux FHS paths.
- `ClapEditor`, `Lv2Editor`, and `Vst3Editor`, plus their JUCE wrapper
  components, are X11-specific today.
- `Vst3Bundle` already delegates loading to the SDK's `Hosting::Module`; its
  main port is selecting the SDK platform source. `Vst3HostContext` and
  `Vst3Editor` are still structurally Linux-only.
- macOS CI installs neither lilv nor suil today. Homebrew currently provides
  both. Windows CI currently installs neither, and H5c will not add them.
- Existing macOS/Windows green jobs prove only the feature-OFF paths until H5
  explicitly enables and asserts the native options.
- AU has no native bundle, host, instance, editor, scanner, slot, persistence,
  tests, or CMake target today.

## H5a — macOS CLAP/LV2/VST3

Branch: `dejuce/hosting-h5a`. One PR. Execute the increments below in order.
Each implementation increment touches at most five files, gets its own focused
verification, and pauses for campaign-manager review before the next.

### H5a.0 — build gates + CI dependencies

Files:

1. `CMakeLists.txt`
2. `tests/CMakeLists.txt`
3. `.github/workflows/macos-build.yml`
4. `.github/workflows/macos-release.yml`
5. `docs/dejuce-hosting-plan.md`

Work:

- Enable Objective-C++ only on Apple.
- Make native CLAP, LV2, and VST3 all available on Linux and macOS - every
  one of the three needs its option, target, and gate wired on macOS; omitting
  any of them fails H5a.0. Select the VST3 SDK's Linux or macOS
  module/thread-checker sources per platform; compile the macOS Objective-C++
  sources with ARC and link the SDK-required Cocoa frameworks.
- Pre-route each native editor source by platform in CMake: compile the
  existing `*Editor.cpp` on Linux and the future `*Editor_Mac.mm` on Apple, so
  H5a.2, H5a.3, and H5a.5 stay within their five-file increment limits.
- Detect lilv/suil/lv2 through pkg-config on macOS as well as Linux. Preserve
  Linux's existing optional-dependency behaviour; on macOS, an explicitly ON
  native-LV2 option with a missing dependency is a configure error.
- Keep third-party warning suppression compiler-correct (`-w` for Clang/GCC,
  `/w` for MSVC in H5c).
- Make native-host test sources follow the same feature gates. Keep
  `vst3_hostcontext_runloop.cpp` Linux-only.
- Install lilv/suil/lv2 in both macOS build and release workflows. Configure
  with `DUSKSTUDIO_NATIVE_CLAP=ON`, `DUSKSTUDIO_NATIVE_LV2=ON`, and
  `DUSKSTUDIO_NATIVE_VST3=ON`; assert the three cache values after configure.
- Refresh the parent plan's stale H4 status and record the locked H5 split and
  Windows-LV2 decision.

Verify: Linux configure/build remains unchanged with all three native formats
ON. A macOS configure cannot silently turn a requested format OFF.

### H5a.1 — portable discovery + CLAP bundle loading

Files:

1. `src/engine/clap/ClapBundle.cpp`
2. `src/engine/clap/ClapScanner.cpp`
3. `src/engine/vst3/Vst3Scanner.cpp`
4. `tests/clap_bundle_load.cpp`
5. `tests/clap_scanner.cpp`

Work:

- Keep Linux CLAP loading byte-for-byte equivalent. On macOS, accept a direct
  dynamic library or a `.clap` directory, resolve the directory's executable
  with CoreFoundation bundle APIs, load it with local/now symbol visibility,
  and retain the original bundle path for identity and entry initialisation.
  Every failure must unload partially-created state and report a reason.
- Discover `.clap` files on Linux and `.clap` directory bundles on macOS.
  Never descend into a matched bundle. Deduplicate absolute paths.
- macOS CLAP defaults:
  `~/Library/Audio/Plug-Ins/CLAP`,
  `/Library/Audio/Plug-Ins/CLAP`.
  Keep `$CLAP_PATH` ahead of defaults and POSIX `:` separation.
- macOS VST3 defaults:
  `~/Library/Audio/Plug-Ins/VST3`,
  `/Library/Audio/Plug-Ins/VST3`.
  Reuse the current file-or-directory bundle collector and `$VST3_PATH`.
- Extend negative-path and directory-bundle tests without requiring a
  third-party plugin. Live load/enumeration remains environment-gated.

Verify: focused scanner/loader tests; full Linux native tests; no scan cache or
descriptor schema change.

### H5a.2 — CLAP Cocoa editor

Files:

1. `src/engine/clap/ClapEditor.h`
2. `src/engine/clap/ClapEditor.cpp` (retain as the Linux implementation)
3. `src/engine/clap/ClapEditor_Mac.mm` (new)
4. `src/ui/ClapPluginEditorComponent.h`
5. `src/ui/ClapPluginEditorComponent.cpp`

Work:

- Make the public editor boundary native-handle-neutral and pointer-width-safe.
  Preserve Linux's X11 implementation in its platform translation unit.
- On macOS, create an `NSView` child container beneath the JUCE peer, request
  `CLAP_WINDOW_API_COCOA`, pass the container through `clap_window`, and preserve
  the current create -> parent -> show lifecycle.
- Resize/move/show/hide/close the container on the message thread. Preserve
  plugin resize/show/hide/closed callbacks and the shutdown leak escape hatch.
- Use logical coordinates on Cocoa; retain the current physical-pixel scale and
  geometry-drift checks on X11.
- `ClapHost::pumpGui` continues to drive callbacks, POSIX fds, and timers on
  macOS.

Verify: Linux CLAP editor compiles and existing tests stay green; macOS app
compiles the Cocoa translation unit with no X11 dependency.

### H5a.3 — VST3 portable host context + Cocoa editor core

Files:

1. `src/engine/vst3/Vst3HostContext.h`
2. `src/engine/vst3/Vst3HostContext.cpp`
3. `src/engine/vst3/Vst3Editor.h`
4. `src/engine/vst3/Vst3Editor.cpp` (retain as the Linux implementation)
5. `src/engine/vst3/Vst3Editor_Mac.mm` (new)

Work:

- Compile the `Linux::IRunLoop` facet, `poll`, fd handlers, timers, and their QI
  only on Linux. Keep the portable host application, component-handler, and
  plug-frame facets on every platform. DONE EARLY: the host-context split landed
  with the H5a.1 macOS build fix, alongside a placeholder `Vst3Editor_Mac.mm`
  whose `open()` reports no embeddable view. Only the Cocoa attach below remains.
- Keep the existing Linux run-loop API/test observable. On macOS, `pump()` has
  no Linux registry to service.
- On macOS, create an `NSView` child container and attach `IPlugView` with
  `kPlatformTypeNSView`; wire `IPlugFrame` before attach; round-trip
  `resizeView`, `onSize`, visibility, and removal in logical coordinates.
- Preserve Linux X11 attachment and content-scale behaviour.

Verify: Linux run-loop test remains unchanged and green; macOS compiles without
Linux interface or `poll.h` references.

### H5a.4 — VST3 Cocoa wrapper + cross-platform tests

Files:

1. `src/ui/Vst3PluginEditorComponent.h`
2. `src/ui/Vst3PluginEditorComponent.cpp`
3. `tests/vst3_hostcontext_runloop.cpp`
4. `tests/vst3_scanner.cpp`
5. `tests/vst3_bundle_load.cpp`

Work:

- Route the JUCE wrapper through the neutral native handle and use logical
  component bounds on macOS while preserving X11 scale/geometry diagnostics.
- Compile and run the run-loop test only on Linux.
- Make scanner shape/default tests platform-aware. Keep live module tests
  environment-gated and the no-module failure path universal.

Verify: focused VST3 tests on Linux; macOS CI compiles and runs the portable
negative/discovery tests.

### H5a.5 — LV2 Cocoa editor

Files:

1. `src/engine/lv2/Lv2Editor.h`
2. `src/engine/lv2/Lv2Editor.cpp` (retain as the Linux implementation)
3. `src/engine/lv2/Lv2Editor_Mac.mm` (new)
4. `src/ui/Lv2PluginEditorComponent.h`
5. `src/ui/Lv2PluginEditorComponent.cpp`

Work:

- Preserve lilv discovery, instance, state-path, parameter, and audio behaviour.
  Lilv owns `$LV2_PATH` and platform default discovery.
- Make the editor boundary pointer-width-safe. Preserve the X11+suil
  implementation on Linux. `Lv2Editor_Mac.mm` already exists as a placeholder
  from the H5a.1 macOS build fix (`open()` reports no embeddable UI); replace
  its body rather than creating the file.
- On macOS, discover a UI that suil can wrap for a `LV2_UI__CocoaUI` container,
  create an `NSView` child container, pass `ui:parent` plus the existing
  instance/data/resize/idle features, and instantiate through suil.
- Keep resize, idle-close, show/hide, teardown, and shutdown-leak semantics.
  Use logical Cocoa bounds.

Verify: Linux LV2 tests and editor build remain green; macOS CI compiles lilv,
suil, the instance tests, and the Cocoa editor.

### H5a.6 — full closeout

Review order: cavecrew reviewer -> fixes -> fresh-eyes reviewer -> fixes ->
full validation -> commit. Do not push.

Linux bar:

```bash
CCACHE_DISABLE=1 cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DDUSK_PLUGINS_PATH=/home/marc/projects/plugins-multicomp-core
CCACHE_DISABLE=1 cmake --build build -j"$(nproc)"
CCACHE_DISABLE=1 cmake -S . -B build-tests -DCMAKE_BUILD_TYPE=Release \
  -DDUSKSTUDIO_BUILD_TESTS=ON \
  -DDUSK_PLUGINS_PATH=/home/marc/projects/plugins-multicomp-core
CCACHE_DISABLE=1 cmake --build build-tests --target dusk-studio-tests -j"$(nproc)"
ctest --test-dir build-tests --output-on-failure
tools/juce-gate.sh
```

Run the app self-test only on a private Xvfb display with
`WAYLAND_DISPLAY` unset. Accept only the known environment flake
`ALSA seq backend does not report`; investigate every other failure. Gate must
stay at or below 179. Build must add zero warnings. Run diff/slop and MANUAL
audits.

After Marc says push, macOS CI must prove:

- Release app and Catch2 suite compile with native CLAP/LV2/VST3 explicitly ON.
- Cache assertions show all three enabled.
- CLAP/VST3 negative loader and scanner tests run on macOS.
- LV2 native instance tests compile/run with Homebrew lilv/suil/lv2.

CI compile success is not runtime sign-off. Record the bench debt in the PR.

## H5b — native AU

Branch after H5a merges: `dejuce/hosting-h5b`. One PR, with bounded internal
increments.

- Add macOS-only `src/engine/au/`:
  `AuBundle`, `AuHost`, `AuInstance`, `AuEditor`, `AuScanner`,
  `NativeAuSlot`.
- `AuBundle` resolves a stable AudioComponent identifier
  (type/subtype/manufacturer) and component metadata.
- `AuInstance` implements `hosting::INativeInstance` with AudioUnit
  initialise/uninitialise, bus/stream-format negotiation, render, MIDI,
  parameter enumeration/writes, state property-list round-trip, and latency.
  No allocation, lock, Objective-C dispatch, or property query on the audio
  thread.
- `AuEditor` loads the Cocoa view factory and embeds its `NSView` through the
  H5a Cocoa-container pattern.
- `AuScanner` enumerates effects/instruments into native `PluginDescriptor`
  rows and uses the existing cache/sandbox rules.
- Add AU rungs to channel/aux slots, picker, editor-open flows, MIDI bindings,
  latency, shutdown, session save/restore, and clone/offline plumbing. New
  session keys are additive; legacy JUCE-AU descriptors migrate one way.
- Add narrow tests for identifier round-trip, descriptor classification,
  instance lifecycle/process, parameter/state/latency, slot persistence, and
  scanner output. Real stock Apple units remain a bench check.

## H5c — Windows CLAP/VST3

Branch after H5b merges: `dejuce/hosting-h5c`. One PR, with bounded internal
increments.

- Enable CLAP/VST3 on Windows and select SDK `module_win32.cpp` +
  `threadchecker_win32.cpp`.
- CLAP dynamic loading uses `LoadLibraryW`/`GetProcAddress`/`FreeLibrary` with
  UTF-16 paths and stable UTF-8 identity/error text.
- Windows scan defaults and environment-list separator follow the CLAP/VST3
  platform conventions.
- CLAP editor uses `CLAP_WINDOW_API_WIN32`; VST3 uses
  `kPlatformTypeHWND`. Both own child `HWND` containers and preserve lifecycle,
  resizing, visibility, and teardown.
- CLAP does not advertise or compile POSIX-fd support on Windows. Timers and
  main-thread callbacks remain.
- Windows LV2 remains OFF and absent from picker rows.
- `windows-tests.yml` explicitly enables CLAP/VST3 and runs native-host tests.
  `windows-build.yml` explicitly enables them and is dispatched after Marc's
  push word. A vcpkg 504 is retried with `gh run rerun <id> --failed`.

## Verify

Each PR gets the Linux full bar before commit and its target-platform CI after
Marc authorises a push. No phase may infer runtime success from cross-platform
compilation.

H5 is complete only when:

- macOS natively scans, loads, processes, saves/restores, and embeds editors for
  CLAP, LV2, VST3, and AU on Marc's bench;
- Windows does the same for CLAP and VST3;
- Linux CLAP/LV2/VST3 is unchanged and fully green;
- sessions preserve native format, stable identifier, state, bypass, latency,
  instrument/effect classification, and editor reopen;
- the picker shows one native row per supported format and does not fall back to
  the JUCE host for a format H5 owns;
- gate is at or below 179 and module count remains 12 pending H6.

## Owed to Marc's bench

- macOS: scan/load/play/save/reload/bypass for real CLAP, LV2, VST3 effects and
  instruments; editor open/resize/hide/reopen; plugin-reported latency; CLAP
  fd/timer UI; stock Apple AU effect + instrument; signed/notarized app smoke.
- Windows: the same CLAP/VST3 matrix in the full app, including editor DPI,
  resize/reopen, state, latency, and ASIO playback.
- Cross-platform session handoff: save native CLAP/VST3 on one OS, open on
  another with the corresponding plugin installed, and verify stable-id restore
  or a clear offline row.

## Resume phrase

"Hosting H5, spec docs/dejuce-hosting-h5-platform.md — on
dejuce/hosting-h5a, execute the first incomplete H5a increment, review, validate,
and commit locally; never push without Marc's word."
