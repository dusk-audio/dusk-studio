# Hosting tower H4 — descriptor/plumbing de-JUCE (executable spec)

Status: **IMPLEMENTED AND VALIDATED 2026-07-29 on
`dejuce/hosting-h4`; not pushed.** Baseline is `main` at `64eb5a3`, plus the
docs-only status commits `698fd71` and `28ac471`; gate 181 -> 179. One PR.
H1d and H2 remain blocked on donor consolidation and are out of scope.
Parent plan: [dejuce-hosting-plan.md](dejuce-hosting-plan.md). The post-H3
re-scout was repeated on `698fd71`; the old 36-file table is obsolete.

Validation: Release app and plugin-host builds completed without new warnings;
the focused descriptor/session/slot/scan suite passed 478 assertions in 60
cases; full CTest passed 488/488; private-Xvfb self-test passed; the synthetic
full-range SFZ harness printed `VERDICT: AUDIO PRESENT`; and the plugin-picker
screenshot passed visual review. Temporary validation assets were removed.

## Goal

Plugin metadata outside the surviving JUCE host implementation is owned by a
Dusk `PluginDescriptor`, not `juce::PluginDescription` or
`juce::KnownPluginList`. The picker, native scan rows and caches, scan-sandbox
wire format, session references, offline placeholders, clone replay, and test
harness fixtures all carry that type.

The JUCE hosting path still exists until H6. H4 therefore creates a narrow,
lossless conversion boundary: JUCE scanning/instantiation may convert at the
inside edge of `PluginManager`, `PluginSlot`, and the scan child, but no
picker, session, cache, or scan-protocol API exposes JUCE descriptor types.
H4 does not change which host processes audio or which native rung wins.

## Locked decisions

- Add a JUCE-free `src/engine/PluginDescriptor.{h,cpp}`. It uses `std::string`
  and fixed-width integers and contains:
  - `name`, `descriptiveName`, `manufacturer`, `category`, `version`;
  - `formatName` preserving the exact scanned format string;
  - `PluginBackend { Native, JuceLegacy }`;
  - `location` and a separate `pluginId` (no newline-packed native identity);
  - `uniqueId`, `deprecatedUid`, input/output channel counts;
  - file-modification and info-update times in milliseconds;
  - `isInstrument`, `hasSharedContainer`, and `hasAraExtension`.
  `formatName` stays a string rather than an enum so unknown/future JUCE
  formats round-trip losslessly. Routing always checks backend plus format;
  native rows never invent names such as `VST3-Native`.
- The descriptor owns versioned nlohmann-JSON conversion. Missing optional
  fields take the field defaults; wrong top-level types, unknown required
  enum values, or malformed JSON fail without partially mutating the output.
  Preserve every field through JSON and JUCE conversion. Do not add a new
  JUCE-coupled adapter translation unit: conversions live in already-coupled
  `PluginManager.cpp` / `PluginHostMain.cpp` boundaries so the gate cannot
  rise.
- `PluginManager` keeps `AudioPluginFormatManager`, `KnownPluginList`, its XML
  cache, and `AudioPluginInstance` creation private as H6 implementation
  residue. Public picker views return `std::vector<PluginDescriptor>`;
  `getKnownPluginList()` is removed and its only caller uses
  `getPluginCount()`. Load/create APIs that accept a description accept the
  Dusk type, then convert immediately before calling JUCE. The conversion
  copies all fields in both directions, including both UIDs, timestamps,
  shell/ARA flags, and channel counts.
- Native description storage in `PluginManager` becomes
  `std::vector<PluginDescriptor>` under the existing lock. CLAP, LV2, and VST3
  rows set `backend = Native`, use canonical format names `CLAP`, `LV2`, and
  `VST3`, and store bundle/path and inner plugin ID separately.
  `NativeScanRows.h` takes `std::filesystem::path` plus a descriptor vector
  and becomes JUCE-free.
- Native caches become versioned JSON descriptor arrays. Keep their existing
  staleness checks: split identity is no longer needed, so check
  `descriptor.location`. On first H4 launch, read the old native XML cache
  best-effort through the private JUCE adapter when no JSON cache exists;
  the next native scan/cache write emits JSON only. The JUCE-format
  `plugin-cache.xml` remains unchanged and private until H6.
- `PluginScanProtocol.h` becomes JUCE-free. Sentinel framing and crash
  semantics are frozen:
  - `makePayload(std::vector<PluginDescriptor>)` emits one compact,
    versioned JSON payload between the existing begin/end sentinels;
  - `extractPayload` ignores plugin stdout before the begin sentinel and
    returns empty if either sentinel is absent;
  - a framed empty descriptor array is a successful zero-result scan, not a
    crash;
  - malformed framed JSON parses to no rows and never appends partial rows;
  - sandbox policy accepts `std::string_view`.
  The JUCE `--scan` child converts scan results to Dusk rows only after JUCE
  finishes probing. The parent's `KnownPluginList::CustomScanner` converts
  parsed rows back only to satisfy that private JUCE callback. Native
  `--scan-native` produces Dusk rows directly.
- `PluginPickerPanel` and `PluginPickerHelpers` carry
  `std::vector<PluginDescriptor>` through callbacks and entries. Preserve the
  exact grouping, filtering, labels, ordering, scan/reopen flow, kind guard,
  and modal lifetime behavior. Native selection routes by
  `backend == Native` plus `formatName`; `location` and `pluginId` are passed
  separately. When a native handler exists, remove only the matching
  `JuceLegacy` LV2/VST3 rows. Browse-file behavior is unchanged.
- Session persistence moves the remaining JUCE-hosted slot reference from
  opaque XML to a structured `plugin_descriptor` object. Keep the existing
  `plugin_state` base64 key.
  - Track and aux models hold an optional Dusk descriptor, the state blob,
    and a raw legacy XML fallback only when an old value cannot be parsed.
  - Read `plugin_descriptor` first. If absent, read `plugin_desc_xml` and
    convert the old JUCE attributes once. Successful conversion clears the
    legacy string; the next save writes only `plugin_descriptor`.
  - A missing descriptor and missing legacy fallback means an empty slot.
  - A failed plugin restore retains descriptor and state byte-for-byte so an
    autosave/manual save cannot erase an unavailable plugin.
  - If legacy XML is malformed, retain and re-emit that raw
    `plugin_desc_xml` plus state instead of destroying user data.
  - Existing native CLAP/LV2/VST3/multisample keys, precedence, and
    unsupported-platform round-trip behavior do not change in H4.
- The legacy DuskMultisample migration consumes the converted Dusk
  descriptor (`formatName == "DuskMultisample"`, `location` = soundfont)
  before generic legacy-host restore. Preserve the H3 one-way migration and
  state bytes.
- `PluginSlot` exposes descriptor-named operations:
  `loadFromDescriptor`, `loadFromDescriptorAsync`,
  `getDescriptorForSave`, and descriptor-based restore. Cache the loaded
  descriptor so instrument classification, OOP saves, offline display, and
  Mac shell reload do not call `fillInPluginDescription` outside the private
  conversion points. OOP `LoadPlugin` may continue sending JUCE XML to the
  child in H4; generate it inside the legacy boundary. That control protocol
  and child processor path die in H6.
- Track clone/freeze replay stores the optional descriptor, raw legacy
  fallback, and state, and restores exactly the same triple on
  perform/undo/redo. `AudioEngine` save/restore and failure labels use the
  descriptor directly. `DuskStudioApp` pipeline diagnostics report the
  structured descriptor; the replace harness obtains and loads Dusk
  descriptors.
- `ScreenshotCapture` fake picker rows use `PluginDescriptor`; the rendered
  plugin-picker figure must remain visually unchanged. Update stale
  `KnownPluginList` / `PluginDescription` prose in `AppConfig.h`,
  `AudioSettingsPanel.cpp`, picker docs/comments, and the affected harness
  comments.
- H4 deliberately leaves these processor/editor surfaces for H6:
  - `PluginSlot` instance ownership, processing, state calls, parameter
    listeners, editor shell, and raw `AudioPluginInstance` access used by the
    UI;
  - `PluginManager`'s private JUCE format/instance creation;
  - `PluginHostMain --ipc-host`, `PluginIpc`, `RemotePluginConnection`, and
    the IPC host self-test load payload;
  - `ChannelStripComponent`, `AuxLaneComponent`, and
    `PlatformWindowing_Mac.mm` editor-owner lifecycles;
  - `MidiBindings` parameter-name lookup through the loaded processor;
  - the `MasteringChain` compatibility accessor and processor-path comments.
  Do not introduce a parameter facade or touch those lifecycles in this
  mechanical descriptor phase.
- No H1d, H2, H5, or H6 implementation; no donor edits; no DSP, routing,
  editor, or scan-sandbox policy changes; no `release/0.12` changes.

## Verify

- Add focused descriptor tests: default/missing optional fields, malformed
  rejection, every-field JSON round-trip, unknown format-name preservation,
  and lossless Dusk↔JUCE adapter conversion.
- Rewrite `tests/plugin_scan_protocol.cpp` around Dusk descriptors:
  multi-row and empty-success round-trips, noisy stdout, missing/truncated
  sentinels, malformed JSON, and exact `location`/`pluginId` preservation.
- Cover native cache JSON round-trip, legacy-XML one-way import, and
  stale-location pruning without loading a plugin.
- Cover track and aux session round-trip, old `plugin_desc_xml` migration,
  malformed-legacy preservation, failed/offline reference preservation,
  unsupported-platform native-key preservation, and clone action
  perform/undo/redo.
- App build completes with zero new warnings. Fresh configure/build commands
  include
  `-DDUSK_PLUGINS_PATH=/home/marc/projects/plugins-multicomp-core`.
- Full `ctest --output-on-failure` is green except the independently
  reproducible `ALSA seq backend does not report` environment flake. Do not
  waive any new failure.
- `tools/juce-gate.sh` never rises. Expected result is 181 -> 179, with
  `src/engine/NativeScanRows.h` and
  `src/engine/ipc/PluginScanProtocol.h` leaving the allowlist. Any different
  movement must be explained before commit; no new allowlist entry.
- Run `DUSKSTUDIO_RUN_SELFTEST=1` only on a private Xvfb display with
  `WAYLAND_DISPLAY` unset; require every printed self-test to pass.
- Run the instrument harness under the same private display with a freshly
  generated full-range synthetic `.sfz`; require the process output to print
  the harness result and `VERDICT: AUDIO PRESENT`. A missing/silent file is
  not a pass. Remove the temporary SFZ/sample directory afterward.
- Run the screenshot harness into a temporary output directory under private
  Xvfb. Inspect `pl-01-plugin-picker.png` visually for unchanged grouping,
  labels, spacing, clipping, and buttons. Do not overwrite tracked manual
  images.
- AI-slop sweep the complete diff: remove change-narration comments,
  duplicated conversion helpers, dead compatibility branches, and
  speculative abstractions. `git diff --check` must be clean.
- Audit `MANUAL.md`. H4 is internal plumbing and should need no manual prose
  change; if picker behavior or visible labels moved, stop and document the
  user-visible change plus updated screenshot instead of assuming it away.
- Commit only after the reviewer and fresh-eyes rounds are clear. Zero
  attribution trailers. Stop before push.

## Owed to Marc's bench

- Real installed-plugin smoke after the H4 commit: rescan, pick, save, reload,
  and reopen one VST3 instrument and one LV2/CLAP effect, plus save/reload with
  one plugin deliberately unavailable to confirm the offline reference
  survives. The existing OOP third-party host smoke remains owed.

## Resume phrase

"Hosting H4, branch dejuce/hosting-h4, spec
docs/dejuce-hosting-h4-descriptors.md — execute the descriptor/plumbing phase
only, then review, validate, and commit locally; do not push."
