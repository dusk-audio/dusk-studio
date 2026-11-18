<!-- summary-start -->
Dusk Studio 0.17.0 puts Multi-Comp 2 on every compressor and lets an idle mixer rest. The channel strips run Multi-Comp 2's Opto, FET and VCA modes, the buses and the master its Bus mode, and the mastering stage its Multiband mode, and each COMP button opens the plug-in's own editor. A compressor that is engaged costs more CPU than the one it replaces, about three times as much at 48 kHz, because it always runs at the plug-in's own 2x oversampling. One that is out costs nothing. The channel EQ's frequency and Q knobs read and sound as they do on 4K EQ 2.

An idle mixer lets the CPU rest: Multicore DSP wakes only the threads a block needs, meters repaint only when what they show changes, several native panels draw far less often when idle, and a track with no plug-in on it that is muted or has no input costs the audio thread about a twentieth of what it did. On Linux the audio thread is now tested for locks and allocations with every change, and the PipeWire and ALSA audio threads no longer take a lock around each block.

MIDI is safer. Each recorded MIDI pass is written to the session folder as a Standard MIDI File, a pass in progress is saved as you play, at most once a second, and every save keeps a backup of the session. A session that cannot be opened offers its autosave and backups instead of failing. Clicking a key on the piano roll keyboard plays it.

SFZ and SF2 instruments that stayed silent after loading in 0.16.0 play again. A take recorded with PRINT on lands where a dry take of the same input does, where it used to land late by the channel's delay. LV2 plug-ins a Linux distribution installs in its lib64 folder appear in the plug-in picker, and the Linux download now needs the liblilv-0-0 and libserd-0-0 packages. A PipeWire graph that moves to a longer block no longer silences playback, a change of its rate is followed instead of going unnoticed, VST3 synths stop their notes at Stop, and sandboxed plug-ins run at audio priority where the system allows it. The built-in reverb, tape echo, tape machine and Sunset are their plug-ins' released versions, DuskVerb 2 1.0.1, Tape Echo 2 1.0.9, Tape Machine 2 1.0.12 and Sunset Circuits 1.1.0. Native panels show Greek, Cyrillic, Chinese, Japanese and Korean names, and Hebrew and Arabic names read right to left.

Sessions save in format 12, which Dusk Studio 0.16.0 and earlier cannot open. Keep a copy before saving if you need to use an older version. An older session opens with its compressor settings converted to Multi-Comp 2's controls, so an older mix can sound different, and with each channel EQ band on the knob marking that plays where it played.
<!-- summary-end -->

### Known limitations

At very small audio buffers, other busy programs can make occasional blocks run late. If you hear clicks, raise the audio buffer size.

On Linux, while a PipeWire graph runs a longer block than the one Dusk Studio started with, notes played in from a MIDI keyboard and MIDI sent to other gear are placed to within one of the graph's blocks rather than to the sample. Exact timing returns once playback and recording have stopped.

### Downloads

- **Linux** (`.tar.xz`, x86_64 and aarch64): unsigned. Extract, then run
  `./DuskStudio/DuskStudio`, or `./install.sh` for a menu entry, dock/taskbar
  icon, PATH launcher and session-file association. On Wayland the dock icon
  only appears after `install.sh`. The aarch64 build targets 64-bit Raspberry Pi
  OS (Pi 3/4/5). The binary needs `libpipewire-0.3-0` (linked in even to run
  the ALSA backend), `libsuil-0-0`, `liblilv-0-0`, `libserd-0-0`, `libmp3lame0`,
  `libsndfile1`, `libcurl4` and `libarchive13` present on the system; the first
  five are not on a stock desktop install. `README-linux.txt`
  in the tarball lists every linked library with its Debian/Ubuntu package name
  and an `apt install` line. libsodium is compiled in and needs nothing.
  System utf8proc builds also need `libutf8proc3`; the pinned static fallback
  does not. The tarball relies on the distro packages for libcurl and
  libarchive and their shared-library dependencies.
- **macOS** (`.dmg`, Apple Silicon / arm64 only): ad-hoc signed, not
  notarized. The first launch is blocked: click Done, then System Settings ->
  Privacy & Security -> Open Anyway, Open Anyway again, and approve with Touch
  ID or your password. Allow microphone access when asked.
- **Windows** (`.msi`, x64): unsigned. SmartScreen may warn - choose More info
  -> Run anyway. Statically linked, no vc_redist needed. The installer includes
  a pinned Mesa llvmpipe renderer, so the notepad works in virtual machines,
  Remote Desktop sessions and systems whose basic display adapter provides only
  OpenGL 1.1. OpenGL surfaces in Dusk Studio and its plugin-host children use
  CPU rendering; the audio engine is unaffected. A first-frame driver failure
  remains guarded so it cannot cost the same session twice. The installer
  sets Dusk Studio up for every account on the PC. If 0.13 or earlier is
  installed, uninstall
  dusk-studio from Settings -> Apps first (Windows cannot upgrade it in place);
  sessions and settings are kept.
- **Manual** (`MANUAL.pdf`): the Dusk Studio user manual for this release.

Check a download against the `SHA256SUMS` asset before installing. It covers
every payload, so verify with `shasum -a 256 --ignore-missing -c SHA256SUMS`
to check the ones you actually downloaded; without `--ignore-missing` the
files you skipped are reported as failures.

`SHA256SUMS.asc` is a detached OpenPGP signature over `SHA256SUMS`. The public
key is `packaging/release-signing.pub` in the source repository; download it
from this release's tag, compare its fingerprint with the one on the project
site, import it, then check the signature before the hashes:

    VERSION=0.17.0   # this release
    curl -fsSLO "https://raw.githubusercontent.com/dusk-audio/dusk-studio/v$VERSION/packaging/release-signing.pub"
    gpg --show-keys --with-fingerprint release-signing.pub
    gpg --import release-signing.pub
    gpg --verify SHA256SUMS.asc SHA256SUMS
