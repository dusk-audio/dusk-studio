# Dusk Studio

> **Disclaimer:** I build Dusk Studio with the help of AI coding tools. If you have an issue with that, this DAW is not for you.

[![Linux build](https://github.com/dusk-audio/dusk-studio/actions/workflows/linux-build.yml/badge.svg)](https://github.com/dusk-audio/dusk-studio/actions/workflows/linux-build.yml)
[![macOS build](https://github.com/dusk-audio/dusk-studio/actions/workflows/macos-build.yml/badge.svg)](https://github.com/dusk-audio/dusk-studio/actions/workflows/macos-build.yml)
[![Windows build](https://github.com/dusk-audio/dusk-studio/actions/workflows/windows-build.yml/badge.svg)](https://github.com/dusk-audio/dusk-studio/actions/workflows/windows-build.yml)
[![Windows tests](https://github.com/dusk-audio/dusk-studio/actions/workflows/windows-tests.yml/badge.svg)](https://github.com/dusk-audio/dusk-studio/actions/workflows/windows-tests.yml)
[![Linux sanitizer (TSan)](https://github.com/dusk-audio/dusk-studio/actions/workflows/linux-sanitizer.yml/badge.svg)](https://github.com/dusk-audio/dusk-studio/actions/workflows/linux-sanitizer.yml)

A deliberately constrained, portastudio-style DAW for Linux, macOS, and Windows. Built for engineers who want to **record, mix, and master without leaving the application** — no plugin paralysis, no menu diving, no infinite-options sprawl.

> *"If it wouldn't exist as a physical control on a $2000 hardware recorder, it probably doesn't belong here."*

JUCE 8 / C++17. PipeWire (primary) via JUCE's JACK backend on Linux; native ALSA backend with USB hot-unplug recovery; macOS CoreAudio + Windows WASAPI / ASIO via JUCE. Authoritative spec: [DuskStudio.md](DuskStudio.md). User manual: [MANUAL.md](MANUAL.md).

## Get Dusk Studio

GPL source on this repo — build from source and the binary costs you nothing but compile time. If you want a precompiled, supported binary, pick one of the paid tiers below.

| Tier | Price | What you get |
|---|---|---|
| **Source** | Free | Clone, build, audit. GPL-3.0. No support tier. |
| **Patreon Supporter** | from $3 / month | Latest release binaries (Linux AppImage + Windows MSI; macOS DMG when the Apple Dev cert lands) delivered as attachments on each release post. Name in plugin credits. Lapse keeps whatever build you've already downloaded. |
| **Patreon Patron** | $5 / month | Everything above + early-access beta builds 1–2 weeks ahead of public. |
| **Patreon Champion** | $10 / month | Everything above + DM support + roadmap-feature votes. |
| **One-time licence** | **$27** | Current major version (1.x.x). Every 1.x minor + patch update included. 2.0 requires a new purchase (or the lifetime upgrade). |
| **Lifetime** | $49 | Current major (1.x.x) plus the next major (2.x.x). Two majors of updates for less than two licences. |

*Version-discipline contract: major bumps (1.x → 2.x) mean roadmap-defining shifts — new stages, core-architecture changes. Plugin additions, UI polish, performance work, new DSP modules all stay within the current major.*

Paid via [Patreon](https://www.patreon.com/cw/DuskAudio) (recurring) or [GitHub Sponsors](https://github.com/sponsors/marc-korte) (one-time, $27 + $49 amounts). Buyer gets invited to the private releases repo where every signed binary lands.

**First-time launch:** binaries are unsigned. macOS Gatekeeper + Windows SmartScreen will warn on first launch — see [MANUAL.md § Installing Dusk Studio](MANUAL.md#installing-dusk-studio) for the 30-second bypass per OS.

## Status

**v0.9.0 (beta).** Feature backlog effectively closed: every spec phase, Tascam DP-24SD parity, MTC + MIDI Clock sync, cross-platform OOP plugin host (audio on all three OSes; editor embedded on Linux + Windows, in-process shell on macOS), and the rename to Dusk Studio have shipped. Remaining 1.0 work is the macOS notarised DMG (deferred until paid Apple Dev cert), deeper accessibility, and cross-process NSView embedding research.

| Stage | Status |
|---|---|
| Live mixer (24 ch in 3 banks of 8 + 4 aux + 4 mix buses + master) | Working |
| Multitrack recording / playback with disk-full + MIDI-overflow detection | Working |
| Atomic session save + 30 s autosave with content-hash dirty check | Working |
| Plugin hosting (per-channel VST3 / LV2 / AU + per-aux return) | Working |
| Native soundfonts (`.sfz` + `.sf2` via sfizz, no external synth) | Working |
| Plugin offline-state preservation (missing plugin doesn't wipe save) | Working |
| Out-of-process plugin host — audio (Linux + macOS + Windows) | Working |
| Out-of-process plugin host — editor embed (Linux XEmbed, Windows SetParent, macOS in-process shell) | Working |
| Mastering view (waveform + 5-band EQ + multiband comp + brick-wall limiter + BS.1770) | Working |
| Bounce / mixdown export (master or stems) | Working |
| Aux sends + reverb / delay returns | Working |
| External hardware inserts (per channel + per aux, with auto-latency ping) | Working |
| MIDI tracks + instrument plugins + piano roll editor | Working |
| Audio region editor (non-destructive trim / fade / gain) + 20-take history | Working |
| Take cycling + comping (cycle previousTakes — Option A) | Working |
| Console automation (Write / Read / Touch on channels + aux + master) | Working |
| MIDI Clock sync + MTC slave + master | Working |
| MIDI bindings + MIDI Learn (transport / strip / sends / EQ / comp / plugin params) | Working |
| Mackie Control surface (tested against Tascam DP-24SD) | Working |
| Multi-file audio + MIDI import with target-track picker | Working |
| Cross-process NSView embed (macOS editor) | Research |
| Windows MSI installer | Working (CI publishes to private releases repo) |
| macOS signed + notarised DMG | Deferred (paid Apple Dev cert) |
| Deeper a11y (full screen-reader labels + keyboard-only mixer nav) | Floor only |

150 Catch2 unit tests across 38 files. Linux (amd64 + arm64) + macOS + Windows builds run on every push; Windows tests run on every push; Linux ThreadSanitizer runs on every PR + push.

## Bug reports

[Open an issue on GitHub.](https://github.com/dusk-audio/dusk-studio/issues) Patreon / paid-tier users can also DM via Patreon; one-time + lifetime licence-holders get a direct support email link in their release-repo invite.

## Why

Most DAWs are built for production studios with infinite track counts and infinite options. They're also paralysing for ADHD-pattern users — every decision branches, every parameter is reachable, every track type wants its own configuration. Dusk Studio flips the constraint: a fixed signal chain, a finite track count, a single visible page per stage. You commit, you move on.

## The seven hard constraints

These are not implementation details — they're the product. Anything that violates them is wrong.

1. **24 channels maximum.** Fixed. Three banks of 8 to match standard control surfaces (each bank drives 8 strips on the surface; all 24 are visible on screen).
2. **Fixed signal chain.** No reordering EQ / comp. Channel-strip processing order is the same on every track, every time. Each channel gets **one optional insert slot** (a single VST3 / LV2 / AU plugin **or** a hardware insert — never a chain), at a fixed position in the strip; aux returns get one plugin slot each.
3. **No waveform editing.** Region-level move / split / delete / trim / fade / gain only. (Draw mode exists only inside the MIDI piano roll.)
4. **Console-style automation only.** Write / Read / Touch via gesture; no curve drawing.
5. **Everything visible within a stage.** Four workflow stages match the portastudio layout (Recording / Mixing / Aux / Mastering); within each stage there are no tabs and no hidden panels. The MIDI piano roll, plugin editors, and audio-settings dialog are embedded modals over the current stage.
6. **No preferences sprawl.** A single Audio Settings panel covers audio device + buffer + oversampling + MIDI sync (Clock in / out, chase, emit) + MIDI bindings + UI scale. No per-feature settings menus, no global preferences window.
7. **Portastudio philosophy.** "Would this exist on a $2000 hardware recorder?" If no, don't build it.

## Architecture

```
Channels 1-24 ───────────────→ 4 Aux Buses ──→ 4 Mix Buses ──→ Master ──→ Output
   Phase / Polarity              EQ              3-band EQ        Pultec EQ
   Insert (plugin or HW)         Comp            SSL Bus Comp     Bus Comp
   HPF + LPF                     Fader                            Tape Saturation
   4-band EQ                                                      Fader
   Compressor (Opto/FET/VCA)
   Aux sends (4) + Pan
   Fader / Mute / Solo
```

- **DSP** is extracted from the Dusk Audio plugin suite (4K EQ, Multi-Comp FET/Opto/VCA, Multi-Q Pultec, TapeMachine, shared AnalogEmulation) so the mixer and the standalone plugins share a single DSP source of truth.
- **Plugin host**: VST3 + LV2 + AU on every channel strip; aux returns host reverb / delay; per-platform IPC backend (Linux `shm` + `eventfd`, macOS `shm_open` + `os_sync_wait_on_address`, Windows `CreateFileMapping` + `WaitOnAddress`) keeps a crashing plugin from taking the host down.
- **Soundfonts**: `.sfz` and `.sf2` play through the built-in [sfizz](https://github.com/dusk-audio/sfizz) engine (SF2 → SFZ on load). No external synth required.

## Repository

```
src/
  dsp/         # ChannelStrip, AuxBusStrip, MasterBus, BrickwallLimiter, etc.
  engine/      # AudioEngine, RecordManager, PlaybackEngine, BounceEngine, MasteringChain
    ipc/       # OOP plugin host: PluginHostMain, RemotePluginConnection
      platform/# Linux / macOS / Windows IPC backends (shm + sync + process)
  session/     # Session model + JSON serialisation
  ui/          # MainComponent, ConsoleView, channel/aux/master strips, mastering view
  util/        # CrashHandler (FileLogger + signal-handler reports)
tests/         # 150 Catch2 unit tests (session, recording, MIDI, IPC, DSP)
packaging/     # .desktop, AppStream, MIME, macOS bundle — for AppImage + DMG builds
DuskStudio.md  # authoritative product spec
MANUAL.md      # end-user manual (Pandoc-buildable to PDF via packaging/build-pdf.sh)
```

## Builds & contributing

Precompiled binaries delivered via Patreon (Linux AppImage + Windows MSI today via the private releases repo; macOS DMG deferred until the paid Apple Developer cert lands). Self-build is fully supported and equivalent at the source level — no support tier for self-builders.

| Platform | Doc |
|----------|-----|
| Linux | [BUILDING-LINUX.md](BUILDING-LINUX.md) |
| Windows | [BUILDING-WINDOWS.md](BUILDING-WINDOWS.md) |
| macOS | Mirror of Linux flow; upstream JUCE 8.0.4 + sibling `plugins-main`. Smoke-built per push on `macos-14` (Apple Silicon Sonoma) — see [.github/workflows/macos-build.yml](.github/workflows/macos-build.yml). |
| AppImage packaging (Linux) | [packaging/README.md](packaging/README.md) |
| End-user manual / troubleshooting | [MANUAL.md](MANUAL.md) |

After a build, sanity check with `Dusk Studio --version` — prints app + JUCE + platform string and exits 0. Useful as a paste-target for Patreon support DMs.

CI runs on every push to `main` against Linux (Ubuntu 22.04 GCC), macOS (14 Apple Silicon, Ninja + ccache), and Windows (Server 2022 MSVC). Windows tests (`windows-tests.yml`) exercise the Catch2 suite on every PR. Linux ThreadSanitizer (`linux-sanitizer.yml`) runs the Catch2 suite under TSan on every PR + push. Tagged releases (`v*`) trigger the Windows MSI workflow (publishes to private releases repo) and the macOS signed + notarised DMG workflow (when the Developer ID cert is configured).

## License

[GPL-3.0-or-later](LICENSE), to match JUCE's licensing. Third-party
component inventory in [LICENSES.txt](LICENSES.txt).

Dusk Studio ships under a **dual access model**:

- **Source**: GPL-3.0. Clone, audit, build, modify, redistribute — all
  fine under GPL terms.
- **Patreon binaries**: precompiled Linux AppImages + Windows MSIs
  delivered to supporters today (macOS DMGs land once the Apple
  Developer cert is in hand). The payment is for packaging, signing,
  and support access — the source remains open. Self-builders get no
  support, but the code is the same.
- **Paid licence ($27)**: same source, same GPL. Buys you the signed
  installer + access to bug triage.
