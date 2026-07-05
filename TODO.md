# Dusk Studio — TODO

## General Settings panel — future candidates

The General section (Settings → General) ships with UI scale, tape-strip
default, follow-playhead default, stop behavior, and scan-on-startup.
Candidates for later additions:

- Theme accent colour
- Default zoom factor
- Default snap-to-grid state
- Autosave interval

## MIDI-binding pickup — per-binding mode + relative CC

Soft takeover ships as a global per-machine toggle (Settings → General).
Possible extensions: a per-binding Pickup/Jump mode in the learn menu
(needs a serializer field), and relative (two's-complement) CC support.

## Loop recording take stacking

Recording while looping keeps the playhead linear (wrap is deliberately
playback-only — see the transport-wrap comment in
AudioEngine::audioDeviceIOCallbackWithContext). Cycle recording that
stacks one take per pass onto previousTakes wants: per-pass region
splitting at the wrap, MIDI capture segmentation, and badge-UI cycling
of the stacked takes (the take-history model already fits).

## Session sample-rate: offline resample on mismatch

Loading a session whose rate the device can't run now warns loudly (and
tries to switch the device). The complete fix is an offline batch
resample: rewrite every region/take/freeze WAV at the new rate and
rescale every sample-domain position (regions, automation points,
markers, loop/punch, tempo-map anchors) in one undoable pass.
