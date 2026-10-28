# 0002: Run natively on Wayland, keep X11, resume the DAF work

Date: 2026-10-28

Status: accepted. Supersedes [0001](0001-ship-1.0-on-juce.md).

## Decision

Dusk Studio runs natively on Wayland desktops and as an X11 client on X11-only
desktops, from one Linux binary. X11-only machines keep working, and that is
tested next to the Wayland-only tests, not assumed.

The move from JUCE to the DAF stack is no longer parked until after 1.0. It
resumes now, led by what native Wayland needs:

1. DAF chooses X11 or Wayland when the program starts. Today its backend is
   fixed at build time, so one binary needs both compiled in and a runtime
   choice between them.
2. DAF gets file drop targets on X11, Wayland, macOS and Windows, each tested
   on its own platform.
3. The JUCE shell gets a native Wayland path. Dusk-owned views and plug-in
   editors then move off X11 embedding.

## Why

Dragging a file from GNOME Files onto the timeline did nothing. JUCE's X11
drop code refused the drag, because GNOME passes it on with a newer protocol
version than JUCE accepts. That fix went into our JUCE fork as a stopgap (tag
`dusk-wayland-v4`). The lasting fix is drop handling in code we own, and the
goal behind it is an app that runs natively on Wayland instead of through
XWayland.

## What changes from 0001

- The rule that no JUCE-reducing PR is accepted before 1.0 is lifted.
  Replacing JUCE with our own code is welcome, most of all where it moves the
  native Wayland work forward.
- The PARKED banners on the `docs/dejuce` plans now point here.

## What stays

- The seams in `src/foundation/`, `src/engine/device/`, `src/engine/midi/`
  and `src/engine/audiofile/` are still where new code goes.
- `tools/juce-gate.sh` and `tools/juce-allowlist.txt` stay enforced in CI.
- 1.0 is still gated on what a user can do with the app and on builds that
  install without a fight.
- Third-party plug-in editors that only support X11 may keep using XWayland,
  as separate windows, until their vendors ship native editors or Dusk Studio
  draws host controls for them.

## Tracking

The order of the towers lives in [docs/dejuce/campaign.md](../dejuce/campaign.md).
The DAF runtime backend and drop targets are developed in the DAF repository,
starting with branch `feat/runtime-backend`.
