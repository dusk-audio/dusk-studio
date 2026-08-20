<!-- summary-start -->
Adds the session notepad, a UI scale control, native PipeWire and macOS plugin hosting, MIDI hot-plug, loop take stacking, realtime hardware bounce, and a broad set of session, recording, audio-device, and plug-in fixes.
<!-- summary-end -->

### Downloads

- **Linux** (`.tar.xz`, x86_64 and aarch64): unsigned. Extract, then run
  `./DuskStudio/DuskStudio`, or `./install.sh` for a menu entry, dock/taskbar
  icon, PATH launcher and session-file association. On Wayland the dock icon
  only appears after `install.sh`. The aarch64 build targets 64-bit Raspberry Pi
  OS (Pi 3/4/5). The PipeWire client library is linked in, so
  `libpipewire-0.3-0` must be installed even to run the ALSA backend; libsodium
  is compiled in and needs nothing.
- **macOS** (`.dmg`, Apple Silicon / arm64 only): unsigned. Right-click the app
  -> Open to bypass Gatekeeper on first launch.
- **Windows** (`.msi`, x64): unsigned. SmartScreen may warn - choose More info
  -> Run anyway. Statically linked, no vc_redist needed.
- **Manual** (`MANUAL.pdf`): the Dusk Studio user manual for this release.

Check a download against the `SHA256SUMS` asset before installing.
