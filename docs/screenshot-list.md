# Dusk Studio — Manual Screenshot List

The 30 unique PNGs referenced by `MANUAL.md` (31 refs — `np-09-tape-strip.png`
is used twice). Capture target: `docs/images/<name>.png`. Filenames must match
the markers embedded in `MANUAL.md` exactly.

Most of these are produced automatically by the capture harness — run:

```bash
docs/capture-screenshots.sh         # builds (if needed), drives the app, writes docs/images/*.png
```

The harness drives the real app via `DUSKSTUDIO_CAPTURE_DIR` (see
`src/ui/ScreenshotCapture.*`). The **Auto** column marks what it produces; the
rest are manual (transient states, popup menus, OS dialogs) with notes below.

**Capture conventions (manual shots)**

- Linux: `gnome-screenshot -w` (window) or `import -window <id>` (ImageMagick).
- Window at the documented size when noted; default 1850×1080 (above the
  compact-mode threshold).
- PNG; shrink with `oxipng` / `pngquant` without quality loss.

---

## Quick Guide

| Filename                   | Manual | Auto | What to capture                                                     |
| -------------------------- | ------ | ---- | ------------------------------------------------------------------ |
| `qg-01-startup.png`        | L62    | ✅   | First-launch window, Startup dialog visible, audio device unset.   |
| `qg-02-audio-settings.png` | L70    | ✅   | `Settings → Audio` panel, a real interface selected.               |
| `qg-03-arm-track.png`      | L82    | ✅   | Channel strip 1 armed (ARM lit), input picked, IN on. RECORDING.   |
| `qg-04-record-rolling.png` | L90    | ✅   | Mid-record: meters lit, a region drawing into the tape strip.      |
| `qg-05-overdub.png`        | L98    | ✅   | Track 1 has a region; track 2 mid-record.                          |
| `qg-06-mixing-stage.png`   | L114   | ✅   | MIXING stage on strip 1 — send knobs replace the input block.      |
| `qg-07-bounce-dialog.png`  | L124   | ✅   | Bounce **file picker** at the session folder, then progress bar. (No format options — see note.) |

## Names and Functions of Parts (annotated — add callouts after capture)

| Filename                            | Manual | Auto | What to capture                                              |
| ----------------------------------- | ------ | ---- | ----------------------------------------------------------- |
| `np-01-main-window.png`             | L138   | ✅   | Full window, six horizontal bands.                          |
| `np-02-transport-bar.png`           | L151   | ✅   | Transport bar, full width.                                  |
| `np-03-channel-strip-mixing.png`    | L178   | ✅   | One full channel strip, MIXING stage (sends visible).       |
| `np-04-channel-strip-recording.png` | L205   | ✅   | Same strip, RECORDING stage (input block + ARM/IN/PRINT).   |
| `np-05-bus-strip.png`               | L216   | ✅   | One bus strip top-to-bottom.                                |
| `np-06-master-strip.png`            | L231   | ✅   | Master strip top-to-bottom.                                 |
| `np-07-aux-view.png`                | L246   | ✅   | One aux lane shown full-width.                              |
| `np-08-mastering-view.png`          | L259   | ✅   | Mastering chain.                                            |
| `np-09-tape-strip.png`              | L274, L1000 | ✅ | Tape strip with regions, a marker, and a loop bracket. (Reused at both lines.) |
| `np-10-region-editor.png`           | L288   | ✅   | Audio region editor modal.                                  |
| `np-11-piano-roll.png`              | L300   | ✅   | Piano roll modal.                                           |

## Chapter figures

| Filename                        | Manual | Auto | What to capture                                            |
| ------------------------------- | ------ | ---- | --------------------------------------------------------- |
| `rec-01-arm-multiple.png`       | L903   | ✅   | Eight tracks armed simultaneously, RECORDING stage.       |
| `ed-04-region-editor-modal.png` | L1102  | ✅   | Region editor modal over a region with fade-in/out.       |
| `ed-05-piano-roll-full.png`     | L1152  | ✅   | Piano roll with notes, a CC ramp, scale highlight.        |
| `fx-01-eq.png`                  | EQ §   | ✅   | Channel EQ editor — HPF/LPF + 4 bands, curve shaped.      |
| `fx-02-comp.png`                | Comp § | ✅   | Channel compressor editor (VCA mode).                     |
| `mm-01-automation-modes.png`    | L1238  | ✅   | A fader's automation-mode label (READ / WRITE / TOUCH).   |
| `mm-02-mastering-chain.png`     | L835   | ✅   | Mastering chain with EQ, comp, and limiter engaged.       |
| `pl-01-plugin-picker.png`       | L1296  | ✅   | Plugin picker panel populated.                            |
| `pl-04-hw-insert.png`           | L1392  | ✅   | Hardware insert editor with I/O pickers and Ping button.  |
| `sync-01-mcu-bindings.png`      | L1492  | ✅   | MIDI Bindings panel populated with a few learned bindings.|
| `sync-02-mtc-rates.png`         | L1430  | ⚠️   | MTC frame-rate dropdown **open** showing all four rates.  |
| `ses-02-session-folder.png`     | L1556  | ❌   | OS file browser showing the session folder contents.      |
| `bnc-01-bounce-dialog.png`      | L1617  | ✅   | Bounce dialog (file picker + progress). (No format options — see note.) |
| `ts-02-plugin-offline.png`      | L1815  | ⚠️   | A plugin slot showing the `⚠ (offline)` state.            |

## I/O config popup (captured, not yet referenced by `MANUAL.md`)

The harness renders the track I/O config popup in all three modes. No
`MANUAL.md` reference is wired yet — pick one after visual review.

| Filename                        | Manual | Auto | What to capture                                           |
| ------------------------------- | ------ | ---- | --------------------------------------------------------- |
| `io-01-input-config-mono.png`   | —      | ✅   | I/O popup, Mono mode: title + Mode / Input captions.      |
| `io-02-input-config-stereo.png` | —      | ✅   | I/O popup, Stereo mode: Input L / Input R rows.           |
| `io-03-input-config-midi.png`   | —      | ✅   | I/O popup, MIDI mode: port / channel / out + activity LED.|

---

## Manual-only shots (notes)

- **`sync-02-mtc-rates.png`** (⚠️): the frame-rate list is a native popup menu;
  `createComponentSnapshot` can't grab it. Open `Settings → MIDI sync`, click the
  MTC rate dropdown, capture the window with `gnome-screenshot -w` while the menu
  is open.
- **`ses-02-session-folder.png`** (❌): this is the OS file manager, not a Dusk
  Studio window. Open the session folder in Files/Nautilus and screenshot it.
- **`ts-02-plugin-offline.png`** (⚠️): needs a session referencing a plugin that
  isn't installed on this box. Either load such a session and grab the strip, or
  let the harness stage a synthetic offline slot (best-effort).

## Stale-caption fix log

The bounce dialog **no longer has format options** (v1 renders stereo 24-bit WAV
at the device rate, fixed 5 s tail). The capture for `qg-07` / `bnc-01` is the
**file picker → progress bar**, not a sample-rate / bit-depth picker. (Older
versions of this list said "sample rate 48k, bit depth 24" — that UI does not
exist.)

---

## Production checklist

- [ ] `docs/capture-screenshots.sh` produces every ✅ row into `docs/images/`.
- [ ] Manual rows (⚠️ / ❌) captured by hand.
- [ ] Annotated `np-*` shots have callouts overlaid (number + leader line).
- [ ] PNGs shrunk (`oxipng -o4` or `pngquant`).
- [ ] `MANUAL.pdf` rebuilds clean via `docs/build-pdf.sh` with all images present.
