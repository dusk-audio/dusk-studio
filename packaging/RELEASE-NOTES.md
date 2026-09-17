<!-- summary-start -->
A built-in plugin suite that works on a fresh install (Utility, DuskVerb 2, Tape Echo 2, Tape and the Sunset synth), the 4K EQ 2 and Tape Machine 2 engines in the console, a one-page quickstart, an offline instrument library, a first launch that picks an input and follows the system audio default, a signed checksum file, and the fixes from walking record, overdub and bounce on all three platforms.
<!-- summary-end -->

### Downloads

- **Linux** (`.tar.xz`, x86_64 and aarch64): unsigned. Extract, then run
  `./DuskStudio/DuskStudio`, or `./install.sh` for a menu entry, dock/taskbar
  icon, PATH launcher and session-file association. On Wayland the dock icon
  only appears after `install.sh`. The aarch64 build targets 64-bit Raspberry Pi
  OS (Pi 3/4/5). The binary needs `libpipewire-0.3-0` (linked in even to run
  the ALSA backend), `libsuil-0-0`, `libmp3lame0` and `libsndfile1` present on
  the system; the first three are not on a stock desktop install. `README-linux.txt`
  in the tarball lists every linked library with its Debian/Ubuntu package name
  and an `apt install` line. libsodium is compiled in and needs nothing.
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
  remains guarded so it cannot cost the same session twice.
- **Manual** (`MANUAL.pdf`): the Dusk Studio user manual for this release.

Check a download against the `SHA256SUMS` asset before installing. It covers
every payload, so verify with `shasum -a 256 --ignore-missing -c SHA256SUMS`
to check the ones you actually downloaded; without `--ignore-missing` the
files you skipped are reported as failures.

`SHA256SUMS.asc` is a detached OpenPGP signature over `SHA256SUMS`. The public
key is `packaging/release-signing.pub` in the source repository; download it
from this release's tag, compare its fingerprint with the one on the project
site, import it, then check the signature before the hashes:

    VERSION=0.14.0   # this release
    curl -fsSLO "https://raw.githubusercontent.com/dusk-audio/dusk-studio/v$VERSION/packaging/release-signing.pub"
    gpg --show-keys --with-fingerprint release-signing.pub
    gpg --import release-signing.pub
    gpg --verify SHA256SUMS.asc SHA256SUMS
