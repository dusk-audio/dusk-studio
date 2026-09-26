# Dusk Studio

> **Disclaimer:** I build Dusk Studio with the help of AI coding tools. If you have an issue with that, this DAW is not for you.

[![Linux build](https://github.com/dusk-audio/dusk-studio/actions/workflows/linux-build.yml/badge.svg)](https://github.com/dusk-audio/dusk-studio/actions/workflows/linux-build.yml)
[![Linux arm64 build](https://github.com/dusk-audio/dusk-studio/actions/workflows/raspberry-pi-build.yml/badge.svg)](https://github.com/dusk-audio/dusk-studio/actions/workflows/raspberry-pi-build.yml)
[![macOS build](https://github.com/dusk-audio/dusk-studio/actions/workflows/macos-build.yml/badge.svg)](https://github.com/dusk-audio/dusk-studio/actions/workflows/macos-build.yml)
[![Windows tests](https://github.com/dusk-audio/dusk-studio/actions/workflows/windows-tests.yml/badge.svg)](https://github.com/dusk-audio/dusk-studio/actions/workflows/windows-tests.yml)
[![Linux sanitizers](https://github.com/dusk-audio/dusk-studio/actions/workflows/linux-sanitizer.yml/badge.svg)](https://github.com/dusk-audio/dusk-studio/actions/workflows/linux-sanitizer.yml)

A deliberately constrained, portastudio-style DAW for Linux, macOS and Windows.
Built for engineers who want to **record, mix and master without leaving the
application**: no plugin paralysis, no menu diving, no infinite-options sprawl.

> *"Fixed signal chain, finite track count, one page per stage. You commit, you move on."*

C++17 on JUCE 8, with a native layer underneath that keeps growing. On Linux the
audio device talks straight to PipeWire or ALSA, with USB hot-unplug
recovery, and MIDI goes through the ALSA sequencer. macOS and Windows still
reach their devices through framework adapters (CoreAudio, WASAPI and ASIO),
with an opt-in native CoreMIDI backend on macOS. Plugin hosting (CLAP, VST3,
LV2, AU), audio file IO and the DSP primitives are Dusk's own code.

New here? [QUICKSTART.md](QUICKSTART.md) takes you from download to a
mixed-down track in five minutes. Full manual: [MANUAL.md](MANUAL.md).
Authoritative spec: [DuskStudio.md](docs/DuskStudio.md).

## Get Dusk Studio

Beta, and paid licences are available now. Buy at
[duskaudio.com](https://duskaudio.com).

The source on this repo is GPL, so building it yourself costs nothing but
compile time. What you pay for is a packaged, ready-to-run build plus support
access: Patreon tiers from $5 a month, a $49 one-time licence for the current
major version, or $89 for every future major version. There is a 30-day
guarantee, and the founder pricing rises at 1.0. Full terms, tiers and what
each one includes are in [PRICING.md](docs/PRICING.md).

**First launch:** beta builds are ad-hoc signed on macOS and unsigned on
Windows, so Gatekeeper and SmartScreen warn the first time you open one.
[MANUAL.md § Installing Dusk Studio](MANUAL.md#installing-dusk-studio) has the
30-second bypass per OS. The Linux tarball needs no bypass. No signing
certificate is planned before the software earns one.

## Status

**Beta.** Built to a production bar, released as beta.
[CHANGELOG.md](CHANGELOG.md) records what shipped in each release, the
[tags](https://github.com/dusk-audio/dusk-studio/tags) give the current
version, and [docs/RELEASE-1.0.md](docs/RELEASE-1.0.md) is the plan of record
for 1.0.

The feature backlog is effectively closed. Every spec phase is in, plus Tascam
DP-24SD parity and project import, MTC and MIDI Clock sync, cross-track plugin
delay compensation, broad undo coverage (notes, automation, tempo, renames),
portable session folders that move between machines, a piecewise tempo map, a
true-peak mastering limiter, session open-with and an update notice on launch.

A fresh install is useful with nothing else on the machine. Five insert units
are compiled into the application and pinned to the top of the plugin picker:
Utility, DuskVerb 2, Tape Echo 2, Tape Machine 2 and the Sunset synthesiser. The
soundfont editor browses the `.sfz` and `.sf2` files already on disk without
touching the network. Third-party plugins host in process by default, which
gives the most responsive editors; `DUSKSTUDIO_USE_OOP_PLUGINS=1` opts into the
crash-isolating sandbox.

Each tag publishes a Linux tarball, a Windows MSI and a macOS DMG to the
private releases repo, with a signed `SHA256SUMS` beside them. What is left
before 1.0 is release engineering, low-spec and Raspberry Pi performance, and
deeper accessibility.

| Stage | Status |
|---|---|
| Live mixer (24 ch in 3 banks of 8 + 4 aux + 4 mix buses + master) | Working |
| Multitrack recording / playback with disk-full + MIDI-overflow detection | Working |
| Atomic session save + 30 s autosave with content-hash dirty check | Working |
| Session templates (Blank / Band / Beats / Singer-Songwriter) | Working |
| Built-in insert units (Utility, DuskVerb 2, Tape Echo 2, Tape Machine 2, Sunset) | Working |
| Plugin hosting (per-channel CLAP / VST3 / LV2 / AU + per-aux return) | Working |
| Native soundfonts (`.sfz` + `.sf2` via dusk-fizz, no external synth) | Working |
| Offline instrument library (lists what is installed, never reaches the network) | Working |
| Plugin offline-state preservation (missing plugin doesn't wipe save) | Working |
| Out-of-process plugin sandbox, audio, opt-in (Linux + macOS + Windows) | Working |
| Out-of-process plugin sandbox, editor embed (Linux XEmbed, Windows SetParent, macOS in-process shell) | Working |
| Mastering view (waveform + 5-band EQ + multiband comp + brick-wall limiter + BS.1770) | Working |
| Offline bounce / mixdown export (master) | Working |
| Single-pass stem export (tracks + buses + aux returns) | Working |
| Realtime bounce (hardware inserts print wet) | Working |
| Aux sends + reverb / delay returns (built-in DuskVerb 2, Tape Echo 2) | Working |
| External hardware inserts (per channel + per aux, with auto-latency ping) | Working |
| MIDI tracks + instrument plugins + piano roll editor | Working |
| Audio region editor (non-destructive trim / fade / gain) + 8-take history | Working |
| Take cycling + comping | Working |
| Loop-record take stacking (each pass kept as its own take) | Working |
| Cross-track plugin delay compensation (PDC) | Working |
| Console automation (Write / Read / Touch on channels + aux + master) | Working |
| MIDI Clock sync + MTC slave + master | Working |
| MIDI hot-plug detection (Linux) | Working |
| MIDI bindings + MIDI Learn (transport / strip / sends / EQ / comp / plugin + built-in params) | Working |
| Mackie Control surface (tested against Tascam DP-24SD) | Working |
| Multi-file audio + MIDI import with target-track picker | Working |
| Session notepad (lyrics / notes + chord chart, saved as `notepad.md`) | Working |
| Staged shutdown on window close, Quit, SIGTERM or logout | Working |
| Linux tarball | Working (CI publishes to private releases repo on tag) |
| Windows MSI installer (unsigned) | Working (CI publishes to private releases repo on tag) |
| macOS DMG (ad-hoc signed) | Working (CI publishes to private releases repo on tag) |
| Signed `SHA256SUMS` per release | Working (a tag cannot publish without it) |
| Deeper a11y (full screen-reader labels + keyboard-only mixer nav) | Floor only |

The C++ suite declares 1291 Catch2 test cases across 227 test source files.
Linux amd64, Linux arm64 and macOS build and run it on every push; Windows runs
it on every push and PR; ThreadSanitizer and ASan plus UBSan run it on every
push and PR. All of those, plus the framework-coupling ratchet, are required
checks on `main`.

## Bug reports

[Open an issue on GitHub.](https://github.com/dusk-audio/dusk-studio/issues)
Patreon supporters and patrons can also DM via Patreon. One-time and lifetime
licence holders get a direct support email link in their release-repo invite.

## Why

Most DAWs are built for production studios with infinite track counts and
infinite options. They are also paralysing for ADHD-pattern users: every
decision branches, every parameter is reachable, every track type wants its own
configuration. Dusk Studio flips the constraint. A fixed signal chain, a finite
track count, a single visible page per stage. You commit, you move on.

## The design constraints

These are not implementation details, they are the product. Features are judged
against them.

1. **24 channels maximum.** Fixed. Three banks of 8 to match standard control
   surfaces (each bank drives 8 strips on the surface; all 24 are visible on
   screen).
2. **Fixed signal chain.** No reordering EQ and comp. Channel-strip processing
   order is the same on every track, every time. Each channel gets **one
   optional insert slot**, holding a single CLAP / VST3 / LV2 / AU plugin, a
   built-in unit **or** a hardware insert, never a chain, at a fixed position in
   the strip. Aux returns get one slot each.
3. **No waveform editing.** Region-level move / split / delete / trim / fade /
   gain only. Draw mode exists only inside the MIDI piano roll.
4. **Console-style automation plus breakpoint editing.** Ride controls with
   Write / Read / Touch, add / drag / delete per-parameter breakpoints in the
   region editor's automation lane, or draw a freehand stroke with the Draw tool
   to lay a run of them. Linear segments between points, no spline or bezier
   curves.
5. **Everything visible within a stage.** Four workflow stages match the
   portastudio layout (Recording / Mixing / Mastering / Aux), and within each
   stage there are no tabs and no hidden panels. The MIDI piano roll, plugin and
   unit editors, and the audio-settings panel are embedded modals over the
   current stage.
6. **No preferences sprawl.** A single Audio Settings panel covers audio device,
   buffer, oversampling, MIDI sync (Clock in / out, chase, emit), MIDI bindings
   and UI scale. No per-feature settings menus, no global preferences window.
7. **Portastudio philosophy.** Stay fixed, finite and commit-first. If a feature
   mainly adds configurability or options, leave it out.

## Architecture

```
Channels 1-24 ───────────────→ 4 Aux Buses ──→ 4 Mix Buses ──→ Master ──→ Output
   Phase / Polarity              EQ              HPF + Tone EQ    Passive Program EQ
   Insert (plugin, unit or HW)   Comp            Bus Comp         Bus Comp
   HPF + LPF                     Fader                            Tape Saturation
   4-band EQ                                                      Fader
   Compressor (Opto/FET/VCA)
   Aux sends (4) + Pan
   Fader / Mute / Solo
```

- **DSP** comes from the Dusk Audio plugin suite (4K EQ 2, Multi-Comp
  FET/Opto/VCA, Multi-Q, Tape Machine 2, shared AnalogEmulation), so the mixer
  and the standalone plugins share one DSP source of truth. The channel and bus
  EQs run the 4K EQ 2 engine; the master tape runs Tape Machine 2 with the
  plugin's own editor.
- **Built-in units** are compiled into the application, so they need no scan and
  work on a fresh install: Utility, DuskVerb 2, Tape Echo 2, Tape Machine 2 and
  Sunset. DuskVerb 2, Tape Echo 2 and Tape Machine 2 show the plugin's own
  editor, embedded in the window.
- **Plugin host**: CLAP, VST3, LV2 and AU on every channel strip, with aux
  returns hosting reverb and delay. In process by default. The **opt-in**
  out-of-process sandbox (`DUSKSTUDIO_USE_OOP_PLUGINS=1`) runs each plugin in a
  child through a per-platform IPC backend (Linux `memfd_create` plus `futex`,
  macOS `shm_open` plus non-blocking pipes, Windows `CreateFileMapping` plus
  inheritable kernel events), so a crashing plugin cannot take the host down.
  Standard-host plugin *scanning* runs in that child. Native CLAP and VST3
  bundle scanning uses a child too, on its own gate, and falls back in process
  when no child binary is present. Native LV2 and AU discovery never loads
  plugin code. The released macOS DMG carries the sandbox helper and keeps its
  macOS 11 deployment target.
- **Soundfonts**: `.sfz` and `.sf2` play through the built-in
  [dusk-fizz](https://github.com/dusk-audio/dusk-fizz) engine, our maintained
  hard fork of sfizz, converting SF2 to SFZ on load. No external synth required.
- **Removing the framework** is a standing campaign, tracked in
  [docs/dejuce/campaign.md](docs/dejuce/campaign.md) and enforced by a ratchet
  in CI that lets framework coupling fall but never rise. It resumes after 1.0.

## Repository

```
src/
  dsp/         # ChannelStrip, AuxBusStrip, MasterBus, BrickwallLimiter
  engine/      # AudioEngine, RecordManager, PlaybackEngine, BounceEngine, MasteringChain
    pipewire/  # native PipeWire backend, preferred on Linux
    alsa/      # native ALSA backend, the Linux fallback
    device/    # device seam + framework adapter for CoreAudio / WASAPI / ASIO
    midi/      # ALSA sequencer, CoreMIDI, WinMM queries, framework fallback
    builtin/   # built-in insert units and their editors
    hosting/   # INativeInstance, the seam every native host implements
    clap/ lv2/ vst3/ au/  # native plugin hosts
    ipc/       # OOP plugin host + per-platform shm / sync backends
    sfz/ multisample/     # dusk-fizz playback, SF2 conversion
    audiofile/ # libsndfile readers and writers
  foundation/  # framework-free primitives: text, paths, JSON, smoothing, FFT, MIDI
  session/     # session model + JSON serialisation
  ui/          # MainComponent, ConsoleView, channel/aux/master strips, mastering view
    imgui/     # native surfaces: startup, audio settings, unit editors, keyboard
  util/        # native log storage + CrashHandler signal reports
tests/         # 1291 Catch2 test cases declared in C++ (session, recording, MIDI, IPC, DSP)
packaging/     # .desktop, AppStream, MIME, macOS bundle, for tarball + DMG builds
docs/          # maintainer guide, the 1.0 plan, migration plans
  DuskStudio.md  # authoritative product spec
MANUAL.md      # end-user manual (Pandoc-buildable to PDF via docs/build-pdf.sh)
```

## Builds and contributing

Precompiled builds go to paying users: a Linux tarball, a Windows MSI and a
macOS DMG, all published to the private releases repo on each tag. Beta builds
are ad-hoc signed on macOS and unsigned on Windows. Self-building is fully
supported and equivalent at the source level, with no support tier attached.

Source builds need libsndfile and libsodium; MP3 bounce also uses LAME. The
Linux and Windows guides list the exact packages and manifest setup.

| Platform | Doc |
|----------|-----|
| Linux | [BUILDING-LINUX.md](docs/BUILDING-LINUX.md) |
| Windows | [BUILDING-WINDOWS.md](docs/BUILDING-WINDOWS.md) |
| macOS | Mirrors the Linux flow: upstream JUCE 8.0.4, the donor plugins checkout at the pinned revision, DAF `main`, and static libsodium supplied through `DUSKSTUDIO_SODIUM_ROOT`. Built and tested per push on `macos-14` (Apple Silicon), see [.github/workflows/macos-build.yml](.github/workflows/macos-build.yml). |
| Linux tarball packaging | [packaging/README.md](packaging/README.md) |
| End-user manual and troubleshooting | [MANUAL.md](MANUAL.md) |

After a build, sanity check with `DuskStudio --version`. It prints the app,
framework and platform strings and exits 0, which makes it a useful
paste-target for support DMs.

CI builds and tests every push to `main` on Linux (Ubuntu 22.04 GCC, amd64 and
arm64) and macOS (14 Apple Silicon, Ninja plus ccache). `windows-tests.yml`
runs the Catch2 suite on Server 2022 MSVC on every push and PR.
`linux-sanitizer.yml` runs it under ThreadSanitizer and under ASan plus UBSan on
every push and PR. A `v*` tag runs `release.yml`, which builds the Windows MSI,
the macOS DMG, the Linux tarballs and the manual PDF, then publishes them to one
shared release in the private releases repo. A tag needs
`RELEASE_SIGNING_KEY` and `RELEASE_SIGNING_KEY_PASSWORD`, which sign
`SHA256SUMS`, and fails without them. The macOS and Windows signing credentials
are optional: with them a tag signs and notarizes the DMG and
Authenticode-signs the MSI, without them it ships the ad-hoc DMG and the
unsigned MSI. A manual dispatch builds the same way and publishes nothing.

## License

[GPL-3.0-or-later](LICENSE), matching JUCE's GPL option and the VST3 SDK's.
Third-party component inventory in [LICENSES.txt](LICENSES.txt). No third-party
plugin is ever bundled in a package.

Dusk Studio ships under a **dual access model**:

- **Source**: GPL-3.0. Clone, audit, build, modify and redistribute, all fine
  under GPL terms.
- **Paid builds**: precompiled Linux tarballs, Windows MSIs and macOS DMGs
  delivered to paying users. The payment is for packaging and support access,
  not for the code; the source stays open. Self-builders get no support, but the
  binary is the same.

Full terms in [PRICING.md](docs/PRICING.md). Buy at
[duskaudio.com](https://duskaudio.com).
