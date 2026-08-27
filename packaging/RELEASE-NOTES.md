<!-- summary-start -->
Restores hosted plugin settings when reopening sessions, prevents hung plugin scans from wedging, and adds scan cancellation.
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
- **macOS** (`.dmg`, Apple Silicon / arm64 only): unsigned. Right-click the app
  -> Open to bypass Gatekeeper on first launch.
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
