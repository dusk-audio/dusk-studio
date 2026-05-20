# Wayland window-teardown smoke test (manual)

GNOME / Mutter on Wayland aborts the desktop session if a top-level
`xdg_toplevel` is destroyed while the compositor still records it as
`focus_window`:

```
libmutter:ERROR:../src/core/window.c:1576:meta_window_unmanage:
assertion failed: (window->display->focus_window != window)
```

Dusk Studio hardens every top-level destruction site by calling
`duskstudio::platform::prepareForTopLevelDestruction(...)` (transfer
keyboard focus + flush windowing system) before the destruct.
This checklist exercises every site so a regression is caught before
shipping.

Required environment: GNOME 48+ on Wayland (Mutter 48.x). openSUSE
Leap 16 / Fedora 42+ / Ubuntu 25.04 all qualify. **Do NOT run this on
your primary desktop session if you can avoid it** — a regression
will log you out. Run inside a nested compositor or VM:

```bash
# Easiest: nested GNOME shell
gnome-shell --nested --wayland

# Or: Xephyr if you only need an X server
Xephyr -screen 1920x1080 :2 &
DISPLAY=:2 ./Dusk Studio
```

## Pre-flight

```bash
# Clear any old coredumps so the post-test check is meaningful
sudo coredumpctl list --since="now" >/dev/null 2>&1 || true

# Optional: stress fractional scaling, which has historically
# surfaced extra Mutter window-tracking edge cases.
export MUTTER_DEBUG_FORCE_FRACTIONAL_SCALING=1
```

## Test runs (50× each)

For each window type below: open it, give it focus (click inside),
close it. Repeat 50 times. Watch for any GNOME freeze / logout.

| # | Window type                       | How to open                                   | How to close                                |
|---|-----------------------------------|-----------------------------------------------|---------------------------------------------|
| 1 | Main window (full app lifecycle)  | Launch `./Dusk Studio`                              | Click X, choose "Don't Save" if prompted    |
| 2 | Plugin editor (channel strip)     | Click the "+ Plugin" slot, pick any VST3      | Click X on the editor window                |
| 3 | Plugin editor (aux pop-out)       | Open AUX stage, add plugin, click pop-out btn | Click X on the popout window                |
| 4 | Audio settings panel              | Menu → Settings → Audio                       | Click outside / Esc                         |
| 5 | Bounce dialog                     | Menu → File → Bounce…                         | Cancel / Esc                                |
| 6 | Mixdown dialog                    | Menu → File → Mixdown                         | Cancel / Esc                                |
| 7 | Save-on-quit modal                | Modify session, click X                       | Save / Don't Save / Cancel — try each       |
| 8 | Self-test panel (audio settings)  | Audio settings → "Run Self-Test"              | Click X on the dialog                       |
| 9 | Virtual MIDI Keyboard             | Press K (or click keyboard icon)              | Esc / click outside                         |

The [Linux-only ones that actually allocate a native peer](../src/ui/PlatformWindowing_Linux.cpp)
are #1, #2, #3, #8. The rest are EmbeddedModal-based and don't have a
separate `xdg_toplevel`, but exercising them flushes any residual
focus-state issues.

If a test loops 50× cleanly with the GNOME session intact, it passes.

## Verification (post-test)

```bash
# Must print nothing — every Mutter focused-destroy assertion shows up here.
journalctl --user -b | grep meta_window_unmanage

# Must list no gnome-shell coredumps from the test run.
coredumpctl list --since="-30 minutes" gnome-shell

# Same for Dusk Studio itself — destruction-order or use-after-free regressions
# in plugin teardown surface as Dusk Studio coredumps with VST3PluginInstance
# at the bottom of the stack.
coredumpctl list --since="-30 minutes" Dusk Studio
```

If any of those produce output, capture:

```bash
coredumpctl debug <pid>
# In gdb:
# (gdb) bt full
# (gdb) info locals
```

…and the relevant `[Dusk Studio/shutdown] phase N: …` lines from Dusk Studio's
stderr. The combination of phase marker + crash frame narrows the
regression to a specific phase of the staged shutdown.

## Why no automated CI

A faithful CI test would need a Wayland session manager (Mutter)
running inside the runner — possible (gnome-shell --nested or
weston-launch in a VM) but heavy enough that the cost / signal
ratio doesn't justify it for this repo's size today. A manual
checklist run before each Linux release is the pragmatic choice.

If we ever want to automate: the building blocks would be
`xdotool` (or `wtype` for Wayland-native input) driving the open /
close steps inside `gnome-shell --nested`, with the journalctl /
coredumpctl checks above as the pass/fail signal.
