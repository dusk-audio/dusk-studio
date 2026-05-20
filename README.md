# Dusk Studio

[![Linux build](https://github.com/dusk-audio/dusk-studio/actions/workflows/linux-build.yml/badge.svg)](https://github.com/dusk-audio/dusk-studio/actions/workflows/linux-build.yml)
[![Linux arm64](https://github.com/dusk-audio/dusk-studio/actions/workflows/linux-build-arm64.yml/badge.svg)](https://github.com/dusk-audio/dusk-studio/actions/workflows/linux-build-arm64.yml)
[![macOS build](https://github.com/dusk-audio/dusk-studio/actions/workflows/macos-build.yml/badge.svg)](https://github.com/dusk-audio/dusk-studio/actions/workflows/macos-build.yml)
[![Windows build](https://github.com/dusk-audio/dusk-studio/actions/workflows/windows-build.yml/badge.svg)](https://github.com/dusk-audio/dusk-studio/actions/workflows/windows-build.yml)
[![Linux sanitizer](https://github.com/dusk-audio/dusk-studio/actions/workflows/linux-sanitizer.yml/badge.svg)](https://github.com/dusk-audio/dusk-studio/actions/workflows/linux-sanitizer.yml)

A deliberately constrained, portastudio-style DAW for Linux (macOS + Windows in beta). Built for engineers who want to **record, mix, and master without leaving the application** — no plugin paralysis, no menu diving, no infinite-options sprawl.

> *"If it wouldn't exist as a physical control on a $2000 hardware recorder, it probably doesn't belong here."*

JUCE 8 / C++17. PipeWire (primary) via JUCE's JACK backend on Linux; native ALSA backend with USB hot-unplug recovery; macOS CoreAudio + Windows WASAPI / ASIO via JUCE. Authoritative spec: [DuskStudio.md](DuskStudio.md). User guide: [docs/USER_GUIDE.md](docs/USER_GUIDE.md).

## Status

**Alpha — Patreon-gated binaries.** Phase A (data safety) + Phase B (hardening) + Phase C smalls (license, Windows fsync, accessibility floor) shipped. Cross-platform plugin-host isolation, macOS notarisation, and Windows MSI still ahead of 1.0.

| Stage | Status |
|---|---|
| Live mixer (16 ch + 4 aux + master, EQ + comp on each) | Working |
| Multitrack recording / playback with disk-full + MIDI-overflow detection | Working |
| Atomic session save + autosave with content-hash dirty check | Working |
| Plugin hosting (per-channel VST3 / LV2) | Working |
| Plugin offline-state preservation (missing plugin doesn't wipe save) | Working |
| Out-of-process plugin host (crash isolation, **Linux only**) | Working |
| Mastering view (waveform + EQ + multiband comp + L4-style limiter) | Working |
| Bounce / mixdown export | Working |
| Aux sends + reverb / delay returns | Working |
| External hardware inserts (with auto-latency ping) | Working |
| MIDI tracks + instrument plugins | Working |
| Take cycling + comping (option A — cycle previousTakes) | Working |
| Console automation (Write / Read / Touch, per-param lanes) | Working |
| MIDI Clock sync (slave + master) | Working |
| MIDI bindings + MIDI Learn (transport / strip / sends / EQ / comp / plugin params) | Working |
| Multi-file audio + MIDI import with target-track picker | Working |
| MTC (SMPTE quarter-frame) | Deferred — picture-sync workflows only |
| OOP plugin host on macOS + Windows | 1.0 blocker |

65 Catch2 unit tests pass under Release + ASAN+UBSan. Linux + macOS + Windows builds + tests are run on every push.

## Why

Most DAWs are built for production studios with infinite track counts and infinite options. They're also paralysing for ADHD-pattern users — every decision branches, every parameter is reachable, every track type wants its own configuration. Dusk Studio flips the constraint: a fixed signal chain, a finite track count, a single visible page per stage. You commit, you move on.

## The seven hard constraints

These are not implementation details — they're the product. Anything that violates them is wrong.

1. **16 channels maximum.** Fixed. Two banks of 8 to match standard control surfaces.
2. **Fixed signal chain.** No reordering EQ / comp. Channel-strip processing order is the same on every track, every time. Each channel gets **one optional insert slot** (a single VST3 / LV2 plugin **or** a hardware insert — never a chain), at a fixed position in the strip; aux returns get one plugin slot each.
3. **No waveform editing.** Region-level move / split / delete / trim only. (Draw mode exists only inside the MIDI piano roll.)
4. **Console-style automation only.** Write / Read / Touch via gesture; no curve drawing.
5. **Everything visible within a stage.** Four workflow stages match the portastudio layout (Recording / Mixing / Aux / Mastering); within each stage there are no tabs and no hidden panels. The MIDI piano roll, plugin editors, and audio-settings dialog are embedded modals over the current stage.
6. **No preferences sprawl.** A single Audio Settings panel covers audio device + buffer + oversampling + MIDI sync (Clock in / out, chase, emit) + MIDI bindings + UI scale. No per-feature settings menus, no global preferences window.
7. **Portastudio philosophy.** "Would this exist on a $2000 hardware recorder?" If no, don't build it.

## Architecture

```
Channel 1-16 ──────────────→ 4 Aux Buses ──→ Master ──→ Output
   HPF                          EQ              Pultec EQ
   4-band EQ                    Comp            Bus Comp
   FET/Opto Comp                Insert slot     Tape Saturation
   Insert slot (plugin or HW)   Fader           Fader
   Pan + Sends
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
  util/        # CrashHandler (FileLogger + signal-handler reports)
tests/         # Catch2 unit tests (session, recording, MIDI, IPC, DSP)
packaging/     # .desktop, AppStream, MIME — for AppImage builds
docs/          # User guide + onboarding docs
DuskStudio.md       # authoritative product spec
```

## Builds & contributing

Precompiled binaries delivered via Patreon (Linux AppImage today; macOS + Windows when the cross-platform plugin host lands). Self-build is fully supported and equivalent at the source level — no support tier for self-builders.

| Platform | Doc |
|----------|-----|
| Linux | [BUILDING-LINUX.md](BUILDING-LINUX.md) |
| Windows | [BUILDING-WINDOWS.md](BUILDING-WINDOWS.md) |
| macOS | Mirror of Linux flow; upstream JUCE 8.0.4 + sibling `plugins-main`. Smoke-built per push on `macos-14` (Apple Silicon Sonoma) — see [.github/workflows/macos-build.yml](.github/workflows/macos-build.yml). |
| AppImage packaging (Linux) | [packaging/README.md](packaging/README.md) |
| User guide / troubleshooting | [docs/USER_GUIDE.md](docs/USER_GUIDE.md) |

After a build, sanity check with `Dusk Studio --version` — prints app + JUCE + platform string and exits 0. Useful as a paste-target for Patreon support DMs.

CI runs on every push to `main` against Linux (Ubuntu 22.04 GCC), macOS (14 Apple Silicon), and Windows (Server 2022 MSVC). Linux sanitizer (ASAN+UBSan) runs nightly and on engine/dsp/session paths.

## License

[GPL-3.0-or-later](LICENSE), to match JUCE's licensing. Third-party
component inventory in [LICENSES.txt](LICENSES.txt).

Dusk Studio ships under a **dual access model**:

- **Source**: GPL-3.0. Clone, audit, build, modify, redistribute — all
  fine under GPL terms.
- **Patreon binaries**: precompiled Linux AppImages (and eventually
  macOS / Windows installers) delivered to supporters. The payment is
  for packaging, signing, and support access — the source remains
  open. Self-builders get no support, but the code is the same.
- **Paid 1.0 ($29)**: same source, same GPL. Buys you the signed
  installer + access to bug triage.
