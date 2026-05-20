# Dusk Studio — User Guide

Dusk Studio is a 16-channel portastudio-style DAW for Linux (macOS + Windows
in beta). Deliberately constrained: fixed signal chain, no plugin
chains on channels, no waveform-level editing. Built to finish songs
instead of fiddling with them.

## Quick start: record your first track

1. **Launch Dusk Studio.** Empty session opens with 16 channel strips on the
   mixer view.
2. **Pick an audio device.** Open the Audio Device panel (top-left
   gear icon or `Audio` menu). Choose your interface, sample rate,
   buffer size. 48 kHz / 256 samples is a safe default; drop to 128 or
   64 if your interface can handle it for live monitoring.
3. **Arm a track.** Click the round `REC` button on a channel strip.
   The strip turns red. Repeat for any additional tracks you want to
   capture simultaneously (up to 16 at once).
4. **Pick the input.** The track defaults to "follow track index"
   (track 1 → input 1, track 2 → input 2, etc.). To override, click
   the `IN` selector at the top of the strip.
5. **Enable input monitor.** Click `IN` button to hear yourself
   through the signal chain while recording. Tracks meter and record
   even when monitor is off; the toggle only affects what you hear.
6. **Hit record.** Press the record transport button (or `R` key).
   The transport starts rolling. Stop with the stop button (or
   `Space` / `R` again).

The take lands as an `AudioRegion` in the timeline. Open the Editor
view to trim, move, split, or cycle to a previous take.

## Signal flow

Per channel strip:

```
Input → HPF → 4-band EQ → FET/Opto/VCA Compressor → Insert (plugin OR
                                                              hardware)
      → Sends (4 aux) → Pan → Bus assign → Fader → Mute/Solo → Output
```

Aux returns:

```
Aux Sends from 16 channels → Aux Lane (4 lanes)
                              ↓
                              EQ → Comp → Plugin/HW Insert → Fader → Master
```

Master bus:

```
Sum of all bus assigns + aux returns → Pultec EQ → Bus Comp → Tape Sat
                                       → Limiter → Output
```

Everything is visible. No reordering, no hidden routing.

## Keyboard shortcuts

| Key | Action |
|-----|--------|
| `Space` | Play / Stop toggle |
| `R` | Record arm-and-roll (or stop if recording) |
| `K` | Toggle the virtual MIDI keyboard overlay |
| `Cmd/Ctrl + S` | Save session |
| `Cmd/Ctrl + Z` / `Shift+Cmd/Ctrl+Z` | Undo / Redo |
| `1`–`8` | Bank A: select track 1–8 on the focused strip |
| `Shift + 1`–`8` | Bank B: select track 9–16 |

Right-click any fader, mute, send, or transport button to bind a MIDI
controller via **MIDI Learn**.

## Troubleshooting

### Xruns / glitches

- **Increase buffer size** in Audio Device panel. 128 → 256 → 512.
  Each step doubles available headroom at the cost of ~5 ms latency.
- **Bypass heavy plugins** with the `BYP` button on each insert slot.
  Dusk Studio auto-bypasses plugins that exceed their CPU budget for several
  consecutive blocks (look for `(stalled)` on the slot label).
- **Out-of-process plugin host** isolates plugin CPU spikes from
  Dusk Studio's audio thread. Set `DUSKSTUDIO_USE_OOP_PLUGINS=1` before launch.
  Currently Linux-only; macOS and Windows ports land in 1.0.

### "Plugin failed - offline"

A slot's saved plugin couldn't be re-instantiated on this machine
(plugin missing, moved, format changed). The slot label shows
`⚠ <PluginName> (offline)`. **Your saved state is preserved on
disk** — the next save round-trips the offline blob, so reinstalling
the plugin and reloading the session restores it. Click the `X` to
clear the slot if you don't plan to reinstall.

### Device not detected

- **Linux + PipeWire**: Dusk Studio uses ALSA directly by default (lower
  latency, no PipeWire graph hops). Force JACK via the dropdown if
  your interface is owned by PipeWire's JACK backend.
- **USB hot-unplug** during recording: Dusk Studio stops the transport and
  finalises the in-flight WAV. If a take is incomplete, check the
  `Recording errors` dialog at the next stop — it lists per-track
  failures with byte counts.
- **No outputs after device change**: open Audio Device panel and
  re-pick the output. Dusk Studio sometimes shows the prior device's last
  output config until the new device's channel mask is published.

### Where logs and crash reports live

| OS | Path |
|----|------|
| Linux | `~/.local/share/Dusk Studio/log/dusk-studio-YYYYMMDD.log` |
| macOS | `~/Library/Application Support/Dusk Studio/log/dusk-studio-YYYYMMDD.log` |
| Windows | `%APPDATA%/Dusk Studio/log/dusk-studio-YYYYMMDD.log` |

Crash reports under `crashes/` next to the log dir. **For Patreon
support, paste the output of `Dusk Studio --version` plus the most recent
log file into your DM.**

## The seven hard constraints

By design — these will never change. If they bother you, Dusk Studio is the
wrong tool:

1. **16 channels.** Two banks of 8.
2. **Fixed signal chain.** No reordering.
3. **No waveform editing.** Region-level only.
4. **Console-style automation.** Write/Read/Touch via gesture.
5. **Everything visible.** No hidden panels.
6. **No preferences sprawl.** Audio device config and that's it.
7. **Portastudio philosophy.** Would this exist on a $2000 hardware
   recorder? If no, it doesn't ship.
