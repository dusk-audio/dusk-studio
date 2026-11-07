<!-- summary-start -->
Dusk Studio 0.16.1 fixes SFZ and SF2 instruments that stayed silent after loading until the audio device restarted. LV2 plug-ins a Linux distribution installs in its lib64 folder, such as openSUSE's and Fedora's sfizz, now appear in the plug-in picker. The Linux download now needs the liblilv-0-0 and libserd-0-0 packages.

Native panels show Greek, Cyrillic, Chinese, Japanese and Korean names, and Hebrew and Arabic names read right to left. Seeks during playback at 192 kHz no longer cause a dropout, mice and tablets plugged in after launch work on X11, and sandboxed plug-in editors on Linux close and reopen reliably. Sessions still save in format 11, so 0.16.0 opens them.
<!-- summary-end -->

### Known limitations

At very small audio buffers, other busy programs can make occasional blocks run late. If you hear clicks, raise the audio buffer size.

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

    VERSION=0.16.1   # this release
    curl -fsSLO "https://raw.githubusercontent.com/dusk-audio/dusk-studio/v$VERSION/packaging/release-signing.pub"
    gpg --show-keys --with-fingerprint release-signing.pub
    gpg --import release-signing.pub
    gpg --verify SHA256SUMS.asc SHA256SUMS
