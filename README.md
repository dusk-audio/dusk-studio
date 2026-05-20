# Focal

A deliberately constrained, portastudio-style DAW for Linux. Built for engineers who want to **record, mix, and master without leaving the application** — no plugin paralysis, no menu diving, no infinite-options sprawl.

> *"If it wouldn't exist as a physical control on a $2000 hardware recorder, it probably doesn't belong here."*

JUCE 8 / C++17. PipeWire (primary) via JUCE's JACK backend; ALSA fallback. Authoritative spec: [Focal.md](Focal.md).

## Status

**Phase 1a-2 in progress.** Alpha. Not for production work yet.

| Stage | Status |
|---|---|
| Live mixer (16 ch + 4 aux + master, EQ + comp on each) | Working |
| Multitrack recording / playback | Working |
| Plugin hosting (per-channel VST3 / LV2) | Working |
| Mastering view (waveform + EQ + multiband comp + L4-style limiter) | Working |
| Bounce / mixdown export | Working |
| Aux sends + reverb / delay returns | In progress |
| MIDI tracks + instrument plugins | Phase 4 |
| Console automation (Write / Read / Touch) | Phase 3 |

6/6 headless self-test passes at 96 kHz / 256 samples on UMC1820 ALSA.

## Why

Most DAWs are built for production studios with infinite track counts and infinite options. They're also paralysing for ADHD-pattern users — every decision branches, every parameter is reachable, every track type wants its own configuration. Focal flips the constraint: a fixed signal chain, a finite track count, a single visible page per stage. You commit, you move on.

## The seven hard constraints

These are not implementation details — they're the product. Anything that violates them is wrong.

1. **16 channels maximum.** Fixed. Two banks of 8 to match standard control surfaces.
2. **Fixed signal chain.** No reordering EQ / comp. No adding / removing processors. Channel-strip processing order is the same on every track, every time.
3. **Simple waveform editing.** Region-level move / split / delete / trim only.
4. **Console-style automation only.** Write / Read / Touch via gesture; no curve drawing.
5. **Everything visible.** No tabs, no hidden panels (MIDI piano roll overlay is the only exception).
6. **No preferences sprawl.** Audio device config and that's it.
7. **Portastudio philosophy.** "Would this exist on a $2000 hardware recorder?" If no, don't build it.

## Architecture

```
Channel 1-16 → 4 Aux Buses → Master → Output
   HPF              EQ          Pultec EQ
   4-band EQ        Comp        Bus Comp
   FET/Opto Comp    Fader       Tape Saturation
   Pan + Sends                  Fader
   Fader
   Mute / Solo / Ø
```

- **DSP** is extracted from the Dusk Audio plugin suite (4K EQ, Multi-Comp FET/Opto, Multi-Q Pultec, TapeMachine, shared AnalogEmulation) so the mixer and the standalone plugins share a single DSP source of truth.
- **Plugin host**: VST3 + LV2 (yabridge-friendly) on every channel strip; aux buses host reverb / delay returns.

## Repository

```
src/
  dsp/         # ChannelStrip, AuxBusStrip, MasterBus, BrickwallLimiter, etc.
  engine/      # AudioEngine, RecordManager, PlaybackEngine, BounceEngine, MasteringChain
  session/     # Session model + JSON serialisation
  ui/          # MainComponent, ConsoleView, channel/aux/master strips, mastering view
Focal.md      # authoritative product spec
```

## Builds & contributing

Pre-built signed binaries will be available at **focal.audio** (coming soon). The source is GPL-3.0 and you're welcome to read it and propose changes; for build instructions and contributor onboarding, please reach out via the project's contact channel rather than relying on what's in this repository.

## License

[GPL-3.0-or-later](LICENSE), to match JUCE's licensing. Third-party
component inventory in [LICENSES.txt](LICENSES.txt).

Focal ships under a **dual access model**:

- **Source**: GPL-3.0. Clone, audit, build, modify, redistribute — all
  fine under GPL terms.
- **Patreon binaries**: precompiled Linux AppImages (and eventually
  macOS / Windows installers) delivered to supporters. The payment is
  for packaging, signing, and support access — the source remains
  open. Self-builders get no support, but the code is the same.
- **Paid 1.0 ($29)**: same source, same GPL. Buys you the signed
  installer + access to bug triage.
