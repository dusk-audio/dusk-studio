# Dusk Studio — TODO

## General Settings panel
Add a "General" tab/section to the Settings panel (sibling to the existing
Audio Device, MIDI Bindings, etc settings surfaces) that hosts app-wide
preferences not tied to a specific device or session.

First option to expose:
- **Expand tape strip by default**: checkbox that toggles whether the
  tape SUMMARY strip starts expanded on app launch (currently always
  collapses). Persisted per-machine via `AppConfig` (same store as
  `scan_plugins_on_startup` + `ui_scale`).
  - Add `appconfig::getTapeStripExpandedDefault()` / `setTapeStripExpandedDefault(bool)` in `src/ui/AppConfig.{h,cpp}`.
  - Read at MainComponent startup; apply to `tapeStripExpanded` before first `resized()`.

Future candidates for the same panel: theme accent colour, default
zoom factor, default snap-to-grid state, autosave interval.
