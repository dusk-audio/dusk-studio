# Hosting tower H3 — DuskMultisample re-home (executable spec)

Status: **IMPLEMENTED 2026-07-27, committed on `dejuce/hosting-h3`,
awaiting review + push.** Verified: build zero new warnings, ctest 470/470,
gate 183 -> 181 (the two deleted format-wrapper files), instrument harness
AUDIO PRESENT through the native rung with a synthetic full-range .sfz,
selftest 36 PASS under Xvfb. Behavior fix shipped: pitch-wheel was passed
uncentred (0..16383) to sfizz in the JUCE-hosted build too - bends now work.
Branch `dejuce/hosting-h3`. One PR.
Parent plan: [dejuce-hosting-plan.md](dejuce-hosting-plan.md). Prior scout
2026-07-27 mapped the full coupling (session notes); re-verify line numbers
before editing.

## Goal

DuskMultisampleProcessor stops being a juce::AudioPluginInstance hosted
through the generic JUCE PluginSlot and becomes the fourth native rung:
an `hosting::INativeInstance` in a `NativeInsertSlot`-based slot on
ChannelStrip, loaded by file like today, state-compatible with existing
sessions. DuskMultisamplePluginFormat is deleted. The multisample test unit
drops its juce_audio_processors link.

## Locked decisions

- Processor implements `hosting::INativeInstance` directly: portLayout
  0-in/2-out + midiIn; activate/deactivate wrap the current
  prepareToPlay/releaseResources; `processBlock (const PortBuffers&)` wraps
  the existing body (sfizz try-lock, override drift, CC drain,
  sfizz_render_block) with `PortBuffers::midiIn` (dusk::MidiBuffer)
  replacing the juce::MidiBuffer argument — convert events at the seam,
  keep the internal logic untouched; saveState/loadState carry the SAME
  ValueTree-binary blob bytes as today's get/setStateInformation (old
  sessions must restore byte-identically); latency 0.
- New `NativeMultisampleSlot` via `NativeInsertSlot<Traits>` (mirror
  NativeClapSlot's 27-line traits shape). Instrument-only: it joins the
  MIDI-track instrument ladder in ChannelStrip::processAndAccumulate at
  precedence CLAP > LV2 > VST3 > multisample > (JUCE pluginSlot fallthrough
  until H5); load/unload/pending-restore/isNativeInstrument/latency wired
  exactly like the other three rungs. Aux lanes: NOT wired (instruments do
  not load on aux; verify nothing routes soundfonts there today).
- Discovery stays file-chooser-driven (canScanForPlugins was already
  false). PluginPickerHelpers' soundfont chooser + .bank.xml flow load into
  the new slot. No KnownPluginList rows; getInstrumentDescriptions loses
  its multisample entries (they only appeared post-load — verify no UI
  depends on them).
- Session compatibility: keep Track::pluginDescriptionXml /
  pluginStateBase64 keys for the OTHER plugins, but multisample save/restore
  moves to the native pending-restore scheme the other rungs use. Old
  sessions with a DuskMultisample description XML must still restore: the
  restore path detects pluginFormatName == "DuskMultisample" and routes
  file + state blob into the new slot (one-way migration, mirror how native
  CLAP restore-by-identifier works). New saves write the native identifier
  form.
- Editor: DuskMultisampleEditor drops the juce::AudioProcessorEditor base
  (plain juce::Component + dusk::Timer), constructed directly by
  ChannelStripComponent's editor-open path when the multisample rung is
  loaded (in-process, no embedding, same EmbeddedModal borrowed-body flow).
  AriaGuiComponent unchanged (already plain refs).
- DuskMultisamplePluginFormat.{h,cpp} deleted; PluginManager registration
  and the DUSKSTUDIO_HAS_MULTISAMPLE format include go; the define keeps
  gating the sources. PluginBackingCheck .sf2 rule stays.
- Selftest / hot-swap harness paths in DuskStudioApp that loadFromFile a
  soundfont must route to the new rung.
- MidiBindings: multisample exposes no params today (refreshParameterList
  empty) — nothing to wire; confirm no binding path assumes the slot.

## Verify

App build zero new warnings; gate must not rise (target: DROP — the format
wrapper files leave src/ entirely; count the allowlist delta honestly);
full ctest: multisample_state_roundtrip adapted to the INativeInstance
surface (blob compat cases preserved — an old-format blob must load), other
multisample tests untouched; tests/CMakeLists: multisample unit loses
juce_audio_processors (and juce_gui_basics only if the editor TU allows —
editor stays a juce Component, so gui_basics likely stays; be honest);
selftest with a .sfz under Xvfb incl. restore-by-description path;
screenshot harness unaffected (no multisample figure). AI-slop sweep.
MANUAL.md: behavior identical — confirm, no edit expected.

## Owed to Marc's bench

- Real SF2/SFZ session: load, play, save, reload, editor open, bounce with
  the instrument on a MIDI track.

## Resume phrase

"Hosting H3, branch dejuce/hosting-h3, spec docs/dejuce-hosting-h3-multisample.md
— continue at first unchecked item."
