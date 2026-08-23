<!-- summary-start -->
Fixes the macOS disk image, which could not launch in 0.13.0, restores the notepad's full size on Retina Macs, protects Windows sessions from incompatible notepad graphics drivers, removes Linux desktop files from macOS and Windows packages, and adds native CLAP and VST3 plugin hosting on Windows.
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
  -> Run anyway. Statically linked, no vc_redist needed. The notepad works on
  a conforming software OpenGL renderer and refuses the known-bad Microsoft
  OpenGL Compatibility Pack, but it has not yet been verified on physical
  Windows GPU hardware. If some other driver does end the application while
  the notepad is drawing, the next launch refuses the notepad rather than
  repeating it, so the failure costs one session and not every session.
- **Manual** (`MANUAL.pdf`): the Dusk Studio user manual for this release.

Check a download against the `SHA256SUMS` asset before installing. It covers
every payload, so verify with `shasum -a 256 --ignore-missing -c SHA256SUMS`
to check the ones you actually downloaded; without `--ignore-missing` the
files you skipped are reported as failures.
