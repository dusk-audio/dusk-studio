# Dusk Studio — Manual Screenshot List

The 31 unique PNGs referenced by `MANUAL.md` (32 refs — `np-09-tape-strip.png`
is used twice), plus `np-12-notepad.png`, whose embed is pulled until it is
reshot. Capture target: `docs/images/<name>.png`. Filenames must match the
markers embedded in `MANUAL.md` exactly.

Most of these are produced automatically by the capture harness — run:

```bash
docs/capture-screenshots.sh         # builds (if needed), drives the app, writes docs/images/*.png
```

The harness drives the real app via `DUSKSTUDIO_CAPTURE_DIR` (see
`src/ui/ScreenshotCapture.*`). The **Auto** column marks what it produces. Only
the notepad is manual — it is a native window the harness cannot reach — with
notes below.

**Capture conventions (manual shots)**

- Linux: `gnome-screenshot -w` (window) or `import -window <id>` (ImageMagick).
- Window at the documented size when noted; default 1850×1080 (above the
  compact-mode threshold).
- PNG; shrink with `oxipng` / `pngquant` without quality loss.

---

## Quick Guide

| Filename                   | Manual | Auto | What to capture                                                     |
| -------------------------- | ------ | ---- | ------------------------------------------------------------------ |
| `qg-01-startup.png`        | L66    | ✅   | First-launch window, Startup dialog visible, audio device unset.   |
| `qg-02-audio-settings.png` | L74    | ✅   | `Settings → Audio` panel, a real interface selected.               |
| `qg-03-arm-track.png`      | L86    | ✅   | Channel strip 1 armed (ARM lit), input picked, IN on. RECORDING.   |
| `qg-04-record-rolling.png` | L94    | ✅   | Mid-record: meters lit, a region drawing into the tape strip.      |
| `qg-05-overdub.png`        | L102   | ✅   | Track 1 has a region; track 2 mid-record.                          |
| `qg-06-mixing-stage.png`   | L118   | ✅   | MIXING stage on strip 1 — send knobs replace the input block.      |
| `qg-07-bounce-dialog.png`  | L128   | ✅   | Bounce **file picker** at the session folder, then progress bar. (No format options — see note.) |

## Names and Functions of Parts (annotated — add callouts after capture)

| Filename                            | Manual | Auto | What to capture                                              |
| ----------------------------------- | ------ | ---- | ----------------------------------------------------------- |
| `np-01-main-window.png`             | L142   | ✅   | Full window, six horizontal bands.                          |
| `np-02-transport-bar.png`           | L155   | ✅   | Transport bar, full width.                                  |
| `np-03-channel-strip-mixing.png`    | L182   | ✅   | One full channel strip, MIXING stage (sends visible).       |
| `np-04-channel-strip-recording.png` | L209   | ✅   | Same strip, RECORDING stage (input block + ARM/IN/PRINT).   |
| `np-05-bus-strip.png`               | L220   | ✅   | One bus strip top-to-bottom.                                |
| `np-06-master-strip.png`            | L235   | ✅   | Master strip top-to-bottom.                                 |
| `np-07-aux-view.png`                | L250   | ✅   | One aux lane shown full-width.                              |
| `np-08-mastering-view.png`          | L263   | ✅   | Mastering chain.                                            |
| `np-09-tape-strip.png`              | L278, L1147 | ✅ | Tape strip with regions, a marker, and a loop bracket. (Reused at both lines.) |
| `np-10-region-editor.png`           | L293   | ✅   | Audio region editor modal.                                  |
| `np-11-piano-roll.png`              | L305   | ✅   | Piano roll modal.                                           |
| `np-12-notepad.png`                 | pulled | ❌   | Notepad chart: title, section markers, chords over syllables. |

## Chapter figures

| Filename                        | Manual | Auto | What to capture                                            |
| ------------------------------- | ------ | ---- | --------------------------------------------------------- |
| `rec-01-arm-multiple.png`       | L1042  | ✅   | Eight tracks armed simultaneously, RECORDING stage.       |
| `ed-04-region-editor-modal.png` | L1264  | ✅   | Region editor modal over a region with fade-in/out.       |
| `ed-05-piano-roll-full.png`     | L1316  | ✅   | Piano roll with notes, a CC ramp, scale highlight.        |
| `fx-01-eq.png`                  | L707   | ✅   | Channel EQ editor — HPF/LPF + 4 bands, curve-shaped.      |
| `fx-02-comp.png`                | L729   | ✅   | Channel compressor editor (VCA mode).                     |
| `fx-03-tape.png`                | L884   | ✅   | Master tape-machine editor (native panel).                |
| `mm-01-automation-modes.png`    | L1404  | ✅   | A fader's automation-mode label (READ / WRITE / TOUCH).   |
| `mm-02-mastering-chain.png`     | L965   | ✅   | Mastering chain with EQ, comp, and limiter engaged.       |
| `pl-01-plugin-picker.png`       | L1478  | ✅   | Plugin picker panel populated.                            |
| `pl-04-hw-insert.png`           | L1597  | ✅   | Hardware insert editor with I/O pickers and Ping button.  |
| `sync-01-mcu-bindings.png`      | L1712  | ✅   | MIDI Bindings panel populated with a few learned bindings.|
| `bnc-01-bounce-dialog.png`      | L1839  | ✅   | Bounce dialog (file picker + progress). (No format options — see note.) |
| `ts-02-plugin-offline.png`      | L2095  | ✅   | A plugin slot in the `⚠ (offline)` state (the harness stages a synthetic one). |

## Compact-mode strips (captured, not yet referenced by `MANUAL.md`)

The harness collapses the channel, bus, and master strips into compact mode
(EQ / COMP — plus TAPE / AUX — become section pills) and snapshots each. The
pills carry the same left-toggle / right-menu / double-click-editor grammar as
the full headers; these figures document that collapsed presentation.

| Filename                     | Manual | Auto | What to capture                                             |
| ---------------------------- | ------ | ---- | ---------------------------------------------------------- |
| `cs-01-channel-compact.png`  | —      | ✅   | One channel strip in compact mode (EQ / COMP / AUX pills). |
| `cs-02-bus-compact.png`      | —      | ✅   | One bus strip in compact mode (EQ / COMP pills).           |
| `cs-03-master-compact.png`   | —      | ✅   | Master strip in compact mode (EQ / COMP / TAPE pills).     |

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

- **`np-12-notepad.png`**: the notepad is a separate DPF/DGL native window, so
  JUCE's `createComponentSnapshot` cannot capture it — and DGL will not open
  under Xvfb, so this one has to be shot on a live desktop session rather than
  through `capture-screenshots.sh`.

  There is a single view, the chart; the ChordPro source is the session's
  `notepad.md` sidecar and there is no source tab in the window. The quickest
  way to stage the sample is to write it into `<session>/notepad.md` with the
  session closed, reopen the session, then open the notepad from the transport
  bar. Building it in the chart works too — **Section** for the markers,
  **Ctrl+K** for the chords.

  ```markdown
  # Little Angel

  [Verse]
  [Am]Sang this one too [F]fast at the [C]first session,
  [Am]keep the pocket [F]lazy and the [G]last word [C]late.

  [Chorus]
  [F]Little angel, [C]don't come [G]down for me [Am]yet.

  Reference: [rough mix](https://example.com/little-angel-rough)

  Warm the tape echo up before take 1, and double the hook an octave
  **down** on the *second* chorus.
  ```

  Capture with the caret in a lyric line. The shot should show the chord labels
  floating over their syllables, `[Verse]` and `[Chorus]` rendered as section
  labels rather than brackets, the Markdown link left intact beside them, and a
  footer reading two sections, four chords, and the detected key.

## Stale-caption fix log

The bounce dialog **no longer has format options** (v1 renders stereo 24-bit WAV
at the device rate, fixed 5 s tail). The capture for `qg-07` / `bnc-01` is the
**file picker → progress bar**, not a sample-rate / bit-depth picker. (Older
versions of this list said "sample rate 48k, bit depth 24" — that UI does not
exist.)

The checked-in `np-12-notepad.png` still shows the pre-0.13 prototype notepad —
raw Markdown textarea, old toolbar, a Close button — none of which exists any
more. Rather than ship a figure that contradicts the prose beside it, the embed
is pulled from `MANUAL.md` for 0.13: the PNG and this recipe stay put, and the
figure goes back under `## The notepad` as a one-line
`![Session notepad.](docs/images/np-12-notepad.png)` once it is reshot.

---

## Production checklist

- [ ] `docs/capture-screenshots.sh` produces every ✅ row into `docs/images/`.
- [ ] `np-12-notepad.png` captured by hand on a live desktop session.
- [ ] Annotated `np-*` shots have callouts overlaid (number + leader line).
- [ ] PNGs shrunk (`oxipng -o4` or `pngquant`).
- [ ] `MANUAL.pdf` rebuilds clean via `docs/build-pdf.sh` with all images present.
