# Dusk Studio

[![Linux build](https://github.com/dusk-audio/dusk-studio/actions/workflows/linux-build.yml/badge.svg)](https://github.com/dusk-audio/dusk-studio/actions/workflows/linux-build.yml)
[![macOS build](https://github.com/dusk-audio/dusk-studio/actions/workflows/macos-build.yml/badge.svg)](https://github.com/dusk-audio/dusk-studio/actions/workflows/macos-build.yml)
[![Windows build](https://github.com/dusk-audio/dusk-studio/actions/workflows/windows-build.yml/badge.svg)](https://github.com/dusk-audio/dusk-studio/actions/workflows/windows-build.yml)
[![Windows tests](https://github.com/dusk-audio/dusk-studio/actions/workflows/windows-tests.yml/badge.svg)](https://github.com/dusk-audio/dusk-studio/actions/workflows/windows-tests.yml)
[![Linux sanitizer (TSan)](https://github.com/dusk-audio/dusk-studio/actions/workflows/linux-sanitizer.yml/badge.svg)](https://github.com/dusk-audio/dusk-studio/actions/workflows/linux-sanitizer.yml)

A deliberately constrained, portastudio-style DAW for Linux, macOS, and Windows. Built for engineers who want to **record, mix, and master without leaving the application**: no plugin paralysis, no menu diving, no infinite-options sprawl.

> *"Fixed signal chain, finite track count, one page per stage. You commit, you move on."*

JUCE 8 / C++17. PipeWire (primary) via JUCE's JACK backend on Linux; native ALSA backend with USB hot-unplug recovery; macOS CoreAudio + Windows WASAPI / ASIO via JUCE. Authoritative spec: [DuskStudio.md](DuskStudio.md). User manual: [MANUAL.md](MANUAL.md).

## This is an alpha

Dusk Studio is at **v0.9.0 (alpha)**. The 1.0 feature set is essentially complete and everything in the status table below works, but "works" is not "polished." Expect rough edges, expect the occasional strange behavior, and please report everything you hit. Think "watch it grow," not "replace your DAW today."

Least-tested areas right now: the mastering-stage UI, the hardware-insert latency ping against real outboard gear, and session-handling edge cases.

## Get Dusk Studio

GPL source on this repo. Build from source and the binary costs you nothing but compile time. If you want a precompiled, supported binary, pick a tier below.

| Tier | Price | What you get |
|---|---|---|
| **Source** | Free | Clone, build, audit. GPL-3.0. No support tier. |
| **Patreon Supporter** | from $3 / month | Latest release binaries (Linux AppImage + Windows MSI + macOS DMG, all unsigned) delivered as attachments on each release post. Name in the credits (Dusk Studio About panel + plugin About panels). Lapse keeps whatever build you've already downloaded. |
| **Patreon Patron** | $5 / month | Everything above + early-access builds 1–2 weeks ahead of public. |
| **Patreon Champion** | $10 / month | Everything above + DM support + roadmap-feature votes. |
| **One-time licence** | **$27** | Current major version (1.x.x). Every 1.x minor + patch update included. 2.0 requires a new purchase (or the lifetime upgrade). **Buying during the alpha includes every alpha and beta build now, plus the entire 1.x cycle when it ships.** |
| **Lifetime** | $49 | Current major (1.x.x) plus the next major (2.x.x). Two majors of updates for less than two licences. |

Everyone backing during the alpha is a **Founding Patron**: your name goes in Dusk Studio's About panel, permanently.

*Version-discipline contract: major bumps (1.x → 2.x) mean roadmap-defining shifts such as new stages or core-architecture changes. Plugin additions, UI polish, performance work, and new DSP modules all stay within the current major.*

Paid via [Patreon](https://www.patreon.com/cw/DuskAudio) (recurring) or [GitHub Sponsors](https://github.com/sponsors/marc-korte) (one-time, $27 + $49 amounts). Buyers get invited to the private releases repo where every build lands.

**First-time launch:** binaries are unsigned by design (no Apple Developer ID, no Windows Authenticode; not currently planned). macOS Gatekeeper and Windows SmartScreen will warn on first launch. See [MANUAL.md § Installing Dusk Studio](MANUAL.md#installing-dusk-studio) for the 30-second bypass per OS. Linux AppImages need no bypass.

## Status

**v0.9.0 (alpha).** Feature backlog for 1.0 effectively closed: every spec phase, Tascam DP-24SD parity, MTC + MIDI Clock sync, cross-platform OOP plugin host (audio on all three OSes; editor embedded on Linux + Windows, in-process shell on macOS), and the rename to Dusk Studio have shipped. All three OSes ship unsigned binaries (Linux AppImage + Windows MSI + macOS DMG) to the private releases repo on each tag. Remaining 1.0 work is polish, deeper accessibility, and cross-process NSView embedding research.

| Stage | Status |
|---|---|
| Live mixer (24 ch in 3 banks of 8 + 4 aux + 4 mix buses + master) | Working |
| Multitrack recording / playback with disk-full + MIDI-overflow detection | Working |
| Atomic session save + 30 s autosave with content-hash dirty check | Working |
| Plugin hosting (per-channel VST3 / LV2 / AU + per-aux return) | Working |
| Native soundfonts (`.sfz` + `.sf2` via sfizz, no external synth) | Working |
| Plugin offline-state preservation (missing plugin doesn't wipe save) | Working |
| Out-of-process plugin host: audio (Linux + macOS + Windows) | Working |
| Out-of-process plugin host: editor embed (Linux XEmbed, Windows SetParent, macOS in-process shell) | Working |
| Mastering view (waveform + 5-band EQ + multiband comp + brick-wall limiter + BS.1770) | Working |
| Bounce / mixdown export (master or stems) | Working |
| Aux sends + reverb / delay returns | Working |
| External hardware inserts (per channel + per aux, with auto-latency ping) | Working |
| MIDI tracks + instrument plugins + piano roll editor | Working |
| Audio region editor (non-destructive trim / fade / gain) + 20-take history | Working |
| Take cycling + comping (cycle previousTakes, Option A) | Working |
| Console automation (Write / Read / Touch on channels + aux + master) | Working |
| MIDI Clock sync + MTC slave + master | Working |
| MIDI bindings + MIDI Learn (transport / strip / sends / EQ / comp / plugin params) | Working |
| Mackie Control surface (tested against Tascam DP-24SD) | Working |
| Multi-file audio + MIDI import with target-track picker | Working |
| Cross-process NSView embed (macOS editor) | Research |
| Windows MSI installer (unsigned) | Working (CI publishes to private releases repo on tag) |
| Linux AppImage | Working (CI publishes to private releases repo on tag) |
| macOS DMG (unsigned, ad-hoc) | Working (CI publishes to private releases repo on tag) |
| Deeper a11y (full screen-reader labels + keyboard-only mixer nav) | Floor only |

150 Catch2 unit tests across 38 files. Linux (amd64 + arm64) + macOS + Windows builds run on every push; Windows tests run on every push; Linux ThreadSanitizer runs on every PR + push.

## Bug reports & community

**[Post in Discussions](https://github.com/dusk-audio/dusk-studio/discussions)**: Bugs, Q&A, Ideas, and Show your music. See the pinned Read First post for what to include in a bug report. Confirmed bugs get promoted to [Issues](https://github.com/dusk-audio/dusk-studio/issues).

Paid-tier users can also DM via Patreon; one-time and lifetime licence-holders get a direct support email link in their release-repo invite.

## Why

Most DAWs are built for production studios with infinite track counts and infinite options. They're also paralysing for ADHD-pattern users: every decision branches, every parameter is reachable, every track type wants its own configuration. Dusk Studio flips the constraint: a fixed signal chain, a finite track count, a single visible page per stage. You commit, you move on.

## Built with AI assistance

I build Dusk Studio with the help of AI coding tools. If you have an issue with that, this DAW is not for you. The 150-test suite, the sanitizer runs, and the CI matrix above are what keep the code honest, and the source is open if you'd rather judge it on its merits.

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
packaging/     # .desktop, AppStream, MIME, macOS bundle: for AppImage + DMG builds
DuskStudio.md  # authoritative product spec
MANUAL.md      # end-user manual (Pandoc-buildable to PDF via packaging/build-pdf.sh)
```

## Builds & contributing

Precompiled (unsigned) binaries delivered via Patreon: Linux AppImage + Windows MSI + macOS DMG, all published to the private releases repo on each tag. Self-build is fully supported and equivalent at the source level; there is no support tier for self-builders.

| Platform | Doc |
|----------|-----|
| Linux | [BUILDING-LINUX.md](BUILDING-LINUX.md) |
| Windows | [BUILDING-WINDOWS.md](BUILDING-WINDOWS.md) |
| macOS | Mirror of Linux flow; upstream JUCE 8.0.4 + sibling `plugins-main`. Smoke-built per push on `macos-14` (Apple Silicon Sonoma). See [.github/workflows/macos-build.yml](.github/workflows/macos-build.yml). |
| AppImage packaging (Linux) | [packaging/README.md](packaging/README.md) |
| End-user manual / troubleshooting | [MANUAL.md](MANUAL.md) |

After a build, sanity check with `Dusk Studio --version`: prints app + JUCE + platform string and exits 0. Useful as a paste-target for Patreon support DMs.

CI runs on every push to `main` against Linux (Ubuntu 22.04 GCC), macOS (14 Apple Silicon, Ninja + ccache), and Windows (Server 2022 MSVC). Windows tests (`windows-tests.yml`) exercise the Catch2 suite on every PR. Linux ThreadSanitizer (`linux-sanitizer.yml`) runs the Catch2 suite under TSan on every PR + push. Tagged releases (`v*`) trigger the Windows MSI, macOS DMG, and Linux AppImage workflows; each builds an unsigned binary and publishes it to the private releases repo (one shared release per tag, distinct asset names).

## License

[GPL-3.0-or-later](LICENSE), to match JUCE's licensing. Third-party component inventory in [LICENSES.txt](LICENSES.txt).

Dusk Studio ships under a **dual access model**:

- **Source**: GPL-3.0. Clone, audit, build, modify, redistribute. All fine under GPL terms.
- **Patreon binaries**: precompiled, unsigned Linux AppImages + Windows MSIs + macOS DMGs delivered to supporters. The payment is for packaging + support access; the source remains open. Self-builders get no support, but the code is the same.
- **Paid licence ($27)**: same source, same GPL. Buys you the prebuilt installer + access to bug triage.
