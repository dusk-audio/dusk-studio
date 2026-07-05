# Dusk Studio — TODO

## General Settings panel — future candidates

The General section (Settings → General) ships with UI scale, tape-strip
default, follow-playhead default, stop behavior, and scan-on-startup.
Candidates for later additions:

- Theme accent colour
- Default zoom factor
- Default snap-to-grid state
- Autosave interval

## LV2 file-backed plugin state

The native LV2 host serializes control values and in-memory
`state:interface` blobs only; state a plugin keeps in files of its own
(sample banks, impulse responses) is not captured (see MANUAL, "Native
plugin hosting"). A full fix needs per-slot state directories under the
session, `LV2_State_Make_Path`/`Map_Path` features at save/restore, and
Save As consolidation of the state tree.

## MIDI-binding soft takeover (pickup)

Absolute CC bindings snap the target on first touch (fader at +12 jumps
to −90 on a CC 0). A pickup mode needs per-binding "passed through the
current value" state on the apply path plus a current-value read-back
per continuous target (the apply switch only writes), and a per-binding
mode in the learn menu / serializer. Relative (two's-complement) CC
support fits the same slot.

## Loop recording take stacking

Recording while looping keeps the playhead linear (wrap is deliberately
playback-only — see the transport-wrap comment in
AudioEngine::audioDeviceIOCallbackWithContext). Cycle recording that
stacks one take per pass onto previousTakes wants: per-pass region
splitting at the wrap, MIDI capture segmentation, and badge-UI cycling
of the stacked takes (the take-history model already fits).

## Long-import responsiveness

fileimport::importAudio now streams in bounded chunks (no more
whole-file allocations), but a multi-fragment DP import and SF2/SFZ
multisample loads still run synchronously on the message thread
(seconds to minutes for big songs / GM banks). Both want a background
thread + progress modal (FreezeDialog is the pattern); the SF2 editor
already uses SafePointer guards throughout, so marshalling is the easy
part.

## Session sample-rate: offline resample on mismatch

Loading a session whose rate the device can't run now warns loudly (and
tries to switch the device). The complete fix is an offline batch
resample: rewrite every region/take/freeze WAV at the new rate and
rescale every sample-domain position (regions, automation points,
markers, loop/punch, tempo-map anchors) in one undoable pass.
