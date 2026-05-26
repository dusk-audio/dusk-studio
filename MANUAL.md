---
title: "Dusk Studio — User Manual"
subtitle: "A portastudio for the desktop"
author: "Dusk Audio"
date: "2026"
documentclass: report
geometry: margin=1in
fontsize: 11pt
toc: true
toc-depth: 3
numbersections: true
colorlinks: true
---

\newpage

# About this manual

Dusk Studio is a deliberately constrained, portastudio-style digital audio workstation. It records up to 24 tracks of audio or MIDI, runs every track through a fixed signal chain inspired by classic analog consoles, and bounces a finished mix to a stereo WAV. It does not host a thousand plug-ins on a thousand tracks. It does not let you draw automation curves with a pencil tool. It does not have tabbed views or hidden panels. Everything is on screen, all the time.

This manual covers Dusk Studio v1. It is written for musicians and engineers who want a reference for every control, every shortcut, and every workflow the program supports. Read it cover to cover the first time and skim by section thereafter.

## What's in the box

Dusk Studio includes:

- 24 tracks of audio or MIDI recording, organised as three banks of 8.
- A fixed channel signal chain: phase, insert, HPF, LPF, 4-band EQ, compressor (Opto/FET/VCA), aux sends, pan, fader.
- Four aux return lanes, each with one plugin or hardware insert slot.
- Four mix buses, each with a 3-band EQ and SSL-style bus compressor.
- A master bus with tape saturation, Pultec-style EQ, bus compressor, and mono-sum check.
- A dedicated mastering stage with 5-band digital EQ, multiband compressor, brick-wall limiter, and BS.1770 loudness metering.
- VST3, LV2, and AU plugin hosting, with optional out-of-process sandboxing for crash isolation.
- External hardware insert per channel and per aux, with automatic latency measurement.
- A multi-sampler (sfizz) that plays `.sfz` and `.sf2` SoundFont files on MIDI tracks.
- MIDI Clock and MIDI Time Code chase and emit.
- Mackie Control surface support (tested against Tascam DP-24SD).
- A piano roll for MIDI editing and an audio region editor with non-destructive trim, fade, and gain.
- Session save/load with automatic 30-second autosave, atomic-write protection, and a 20-take history per region.

## What it deliberately does not have

- No more than 24 tracks. The limit is fixed.
- No reorderable signal chain. EQ is always before the compressor; the compressor is always before the fader. No chains of seventeen plugins on a single channel.
- No sample-level audio editing. Regions can be moved, split, trimmed, faded, normalised, and gained. The waveform itself is not editable.
- No automation curves. Fader and pan respond to Write/Touch/Read in a console style; you ride controls, the program records the ride.
- No preferences sprawl. The only preferences panel is for audio device configuration.
- No tabs, no hidden panes, no project-explorer-trees-within-trees.

If a feature would not have existed on a $2000 hardware portastudio, it is not in Dusk Studio.

\newpage

# Getting started

## System requirements

- **Linux**: PipeWire (recommended) or JACK or ALSA. JUCE 8's JACK backend drives PipeWire transparently.
- **macOS**: 14.4 (Sonoma) or later for the out-of-process plugin sandbox; older macOS still runs plugins in-process.
- **Windows**: Windows 10 or later, ASIO driver recommended.

Any modern multi-core CPU (Intel, AMD, or Apple Silicon) is sufficient for a 24-track session at 48 kHz. The compressor and EQ on every channel are oversampled when the global oversampling factor is raised; expect roughly 2–3× CPU on the mix engine when you select 4× oversampling.

## First launch

On first launch Dusk Studio opens a blank session named `Untitled`. The window is divided, top to bottom, into:

- A thin menu bar (File and Settings).
- A row of large coloured buttons for the four stages: **RECORDING**, **MIXING**, **AUX**, **MASTERING**.
- A bank selector (only visible when the window is too narrow to show all 16 channel strips at once).
- The transport bar.
- The tape strip (the timeline view), collapsed by default.
- The console view, showing 16 channel strips, 4 buses, and the master strip.

Press **RECORDING** to focus the channel strips on input routing and arm controls. Press **MIXING** to focus them on send levels and inserts. Press **AUX** to see the four aux lanes, their inserts, and their send sources. Press **MASTERING** to load a finished mix and run it through the mastering chain.

## Configuring audio

Open **Settings → Audio…** to choose your audio device. The panel is divided into six sections.

### Audio

- **Device**: lists every backend driver Dusk Studio detected. On Linux, PipeWire and JACK appear as "JACK"; ALSA devices appear separately.
- **Sample rate**: any rate the device supports. 44.1 kHz, 48 kHz, 88.2 kHz, 96 kHz are common.
- **Block size**: smaller blocks give lower latency but cost more CPU per sample. 256 or 512 samples is a good starting point.
- **Periods (Linux/ALSA only)**: how many buffers the ALSA driver keeps in flight. Two is the lowest-latency safe value; three or more is more robust on a busy machine.
- **Rescan devices**: re-enumerates every backend, useful if you plugged in a USB interface after launch.

### Control surface

- **MCU input** and **MCU output**: pick the MIDI ports your Mackie Control surface is connected to. Selecting both enables motorised-fader feedback and LED state mirroring.

### MIDI bindings

A single button opens the **MIDI Bindings** panel, which lists every CC-to-control mapping you have learned. You can remove any binding, clear them all, or export and import a binding preset as JSON.

### MIDI sync

- **Sync source input**: the MIDI input port to chase Clock or MTC from. Choose `(none)` to disable chase.
- **Chase transport (Start/Stop)**: enables transport chase on MIDI Start (FA) and Stop (FC).
- **Sync output**: the MIDI output port to emit Clock or MTC on.
- **Emit clock**: emit 24 PPQN MIDI Clock when Dusk Studio is playing.
- **Chase MTC**: chase SMPTE position from incoming MIDI Time Code quarter-frame messages.
- **Emit MTC**: emit MTC quarter-frame messages.
- **MTC frame rate**: 24, 25, 29.97 drop-frame, or 30 frames per second.

### General

- **UI scale**: a global zoom factor for the entire interface. Restart Dusk Studio after changing this for best results.
- **Expand tape strip by default**: show the tape strip on every session open.
- **Scan plugins on startup**: re-run the plugin scanner every time Dusk Studio launches. Off by default; large plugin collections take 10–30 seconds to scan.

### Advanced

- **Effect oversampling**: 1×, 2×, or 4×. Raises the internal sample rate of every channel EQ and compressor, every bus EQ and compressor, the master EQ and compressor, and the mastering EQ and compressor. Reduces aliasing on saturation stages at the cost of CPU. Tape saturation has its own internal oversampling controlled separately.
- **Run self-test**: runs Dusk Studio's headless audio engine against a synthetic test signal and reports pass/fail.

\newpage

# The interface

## Window layout

The window is structured as a stack of horizontal bands.

```
┌───────────────────────────────────────────────────────┐
│ File   Settings              Session name      CPU…   │  Menu bar
├───────────────────────────────────────────────────────┤
│  RECORDING   MIXING   AUX   MASTERING                 │  Stage selector
├───────────────────────────────────────────────────────┤
│  1-8   9-16                                           │  Bank selector
├───────────────────────────────────────────────────────┤
│ ◀◀  ▶  ▶▶  ●  ⟳  ◉   CLK ♩=120 4/4 00:01:23  SNAP ⊕⊖│  Transport bar
├───────────────────────────────────────────────────────┤
│  ▾ SUMMARY                                            │  Tape strip toggle
├───────────────────────────────────────────────────────┤
│                                                       │
│      [ ConsoleView, AuxView, or MasteringView ]       │
│                                                       │
└───────────────────────────────────────────────────────┘
```

Modal dialogs (audio settings, plugin picker, region editor, piano roll, import target picker) appear centered with a dimmed backdrop behind them. Press **Esc** or click outside the modal to dismiss.

## The four stages

Only one stage is visible at a time, but the same engine drives all four. Switching stages is purely a UI change; audio keeps flowing.

- **RECORDING** shows each channel strip's input source, arm button, monitor toggle, and "print" toggle (for whether EQ and compression are committed to the recorded file or kept live).
- **MIXING** replaces the input block with the channel's four aux send knobs. Inserts and EQ stay on screen.
- **AUX** swaps the console view for the four aux return lanes, with a full-width view of each lane's plugin chain.
- **MASTERING** swaps the console view for the mastering chain, including a file picker for loading a finished mix.

Switching into or out of MASTERING force-stops the transport. The mix engine and the mastering engine cannot run at the same time.

## The transport bar

From left to right:

- **Stop** (■). Halts playback or recording, returns the playhead to bar 1.
- **Rewind** (◀◀). Brief press jumps to the previous marker; if there is no previous marker, jumps to bar 1. Hold for more than 180 milliseconds to scrub backwards at 10× speed.
- **Play** (▶). Toggles play. If loop is enabled and the playhead is outside the loop region, the playhead snaps to the loop start before playback begins.
- **Forward** (▶▶). Brief press jumps to the next marker (no overshoot past the last one). Hold to scrub forward at 10× speed.
- **Record** (●). Toggles record. Requires at least one track armed.
- **Loop** (⟳). Toggles loop playback.
- **Punch** (◉). Toggles punch recording. Right-click to set pre-roll and post-roll seconds.
- **Virtual keyboard** (⌨). Opens an on-screen MIDI keyboard.
- **Metronome** (♩). Toggles the click. Right-click for click settings.
- **C/I**. Toggles count-in (one bar of click before record starts).
- **BPM**. Click to type a tempo; drag up or down to nudge. Changing BPM while MIDI regions exist prompts for confirmation, because tick positions are interpreted relative to BPM.
- **TAP**. Click on each beat; Dusk Studio averages the last four intervals over a two-second window and sets the tempo.
- **Time signature**. Click to choose from common signatures or enter a custom one.
- **Clock display**. Shows the current playhead position. Right-click to flip between **Bars.Beats.Ticks** (e.g. `5.2.120`) and **mm:ss.mmm** (e.g. `01:23.456`).
- **Tuner**. Opens a chromatic tuner that listens to the selected input.
- **▾ SUMMARY / ▴ TAPE**. Collapses or expands the tape strip below the console.

On the right end of the transport bar:

- **SNAP**. Global grid snap toggle. When on, region drags, trims, and pastes snap to the snap denomination set in the edit-mode toolbar.
- **−** / **+** / **Fit**. Timeline zoom out, in, and fit-to-window.

In compact mode (window narrower than 1850 pixels), labels shorten: `SNAP` becomes `S`, `SUMMARY` becomes `▾`, and the time-format toggle hides — right-click the clock display to flip format instead.

\newpage

# The channel strip

Every track is processed by an identical channel strip. The order is fixed; you cannot reorder blocks. From top to bottom of the signal flow:

```
input → phase invert → insert (plugin OR hardware) → HPF → LPF →
4-band EQ → compressor → aux sends → pan → fader → mute/solo gate → bus assigns → master
```

This is the order the audio actually flows. On screen, controls are arranged for ergonomics — the fader is at the bottom, the EQ in the middle — but the underlying chain never changes.

## Track name and colour

Click the name label at the top of the strip to rename the track. Right-click to choose a colour from the 12-hue palette or open a custom colour picker. The colour appears as the strip's accent and on every region that track owns in the tape strip.

## Input block (RECORDING stage)

This block is only visible in the RECORDING stage. In other stages it collapses into a single small **I/O** button you can click to reopen it.

- **Mode**: **Mono**, **Stereo**, or **MIDI**. Determines whether the track records one audio channel, two audio channels, or MIDI events.
- **Input** (Mono mode): the audio device input to record from. The default `−2: follow track index` means track 1 reads device input 1, track 2 reads device input 2, and so on. Choose a specific input to override.
- **Input L / Input R** (Stereo mode): the left and right device inputs.
- **MIDI port** (MIDI mode): which MIDI input to record from.
- **MIDI channel** (MIDI mode): **Omni** (all 16 channels) or a single channel filter.
- **MIDI output** (MIDI mode): optional external MIDI output to drive a hardware synth as you play.
- **Activity LED**: blinks green when MIDI arrives on the chosen channel.

## ARM, IN, PRINT (RECORDING stage)

- **ARM**: light red when on. Marks the track for recording on the next Record press.
- **IN**: input monitor. When on, you hear the live input through the channel strip. Useful for tracking with effects.
- **PRINT**: when on, the channel's EQ, compressor, and insert are committed to the recorded file. When off (the default), they are kept live, so you can tweak them after the take.

## Insert slot

Each channel has one insert slot, which can hold either a plugin or a hardware insert (configured via the Settings or right-click menu). Switching between plugin and hardware uses a 20-millisecond equal-power crossfade, so the change is inaudible.

- Click **+ Plugin** to open the plugin picker.
- Right-click the slot for **Add / Replace / Remove / Edit / Configure as hardware insert**.
- When a plugin is loaded, the slot shows its name. Click to open the editor.

The insert sits **before** the HPF, so any plugin you load drives the rest of the channel's tone shaping.

## HPF (high-pass filter)

- **Enable**: click the label to toggle. The LED lights green when on.
- **Frequency**: 20 to 300 Hz. At 20 Hz the filter is effectively bypassed (the LED stays unlit until you drag above 20 Hz).

A modest HPF (60–80 Hz on vocals, 100 Hz on most instruments, 30 Hz on bass) cleans up rumble before it hits the EQ.

## LPF (low-pass filter)

- **Enable**: toggle as for HPF.
- **Frequency**: 3 kHz to 20 kHz. At 20 kHz the filter is effectively bypassed.

Useful for taming hi-hat bleed, cymbal harshness on a mic that's picking up too much top, or muffling a track you want pushed to the back of the mix.

## 4-band parametric EQ

A British-style SSL-grammar EQ. Left-click the **EQ** header to enable; the LED lights green. Right-click to choose the saturation character:

- **E** (brown, default): SSL E-series character, slightly more aggressive midrange.
- **G** (black): SSL G-series character, smoother high band.

The four bands are:

| Band | Type | Freq range | Default freq | Gain | Q range |
|------|------|------------|--------------|------|---------|
| **LF** | Low shelf | 20–400 Hz | 100 Hz | ±15 dB | n/a |
| **LM** | Peaking | 100 Hz–4 kHz | 600 Hz | ±15 dB | 0.4–4.0 |
| **HM** | Peaking | 600 Hz–13 kHz | 2 kHz | ±15 dB | 0.4–4.0 |
| **HF** | High shelf | 1–20 kHz | 8 kHz | ±15 dB | n/a |

Each knob is a rotary slider. Drag up to increase, down to decrease. Use a vertical drag for gain, a horizontal drag for frequency. There are no numeric text boxes on the knobs; the values display below.

EQ in Dusk Studio does **not cramp** near Nyquist; the British EQ does its own internal pre-warping and benefits further when the global oversampling is raised.

## Compressor

The channel compressor has three mutually-exclusive modes. Settings are remembered per mode — switch from FET back to Opto and your Opto settings are exactly as you left them.

Left-click the **COMP** header to enable. Right-click to pick **Opto**, **FET**, or **VCA**.

### Opto (LA-2A style)

A program-dependent optical compressor with two main controls.

- **Peak Reduction**: 0 to 100%. How much the optical cell pulls the signal down on peaks.
- **Gain**: 0 to 100%. 50% is unity. Compensates for level loss.
- **Limit**: toggles a hard ceiling on the output.

The Opto's character is slow attack, slow release, and a frequency-dependent gain reduction curve that is gentle in the lows and firmer in the mids and highs. Good on vocals, bass, and any source where you want compression to be felt but not heard.

### FET (1176 style)

A fast solid-state compressor with five controls.

- **Input**: −20 to +40 dB. Pre-compression gain. More input drives the compressor harder.
- **Output**: −20 to +20 dB. Post-compression makeup.
- **Attack**: 0.02 to 80 ms.
- **Release**: 50 to 1100 ms.
- **Ratio**: a discrete selector with five positions: **4:1**, **8:1**, **12:1**, **20:1**, **All** (the "all-buttons-in" mode of the original 1176 — extremely aggressive, almost a distortion effect).

The FET is the right tool for drums, electric guitar, and anything where you want compression to be heard. The "All" setting is famously useful on parallel drum buses.

### VCA (Classic 160-style)

A clean, fast, full-featured compressor.

- **Threshold**: −38 to +12 dB. Above this, compression begins.
- **Ratio**: 1:1 to 120:1.
- **Attack**: 0.1 to 50 ms.
- **Release**: 10 to 5000 ms.
- **Output**: −20 to +20 dB makeup.
- **Soft knee**: when on, the OverEasy-style parabolic knee replaces the hard knee.
- **Detector**: **Adaptive** (default, level-dependent RMS time constant from 35 ms down to 5 ms) or **Classic** (fixed 10 ms RMS).

The sidechain has a built-in 60 Hz high-pass filter in VCA mode (so bass doesn't pump the compressor); Opto and FET have it disabled by default to preserve their period-correct character.

All three modes share a unified **makeup gain** parameter (−12 to +24 dB) that is read and written across modes for consistency.

The gain-reduction meter (the thin vertical bar to the left of the comp section) shows real-time reduction in dB, regardless of mode.

## Aux sends (MIXING stage)

In the MIXING stage the input block is replaced by four send knobs, one per aux lane. Each knob is colour-matched to its destination aux.

- **Range**: −60 to +6 dB, or **OFF** (−100 dB, the bottom of the knob's travel).
- **Pre / post fader**: right-click the knob to toggle. Post-fader is the default.

Pre-fader sends are used for headphone cue mixes: the singer can hear their voice at the same level even when you pull their fader down. Post-fader sends are used for effects: when you pull the channel fader down, the reverb level falls with it.

## Pan

A rotary knob. Centre is unity; full left is −1.0, full right is +1.0. The pan law is equal-power, with a 3 dB centre dip on each leg so that panning a mono source from left to right keeps perceived loudness constant.

Stereo tracks pan by adjusting the balance between L and R channels rather than by re-summing. A hard-left pan on a stereo track silences the R channel.

## Fader

A vertical fader with a range of **−∞ dB** (true mute, below −90 dB floor) to **+12 dB**. Unity is at 0 dB.

- Drag the fader to ride the level.
- Click the dB readout beneath the fader to type a precise value.
- Right-click to enter MIDI Learn mode (the next CC you move binds to this fader).

The fader is automatable. The automation modes are **OFF** (no automation), **READ** (play back recorded automation), **WRITE** (record automation continuously while transport rolls), and **TOUCH** (record while you are touching the control; revert to read when you release). Click the small mode label below the fader to cycle, or right-click to pick from a menu.

## Mute, Solo, Phase

Below the fader:

- **M** (mute). Lights red when on.
- **S** (solo). Lights blue when on. Solo is **solo-in-place, additive**: every un-soloed track is muted while any track is soloed; soloing multiple tracks plays them all together. There is no PFL or AFL mode in v1.
- **Ø** (phase invert). Lights yellow when on. Inverts the polarity of the channel's audio before the insert.

## Bus assigns

Each channel can be routed to any combination of the four mix buses. Right-click the fader area for a bus-assign menu. Bus routing is independent of the master send — a channel always reaches the master regardless of its bus assigns.

## Meters

- **Input meter** (left of the fader): peak level in dBFS, with a brief peak-hold. Two columns for stereo tracks.
- **GR meter** (right of the fader): real-time compressor gain reduction.

A short red bar at the top of the input meter indicates a clip on that track. The bar holds for one second before clearing.

\newpage

# The bus strips

Between the channel strips and the master strip are four bus strips. They are smaller than the channels but follow a similar grammar.

## Signal flow

```
bus input → 3-band EQ → bus compressor → pan → fader → mute/solo gate → master
```

## 3-band EQ

A simplified British EQ with three bands at fixed musical defaults. Gain range is ±9 dB per band (a Mixbus-style restrained range — buses don't need wide cuts and boosts).

- **LF**: low shelf.
- **MID**: peaking.
- **HF**: high shelf.

## Bus compressor

An SSL-style glue compressor.

- **Threshold**: −30 to 0 dB.
- **Ratio**: 1:1 to 10:1. 4:1 is the default.
- **Attack**: 0.1 to 50 ms.
- **Release**: 50 to 1000 ms.
- **Auto release**: on by default. Switches the release to a hardware-style program-dependent 150–450 ms envelope.
- **Makeup**: −10 to +20 dB.

A 2:1 ratio with a 10 ms attack and auto-release is a classic drum-bus setting. A 4:1 with a slower attack glues a full mix.

## Pan, fader, mute, solo

Identical in function to the channel strip versions. Bus solos follow the same additive solo-in-place rule.

## Meters

Each bus shows a post-bus peak meter (L and R) plus a slim VU-style RMS meter integrated at 300 ms, matched to the tape saturation's internal VU integrator. The compressor's gain-reduction meter is visible alongside the bus comp section.

\newpage

# The master strip

The rightmost strip. Receives the sum of every channel that is not routed exclusively through a bus, plus every bus's output, plus every aux return.

## Signal flow

```
master input → tape saturation → Pultec EQ → master bus compressor → master fader → mono sum → output
```

## Tape saturation

Models a small reel-to-reel tape machine.

- **Enable**: click the **TAPE** header.
- **HQ**: right-click the header to toggle 4× internal oversampling. Use HQ on the master bounce; the native rate is fine for live mixing.
- **Drive and bias**: not user-controllable in v1 — fixed to a musically useful setting. To tweak more deeply, right-click and choose **Open editor** for the full tape-machine modal.

Tape saturation is the right tool to glue a mix together. A light application thickens the low-mids, rounds the transients, and adds a touch of harmonic colour.

## Pultec-style EQ

A program-EQ inspired by the Pultec EQP-1A.

- **LF Boost** and **LF Atten**: a low shelf with separately controllable boost and cut. The famous "Pultec trick" — boost and cut at the same low frequency — creates a notch above the boost band and a slight bass lift.
- **LF Boost Freq**: 20, 30, 60, or 100 Hz.
- **HF Boost**: a peaking boost band, 0–10 scale.
- **HF Boost Freq**: 3, 4, 5, 8, 10, 12, or 16 kHz.
- **HF Bandwidth**: Sharp (0) to Broad (10). 0.5 is the default.
- **HF Atten**: a high-shelf cut, 0–10 scale.
- **HF Atten Freq**: 5, 10, or 20 kHz.
- **Output**: ±12 dB.

The Pultec is tube-saturated; pushing the boosts harder adds harmonic content rather than just a clean gain change.

## Master bus compressor

Identical in DSP to the bus-strip compressor but typically used with slower settings: a 10 ms attack, auto-release, 2:1 to 4:1 ratio, and 1–3 dB of gain reduction on peaks. Click the **COMP** header to enable.

## Master fader

Same range as channel and bus faders: −∞ to +12 dB.

## Mono

A small **MONO/STEREO** button below the master fader. When pushed, the master output is summed to mono on both channels. Use this to check that your mix translates to a single-speaker environment without phase cancellations.

## Output meters

Two peak meters (L and R) plus two RMS VU meters. The compressor's gain-reduction meter is at the bottom of the comp section.

\newpage

# Aux lanes

The **AUX** stage swaps the console view for a single full-width aux lane. Four selector buttons at the top choose which aux you are looking at.

## Aux lane layout

Each lane is divided into three columns:

```
┌─────────┬───────────────────────────────┬──────────────┐
│ Return  │  Plugin / hardware insert      │  Sources     │
│ strip   │  chain                         │  panel       │
│ (M, ▲)  │                                │              │
└─────────┴───────────────────────────────┴──────────────┘
```

### Return strip (left column)

- **Name** label. Double-click the lane's top title to rename.
- **Mute** button.
- **Return fader**: −∞ to +12 dB. This is the level of the aux's processed output into the master.
- **Output meter**: pre-master return level.
- **Automation mode**: same OFF / READ / WRITE / TOUCH cycle as channel faders.

### Plugin chain (centre column)

Each aux lane has one insert slot. Click **+ Plugin** to open the picker. Right-click for **Add / Replace / Remove / Edit / Configure as hardware insert**. The slot can hold a plugin or a hardware insert, mutually exclusive, with a 20 ms crossfade between modes.

Common uses:
- Reverb on aux 1.
- Tape delay on aux 2.
- Mid-side analog summing through a hardware EQ on aux 3.
- Headphone-cue chorus on aux 4.

### Sources panel (right column)

Lists every channel that is sending to this aux, with its send level and a small meter. Useful for debugging "where is that signal coming from?" and for adjusting cue mixes in one place.

\newpage

# The mastering stage

The **MASTERING** stage is a separate signal path. It does not play your tracks; it plays a single stereo audio file through a dedicated mastering chain. Switching into or out of MASTERING force-stops the transport, because the mix engine and the mastering engine cannot run simultaneously.

## Loading a mix

- **Load mix…**: opens a file chooser. Pick any WAV, AIFF, FLAC, or OGG file.
- **Load latest mixdown**: automatically loads the most recently exported bounce from the current session's bounce folder.

The source file path is displayed below the buttons.

## Transport

A miniature transport bar with **Play**, **Stop**, **Rewind**, a clock display, and a gain-reduction readout summing the compressor and limiter.

## Waveform

A full-width audio thumbnail of the loaded file. Click anywhere on the waveform to seek. The playhead is a vertical line.

## Mastering chain

Three columns of DSP, each with its own enable toggle:

### Digital 5-band EQ

A clean linear-phase-style mastering EQ.

| Band | Type | Default freq | Gain | Q |
|------|------|--------------|------|---|
| 0 | Low shelf | 80 Hz | ±15 dB | n/a |
| 1 | Peaking | 250 Hz | ±15 dB | 0.4–4.0 |
| 2 | Peaking | 1 kHz | ±15 dB | 0.4–4.0 |
| 3 | Peaking | 4 kHz | ±15 dB | 0.4–4.0 |
| 4 | High shelf | 12 kHz | ±15 dB | n/a |

### Bus compressor

The same UniversalCompressor in Bus mode as the channel / master compressors, but tuned to mastering defaults: 2:1 ratio, 30 ms attack, 250 ms release, auto-release on. Apply 0.5–2 dB of gain reduction to glue a final mix without squashing transients.

### Brickwall limiter

A true-peak brickwall limiter. **Enabled by default.**

- **Ceiling**: −20 to 0 dB. Default **−0.3 dB** (matches the headroom expected by most streaming platforms).
- **Drive**: 0 to +20 dB pre-limiter gain. Drives the input harder for more limiting.
- **Release**: 50 to 300 ms.

The limiter handles inter-sample peaks by oversampling its detection 4× per ITU BS.1770.

## Loudness metering

Right of the chain are three loudness readouts:

- **Momentary LUFS** (400 ms window).
- **Short-Term LUFS** (3 second window).
- **Integrated LUFS** (entire program, gated per BS.1770).
- **True Peak (dBTP)** (4× oversampled).

A streaming-platform preset picker (Spotify, Apple Music, YouTube, Netflix, etc.) colour-codes the integrated LUFS and true-peak readings according to that platform's target. Pressing **Reset integrated** clears the integrated reading so you can re-measure from a known point.

## Exporting the master

**Export master…** opens a bounce dialog: pick the destination file, sample rate, and bit depth (16, 24, or 32 float), and click **Bounce**. The mastering chain renders offline as fast as the CPU allows.

\newpage

# Recording

The recording workflow in Dusk Studio is intentionally simple. There are five steps.

1. Switch to the **RECORDING** stage.
2. Choose each track's input source.
3. Click **ARM** on every track you want to record.
4. Optionally enable count-in, punch, or loop.
5. Press **Record** on the transport.

## Setting the input

For an audio track, the **Input** dropdown lists every input channel on your audio device, plus the special default `−2: follow track index`. The default makes track 1 read input 1, track 2 read input 2, and so on — useful when you're tracking a band live and you have wired the patch bay one-for-one.

For a stereo track, you select two inputs (left and right) independently.

For a MIDI track, you select a MIDI input port and a channel filter. The activity LED next to the input blinks each time a MIDI message arrives, so you can confirm your controller is talking to the right track.

## Arming a track

Click **ARM**. The button lights red. Until at least one track is armed, pressing Record on the transport does nothing.

## Monitor mode

Click **IN** to monitor the live input through the channel strip. When the IN light is on, you hear yourself in real time, processed by the channel's EQ and compressor (and any plugin you have loaded on the insert).

## Print mode

When you record an audio take, you choose whether to print the EQ and compressor to disk or keep them live. Click **PRINT** to commit them; leave it off to record dry and shape the take later. The default is off (dry recording), which is the more flexible choice — it lets you re-EQ and re-compress in mixing without re-recording.

## Count-in

Toggle **C/I** on the transport bar. The next time you press Record, the metronome will click for one full bar before recording actually starts. The playhead rolls back by one bar so that the first recorded sample lines up with the intended start position.

The count-in always uses the metronome click, even if you have the click disabled for normal playback.

## Punch recording

To overdub a specific section without erasing material before or after:

1. Set the **punch in** and **punch out** points by clicking the timeline ruler at the desired in and out positions, holding **Shift**.
2. Click the **Punch** button on the transport bar.
3. Right-click the **Punch** button to set the pre-roll seconds — how much existing material plays back before the punch-in.
4. Press Record. Playback begins at the pre-roll position. Recording begins exactly at the punch-in sample and ends exactly at the punch-out sample. The audio before and after is untouched.

When the new take begins, a 64-sample raised-cosine fade-in shapes its edge against the existing material. When the new take ends, a 64-sample fade-out shapes the other edge. The result is a click-free splice.

## Loop recording

To repeat a section while you experiment:

1. Set the loop region with the **[** and **]** keys at the desired in and out positions.
2. Click the **Loop** button on the transport bar.
3. Press Play (for loop playback) or Record (for loop recording).

In loop play, the transport wraps at the loop boundary indefinitely. In loop record, playback wraps but recording stays linear — you get a single take that extends from the loop start to wherever you press Stop. There is no take-stacking loop recording in v1.

## Where files are saved

When you save a session, Dusk Studio creates a folder structure:

```
MySession/
├── session.json
└── audio/
    ├── track01_20260526-143025.wav
    ├── track02_20260526-143025.wav
    └── ...
```

Every recorded audio file is a **24-bit WAV** at the session's sample rate. Filenames include the track number and a timestamp so re-recording the same track on the same day still produces distinct files.

MIDI tracks do not produce separate files; their note and CC data is embedded in `session.json`.

## Take history

Each region keeps a stack of up to **20 previous takes**. When you record a new take whose timeline range fully contains an existing region, the existing region is pushed onto that stack. Partially-overlapping takes are not absorbed — they stay visible on either side of the punch.

To cycle through takes:

- Press **Alt+T** for next take.
- Press **Alt+Shift+T** for previous take.

Or click the take badge on the region itself (visible when more than one take exists).

The 20-take cap bounds memory and disk growth across long sessions.

## Recording errors

If Dusk Studio can't open a file for writing (full disk, permission denied, missing audio directory), the error is captured at record start and displayed as an alert before the take begins, listing the affected tracks. You don't lose a take thinking it was captured.

If something goes wrong mid-take (ring-buffer overrun on a stressed disk, MIDI FIFO overflow), an alert appears when you press Stop. The portion of the take that was written successfully is preserved.

\newpage

# The tape strip

The tape strip is Dusk Studio's timeline view. It is collapsed by default. Click **▾ SUMMARY** at the top right of the transport bar (or the small drawer-handle below the bar) to expand it.

When expanded, the channel strips automatically compact so that the timeline gets vertical space. EQ and compressor controls collapse into header buttons; click a header to open a modal editor with the full controls.

## Layout

```
            time ruler  | ◀ playhead
┌──────────┬─────────────────────────────┐
│ Track 1  │ ▓▓▓▓▓▓░░░░░  ▓▓▓▓▓▓▓▓░░░░░░ │
│ Track 2  │ ░░░  ░░░░░░  ▓▓▓▓▓▓▓▓▓▓▓░░ │
│ Track 3  │           ░░░░  ░░░  ░░░░░ │  (MIDI region)
│ Track 4  │ ▓▓▓▓▓▓▓▓▓▓▓░░░░  ░░░░░░░░░ │
└──────────┴─────────────────────────────┘
```

The left column shows each track's number, colour, and small ARM/SOLO/MUTE buttons. The right area is the timeline canvas.

## The ruler

The top band shows bars and beats (when the clock display is in Bars mode) or minutes and seconds (when in Time mode). Below the bar/beat band is a pill row showing markers and loop/punch brackets.

Click the ruler to seek the playhead. Drag with Shift held to set the loop range.

## Regions

Each region is drawn as a rounded coloured rectangle. Audio regions show a waveform thumbnail; MIDI regions show a piano-keyboard glyph and the first few notes. The region's left edge is its start position; the right edge is start + length.

### Selecting and moving

- Click a region to select it. Other regions deselect.
- **Cmd+click** (Linux/Mac) or **Ctrl+click** (Windows) toggles selection — useful for moving multiple regions together.
- Drag a region body to move it. With **SNAP** on, it snaps to the grid resolution.
- Drag the left or right edge to trim.
- Drag the pink fade discs in the top corners to set fade-in / fade-out lengths.
- Middle-mouse-drag pans the timeline left or right.

### Splitting

Position the playhead and press **T** to split the selected region at the playhead. Or right-click and choose **Split**.

### Duplicating

**Cmd+D** clones the selected region and places the copy immediately after the original.

### Nudging

**Cmd+←** / **Cmd+→** nudges the region by one beat. Hold **Shift** to nudge by a bar.

### Right-click menu

A right-click on any region shows a context menu:

- **Split** at click position.
- **Colour**: 12 hues.
- **Label**: type a custom name.
- **Mute** the region (silences it without deleting it).
- **Lock** the region (prevents accidental edits).
- **Reverse** (audio only; destructive).
- **Normalize** (audio only; non-destructive — adjusts the region's gain to peak-align at 0 dB).
- **Delete**.

### Take cycling

Right-click a region to see a take submenu (if more than one take exists), or use **Alt+T** / **Alt+Shift+T** to cycle.

## Markers

Press **M** to drop a marker at the current playhead. A marker pill appears in the ruler's lower band.

- Drag a marker pill to move it.
- Right-click for **Rename** and **Delete**.
- **Rewind** and **Forward** transport buttons jump to the previous and next marker.

## Loop and punch brackets

When **Loop** or **Punch** is enabled, coloured brackets appear in the ruler.

- **Cyan**: loop start and loop end.
- **Red**: punch in and punch out.

Drag the bracket ends to adjust. The keyboard shortcuts **[** and **]** set the loop in and out at the current playhead. Hold **Shift** to set punch in and out instead.

## Zoom

- **−** / **=** (or **+**): zoom out, zoom in.
- **0**: zoom to fit the entire timeline width.
- **Cmd/Ctrl+mouse wheel** over the timeline: zoom around the cursor.

## Drag-and-drop import

Drop audio or MIDI files onto the tape strip. If you drop one file, the **Import target picker** opens to confirm the destination track. If you drop several, the **Multi-import target picker** opens with one row per file, each row showing the file name and a destination dropdown. Use the **Sequential** preset to spread files across adjacent tracks, or **Same track** to stack them as takes on a single track.

\newpage

# The audio region editor

Double-click an audio region in the tape strip to open the audio region editor as a centred modal. Press **Esc** or click outside to close.

## What's editable

- **Trim** the start or end (non-destructively — the underlying file is untouched).
- **Fade in** and **fade out** curves and lengths.
- **Gain** adjustment (±24 dB, non-destructive).
- **Position** of the region on the timeline.

You **cannot** edit individual samples. There is no pencil tool, no zoom-to-sample, no spectral edit, no destructive trim. The portastudio philosophy is that you commit to good takes and work non-destructively from there.

## Layout

The top is a row of icon buttons:

- **Undo / Redo** (also **Cmd+Z** and **Cmd+Shift+Z**).
- **Split** at the edit cursor (also **S**).
- **Normalize** (peak-aligns the region to 0 dB by adjusting its gain).
- **Properties** (file path, sample rate, channel count, length).
- **Zoom out / Zoom in / Zoom fit** (also **−**, **+**, **0**).

The edit-mode toolbar follows: **Grab**, **Range**, **Cut**, **Grid**, **Draw**. Most editing uses Grab. Range lets you highlight a time band for split or fade-fit. Cut splits the region at every click. Grid and Draw are reserved for later phases.

The **Snap** toggle and snap-denomination dropdown are at the right of the toolbar.

Below the toolbar:

- **Bar/beat ruler** for the region.
- **Waveform area** showing the region centred, with adjacent regions on the same track faded so splits don't shift the view.
- **Status bar** at the bottom showing position, gain, fade lengths, mute and lock toggles.

## Editing gestures

- **Click on the waveform**: place the edit cursor. The cursor snaps to the grid if Snap is on.
- **Drag the fade-in disc** (top-left): extend the fade-in length.
- **Right-click the fade-in disc**: choose **Linear**, **Exponential**, or **Logarithmic** curve.
- **Drag the fade-out disc** (bottom-right): extend the fade-out length.
- **Drag the trim-start handle**: shorten from the start.
- **Drag the trim-end handle**: shorten from the end.
- **Drag the gain line** (the dashed horizontal line through the waveform): adjusts the region's gain ±24 dB. The cursor displays the new value.
- **Shift+drag** on the waveform: select a time range (yellow highlight).
- **Cmd/Ctrl+]** / **Cmd/Ctrl+[**: navigate to the next / previous region on the same track without closing the modal.
- **Delete**: delete the selected region.

\newpage

# The piano roll

Double-click a MIDI region to open the piano roll as a centred modal.

## Layout

```
┌──────────────────────────────────────────┐
│  toolbar (undo, split, glue, quantize…)  │
├──────────────────────────────────────────┤
│  bar/beat ruler                          │
├──────────┬───────────────────────────────┤
│ keyboard │    note grid                  │
│ C5       │    ▮▮          ▮  ▮▮        │
│ C4       │       ▮▮       ▮      ▮     │
│ C3       │          ▮▮▮▮              │
├──────────┴───────────────────────────────┤
│  velocity strip                          │
├──────────────────────────────────────────┤
│  CC lane (optional)                      │
├──────────────────────────────────────────┤
│  status bar                              │
└──────────────────────────────────────────┘
```

## Creating notes

Click an empty grid cell to create a 1/4-note at that pitch and tick (or whatever your current snap denomination is). The note's velocity defaults to 100.

## Selecting and moving

- Click a note to select it.
- **Shift+click** adds to the selection.
- **Cmd/Ctrl+click** toggles selection.
- Drag a selected note's body to move it (snaps to grid; hold **Cmd** to bypass snap).
- Drag a selected note's right edge to resize.
- Drag in empty grid space to rubber-band select.
- **Backspace** or **Delete** deletes the selection.

## Transposing and nudging

- **↑** / **↓**: transpose ±1 semitone.
- **←** / **→**: nudge ±1 grid step.

## Velocity

The velocity strip below the grid shows one vertical bar per note. Drag the top of a bar to adjust velocity (1–127). The colour of the bar reflects the value.

The strip is resizable — drag its top edge up or down. Scroll-wheel inside the strip to zoom the value axis.

## CC editing

Open the **CC lane** below the velocity strip. Choose a controller from the dropdown (defaults to CC 1, Mod Wheel). Each CC event is a vertical bar; drag to adjust value, click empty grid to add a new event.

The CC lane is also resizable.

## Quantize and scale

- **Q**: opens a quantize popup. Pick the grid resolution and the strength (0 = none, 1 = full).
- **S**: opens a scale picker. Pick a root and a scale (Major, Minor, modes). Non-scale notes display dimmed.
- **L**: cycles the note-creation grid (Grid, Free, Triplet, Dotted).
- **C**: cycles the note colour mode (Pitch, Velocity, Channel).

## Zoom and scroll

- **=** / **−**: zoom in / out.
- **Cmd+0**: zoom to fit the region.
- **Mouse wheel**: scroll vertically across the 128-key range.
- **Cmd/Ctrl+wheel**: horizontal zoom.
- **Shift+wheel**: horizontal scroll.

## Step record

Open the virtual keyboard from the transport bar (the keyboard icon, or **K**). Each note you press in the keyboard is entered at the current edit cursor. When all keys are released, the cursor advances by one snap step. This is the fastest way to enter a chord progression without playing in real time.

## Navigation

- **Cmd/Ctrl+]** / **Cmd/Ctrl+[**: jump to the next / previous MIDI region on the same track.

\newpage

# Mixing

Mixing is the act of balancing your tracks, shaping them with EQ and dynamics, placing them in the stereo field, and gluing the whole thing together on the buses and master.

## A suggested order of operations

1. **Switch to MIXING stage** so the channel strips show send knobs instead of the input block.
2. **Roll the song.** Hit Play and listen all the way through with all faders at unity. Make a mental list of what is too loud, too quiet, too dull, too sharp.
3. **Set rough levels.** Pull faders until the rough balance is right. Do not boost faders above unity if you can avoid it; if a track is too quiet, ask whether the source recording is too quiet first.
4. **Pan.** Place each track in the stereo field. Hard-pan things that occur naturally on one side (drum overheads, double-tracked guitars). Centre things that are foundational (kick, snare, bass, vocal).
5. **HPF every track that doesn't need lows.** Vocals, guitars, and most overdubs need a 60–100 Hz high-pass. Kick and bass don't; everything else probably does.
6. **EQ.** Cut before you boost. If something is muddy, find the muddy frequency and pull it down before you reach for a top-end boost.
7. **Compress.** Use the Opto on smooth sources (vocals, bass), the FET on transients (drums, guitars), the VCA when you need precision.
8. **Bus routing.** Assign drums to bus 1, vocals to bus 2, etc. Tighten the buses with the SSL-style compressor (2:1 ratio, ~10 ms attack, auto-release, 1–3 dB of reduction).
9. **Aux sends.** Route what needs reverb to aux 1, what needs delay to aux 2.
10. **Master bus.** Glue the whole mix with the master compressor and Pultec EQ. Add a touch of tape if it needs body.
11. **Listen on multiple systems.** Check the **MONO** button on the master strip. Check headphones, laptop speakers, the car. Adjust.

## Aux sends in detail

In the MIXING stage, each channel's four send knobs appear where the input block was in RECORDING. Each knob is colour-matched to its destination aux.

- **Post-fader** (default): pulling the channel fader down also pulls the send level down. Use for effects (reverb, delay) so the wet level stays proportional to the dry.
- **Pre-fader**: the send is independent of the channel fader. Use for cue mixes (you can mute the channel in the main mix while still sending it to the headphone aux).

Right-click a send knob to toggle pre/post.

## Bus routing

Right-click anywhere in the channel strip's fader area for the bus-assign menu. Assigning to a bus does **not** unassign from the master — the bus routing is additive. To remove a channel from the master mix entirely while still sending to a bus, mute the channel and rely on the bus mute being off.

## Solo and PFL

Dusk Studio's solo is **solo-in-place, additive**:

- Press **S** on any track to solo it. Every other track is muted while any track is soloed.
- Press **S** on a second track to add it to the solo set.
- Press **S** again to remove it.

There is no PFL (pre-fader listen) or AFL (after-fader listen) mode in v1. If you need to audition without the effects of the channel strip, set the **IN** monitor toggle and pull the rest of the mix down.

## Automation

Each channel and the master strip have an automation mode button below the fader. Cycle through:

- **OFF**: the fader does what you do. No recording, no playback of past rides.
- **READ**: previously recorded automation drives the fader during playback. You can move the fader to "preview" but your changes are not recorded.
- **WRITE**: every fader movement is recorded for as long as the transport rolls. Existing automation in the region played over is overwritten.
- **TOUCH**: while you are touching the fader, your movement is recorded. When you let go, the automation reverts to the previously recorded value via a short ramp.

The same modes apply to pan, mute, and solo.

Dusk Studio's automation is intentionally console-style: you ride the controls and the program writes what you did. There is no graphical curve editor.

\newpage

# Plugins

## Plugin formats

Dusk Studio scans and hosts:

- **VST3** on Linux, macOS, and Windows.
- **LV2** on Linux and macOS.
- **AU** on macOS only.
- **Native multi-sampler** (`.sfz` and `.sf2` files) on all platforms.

There is no VST2 support.

## Scanning

Dusk Studio does not automatically scan plugins on first launch. Open any plugin picker (right-click any channel insert slot → **+ Plugin**, or the aux lane plugin slot) and click **Scan plugins**. Scanning takes 10–30 seconds for a large collection.

Plugin scan results are cached in:

- Linux: `~/.config/Dusk Studio/plugin-cache.xml`
- macOS: `~/Library/Application Support/Dusk Studio/plugin-cache.xml`
- Windows: `%APPDATA%\Dusk Studio\plugin-cache.xml`

To re-scan on every launch, enable **Settings → General → Scan plugins on startup**.

## Loading a plugin

In the **plugin picker** modal:

- Use the filter field at the top to narrow by name.
- The list is grouped by manufacturer. Click a manufacturer to expand or collapse.
- Each row shows the plugin name and its format (VST3 / LV2 / AU).
- Click a row to load and dismiss.

The picker filters by intent: only effect plugins appear when you're loading onto a channel insert or aux lane; only instruments appear when you're loading onto a MIDI track.

At the bottom of the picker are alternative buttons:

- **Hardware insert**: configure the slot as an external hardware insert instead of a plugin.
- **Load Soundfont**: open a file chooser for `.sfz` or `.sf2` files.
- **Browse file…**: load a plugin by file path (useful for plugins not yet in the scan cache).
- **Scan plugins**: re-scan.

## Opening the editor

Click the loaded plugin's slot to open its editor. The editor appears as a centred modal with a dimmed backdrop. Press **Esc** or click outside to dismiss. There is no floating-window option in v1.

## Out-of-process sandboxing

Dusk Studio can run plugins in a child process (the **OOP** sandbox), so that a crashing plugin does not take the whole DAW with it.

OOP is supported on:

- **Linux**: always.
- **Windows**: always.
- **macOS**: requires macOS 14.4 or later.

OOP is enabled per-session by setting the environment variable `DUSKSTUDIO_USE_OOP_PLUGINS=1` before launching Dusk Studio. A future release will expose this as a per-plugin or per-session UI toggle.

When a plugin crashes in OOP mode:

- The slot auto-bypasses and shows a "Plugin crashed — reload to recover" message.
- The plugin's last-known state (parameters, preset) is preserved in the session and will be re-applied when you reload the plugin.
- You can load a different plugin to clear the slot.

The OOP child process is named `dusk-studio-plugin-host` and lives next to the main Dusk Studio binary. Each loaded OOP plugin runs in its own child process.

## Auto-bypass on overrun

Plugins have a CPU time budget: 60% of the buffer time when in-process, 85% when out-of-process. If a plugin exceeds this for three consecutive blocks, it is automatically bypassed and the slot shows a warning. Right-click the slot and choose **Re-enable plugin** to restore.

## Plugin state in sessions

When you save a session, each loaded plugin's identity (description XML — UID, format, file path) and full state blob (the plugin's `getStateInformation` bytes, base64-encoded) are written to `session.json`.

When you load a session:

- If the plugin is found on the system, it is loaded and the saved state is applied.
- If the plugin is missing or moved, the slot shows "(plugin name) — offline". The saved description and state are preserved; the next session save round-trips them unmodified, so you do not lose the data by opening a session on a machine without that plugin installed. Reinstall the plugin and reload to restore.

## Multi-sample instruments

Drop a `.sfz` or `.sf2` file onto a MIDI track's insert slot to load it through the **sfizz** engine. Most SoundFont and SFZ instrument libraries work directly. The processor exposes three runtime overrides:

- **Master volume**: −60 to +12 dB.
- **Master tune**: −100 to +100 cents.
- **Polyphony cap**: 1 to 256 voices.

The loaded file path is saved with the session.

\newpage

# Hardware inserts

A hardware insert routes a channel or aux's signal out through one of your audio interface's outputs, into an external piece of gear (a compressor, EQ, tape echo, guitar pedalboard, anything), and back in through one of the interface's inputs. The returned signal continues through the channel's chain as if the hardware were a plugin.

## Configuring an insert

Right-click any insert slot (channel or aux) → **Configure as hardware insert**, or click **Hardware insert** in the plugin picker.

The configuration panel has six controls:

- **Output channel**: which audio device output the channel's pre-insert signal is sent to. Choose a stereo pair on a stereo track; a single output on a mono track.
- **Output volume**: a level trim on the send side, 0.0 to 1.0 (linear).
- **Input channel**: which audio device input the returned signal is read from.
- **Input volume**: a level trim on the return side, 0.0 to 1.0.
- **Latency samples**: the round-trip delay through the external gear, in samples. Determines the dry-path delay applied to the rest of the mix so the hardware insert remains time-aligned.
- **Dry/Wet**: 0.0 plays only the dry signal (insert bypassed), 1.0 plays only the returned wet signal. Useful for parallel processing.
- **Format**: **Stereo** or **Mid/Side**. Mid/Side encodes the signal so that the hardware EQ processes the mid and side components independently.

## Auto-measuring latency

Click the **Ping** button. Dusk Studio plays a short chirp through the send, captures the return, and measures the round-trip latency by cross-correlation.

The status label shows one of:

- `Measuring…`
- `Detected: N samples (X.X ms)` — measurement succeeded, the value is filled in.
- `Ping failed — check level / cables` — no correlation peak found.

Run the ping after any change to your interface routing or your external gear's settings. There is no automatic re-ping on session load in v1; you ping when you set up the insert and re-ping if anything in the chain changes.

## Latency compensation

The measured latency is reported to Dusk Studio's plugin delay compensation system. Every track that does not have an insert (and the aux returns, and the master) is delayed by the longest insert latency in the session so that everything stays sample-accurate.

\newpage

# Sync to external gear

## MIDI Clock

To slave Dusk Studio to an external master (drum machine, sequencer, or another DAW):

1. Open **Settings → MIDI Sync**.
2. Set **Sync source input** to the MIDI port the master is sending Clock on.
3. Enable **Chase transport (Start/Stop)** if you want Dusk Studio to also follow the master's transport.

Dusk Studio derives its tempo from incoming F8 clock bytes, averaged over the last 24 ticks. Big jumps (>50% drift) are treated as glitches and skipped.

To be the master and emit Clock for downstream gear:

1. Set **Sync output** to the MIDI port the slave is listening on.
2. Enable **Emit clock**.

Dusk Studio emits 24 PPQN MIDI Clock plus FA (Start) and FC (Stop) bytes when the transport rolls.

You can be a master and a slave simultaneously — different ports for input and output. Avoid feedback loops by not looping the same physical port back.

## MIDI Time Code

To slave to SMPTE time code (MTC):

1. Set **Sync source input** to the MTC source.
2. Enable **Chase MTC**.

Dusk Studio decodes quarter-frame messages into SMPTE time, applies a +2 frame compensation for transmission delay, and supports 24, 25, 29.97 drop-frame, and 30 fps frame rates.

To emit MTC:

1. Set **Sync output** to the destination.
2. Enable **Emit MTC**.
3. Pick **MTC frame rate**.

Dusk Studio emits quarter-frame messages while rolling, plus a full-frame sysex on transport edges and large playhead jumps.

## Mackie Control surfaces

To control Dusk Studio from an external surface (tested against the Tascam DP-24SD):

1. Open **Settings → Control surface**.
2. Set **MCU input** to the surface's MIDI output.
3. Set **MCU output** to the surface's MIDI input.

Once connected:

- **Motorised faders** mirror Dusk Studio's channel and master faders.
- The **eight strip faders** drive the active bank (tracks 1–8, 9–16, or 17–24).
- **Bank Left** / **Bank Right** step the bank by 8.
- **Channel Left** / **Channel Right** step the selected channel by 1.
- **Mute / Solo / Arm / Select** buttons mirror and drive the on-screen buttons. LEDs reflect state.
- **V-pot** rotaries drive pan, sends, EQ band, or compressor depending on the **assign mode**. Press one of the **Track / Send / Pan / Plugin / EQ / Inst** buttons to switch.
- **Transport buttons** map to Play, Stop, Record, Rewind, Forward, Loop.
- **Jog wheel** scrubs the playhead.
- **Touch sense** drives Touch automation: touching a fader on the surface puts it into touch-write mode while you hold it.

\newpage

# MIDI bindings

In addition to MCU support, any control in Dusk Studio can be bound to a MIDI CC, note, or pitch-bend from any controller.

## Learning a binding

1. Right-click the control you want to bind (a fader, pan, knob, mute button, etc.).
2. Choose **MIDI Learn**.
3. Move the corresponding control on your MIDI controller.

The binding is captured and immediately active. The next time that CC arrives, the control responds.

## Trigger types

Dusk Studio recognises four kinds of incoming messages:

- **CC** (control change): the data value (0–127) drives a continuous control.
- **Note**: a key press triggers a discrete control (a button).
- **Pitch bend**: 14-bit value (0–16383) drives a continuous control.
- **MMC** (Mackie sysex transport commands): Play, Stop, Record, Locate, etc.

## Button modes

Discrete bindings (mute, solo, arm, transport buttons) can be set to one of two button modes:

- **Press**: triggers on the rising edge only (CC ≥ 64, or note on with velocity ≥ 1). Use for momentary buttons.
- **Toggle**: triggers on every message. Use for latching foot-switches.

Right-click the binding in the MIDI Bindings panel to change its mode.

## Available targets

- **Transport**: Play, Stop, Record, Toggle play/stop.
- **Per-track**: Mute, Solo, Arm, Select.
- **Per-track continuous**: Fader (dB), Pan.
- **Per-track DSP**: HPF frequency, EQ band gain (4 bands), compressor threshold, compressor makeup.
- **Per-track plugin parameter**: any indexed parameter on the loaded plugin.
- **Per-bus**: Fader, Pan, Mute, Solo.
- **Per-aux**: Fader, Mute.
- **Master**: Fader.

Aux sends are not yet wired as bindable targets in v1.

## The MIDI Bindings panel

Open from **Settings → MIDI bindings…**.

Each row shows:

- The bound target (e.g., "Track 3 Fader").
- The source (e.g., "CC 7 on Device X, channel 1").
- A **Remove** button.

At the bottom:

- **Export…** writes the entire binding set to a JSON file.
- **Import…** loads a binding set from JSON (replaces the current set).
- **Clear all** removes every binding (with confirmation).

\newpage

# Saving and loading sessions

## Where sessions live

A Dusk Studio session is a folder, not a single file. The folder contains:

```
MySession/
├── session.json
└── audio/
    ├── track01_20260526-143025.wav
    └── …
```

`session.json` holds every parameter, region, marker, plugin description, and plugin state in JSON form. `audio/` holds the recorded WAVs and any audio files you imported.

Because the session is a folder, you can copy or back up a session by copying the whole directory. Move it to another machine and the relative paths to the audio files remain valid.

## Save commands

- **File → Save** (or **Cmd+S**): write the current session over the existing `session.json`. The write is atomic — a temporary file is written and fsynced to disk, then renamed over the target. A crash during a save never produces a corrupted file.
- **File → Save As…** (or **Cmd+Shift+S**): pick a new session directory. The audio files are copied to the new directory's `audio/` folder.
- **File → Open…** (or **Cmd+O**): load a session by choosing its folder.

## Autosave

Every 30 seconds, if anything has changed since the last save, Dusk Studio writes `session.json` automatically. The autosave is atomic (same temp-file-and-rename pattern) and silent — it never interrupts playback or recording.

If Dusk Studio crashes or loses power, the next launch detects the autosave file and offers to recover.

Plugin state is captured only on manual save, not on autosave, to avoid audio dropouts. A crash recovery loads the most recent manual plugin state, which may be slightly stale relative to in-flight knob tweaks.

## What's in a session

A session captures everything user-visible:

- Tracks: names, colours, modes, armed state, input sources, channel strip parameters.
- Regions: file paths, timeline positions, lengths, source offsets, fades, gains, labels, colours, locks, mutes, take history.
- Mixer: aux lane names and contents, bus parameters, master parameters.
- Plugins: descriptions and state blobs for every loaded plugin.
- Transport: loop and punch points, current playhead, BPM, time signature.
- Markers: positions, names, colours.
- MIDI bindings.
- MIDI sync source/output, MCU port identifiers.
- The currently loaded mastering source file (if any).

## Backing up

Because sessions are self-contained folders, backup is just a copy:

```
cp -r MySession ~/Backups/MySession-20260526
```

For larger archives, `tar czf` the folder and store the tarball.

\newpage

# Bouncing and exporting

## Bouncing the mix

To export your finished mix as a stereo audio file:

1. From any stage, choose **File → Bounce…** (or **Cmd+B**).
2. The bounce dialog asks for:
   - The destination file path.
   - Sample rate (defaults to the device rate).
   - Bit depth (16, 24, or 32-bit float).
   - Tail length in seconds (default 5; allows reverb and compression tails to decay naturally).
3. Press **Bounce**.

Dusk Studio detaches from the realtime audio device and renders the project offline as fast as the CPU allows. A progress bar shows status. **Cancel** stops the render.

When the bounce completes, the audio device is automatically re-attached.

## Bouncing the master vs the mastering chain

- **Bounce master mix**: captures the post-master-fader stereo output of the live mix (channels → buses → master → output).
- **Bounce mastering chain**: from the MASTERING stage, **Export master…** captures the post-limiter output of the mastering chain on the loaded source file.

## Where bounces go

By default, bounces are written to `MySession/bounces/`. The most-recent bounce is what the mastering stage's **Load latest mixdown** button picks up.

\newpage

# Keyboard reference

Shortcuts use **Cmd** on macOS and **Ctrl** on Linux and Windows unless noted.

## File

| Shortcut | Action |
|----------|--------|
| **Cmd+S** | Save session |
| **Cmd+Shift+S** | Save As… |
| **Cmd+O** | Open session… |
| **Cmd+B** | Bounce… |

## Edit

| Shortcut | Action |
|----------|--------|
| **Cmd+Z** | Undo |
| **Cmd+Shift+Z** / **Cmd+Y** | Redo |
| **Cmd+C** | Copy selected region |
| **Cmd+X** | Cut selected region |
| **Cmd+V** | Paste at playhead |
| **Cmd+D** | Duplicate selected region |
| **Delete** / **Backspace** | Delete selected region |
| **T** | Split selected region at playhead |
| **Cmd+←** | Nudge selected region one beat earlier |
| **Cmd+→** | Nudge selected region one beat later |
| **Cmd+Shift+←** | Nudge by one bar (earlier) |
| **Cmd+Shift+→** | Nudge by one bar (later) |
| **Alt+T** | Cycle to next take on selected region |
| **Alt+Shift+T** | Cycle to previous take |

## Transport

| Shortcut | Action |
|----------|--------|
| **Space** | Play / Stop |
| **R** | Record (requires armed track) |
| **Home** | Seek to start |
| **.** | Stop and seek to start |
| **L** | Toggle loop |
| **P** | Toggle punch |
| **C** | Toggle metronome |
| **M** | Drop marker at playhead |
| **[** | Set loop start at playhead |
| **]** | Set loop end at playhead |
| **Shift+[** | Set punch in at playhead |
| **Shift+]** | Set punch out at playhead |
| **K** | Toggle virtual MIDI keyboard |

## Tracks

| Shortcut | Action |
|----------|--------|
| **A** | Toggle ARM on selected track |
| **S** | Toggle SOLO on selected track |
| **X** | Toggle MUTE on selected track |

## Timeline

| Shortcut | Action |
|----------|--------|
| **=** / **+** | Zoom in |
| **−** | Zoom out |
| **0** | Zoom fit |
| **Cmd+wheel** | Zoom around cursor |
| **Shift+wheel** | Horizontal scroll |
| **Middle-mouse drag** | Pan |

## Region editor

| Shortcut | Action |
|----------|--------|
| **S** | Split at edit cursor |
| **G** | Cycle edit mode (Grab / Range / Cut / Grid / Draw) |
| **Cmd+]** / **Cmd+[** | Next / previous region |
| **Esc** | Close modal |

## Piano roll

| Shortcut | Action |
|----------|--------|
| **↑** / **↓** | Transpose ±1 semitone |
| **←** / **→** | Nudge ±1 grid step |
| **Q** | Quantize popup |
| **S** | Scale picker popup |
| **L** | Cycle note-entry grid |
| **C** | Cycle colour mode (Pitch / Velocity / Channel) |
| **Cmd+0** | Zoom fit |
| **Cmd+]** / **Cmd+[** | Next / previous MIDI region |
| **Esc** | Close modal |

## Window

| Shortcut | Action |
|----------|--------|
| **F11** | Toggle fullscreen |
| **Esc** | Close current modal |

\newpage

# Tips and recipes

## A clean vocal chain

1. **HPF** at 80–100 Hz to remove low-frequency rumble.
2. **EQ**: a 2–4 dB cut at the muddy frequency (usually 200–400 Hz) and a 1–2 dB shelf boost above 8 kHz for air.
3. **Compressor → Opto** with 40% peak reduction and 50% gain. Should pull 3–5 dB on peaks.
4. **Aux 1** send (post-fader) at −12 dB to a reverb on the aux lane.
5. **Aux 2** send (post-fader) at −18 dB to a delay on the aux lane.

## Glued drums on a bus

1. Route every drum channel to **Bus 1** (right-click → bus assigns).
2. On Bus 1, enable the bus compressor: **4:1 ratio**, **10 ms attack**, **auto-release on**. Set the threshold to pull 2–3 dB on the loudest hits.
3. Boost the bus EQ's **HF** band 1–2 dB at 8 kHz to bring out the cymbals.

## Mastering for streaming

1. Switch to the **MASTERING** stage and **Load latest mixdown**.
2. Enable all three stages: **EQ**, **Comp**, **Limiter**.
3. On the EQ, a 1–2 dB shelf boost at 10 kHz and a 0.5–1 dB cut at 250 Hz is a safe starting point.
4. On the bus comp, aim for 0.5–1 dB of reduction on peaks. Slow attack (30 ms), slow release (250 ms), 2:1.
5. On the limiter, leave the ceiling at **−1.0 dB** for Spotify, **−1.0 dB** for Apple Music, **−1.0 dB** for YouTube. Push the **Drive** until the integrated LUFS reads −14 (Spotify and YouTube) or −16 (Apple Music) — but stop pushing as soon as the limiter is regularly pulling more than 2 dB.
6. Use the **streaming-platform preset** picker to colour-code the readouts and confirm you're within target.

## Headphone cue mix for tracking

1. Send each tracking channel pre-fader to **Aux 4**.
2. Set the aux 4 output to a separate physical output (the headphone amp).
3. Adjust each channel's aux 4 send to balance what the performer hears, independently of what you hear on the main mix.

## Parallel compression on drums

1. Send the drum bus to **Aux 1** at unity, pre-fader.
2. On Aux 1, load a heavy FET-style compressor (or use a plugin) with the ratio at **All buttons in** and a fast attack.
3. Bring the Aux 1 return up underneath the drum bus until the drums punch.

## Splicing a vocal comp from multiple takes

1. Record three or four passes into the same region, each fully containing the last. Each pass pushes the previous one into the take history.
2. In the audio region editor, cycle through takes (**Alt+T** / **Alt+Shift+T**) and listen to each.
3. Pick the best phrases by splitting (**S** or **T**) at the breaths, choosing the best take per phrase, and using fades to mask the joins.

\newpage

# Troubleshooting

## "Plugin crashed — reload to recover"

A plugin running in the OOP sandbox has exited. The slot is auto-bypassed and the plugin's state is preserved.

- Right-click the slot and choose **Reload** to relaunch the plugin with its saved state.
- If the plugin keeps crashing on a specific session, try loading it without OOP (launch Dusk Studio without `DUSKSTUDIO_USE_OOP_PLUGINS=1`) to see if the in-process path is more stable.
- Some plugins are not RT-safe and may misbehave; replace with a different plugin if reloading does not help.

## "(plugin name) — offline"

The session references a plugin Dusk Studio cannot find on this machine.

- The saved state is preserved — the next session save will round-trip it intact.
- Install the missing plugin (or run a fresh plugin scan if it is already installed) and reload the session. The slot will populate.

## Auto-bypass

A plugin is using more than 60% (or 85% in OOP mode) of the audio buffer time for three blocks in a row, and has been bypassed for safety.

- Right-click the slot and choose **Re-enable plugin**.
- If it auto-bypasses again, the plugin is too expensive for the current buffer size. Raise the buffer size in **Settings → Audio**, or use a less expensive plugin.

## Pops, clicks, or glitches

- Raise the audio device's block size in **Settings → Audio**.
- On Linux, raise the ALSA period count from 2 to 3.
- Reduce the effect oversampling to 1× in **Settings → Advanced**.
- Check that no track is clipping (red bar at the top of the input meter).
- Close other CPU-heavy programs.

## "Ping failed — check level / cables"

The hardware-insert latency ping could not find a correlation peak.

- Confirm the output cable is going to the external gear's input, and the gear's output is going to the input cable.
- Raise the output volume on the hardware insert.
- Raise the input gain on your audio interface.
- Confirm the external gear is powered and not in bypass.

## Session won't open

- Check that the session folder contains both `session.json` and an `audio/` subfolder.
- If `session.json` is missing but an autosave file exists (look for `session.autosave.json` in the same folder), rename it to `session.json` and try again.
- Open `session.json` in a text editor to confirm it parses as JSON — a power loss during a non-atomic save on an old filesystem could in theory corrupt it. The atomic-write strategy makes this extremely unlikely.

## Missing audio files

- If you moved or renamed individual files in the `audio/` folder, the regions that reference them will be silent. Restore the file names.
- If you moved the session folder to another machine, the relative paths inside `session.json` should still resolve because every reference is relative to the session folder.

\newpage

# Appendix A — Parameter ranges and defaults

## Channel strip

| Block | Param | Range | Default |
|-------|-------|-------|---------|
| Phase | Invert | Off / On | Off |
| Insert | Mode | Empty / Plugin / Hardware | Plugin |
| HPF | Enable | Off / On | Off |
| HPF | Frequency | 20–300 Hz | 20 Hz |
| LPF | Enable | Off / On | Off |
| LPF | Frequency | 3–20 kHz | 20 kHz |
| EQ | Enable | Off / On | Off |
| EQ | Mode | E (brown) / G (black) | E |
| EQ LF | Frequency | 20–400 Hz | 100 Hz |
| EQ LF | Gain | ±15 dB | 0 dB |
| EQ LM | Frequency | 100 Hz–4 kHz | 600 Hz |
| EQ LM | Gain | ±15 dB | 0 dB |
| EQ LM | Q | 0.4–4.0 | 0.7 |
| EQ HM | Frequency | 600 Hz–13 kHz | 2 kHz |
| EQ HM | Gain | ±15 dB | 0 dB |
| EQ HM | Q | 0.4–4.0 | 0.7 |
| EQ HF | Frequency | 1–20 kHz | 8 kHz |
| EQ HF | Gain | ±15 dB | 0 dB |
| Comp | Enable | Off / On | Off |
| Comp | Mode | Opto / FET / VCA | Opto |
| Opto | Peak reduction | 0–100% | 0% |
| Opto | Gain | 0–100% | 50% |
| Opto | Limit | Off / On | Off |
| FET | Input | −20 to +40 dB | 0 dB |
| FET | Output | −20 to +20 dB | 0 dB |
| FET | Attack | 0.02–80 ms | 0.2 ms |
| FET | Release | 50–1100 ms | 400 ms |
| FET | Ratio | 4:1 / 8:1 / 12:1 / 20:1 / All | 4:1 |
| VCA | Threshold | −38 to +12 dB | +12 dB |
| VCA | Ratio | 1:1–120:1 | 4:1 |
| VCA | Attack | 0.1–50 ms | 1 ms |
| VCA | Release | 10–5000 ms | 100 ms |
| VCA | Output | −20 to +20 dB | 0 dB |
| VCA | Soft knee | Off / On | Off |
| VCA | Detector | Adaptive / Classic | Adaptive |
| Comp | Makeup | −12 to +24 dB | 0 dB |
| Send 1–4 | Level | −60 to +6 dB (or OFF) | OFF |
| Send 1–4 | Pre/Post | Pre / Post | Post |
| Pan | Position | −1.0 to +1.0 | 0 |
| Fader | Level | −∞ to +12 dB | 0 dB |
| Mute | On/Off | Off / On | Off |
| Solo | On/Off | Off / On | Off |
| Bus 1–4 | Assign | Off / On | Off |

## Bus strip

| Block | Param | Range | Default |
|-------|-------|-------|---------|
| EQ | Enable | Off / On | Off |
| EQ LF | Gain | ±9 dB | 0 dB |
| EQ MID | Gain | ±9 dB | 0 dB |
| EQ HF | Gain | ±9 dB | 0 dB |
| Comp | Enable | Off / On | Off |
| Comp | Threshold | −30 to 0 dB | 0 dB |
| Comp | Ratio | 1:1–10:1 | 4:1 |
| Comp | Attack | 0.1–50 ms | 10 ms |
| Comp | Release | 50–1000 ms | 100 ms |
| Comp | Auto release | Off / On | On |
| Comp | Makeup | −10 to +20 dB | 0 dB |
| Pan | Position | −1.0 to +1.0 | 0 |
| Fader | Level | −∞ to +12 dB | 0 dB |
| Mute | On/Off | Off / On | Off |
| Solo | On/Off | Off / On | Off |

## Aux return lane

| Block | Param | Range | Default |
|-------|-------|-------|---------|
| Insert | Mode | Empty / Plugin / Hardware | Plugin |
| Return fader | Level | −∞ to +12 dB | 0 dB |
| Mute | On/Off | Off / On | Off |

## Master bus

| Block | Param | Range | Default |
|-------|-------|-------|---------|
| Tape | Enable | Off / On | Off |
| Tape | HQ (4× OS) | Off / On | Off |
| Pultec | Enable | Off / On | Off |
| Pultec | LF Boost | 0–10 | 0 |
| Pultec | LF Atten | 0–10 | 0 |
| Pultec | LF Freq | 20 / 30 / 60 / 100 Hz | 60 Hz |
| Pultec | HF Boost | 0–10 | 0 |
| Pultec | HF Freq | 3, 4, 5, 8, 10, 12, 16 kHz | 8 kHz |
| Pultec | HF Bandwidth | 0–10 | 0.5 |
| Pultec | HF Atten | 0–10 | 0 |
| Pultec | HF Atten Freq | 5, 10, 20 kHz | 10 kHz |
| Pultec | Output gain | ±12 dB | 0 dB |
| Comp | Enable | Off / On | Off |
| Comp | Threshold | −30 to 0 dB | 0 dB |
| Comp | Ratio | 1:1–10:1 | 4:1 |
| Comp | Attack | 0.1–50 ms | 10 ms |
| Comp | Release | 50–1000 ms | 100 ms |
| Comp | Auto release | Off / On | On |
| Comp | Makeup | −10 to +20 dB | 0 dB |
| Master fader | Level | −∞ to +12 dB | 0 dB |
| Mono | Off / On | Off / On | Off |

## Mastering chain

| Block | Param | Range | Default |
|-------|-------|-------|---------|
| EQ | Enable | Off / On | Off |
| EQ band 0 | Low shelf, 80 Hz, ±15 dB | | 0 dB |
| EQ band 1 | Peaking, 250 Hz, ±15 dB, Q 0.4–4 | | 0 dB, Q 1.0 |
| EQ band 2 | Peaking, 1 kHz, ±15 dB, Q 0.4–4 | | 0 dB, Q 1.0 |
| EQ band 3 | Peaking, 4 kHz, ±15 dB, Q 0.4–4 | | 0 dB, Q 1.0 |
| EQ band 4 | High shelf, 12 kHz, ±15 dB | | 0 dB |
| Comp | Enable | Off / On | Off |
| Comp | Threshold | −30 to 0 dB | 0 dB |
| Comp | Ratio | 1:1–10:1 | 2:1 |
| Comp | Attack | 0.1–50 ms | 30 ms |
| Comp | Release | 50–1000 ms | 250 ms |
| Comp | Auto release | Off / On | On |
| Comp | Makeup | −10 to +20 dB | 0 dB |
| Limiter | Enable | Off / On | On |
| Limiter | Ceiling | −20 to 0 dB | −0.3 dB |
| Limiter | Drive | 0 to +20 dB | 0 dB |
| Limiter | Release | 50–300 ms | 100 ms |

\newpage

# Appendix B — File formats

| Artifact | Format |
|----------|--------|
| Session | JSON, atomic write |
| Recorded audio | 24-bit PCM WAV, session sample rate |
| Imported audio | re-encoded to 24-bit WAV in session/audio/ |
| Recorded MIDI | embedded in session.json (note + CC arrays per region) |
| Plugin scan cache | XML, per-user config directory |
| MIDI bindings export | JSON |
| Bounce | WAV (16, 24, or 32-bit float), chosen at bounce time |

\newpage

# Appendix C — Glossary

**Aux lane.** A return path that receives sends from any number of channels, processes them through one plugin or hardware insert, and feeds the result into the master. Four aux lanes total.

**Bank.** A group of 8 channels mapped to a Mackie Control surface's eight strips. Three banks cover the full 24 channels.

**Bounce.** Offline rendering of the project to a stereo audio file.

**Brickwall limiter.** A compressor with infinite ratio and a hard ceiling, used as the final stage of mastering to prevent inter-sample peaks.

**Bus.** A summing point for multiple channels. Dusk Studio has four buses.

**Count-in.** A configurable number of clicks before recording starts.

**FET compressor.** A solid-state compressor modelled after the 1176, with very fast attack and a discrete ratio selector.

**LUFS.** Loudness Units, Full Scale. A perceptual loudness measurement defined by ITU BS.1770.

**Mackie Control (MCU).** A protocol for control surfaces with motorised faders and bank-of-eight ergonomics.

**Mastering chain.** A separate signal flow that processes a finished mix file through dedicated EQ, compression, and limiting.

**OOP plugin sandbox.** Out-of-process plugin hosting. Each loaded plugin runs in a child process so a crash does not take the DAW with it.

**Opto compressor.** A compressor whose gain reduction is performed by a photo-resistor, modelled after the LA-2A.

**Pultec EQ.** A program-EQ topology with separately controllable boost and cut at the same frequency.

**Punch in / punch out.** Pre-set in and out positions that constrain recording to a specific window.

**Region.** A piece of audio or MIDI placed on the timeline at a specific position.

**SIP (solo-in-place).** A solo mode in which un-soloed tracks are silenced from the main mix output (as opposed to PFL, which only affects monitoring).

**Take.** A single recording pass. Dusk Studio keeps up to 20 previous takes per region.

**Tape strip.** Dusk Studio's timeline canvas.

**VCA compressor.** A compressor with a Voltage-Controlled Amplifier as the gain element. Clean, fast, with wide attack/release ranges and adjustable knee.

---

*Dusk Studio — a portastudio for the desktop.*
