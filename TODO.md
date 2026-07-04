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
