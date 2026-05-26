# Dusk Studio — Manual Screenshot List

Capture target: `docs/images/<NN-name>.png` (PNG, scaled 1:1, no window chrome unless noted). Filenames must match the markers embedded in `MANUAL.md`.

**Capture conventions**

- macOS: `Cmd-Shift-4`, then `Space`, then click the window → captures with shadow; or drag a region for no shadow.
- Linux: `gnome-screenshot -w` or `flameshot gui`.
- Drop the `.shadow.` suffix from macOS shots if you prefer no-shadow; the manual references the bare filename either way.
- Window must be at the documented size when noted (resize first, then capture). Default: 1850×1080 (above the compact-mode threshold).
- Sample session for repeatable shots: keep a `screenshot-session.dusk` checked into `docs/sessions/` once captured — 8 channels with a small region on each, one bus assignment, one aux send.

---

## Chapter 2 — Quick Guide (narrative tutorial)

| #   | Filename                   | What to capture                                                                | Setup notes                                                    |
| --- | -------------------------- | ------------------------------------------------------------------------------ | -------------------------------------------------------------- |
| 1   | `qg-01-startup.png`        | First-launch window, blank session, full window.                               | Fresh session. Audio device unset.                             |
| 2   | `qg-02-audio-settings.png` | `Settings → Audio` panel open.                                                 | Real interface listed; sample rate and block size at defaults. |
| 3   | `qg-03-arm-track.png`      | Channel strip 1 armed (red ARM lit), input source `1: <interface input>`.      | RECORDING stage. Crop to one strip.                            |
| 4   | `qg-04-record-rolling.png` | Transport mid-record, level meters lit, single region drawing into tape strip. | Tape strip expanded; ~2 seconds in.                            |
| 5   | `qg-05-overdub.png`        | Track 1 has a region; track 2 armed and recording.                             | Two tracks visible.                                            |
| 6   | `qg-06-mixing-stage.png`   | MIXING stage active; channel strip 1 showing send knobs replacing input block. | Sends 1 and 2 at noon.                                         |
| 7   | `qg-07-bounce-dialog.png`  | Bounce dialog open.                                                            | File picker at session folder; sample rate 48k, bit depth 24.  |

## Chapter 3 — Names and Functions of Parts (annotated reference)

| #   | Filename                            | What to capture                                                                                                                     | Annotation count                                                                                                                                                       |
| --- | ----------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 8   | `np-01-main-window.png`             | Full main window with all six horizontal bands visible (menu / stage selector / bank selector / transport / tape toggle / console). | 6 callouts (one per band)                                                                                                                                              |
| 9   | `np-02-transport-bar.png`           | Just the transport bar, full width.                                                                                                 | 15 callouts (Stop, Rewind, Play, Forward, Record, Loop, Punch, Virtual kbd, Metronome, C/I, BPM, TAP, Time sig, Clock, Tuner) plus right-edge cluster (SNAP, −/+/Fit). |
| 10  | `np-03-channel-strip-mixing.png`    | One full channel strip top-to-bottom, MIXING stage (sends visible).                                                                 | 14 callouts (name, sends 1–4, insert, HPF, LPF, EQ, comp, pan, fader, mute, solo, phase, meter)                                                                        |
| 11  | `np-04-channel-strip-recording.png` | Same strip, RECORDING stage (input block + ARM/IN/PRINT).                                                                           | 4 callouts (input mode, source, ARM/IN/PRINT row, activity LED)                                                                                                        |
| 12  | `np-05-bus-strip.png`               | One bus strip top-to-bottom.                                                                                                        | 8 callouts (name, EQ LF/MID/HF, comp, pan, fader, mute, solo, meters)                                                                                                  |
| 13  | `np-06-master-strip.png`            | Master strip top-to-bottom.                                                                                                         | 9 callouts (Pultec EQ, bus comp, tape sat, fader, mono, peak meters, VU meters, GR meter)                                                                              |
| 14  | `np-07-aux-view.png`                | AUX stage with one lane shown — return strip + insert chain + sources panel.                                                        | 6 callouts (selector buttons, name, mute, fader, insert slot, sources panel)                                                                                           |
| 15  | `np-08-mastering-view.png`          | MASTERING stage with a loaded file and the chain visible.                                                                           | 8 callouts (file picker, transport, waveform, 5-band EQ, comp, limiter, loudness panel, export button)                                                                 |
| 16  | `np-09-tape-strip.png`              | Tape strip expanded, two tracks with a few regions each, a marker, a loop bracket.                                                  | 7 callouts (ruler, region, region edge handle, marker, loop bracket, punch bracket, snap toggle)                                                                       |
| 17  | `np-10-region-editor.png`           | Audio region editor modal open over a region with a fade-in and fade-out.                                                           | 5 callouts (waveform, fade handle, trim handle, gain slider, edit-mode toolbar)                                                                                        |
| 18  | `np-11-piano-roll.png`              | Piano roll modal open with notes + a CC lane.                                                                                       | 6 callouts (keyboard, note grid, selected note, velocity strip, CC lane, scale highlight)                                                                              |

## Chapter 4 — Preparation

| #   | Filename                   | What to capture                                     | Notes                                                      |
| --- | -------------------------- | --------------------------------------------------- | ---------------------------------------------------------- |
| 19  | `prep-01-audio-device.png` | Audio Device panel, every section visible.          | Same as #2 but full-panel scroll.                          |
| 20  | `prep-02-mcu-bindings.png` | MCU MIDI port pickers + MIDI Bindings panel teaser. | Hardware optional; show empty bindings list if no surface. |
| 21  | `prep-03-midi-sync.png`    | MIDI sync section with chase + emit toggles.        |                                                            |

## Chapter 5 — Session Management

| #   | Filename                    | What to capture                                                                                        | Notes                                      |
| --- | --------------------------- | ------------------------------------------------------------------------------------------------------ | ------------------------------------------ |
| 22  | `ses-01-startup-dialog.png` | Startup dialog (new / open recent).                                                                    | First launch with a populated recent list. |
| 23  | `ses-02-session-folder.png` | OS file browser showing session folder contents (`session.json`, `audio/`, `bounces/`, autosave file). | Real session on disk.                      |

## Chapter 6 — Recording

| #   | Filename                      | What to capture                                                                   | Notes                                                  |
| --- | ----------------------------- | --------------------------------------------------------------------------------- | ------------------------------------------------------ |
| 24  | `rec-01-arm-multiple.png`     | Eight tracks armed simultaneously, RECORDING stage.                               |                                                        |
| 25  | `rec-02-monitor-print.png`    | Single strip zoomed; IN lit, PRINT off, then IN lit + PRINT on.                   | Two-shot composite.                                    |
| 26  | `rec-03-count-in.png`         | Transport with C/I lit + metronome lit, 1-bar countdown ticking.                  | Mid-countdown.                                         |
| 27  | `rec-04-punch-brackets.png`   | Tape strip with punch in/out brackets set, PUNCH transport button lit.            |                                                        |
| 28  | `rec-05-loop-recording.png`   | Loop bracket set, transport rolling, take history > 1 on a region.                |                                                        |
| 29  | `rec-06-take-history.png`     | Region right-click menu open showing the take-history submenu with several takes. |                                                        |
| 30  | `rec-07-recording-errors.png` | Recording errors dialog with one or two per-track failures listed.                | Synthesise by yanking interface mid-record if you can. |

## Chapter 7 — Recorder Functions

| #   | Filename                       | What to capture                                    | Notes |
| --- | ------------------------------ | -------------------------------------------------- | ----- |
| 31  | `rf-01-markers.png`            | Tape strip with three markers (different colours). |       |
| 32  | `rf-02-metronome-settings.png` | Metronome right-click popover.                     |       |
| 33  | `rf-03-tuner.png`              | Tuner overlay open, listening to an input.         |       |

## Chapter 8 — Region & Take Editing

| #   | Filename                        | What to capture                                               | Notes           |
| --- | ------------------------------- | ------------------------------------------------------------- | --------------- |
| 34  | `ed-01-region-trim.png`         | Region with the trim handle being dragged.                    | Cursor visible. |
| 35  | `ed-02-region-context.png`      | Right-click menu over a region (all entries visible).         |                 |
| 36  | `ed-03-take-cycle.png`          | Region showing take cycling indicator.                        |                 |
| 37  | `ed-04-region-editor-modal.png` | Full region editor modal.                                     |                 |
| 38  | `ed-05-piano-roll-full.png`     | Full piano roll modal with notes + CC ramp + scale highlight. |                 |
| 39  | `ed-06-import-picker.png`       | Multi-import target picker mid-drop with 4 files.             |                 |

## Chapter 9 — Built-in Effects

| #   | Filename                  | What to capture                                                     | Notes                             |
| --- | ------------------------- | ------------------------------------------------------------------- | --------------------------------- |
| 40  | `fx-01-eq-curve.png`      | Channel EQ engaged with all four bands shaped, on a strip selected. | EQ editor visible if it pops out. |
| 41  | `fx-02-comp-opto.png`     | Channel comp in Opto mode, GR meter showing 4–6 dB reduction.       |                                   |
| 42  | `fx-03-comp-fet.png`      | FET mode, ratio set to "All".                                       |                                   |
| 43  | `fx-04-comp-vca.png`      | VCA mode with sidechain HPF on.                                     |                                   |
| 44  | `fx-05-bus-comp.png`      | Bus compressor in auto-release.                                     |                                   |
| 45  | `fx-06-master-pultec.png` | Master Pultec with LF boost+atten engaged.                          |                                   |
| 46  | `fx-07-tape-machine.png`  | Tape machine modal editor (right-click → Open editor).              |                                   |

## Chapter 10 — Mixing & Mastering

| #   | Filename                     | What to capture                                                                   | Notes                 |
| --- | ---------------------------- | --------------------------------------------------------------------------------- | --------------------- |
| 47  | `mm-01-automation-modes.png` | Fader with automation mode label cycling (READ / WRITE / TOUCH).                  | Three-shot composite. |
| 48  | `mm-02-mastering-chain.png`  | Full mastering chain with EQ + comp + limiter all engaged, mid-playback.          |                       |
| 49  | `mm-03-lufs-readout.png`     | Loudness panel reading integrated LUFS + true peak after a full pass.             |                       |
| 50  | `mm-04-platform-target.png`  | Streaming-platform preset picker with one preset selected, target-met chip green. |                       |

## Chapter 11 — Plugins & Hardware Inserts

| #   | Filename                  | What to capture                                                         | Notes                                     |
| --- | ------------------------- | ----------------------------------------------------------------------- | ----------------------------------------- |
| 51  | `pl-01-plugin-picker.png` | Plugin picker panel with a scan in progress.                            |                                           |
| 52  | `pl-02-plugin-loaded.png` | Strip with a 3rd-party plugin loaded into the insert slot, editor open. |                                           |
| 53  | `pl-03-oop-stalled.png`   | Strip showing `(stalled)` on a plugin slot after auto-bypass trigger.   | Hard to repro on demand; defer if needed. |
| 54  | `pl-04-hw-insert.png`     | Hardware insert editor with input/output channel pickers + Ping button. |                                           |
| 55  | `pl-05-ping-result.png`   | Hardware insert after a successful ping (latency value filled in).      |                                           |

## Chapter 12 — Sync to External Gear

| #   | Filename                   | What to capture                                            | Notes |
| --- | -------------------------- | ---------------------------------------------------------- | ----- |
| 56  | `sync-01-mcu-bindings.png` | MIDI Bindings panel populated with a few learned bindings. |       |
| 57  | `sync-02-mtc-rates.png`    | MTC rate dropdown open showing all four rates.             |       |

## Chapter 13 — Bouncing & Exporting

| #   | Filename                      | What to capture                            | Notes |
| --- | ----------------------------- | ------------------------------------------ | ----- |
| 58  | `bnc-01-bounce-dialog.png`    | Same as #7 but documented in detail.       |       |
| 59  | `bnc-02-mastering-export.png` | Export master dialog from MASTERING stage. |       |

## Chapter 14 — Various Other Functions

| #   | Filename                      | What to capture                                      | Notes |
| --- | ----------------------------- | ---------------------------------------------------- | ----- |
| 60  | `var-01-virtual-keyboard.png` | Virtual MIDI keyboard overlay.                       |       |
| 61  | `var-02-multisample.png`      | Multi-sampler editor with a loaded `.sfz` or `.sf2`. |       |
| 62  | `var-03-self-test.png`        | Self-test panel mid-run / passed.                    |       |

## Chapter 15 — Troubleshooting

| #   | Filename                   | What to capture                                               | Notes                                         |
| --- | -------------------------- | ------------------------------------------------------------- | --------------------------------------------- |
| 63  | `ts-01-plugin-crashed.png` | "Plugin crashed — reload to recover" alert.                   | OOP build, induce by killing the plugin host. |
| 64  | `ts-02-plugin-offline.png` | Slot label showing `⚠ <Name> (offline)`.                      | Load a session referencing a missing plugin.  |
| 65  | `ts-03-ping-failed.png`    | Hardware insert showing "Ping failed — check level / cables". |                                               |

## Chapter 16 — Messages (one per documented dialog)

To be enumerated once the Messages chapter is drafted. Approximate target: 10–15 alert / confirmation dialogs.

---

## Production checklist

- [ ] All filenames above exist under `docs/images/`.
- [ ] Annotated screenshots have callouts overlaid (number + leader line).
- [ ] Each capture matches the documented window size and stage.
- [ ] PNG; reasonable compression (`pngquant` or `oxipng` to shrink without quality loss).
- [ ] `MANUAL.pdf` rebuilds clean via `docs/build-pdf.sh`.
