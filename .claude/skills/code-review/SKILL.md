---
name: code-review
description: Dusk Studio review checklist - the repo-specific defects that have actually shipped here. Use when reviewing a diff, branch, or PR in this repo, on top of the normal review pass.
---

# Dusk Studio code review

Run the normal review first. These are the checks that have caught real defects
in this repo; each one exists because the class of bug reached a commit.

## Cross-platform native hosting (`src/engine/{clap,lv2,vst3}/`)

The editors are per-platform translation units behind one shared header:
`*Editor.cpp` on Linux, `*Editor_Mac.mm` on Apple. Review the pair, never one
alone.

- **Every method in the header has a definition in every platform TU.** A
  missing `.mm` breaks configure ("Cannot find source file"), not just the
  build, so CMake pre-routing of a file that does not exist yet fails the whole
  macOS job. Verify the file list in `CMakeLists.txt` against `ls`.
- **Header comments must not state one platform's behaviour as universal.**
  `embed()` maps the X11 window immediately but leaves the Cocoa container
  hidden until `reveal()`; a comment claiming either as the rule is wrong half
  the time.
- **Native handles cross the boundary as `std::uintptr_t`,** never X11's
  `unsigned long` - Windows LLP64 truncates `HWND`. Check the JUCE wrapper's
  accessor too, not just the editor signature.
- **Coordinate space is per-platform.** X11 wants physical pixels
  (`embedscale::toPhysical`); Cocoa wants logical points. The house pattern is
  `editorBoundsInPeer()` / `componentExtentFromEditor()` with `#if
  defined(__APPLE__)` inside - see `ClapPluginEditorComponent`. A wrapper
  calling `toPhysical` unconditionally is correct only while UI zoom is 1.0.
- **X11-only diagnostics stay Linux-gated.** `getActualGeometry` returns false
  on macOS by design, so an ungated `verifyGeometry()` prints "host window lost
  (XGetGeometry failed)" on every macOS editor. Gate declaration, definition,
  and call site.
- **Linux behaviour is an invariant.** Any change to a shared header or the
  Linux TU that cannot be validated on this machine is a flag, not an edit.

## Lifetime of deliberately-leaked resources

`setLeakOnClose(true)` exists because foreign-toolkit UIs hang in teardown. When
a resource is leaked on purpose, **everything it holds a pointer to must be
leaked with it**. A leaked suil instance keeps its controller and `ui:resize`
handle; a leaked CLAP GUI keeps its `ClapHost::Callbacks`. If the owning object
is destroyed while the leaked resource can still tick a timer, that is a
use-after-free at shutdown. Check both directions of the pointer graph.

## Feature gates and defines

- **A constant or helper used under gate A but defined under gate B breaks the
  first platform where the gates disagree.** `kScanTimeoutMs` was defined under
  `DUSKSTUDIO_HAS_OOP_PLUGINS` and used by the native-bundle scan sandbox; macOS
  without OOP support stopped compiling. Grep every use of a gated symbol for
  the gate it sits under.
- **A default-ON option must derive its default from its dependency,** and
  `FATAL_ERROR` only when the user asked for it explicitly. Otherwise a machine
  missing the dependency cannot configure at all. Keep the "requested but
  absent is fatal" half - a silently-OFF format is not verification.

## Platform API contracts

Verify the contract, do not assume the common case:

- CoreFoundation URLs from `CFBundleCopyExecutableURL` are **bundle-relative**;
  `CFURLCopyFileSystemPath` yields the bare executable name and `dlopen` then
  searches the dyld paths. Use `CFURLGetFileSystemRepresentation(url, true, ...)`.
- Cocoa subview placement is governed by the **superview's** `isFlipped`, not the
  child's. JUCE's peer view is flipped, so top-left coordinates already land
  correctly - a claim that placement is inverted needs that checked first.

## Tests

- Temp directories need a **collision-proof suffix** (pid or `random_device`
  plus a clock tick); ctest runs cases as parallel processes.
- Paths compared against scanner output must be **normalised** -
  `lexically_normal()` resolves `.` but keeps a trailing separator, which then
  shows up as an empty final filename.
- An error-message assertion that accepts a broad prefix ("dlopen failed:")
  hides the bug underneath it. Assert on the part that proves the code under
  test did its job.

## Documentation sync

- **MANUAL.md platform claims** go stale the moment a format gains a platform.
  Grep it for the format name and for "on Linux" whenever hosting changes.
- **A behaviour switch documented for "plugins" must name which rows it
  reaches.** `DUSKSTUDIO_USE_OOP_PLUGINS` is read only in `PluginSlot.cpp`, so
  it never touches native CLAP/LV2/VST3 rows.
- **Spec status lines and handoff prompts are load-bearing** - a stale "next
  increment" line makes the following session redo finished work. When a phase
  lands, update the status line, the resume phrase, and any prompt that names
  the phase.
