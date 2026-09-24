# Plugin hosting + instant editor display — design spike

Goal/requirement: a plugin's editor must appear **instantly** (no perceptible lag) the
first time it's opened, with **no stray-window flash**, on **both X11 and Wayland**.

Spike method: 4 parallel research agents — local cost breakdown (DuskStudio + the
JUCE-wayland fork), how Reaper/Ardour/Bitwig embed editors natively, JUCE viability,
and native-host scope. All four converged.

## TL;DR

1. **The flash is JUCE-specific and already solved** for the hidden case (the
   `isShowing()` lazy-build gate). It comes from JUCE's `XEmbedComponent` host window
   being born parented to the X11 **root** and mapped before it reparents.
2. **The lag is the first-open editor BUILD** (`createEditorIfNeeded` → the plugin
   constructs its GUI widget tree + the synchronous `view->attached()` XEmbed
   handshake), done on the click. Subsequent opens are already instant (Dusk keeps the
   editor alive and re-shows via a cheap X remap).
3. **The universal fix every native host uses: build the editor ONCE, reparent it into a
   host container, then show/hide = pure X map/unmap.** Dusk already does this for
   *return* visits; the only gap is the *first* open still builds on click.
4. **No host embeds plugin editors on Wayland.** Wayland forbids cross-process surface
   embedding by design — even CLAP's spec says "embed not supported on Wayland, use
   floating." Reaper, Ardour, Bitwig, and any homegrown Dusk host **all force plugin
   editors onto XWayland**. Dusk's `preferX11ForNextNativeWindow()` latch already does
   exactly this.
5. **Therefore: instant + flashless is achievable in the EXISTING JUCE path** with one
   missing piece — **eager editor pre-creation at plugin load** (~3–5 days, low risk).
   A native host delivers the *same* result via the *same* XWayland+XEmbed mechanism;
   it's the right strategic direction for *other* reasons, not because JUCE can't hit
   this bar.

## Why editors lag / flash (decomposition)

Per-layer, on Linux:

- `createEditorIfNeeded()` (`AuxLaneComponent.cpp:966`, `ChannelStripComponent.cpp:2238`)
  → VST3 wrapper `new VST3PluginWindow` ctor builds the plugin's GUI
  (`juce_VST3PluginFormat.cpp:241-258`). **Intrinsic** — any host instantiating an
  `IPlugView` pays it.
- Native attach is **deferred to first visibility**: `attachPluginWindow()`
  (`:460`) from `componentVisibilityChanged()` → blocking `view->attached(...)` (`:483`).
  Mostly intrinsic; the XEmbed reparent dance is JUCE/XWayland overhead on top.
- **Flash origin**: `XEmbedComponent::Pimpl::createHostWindow()` does
  `XCreateWindow(dpy, ROOT, 1x1, override_redirect)` (`juce_XEmbedComponent_linux.cpp:415`).
  While the editor has no peer the host stays on root; if it's **mapped** while still on
  root (`WindowMapper`, `:334`) you get a stray override-redirect top-level = the flash.
  Dusk avoids it by only building when `isShowing()` (`AuxLaneComponent.cpp:953`).

Editor structure: plain JUCE child (`addAndMakeVisible(*ui.editor)`) → nested
`XEmbedComponent` → native X11 host window → plugin's own X11 window via `attached()`.

## The universal native pattern (Reaper / Ardour / CLAP)

Build the plugin's editor window **once** into a host-owned embedder, XEmbed-reparent it
**once**, then show/hide = X map/unmap (or the `_XEMBED_INFO` `XEMBED_MAPPED` flip). First
display is instant because no construct / reparent / size-negotiation happens on the
click — it happened at build time. Flash-free because you **create the container
unmapped, reparent while unmapped, map only on reveal** (a child under an unmapped parent
draws nothing).

- **Ardour**: LV2 UIs via suil `x11_in_gtk2` — a GtkSocket (persistent embedder) holds a
  GtkPlug; XEmbed handshake + XReparentWindow under the hood.
- **Reaper**: SWELL creates an empty X11 window, wraps in GDK, reparents the plugin
  window into Reaper's window.
- **CLAP**: `create(api="x11", floating=false)` → `set_parent(x11 window)` → `show()/hide()`.
  The API *bakes in* build-once-then-cheap-show. Plugin runs its own X11 loop; host pumps
  it via the small `posix-fd-support` + `timer-support` extensions.
- **Wayland**: none of them embed natively — all use XWayland. CLAP X11 only; CLAP
  Wayland = floating windows, no embed.

## Option A — fix it in JUCE now (eager pre-create + cache)

The repo already has (b) cache, (c) keep-alive remap, (d) force-XWayland. Only (a) eager
pre-creation is missing: today `openPluginEditor` builds the editor lazily on first click
under a process-exclusion SpinLock (`ChannelStripComponent.cpp:2235-2238`).

Fix: when the plugin instance loads, **pre-create the editor into its embed container
unmapped/occluded**, keep it alive; first user open becomes the existing instant
`setVisible(true)` remap path (`AuxLaneComponent.cpp:973-982`) instead of build-on-click.
The off-screen-prewarm attempt regressed because building while parked-but-on-a-peer trips
the map-at-root flash; the clean way is either build shown-but-occluded for one frame, or
add a small **"create-but-defer-map"** mode to the fork's `XEmbedComponent` (extends the
existing Role split in commit `75e831`) so the editor builds fully off-screen and maps
exactly on click.

- Effort: ~3–5 days. Risk: low–medium (needs live-Wayland verification).
- Result: instant + flashless first open on X11 and XWayland, no rewrite.

## Option B — native CLAP-first host (the homegrown direction)

What transfers from the existing OOP IPC host (substantial):
- Scanner (`OutOfProcessPluginScanner`, `PluginManager.cpp:27`) — format-agnostic,
  crash-isolated; add CLAP to `isSandboxedFormat`.
- SHM+futex `processBlockSync` (RT-safe), control-plane demuxer, state over SHM, param
  mirror, **editor RPC** (`showEditor/hideEditor/resizeEditor` returning a native window
  id for XEmbed — `RemotePluginConnection.h:97`, `PluginHostMain.cpp:528`).
- All the hard-won X11 plumbing: `XEmbedComponent`, the non-fatal X error handler
  (`PlatformWindowing_Linux.cpp:51`), the X11 peer latch, Wayland focus-roundtrip.

Staged:
- **A.** CLAP host shim (instance/params/state/process + `posix-fd` & `timer` ext) +
  X11 editor container with pre-create+cache+map/unmap. **~3–5 weeks**, low–med risk.
- **B.** Fold CLAP into the existing scanner child. ~2–3 days.
- **C.** Native VST3 (`IRunLoop` + `IPlugView`/`IPlugFrame`). **+4–6 weeks, HIGH risk,
  no embed-cleanliness gain over CLAP** — only if JUCE's VST3 editor lag survives
  pre-create+cache.
- **D.** LV2 via lilv+suil. +1–2 weeks, low risk, low payoff (JUCE LV2 already works).

Format effort: LV2 < CLAP < VST3. Embed cleanliness: CLAP ≈ VST3 > LV2.

## The honest caveat for "go homegrown"

A native host does **not** make plugin editors Wayland-native and does **not** avoid
XWayland — that's a platform limitation every host hits. What homegrown actually buys:
- Removes JUCE's editor-wrapper quirks (the map-at-root flash sequencing, the
  100/350/800 ms size-refit timers, the XEmbed mapping policy).
- Full control over **when** the editor is built/cached (→ guaranteed instant first open).
- CLAP's clean `show()/hide()` lifecycle and a smaller, owned codebase.
- Strategic: less JUCE on the platform you're shipping first; CLAP-first posture.

It does **not** buy: native Wayland plugin embedding, or removal of the intrinsic
plugin-GUI build cost (only the ability to pay it off-click).

## Recommendation

The two are not exclusive — sequence them:
1. **Now:** Option A (eager pre-create in JUCE) — get instant + flashless editors this
   week, on X11 and XWayland, without blocking on a rewrite.
2. **Then:** Option B (CLAP-first native host) as the strategic migration, reusing the
   IPC host + scanner + X11 infra. VST3-native only if measurement proves JUCE's VST3
   editor still lags after pre-create+cache.

This gives the user-facing win immediately and de-risks the big build (you'll have proven
the pre-create+cache+map/unmap model in JUCE before re-implementing it natively).
