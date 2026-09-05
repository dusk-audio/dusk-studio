# Changelog

All notable changes to Dusk Studio. Format loosely follows
[Keep a Changelog](https://keepachangelog.com/). Pre-1.0 entries are
back-filled from `git log`; once tags exist this file is the
canonical source.

## [0.13.3] - Unreleased

This release hardens session and plugin recovery, transport MIDI cleanup,
native plugin hosting, and cross-platform launch and window behaviour. It also
closes release-metadata gaps found during the pre-tag audit.

### Fixed

- **Unreadable or rejected plugin settings are preserved.** A damaged native
  plugin state blob now leaves the slot offline and remains unchanged in the
  session. If a plugin rejects saved state, it is unloaded from the strip
  instead of being left active at its defaults and overwriting the only saved
  copy.
- **Transport panic reaches every MIDI routing path.** Panic events still reach
  MIDI tracks while they are muted or excluded by solo, VST3 synths that do not
  map the standard panic controllers receive note-offs for their sounding
  notes, and CLAP instruments receive a choke on every note-input port.
- **CLAP-only voices stop with the transport.** Instruments whose note port does
  not accept raw MIDI now receive a native CLAP choke when transport stops,
  loops, or jumps, including plugins that expose both CLAP and MIDI note ports.
- **Offline native inserts no longer add delay compensation.** An insert that is
  quarantined after a failed reactivation contributes zero latency until it
  successfully returns online, and compensation is recalculated at both
  transitions.
- **Clone Track preserves native plugin state on Linux.** CLAP, LV2, VST3, and
  multisample inserts now clone with their saved settings instead of appearing
  empty or committing a partially restored sampler.
- **Irregular Standard MIDI Files import correctly.** Import now skips vendor
  chunks between tracks, consumes the correct data lengths for system status
  messages, and keeps same-tick retriggers in their original order.
- **Dense MIDI output blocks keep every event.** Per-track output scratch space
  is sized from the source block and stable-sorted without audio-thread
  allocation, instead of dropping the whole block when fixed scratch capacity
  was exceeded.
- **Audio callback registration no longer stalls the audio thread.** Device
  callback fan-out uses immutable atomic snapshots, so adding or removing a
  callback cannot contend with real-time processing on a mutex.
- **Session handoff is cross-platform.** Opening a session while Dusk Studio is
  already running now reuses and activates the existing window on macOS and
  Windows as well as Linux.
- **Windows session handoff can foreground the existing window reliably.** The
  secondary process grants the primary process foreground permission before
  sending the request, with a taskbar flash when Windows still refuses focus.
- **Windows paths remain Unicode end to end.** Plugin-host launch paths, MP3
  bounce destinations, profile directories, and temporary directories now work
  when the user name or installation path contains non-ASCII characters.
- **macOS no longer opens two windows for one out-of-process plugin editor.** A
  working in-process shell editor is reused without first showing the child
  process fallback window.
- **Sandboxed plugin editor failures no longer strand their window.** On macOS,
  an available shell editor opens even if the child cannot hide its fallback
  window. Linux and Windows release stale embedded windows after a child crash.
- **Windows out-of-process plugin editors stay aligned with their host panel.**
  Embedded child coordinates now account for the top-level window origin and
  display scale after moves and resizes. They also hide beneath application
  dialogs and reattach safely after the parent window is recreated.
- **Sandboxed plugin windows stay responsive on Windows and macOS.** Window
  creation, visibility changes, resizing, and teardown now run on the child
  process's pumping message thread instead of its blocking control thread.
- **macOS sandbox connections no longer collide on one shared-memory name.**
  The name carried a prefix long enough to crowd out the process id and counter
  that made it unique, so two plugins loading at once could fail to connect, and
  a name left behind by a crash kept every later connection failing until the
  machine was restarted.
- **A macOS sandbox wake backlog no longer lands in one audio callback.** The
  parent usually picks up a finished block without waiting, which leaves the
  child's wake bytes queued; clearing the whole backlog is now spread across
  waits rather than paid for by whichever block happened to arrive late.
- **A macOS sandbox plugin that automates its own parameters no longer bypasses
  itself.** Parameter changes reported by the plugin are handed to the child
  process's message thread instead of being sent to Dusk Studio from whichever
  thread the plugin used, which for a plugin automating every block was the
  thread rendering its audio. Sending from there stalled rendering long enough
  for Dusk Studio to give up on the block and leave the plugin bypassed until
  it was reloaded.
- **A Windows sandbox connection cannot be taken by another local process.**
  The control pipe's name was predictable, so any process on the machine could
  create it first and make Dusk Studio's own attempt fail, which silently
  dropped the plugin back into the main process. The name now carries random
  bits, and the pipe refuses to open a name somebody else already serves.
- **A stalled Windows sandbox link drops instead of desynchronising.** A control
  transfer that stops part-way through a message now marks the connection dead
  rather than resuming in the middle of one, and a control write carries a
  deadline instead of blocking the interface for as long as the child leaves
  the pipe unread.
- **The Windows sandbox child's diagnostics reach Dusk Studio's console.** The
  child was started with no output handles of its own, so everything it
  reported about a failed startup was discarded; start Dusk Studio from a
  console to read it.
- **A Windows sandbox child's exit is handled once.** The child's exit stayed
  readable after it had been collected, so a caller polling for it could run
  crash recovery again on every check.
- **Loading a sandboxed plugin no longer freezes the window.** Starting the
  plugin host and asking it to load waits up to five and thirty seconds
  respectively, and both waits used to happen on the thread that draws the
  application, so a plugin that stalled on startup in its own process could
  lock the interface for over half a minute. Both now happen away from it, and
  a load that fails still falls back to loading the plugin in-process.
- **A sandboxed plugin whose process is killed drops out for a few buffers, not
  a tenth of a second.** No operating system wait the audio thread uses carries
  news of the child's death, so a killed plugin host used to hold the audio
  thread for the whole hundred-millisecond block timeout before the strip was
  bypassed. The wait is now split into short slices that re-check whether the
  child's connection has closed.
- **Switching sandboxed plugins in quick succession no longer risks a dropout
  or a crash.** Retiring an out-of-process connection now waits for the audio
  thread to leave the plugin's processing call before the connection's shared
  memory is torn down, so several loads, unloads, or re-enables inside one
  block cannot pull the buffer out from under playback. The child process also
  parks its audio worker before swapping a plugin, and a load that fails leaves
  the previously loaded plugin playing instead of silencing the strip.
- **A damaged recent session no longer breaks the startup screen.** Errors while
  scanning its audio directory now leave the format summary unavailable instead
  of escaping through the recent-session list.
- **VST3 bus activation now matches the buffers passed to the plugin.** Optional
  and zero-channel buses remain inactive, while selected buses keep one
  consistent shape through setup and processing.
- **Cancel now stops LV2 and Audio Unit scans too.** Cancellation is checked
  between native plugin bundles and a stopped pass does not publish a partial
  cache.
- **sfizz Rectify no longer reads beyond its SIMD stack input.** The pinned
  sfizz revision includes the bounds fix with no intended sound change.
- **The Tape processor no longer starts from an invalid mode sentinel.** The
  donor DSP pin moves from `69f04318` to `0a1b17f8`, removing first-block
  undefined behaviour while keeping audio output bit-identical.
- **The macOS release now includes the plugin sandbox.** The disk image kept
  its macOS 11 deployment target, but the sandbox's shared-address wake API
  required macOS 14.4, so CMake omitted the plugin-host helper and scans ran
  inside Dusk Studio. The macOS IPC backend now uses a deadline-aware pipe
  signal available on Big Sur, and release verification refuses a disk image
  without the helper.
- **LV2 plugin windows open with the sound's current settings.** Opening or
  reopening an LV2 editor used to leave its controls at the plugin's factory
  defaults even though the restored audio engine was already using the saved
  values. The host now seeds the window from the live parameter state and keeps
  it in sync with MIDI Learn and other host-side changes.
- **LV2 plugin parameters keep the same numbering between launches.** For plugins
  whose parameters are properties rather than ports - anything built with JUCE,
  among others - the host built its parameter list in whatever order the plugin
  library handed the properties over, which is not the same order every time. A
  MIDI control learned to one knob could therefore end up on a different knob
  after a restart. The list is now ordered by each parameter's property URI - the
  identifier the plugin itself gives it, which does not change between launches -
  so a saved binding keeps pointing at the parameter it was learned on. Bindings
  saved by an earlier release may land on the wrong parameter once, and stay put
  after they are re-learned.
- **LV2 file state loads only from the session's own state folder.** A damaged
  or hand-edited session could hand a plugin a file path the host had already
  rejected, or hand it nothing at all, which some plugins read straight into a
  crash. A rejected path now comes back as a path inside the session's state
  folder that does not exist, and the restore is reported as failed. Sessions
  written before Dusk Studio managed LV2 state generations are now read from
  that folder as well, rather than relative to wherever the app was launched.
- **Saving a session with a large LV2 sampler is no longer slow.** Every save
  swept the whole LV2 file store, so a multi-gigabyte sample bank made each save
  take longer the more files it held, even when nothing had changed. The sweep
  now runs when a save actually writes a new copy of a file, and once when the
  session opens. A file rewritten in place inside a single timestamp tick is
  also flushed to disk now, so a power cut cannot leave the current state
  pointing at half-written bytes.
- **A plugin that loses or refuses its settings now says so.** The terminal and
  alert identify the track and format. A plugin that rejects restored state is
  unloaded and left offline so its preserved state cannot be replaced by
  defaults.
- **A CLAP plugin whose window does not appear now says so.** Some plugins put
  their window inside the editor area without making it visible, or build it a
  moment after the host asks them to; either way the editor opened as a blank
  panel with nothing in it. Dusk Studio now makes the plugin's window visible
  itself, keeps watching for one that arrives late, and if the plugin never
  opens a window, the editor area says that instead of staying blank. A plugin
  that reports a failure from its own show step but draws anyway keeps its
  editor rather than losing it, and any reason an editor could not open is
  printed to the terminal and shown in the panel.
- **Closing a plugin editor on Linux no longer risks a crash.** While an editor
  is torn down, Dusk Studio narrows how X server errors are handled so a stale
  editor window cannot abort the application. That narrowing is now lifted on
  every exit path, including a teardown that fails part-way, so a later
  unrelated X error can no longer bring Dusk Studio down. Closing a window also
  only ever hands focus to another X11 window.

### Changed

- **The manual and activation checklist now match 0.13.3.** They document
  all-platform session handoff, Windows Unicode paths, live LV2 editor state,
  and the offline policy for rejected plugin settings; screenshot references
  were refreshed at the same time.
- **The bundled sfizz licence records now match the shipped submodule.** Every
  sfizz revision recorded there names `0bb8aae364dc648c7c55438d17c7564a5d5eaef5`;
  the nested dependency submodules sfizz vendors (Abseil, ghc-filesystem, SIMDe,
  dr_libs and the rest) keep their own recorded revisions. Release-mechanics
  tests reject future drift.
- **The de-JUCE ledger now records the real 0.13.3 baseline.** The increases
  introduced in #398 are disclosed for `AuxLaneComponent.cpp` (168 to 175),
  `ClapPluginEditorComponent.cpp` (11 to 18), its header (7 to 8), and
  `MainComponent.cpp` (457 to 459). Five stale ceilings were tightened to their
  actual counts so later regressions cannot hide in unused allowance.

## [0.13.2] - 2026-08-26

### Fixed

- **Plugin settings are restored again when a session is reopened.** Every
  plugin loaded through Dusk Studio's own CLAP, LV2, VST3, Audio Unit and
  multisample hosts came back at its default settings after a save, a quit and
  a reopen, no matter which format it was. The settings were written to
  `session.json` correctly all along; the code that read them back used a
  different, incompatible text encoding to the one that wrote them, so it saw
  nothing and left the plugin untouched. Plugins hosted through the application
  framework were never affected. Sessions saved by an earlier release carry
  usable settings and recover them on this one, in either of the two forms
  earlier releases wrote, with one exception:
  a session that was reopened under the fault and then saved again had its
  stored settings overwritten with the defaults that were on screen at the
  time, and those are gone. Opening a session now hands each plugin its
  settings as it loads, so a session carrying several heavy plugins can take
  a little longer to open, with a brief silence while that happens.
- **A plugin that hangs while being scanned no longer takes the scan with it.**
  Scanning probes each plugin in a separate process so a bad one cannot crash
  Dusk Studio, and gives it 30 seconds before declaring it stuck. That deadline
  could never fire: the code waiting for the child process to say something did
  not come back until the child said it, so a plugin blocking on a licence or
  authorisation dialog it cannot show during discovery held the scan open
  indefinitely. A watchdog now enforces the deadline and ends the plugin so the
  scan moves on, for CLAP and VST3 bundles read by Dusk Studio's own hosts as
  well as for the standard host, which had the same fault. A plugin that
  finishes just as the clock runs out is no longer quarantined for it.
- **The plugin scan can be stopped.** The progress window has a **Cancel**
  button, and Esc does the same: the scan stops at the plugin being probed and
  skips it rather than quarantining it. Plugins found before the stop stay in
  the picker, though a CLAP or VST3 pass through Dusk Studio's own hosts is
  discarded rather than recorded half-finished, so run the scan again when you
  want a complete list. Before this the window could not be dismissed at all,
  so a scan that went wrong left no way into the rest of the application.

## [0.13.1] - 2026-08-24

A packaging and Windows-safety fix release. The 0.13.0 macOS disk image could
not launch on any machine, and both the disk image and the Windows installer
carried Linux desktop-integration files they had no use for. Windows also gains
Dusk Studio's own CLAP and VST3 hosts, which until now were Linux and macOS
only.

### Added

- **Native CLAP and VST3 hosting on Windows.** Both formats now load through
  Dusk Studio's own hosts rather than the application framework's, matching
  Linux and macOS. Plugin paths are handled as Unicode throughout, so an
  installation under a non-ASCII user name loads; scanning searches the
  standard per-user and machine-wide install directories; and plugin editors
  embed as child windows that are told their scale, display DPI times the
  interface zoom, before they are attached. LV2 remains Linux and macOS.

### Fixed

- **The Windows installer now opens the session notepad on basic-display,
  virtual-machine, and Remote Desktop sessions.** The MSI carries a pinned
  Mesa software OpenGL renderer, and the application selects llvmpipe before
  creating its first OpenGL context regardless of how it was launched.
  This avoids both Windows' OpenGL 1.1 `GDI Generic` fallback and the unstable
  Mesa-on-D3D12 Compatibility Pack path. The payload and archive are SHA-256
  verified during CI packaging, and a missing renderer makes packaging fail
  instead of producing another installer with a non-working notepad. Upgrades
  also retire a first-frame failure marker written by an older release.
- **The session notepad was half-size on Retina Macs.** JUCE places its window
  in Cocoa points while the embedded DGL editor expects backing pixels. The
  notepad now uses the parent view's actual backing scale for its bounds and
  font atlas, without changing Windows, Linux or non-Retina sizing.
- **The Windows notepad could end Dusk Studio.** Source builds still refuse a
  display limited to OpenGL 1.1 cleanly. Microsoft's OpenGL Compatibility Pack
  advertises OpenGL 4.6 through Mesa's D3D12 renderer but terminates the host
  while presenting the first frame, so that known-bad renderer is refused too;
  the session and the rest of the application remain open. The installer avoids
  both paths with its bundled llvmpipe renderer. Temporary first-frame stage
  tracing used to place the failures has been removed, while the GL identity
  and actionable failure diagnostics remain.
- **A graphics driver can no longer cost the same session twice.** Refusing a
  renderer by name only helps against drivers somebody has already lost work
  to, and the failure exits cleanly, so there is no error for the notepad to
  catch and act on. Dusk Studio now records which renderer it is about to draw
  the notepad's first frame on and clears that record once a frame completes.
  A run that never comes back leaves it behind, and the next launch declines to
  open the notepad and names the renderer responsible. The notepad can also be
  switched off outright with `notepad_enabled` in `app-config.properties`.
- **macOS: the 0.13.0 disk image could not launch.** The application linked
  four Homebrew libraries by absolute path with none of them copied into the
  bundle, so the loader looked in a location that exists only on the build
  machine, and the install step rewrote the load commands after the signature
  had been sealed, invalidating it. The bundle now carries the whole
  fourteen-library closure with its install names rewritten to point inside
  itself, every binary in it is checked for a load command pointing outside,
  and the installed copy is what gets signed. Nothing in the audio path
  changed; the shipped 0.13.0 binary passes the full self-test once repaired.
- **Desktop-integration files shipped inside the macOS and Windows packages.**
  The `.desktop`, appdata, mime and icon install rules ran on every platform,
  so the disk image carried a `share/` tree beside the application and the
  installer carried one under its install root. Only the Linux packages
  consume those files, and the rules are now Linux-only.

### Changed

- **Release and build tooling.** The asset verifier now expects the six-asset
  layout that releases actually carry, instead of the ten-name layout that
  predates the consolidated checksum file, and the maintainer guide documents
  the six filenames and the single `SHA256SUMS`. The Linux release notes name
  libpipewire as a runtime dependency, which a machine without it needs in
  order to make sense of the loader error. Cutting a release is now driven by
  a `/release` command whose acceptance step requires the packaged artifact to
  be launched on each platform, which is the check 0.13.0 lacked. Dependency
  installation in CI is bounded by a timeout and switches package mirrors
  after a failure, so a sick mirror costs minutes rather than a hung job.
- **macOS sandbox documentation.** The manual described the plugin sandbox in
  terms the macOS build does not deliver; it now says what actually happens.

## [0.13.0] - 2026-08-19

The first release cut from the main line since 0.12 branched. Linux gets a
native PipeWire backend and MIDI hot-plug; macOS gets Dusk Studio's own plugin
hosts for all four formats, Audio Units included. Stems render in a single
pass, a new realtime bounce prints hardware inserts wet instead of dry, loop
recording keeps every pass as its own take, every session now carries a
notepad with a chord chart, and the whole interface zooms from half to double
size. Underneath, the audio device layer, MIDI, audio file I/O and the FFT
stopped going through the application framework and became Dusk Studio's own.

The 0.12.7 fix wave was written on the 0.12 line but never released; those
fixes are ported here and are listed below rather than under a 0.12.7 heading.

### Added

- **Session notepad.** Every session carries a notepad for lyrics and notes,
  opened from the transport bar. It is a page-style document editor - headings,
  bold / italic, bullets, numbering, quotes, task boxes and clickable links,
  with its own undo - and it writes chords over the words they land on: put the
  caret in a syllable, press Ctrl+K, and type. Completions rank diatonic chords
  first once the key is detectable, chord labels stay glued to their syllable
  as the lyric is edited around them, and the transpose buttons shift the whole
  sheet while keeping your own accidental spelling. Sections are markers such
  as `[Chorus]` on their own line. Notes save atomically beside the session as
  a plain `notepad.md` using ChordPro brackets, so the file opens in any
  chord-sheet app and follows the session through Save As. A toolbar button
  flips the page to its Markdown source and back without losing unsaved
  edits. Typing in the notepad never triggers a transport shortcut.
- **Native PipeWire audio backend (Linux).** Dusk Studio now speaks
  libpipewire directly instead of borrowing a JACK compatibility layer.
  PipeWire sinks, sources and duplex nodes list as devices of their own,
  buffer size is requested as a PipeWire quantum, and capture and playback
  share one graph cycle so they stay sample-aligned. The native ALSA backend
  remains as the fallback, including when the saved device is busy at launch.
- **Native Audio Unit hosting (macOS).** AU effects and instruments load
  through Dusk Studio's own host - discovery, rendering, session state and
  Cocoa editor views. Native CLAP, LV2 and VST3 hosting arrives on macOS in
  the same release, editors embedded in-window.
- **MIDI hot-plug (Linux).** Connecting or disconnecting a MIDI interface is
  picked up automatically; the input list refreshes with no manual rescan. A
  refresh that lands mid-take is held until the transport stops, so a take
  never loses its input part-way through.
- **Loop recording stacks takes.** Recording over a loop no longer overwrites
  the previous lap. Each pass lands as its own take, aligned to the loop
  start, and the track keeps the current take plus the last eight passes to
  cycle through from the take badge. Audio and MIDI both stack, punch inside
  the loop included; held notes and controller state carry across the seam so
  a pass never starts mid-phrase, and punch's post-roll no longer stops the
  transport part-way through a loop. A silent audio pass is still selectable -
  it may be the room tone you wanted - while an empty MIDI pass is dropped.
- **Realtime bounce.** Mixdown and stems can play the session through the
  audio device and capture the result, so hardware inserts print wet. Offered
  whenever the session uses a hardware insert; it needs a stopped transport
  and always writes 24-bit WAV at the device rate. Offline renders are
  unchanged and keep their dry-insert warning.
- **Hardware inserts on frozen tracks.** A frozen track is an audio source, so
  its strip now runs a hardware insert like any other - latency-compensated
  from the measured loop, and the latency ping works with the transport
  stopped. Mono hardware inserts gain their own I/O format (single send,
  single return) instead of being modelled as a half-empty pair.
- **UI scale control.** A slider in the audio settings panel zooms the whole
  interface, 0.50x to 2.00x on top of the OS display scaling. The interface
  rescales live as the slider moves, and the value is remembered per machine
  rather than per session. A denser console layout fits a full bank of eight
  strips plus the buses and master on a 1080p display.
- **View menu.** Full Screen (F11) and a Show Timeline toggle.
- **New folder button in the file browser.** Available on every save and
  directory-pick flow; creating a folder navigates into it.

### Changed

- **Stems render in a single pass.** Exporting stems used to re-render the
  whole session once per track with solo isolation, so N stems cost N renders
  and each one heard the master strip reacting to a single track. One render
  now captures every stem at its own tap: tracks post-fader and post-pan with
  no master processing, plus bus-group and aux stems taken at the engine's sum
  points. The whole set shares one latency trim, so the stems stay
  sample-aligned and tracks + buses + aux reconstruct the pre-master mix. A
  stems export defaults into a `stems/` subfolder instead of burying the
  session root, and warns when solo is active.
- **Engine internals.** Audio devices, MIDI in and out, audio file reading and
  writing, timers and message-thread dispatch, the FFT, and the string and
  math utilities are now Dusk Studio's own code rather than the application
  framework's. Behaviour is unchanged by design; this is the groundwork for
  the interface rewrite.
- **Groundwork for signed SFZ catalogs.** Catalog parsing and detached Ed25519
  signature verification now exist and are covered by tests. Verification uses
  trusted keys supplied by its caller; the application does not consume the
  catalog layer yet, and no key is built in. Source builds now need libsodium
  (`libsodium-dev` on Debian and Ubuntu, the vcpkg manifest on Windows).
- **License texts ship with the packages.** The tarball, the deb and rpm,
  the DMG and the MSI now carry `LICENSE` and `LICENSES.txt`, and the
  attribution list credits the notepad's UI stack (DAF, Dear ImGui, pugl and
  the components they embed) along with the CLAP headers. A completeness pass
  then covered the framework's bundled text-shaping, image and codec
  libraries, reproduced every license text that must accompany a binary - the
  AGPLv3 included - recorded per-platform version inventories, corrected the
  framework's license terms and libsndfile's real scope. The Windows
  statically-linked codec chain is now covered at its exact pinned versions -
  the LGPL-2.1 text, FLAC, Ogg, Opus and mpg123 notices read from each
  project's own license file - and the deb and rpm gained a Debian-policy
  copyright record in place of a bare license copy.

### Fixed

- **Session load.** A truncated, hand-edited or older `session.json` could
  keep the previous session's values for anything it left out - an insert
  slot's plugin, its name and colour, the whole mastering and transport
  sections. Every absent key now falls back to the model default. Values that
  are present but corrupt are checked finite and clamped to the range their
  control enforces, across track and bus fader and pan, HPF and LPF frequency,
  the whole compressor block, the EQ bands, the hardware-insert gains and the
  mastering and master floats. A corrupt aux send now resolves to OFF instead
  of unity, so a damaged file no longer opens feedback-loud.
- **MIDI sync settings.** Chase, emit and frame rate were written by the Audio
  Settings panel but never saved, so they reset every time the session was
  reopened. They round-trip now, and fall back to the model default when the
  keys are absent.
- **Console bank switching.** Changing bank left MIDI bank bindings and the
  Mackie surface pointing at the previous eight strips. Resizing the window no
  longer strands the visible page away from the bank the surface is driving,
  and a bank press made during a window drag is no longer swallowed.
- **Compressor makeup on the control surface.** The makeup encoder wrote a
  parameter no processing read, so turning it did nothing and pushing it to
  reset did nothing either. It now drives the active compressor mode's gain,
  the same one a MIDI binding drives. The optical mode's makeup was also
  labelled at half its real value everywhere it appeared, so a reading of
  +12 dB was delivering +24 dB; the numbers now match the gain.
- **Compressor threshold, ratio, attack and release on the control surface.**
  The threshold encoder wrote a parameter no processing read, and the other
  three always wrote the VCA mode's, so in optical and FET modes four of the
  five compressor encoders were silent. Each now drives the active mode's own
  parameter over that mode's real range. Pushing one resets it - threshold to
  no compression, ratio, attack and release to the mode's own default rather
  than VCA's. Attack, release and the VCA ratio also step by a percentage per
  detent instead of a fixed amount, so the fast end of the FET's range is
  reachable and the VCA's long releases and high ratios no longer take
  hundreds of turns. The optical mode has no ratio and no attack or release -
  its curve and timings are the model - so those three encoders leave it
  alone, matching the editor, which hides those knobs. A MIDI binding to the
  compressor threshold follows
  the same mapping: the controller now sweeps threshold in dB in every mode,
  where before it ran backwards in optical mode and moved the FET's input
  drive instead of its threshold. A binding that had been driving that input
  drive leaves it wherever it last wrote it - the value is saved with the
  session and is now reachable only from the strip's FET INPUT knob.
- **Compressor controls that could not reach their range in VCA mode.** The
  ratio knob stopped at 20:1 while the parameter and the manual both go to
  120:1, so a strip set higher from the control surface read back as 20:1 and
  the next touch of the knob wrote that back. The threshold handle on the
  editor's gain-reduction meter ran −60 to 0 dB in every mode, so it could
  not reach VCA's +12 dB and the bottom third of its travel was dead against
  that mode's −38 dB floor. Both now follow the active mode.
- **Cloning a track.** The clone carried every compressor value except the
  FET threshold, which the destination kept from whatever was there before.
- **Joining regions.** A join could land on the wrong slot, cut the merged
  region short, or drop the last region's fade-out; the merge now sizes from
  the latest end across the selection and carries that region's fade. Editing
  or deleting a MIDI region while the transport rolls is also safe now.
- **Loop seam.** The declick ramp silenced the run-up to the loop point
  instead of fading down into it, so every cycle had an audible gap before
  the wrap.
- **Real-time safety.** The tuner's pitch scan, the hardware-insert latency
  ping and the loudness meter's gating scan could each overrun the audio
  callback's budget on a small buffer; all three are now bounded. Empty and
  oversized blocks from the device are guarded, and a channel strip coming out
  of mute no longer replays up to a third of a second of pre-mute audio.
- **Plugin editors.** Replacing a plugin with its editor open could read freed
  memory (CLAP, LV2, VST3, and the macOS AU editor); the editor is now dropped
  with the instance it belongs to. A plugin slot also re-publishes reliably
  after a reload. Going fullscreen, or anything else that rebuilds the main
  window, used to leave a channel's native editor stranded; the editor is
  rebuilt into the new window instead, reopening straight away unless a modal
  is covering it. On Linux, an embedded VST3 view no longer comes back blank or
  off-centre after a resize, and its child windows map correctly.
- **MIDI and soundfonts.** Hung notes no longer lose their tail, CC values no
  longer diverge from what was sent, and several SF2-to-SFZ translation faults
  are corrected. A dense burst that filled the input ring but overflowed a
  smaller downstream buffer used to be dropped whole, note-offs included; every
  buffer along the path now matches the ring.
- **Interface.** Modals opened over the notepad are visible and no longer leak
  keystrokes to the transport; the on-screen keyboard's note keys no longer
  double as transport shortcuts; modal teardown, plugin editor embedding and
  timer shutdown gaps are closed.
- **Narrow windows.** Shrinking the window used to squeeze the faders past
  the point of being playable and let the strips run under the bus and master
  columns. Console sizing is now driven from one place: a page holds as few as
  three channels at the minimum width, the page buttons and their number-key
  shortcuts follow, and the visible page stays in step with the control
  surface's fixed banks of eight across a resize.
- **Input metering while armed.** An armed audio track monitored through the
  interface showed no level in the Recording stage unless IN was enabled, so
  a hardware-monitored input looked dead while it was being played. Arming an
  unfrozen audio track now meters its live pre-fader input; IN remains the
  explicit override, and retained, MIDI and frozen tracks keep their own
  meter source.
- **Opening a session while one is running.** The hand-off to the already-open
  instance could discard unsaved work or let two instances write the same
  session folder.
- **MIDI input shutdown.** Closed a window where a MIDI input could dispatch
  into a callback that had already gone away.
- **Monitoring click in offline renders.** An enabled play-along metronome
  printed into the render; it is a monitoring aid and never prints now.
- **Mastering view playback.** The waveform no longer shimmers as the playhead
  advances, and clicking a button in the view no longer swallows the Space
  play / stop toggle.
- **Audio device changes.** Switching backend no longer raises "Audio device
  disconnected" while you are still choosing an interface. On ALSA, a
  `plughw:` device is no longer mistaken for the raw card of the same number
  and clock-linked to it, and changing the period count now actually reopens
  the device instead of waiting for the next manual switch.
- **Windows plug-in scanning.** The MSI now includes
  `dusk-studio-plugin-host.exe` beside the main application. The helper was
  built but omitted from CMake's install set, so every third-party plug-in was
  left unscanned and the scan completed immediately with a
  sandbox-host-unavailable warning. The Windows packaging script now validates
  the staged install contains both executables before creating the MSI. The
  scanner also mangled plug-in paths containing non-ASCII characters - a
  user name with an accent was enough - so those plug-ins scanned as empty;
  the helper now reads its arguments as UTF-16 and converts them itself. And
  when a scan quarantines files, the report now says how many and where the
  cache lives instead of silently skipping them on every later scan.
- **Windows with a busy ASIO driver.** If the saved audio device could not be
  opened at launch - an ASIO driver held by another application, or powered
  off - Windows was left with no device at all, and every plug-in and
  soundfont load was refused with no explanation. Startup now falls back to
  another backend's default device (without re-entering the driver that just
  failed) and says what happened, including that loading is disabled while no
  device is open.
- **Soundfonts on Windows.** SFZ and SF2 files under paths with non-ASCII
  characters failed to load; file reading and writing now hand the operating
  system proper wide-character paths.
- **Soundfont failure recovery.** An `.sfz` whose samples cannot be found no
  longer "loads" as a silent instrument. A failed first load leaves the slot
  empty and names the missing samples; a failed Reload or Browse keeps the last
  successful file reference and SF2 preset. If a saved SF2 preset is no longer
  playable, Dusk Studio reports the fallback and loads preset 0 so the slot
  remains playable.
- **Notepad on Windows.** The notepad's renderer did not compile under MSVC,
  which would have shipped a Windows build without the release's headline
  feature; the GL functions it needs are now loaded at run time the same way
  the rest of the UI stack does, and the Windows test workflow builds the
  notepad on every push so it cannot regress unnoticed.
- **Package contents.** The deb, rpm, DMG and MSI included the application
  framework's entire source tree, its build tool and its CMake config -
  roughly 65 MB and three thousand files nothing at runtime reads, claiming
  paths a real framework package would own. The install set is now the
  application payload alone.
- **Manual.** The control-surface chapter still described the pre-0.13 bank
  behaviour; it now explains the two axes - screen pages sized by window
  width, the surface's fixed banks of eight - and how they stay in step. The
  notepad chapter matches the shipping editor, the keyboard reference gains
  the notepad shortcuts including the word-motion keys macOS actually uses,
  and the PDF renders the sharp and flat signs instead of dropping them.
- **Build docs.** The source-build instructions referenced a donor checkout
  layout that no longer exists, omitted the submodules and the notepad's UI
  dependencies, and marked packages optional that configure requires; a
  source build now follows them as written.
- **Linux builds.** No workflow installed the PipeWire development package,
  so the probe missed on every runner and the published Linux artifact
  carried the ALSA backend alone - the native PipeWire backend this release
  headlines has never actually shipped in a binary. The Linux builds install
  the package, and a configure that cannot find it now fails outright rather
  than dropping the backend in silence.

## [0.12.6] - 2026-07-25

Beta patch on the 0.12 line: manual recording latency compensation for
interfaces that misreport their round-trip latency.

### Added

- **Manual recording latency offset.** Audio Settings > Advanced gains an
  offset (in samples) applied to every recorded take at placement, for
  interfaces whose reported latency leaves overdubs drifting. Count-in, the
  write gate, and MIDI placement are unaffected; a shift that would cross
  timeline zero trims the take's head instead of playing it late, and a take
  fully consumed by the offset is surfaced rather than silently discarded.
- **Sample readout in the region editor.** The status bar shows the cursor
  position - or range start and length - in raw samples, so a loopback
  calibration recording can be measured directly.

### Fixed

- **Stream leak** when WAV writer creation failed at record start.
- **Import races** in the region editor.
- **Device restarts.** A pending DSP restart now defers to transport stop and
  is consumed at every stop, and teardown around the restart is thread-safe.
  The fallback device choice persists across restarts.

### Changed

- **Audio settings layout.** Advanced rows aligned, effect oversampling on its
  own row, Rescan devices moved into the Audio section.

## [0.12.5] - 2026-07-18

Beta patch: the Windows build of 0.12.4. Same feature set, now compiling on
every platform.

### Fixed

- **Windows build.** 0.12.4 failed to compile on MSVC - a `juce::WeakReference`
  built inside a lambda init-capture, which MSVC rejects and Clang accepts, so
  macOS and Linux were unaffected. Constructed as a named local instead.

## [0.12.4] - 2026-07-18

Beta patch on the 0.12 line: a filterable, program-grouped preset browser for
multi-preset SoundFonts.

### Changed

- **SoundFont preset picker.** Loading a multi-preset `.sf2` (GM/GS/XG) now
  opens a compact, filterable grid instead of one long scrolling dropdown.
  Presets group program-first - an instrument and its bank variants list
  together, drum kits last - each shown as `PPP [bank] Name`. Type to filter by
  name or number; the popup stays short and reads across in columns.

## [0.12.3] - 2026-07-15

Beta patch on the 0.12 line: monitored inputs now feed their aux sends during
playback.

### Fixed

- **Aux sends on a monitored input during playback.** With **IN** engaged,
  pressing PLAY selected only the disk take for channel-strip processing, so
  an audio track's live monitored input - and every aux send fed from it -
  went silent, returning only on Record - so a vocal monitored with reverb
  lost the reverb while playing along, then got it back on record. Live input
  now runs the full channel strip during playback, so
  its sends (headphone reverb/delay while tracking) sound in every transport
  state. The audio twin of the 0.12.1 live-play-along fix.

## [0.12.2] - 2026-07-13

Beta patch on the 0.12 line: fixes a crash when bouncing, rendering stems,
or freezing a track through a CLAP (or VST3/LV2) plugin insert.

### Fixed

- **Mixdown crash with native plugin inserts.** Bouncing to a file, rendering
  stems, or freezing a track could crash when a channel or aux strip hosted a
  CLAP plugin. The offline render detaches the audio device and re-prepares
  the engine, which reaches every hosted plugin's activate/deactivate - the
  CLAP contract (and VST3/LV2) requires those calls on the main thread, but
  they were running on the bounce worker. The re-prepare now marshals to the
  message thread, so the plugin's thread contract is honoured. Shutting down
  mid-bounce can no longer leave a dangling callback either.

## [0.12.1] - 2026-07-12

Beta patch on the 0.12 line: live play-along, control-surface calibration,
and a Windows plugin-scan crash.

### Fixed

- **Live play-along during playback.** An armed, input-monitored instrument
  or MIDI track went silent the moment PLAY was pressed and came back on
  STOP - the playback path never merged live input. With **IN** engaged the
  live controller and on-screen keyboard now sound on top of the timeline
  while the transport rolls.
- **Control-surface fader calibration.** The MCU fader map was linear in dB,
  so a surface's printed **0** landed around -10 dB on screen. Faders now
  follow the standard Mackie taper in MCU mode and for hand-mapped pitchbend
  fader bindings alike: printed 0 means 0.0 dB, full-up +12, and motor
  positions round-trip exactly.
- **Windows plugin-scan crash.** The out-of-process scan sandbox never
  actually engaged on Windows (the helper binary was looked up without its
  .exe suffix), so one bad or unauthorized plugin could take down the whole
  app during a scan. The sandbox now runs; a plugin that hangs on a license
  dialog is quarantined after 30 s with a distinct reason; if the sandbox
  is unavailable, third-party plugins are skipped and reported instead of
  scanned in-process; cancelling a scan no longer blacklists the plugin in
  flight.
- **Deterministic bounces.** Live MIDI input can no longer print into an
  offline bounce, stem render, or track freeze.

### Changed

- **Track input picker.** The I/O popup now carries a title with the track
  name, a caption on every field (Mode, Input L/R, MIDI port, Channel, MIDI
  out), a labelled MIDI-activity LED, equal-width dropdowns, and honest
  wording: the follow-track default reads "In N (follow)" and empty
  selections read "None" everywhere.
- **One interaction grammar for strip sections.** EQ, COMP, AUX and TAPE behave
  identically on channel, bus and master strips, expanded or compact:
  left-click toggles the section, right-click opens its menu (character or
  mode, reset where supported, open editor), double-click opens the editor.
- **Update banner links to Patreon.** The startup dialog's update notice is
  clickable and opens the downloads page.
- Clearer startup messages when no usable display is found (headless vs
  Wayland-without-XWayland vs unreachable X server).
- Mixer-style controllers with a Mackie/MCU emulation mode (Tascam Model 12
  and similar) should use MCU mode rather than hand-mapped MIDI bindings -
  the manual's control-surface chapter now says so and explains why
  (bindings are one-way: no LEDs, no motor feedback, no transport).

## [0.12.0] - 2026-07-11

Beta. The biggest release so far: Linux-native plugin hosting across CLAP,
LV2 and VST3 (effects and instruments), a production-readiness pass over the
whole engine — sample-rate safety, seamless looping, Save As that takes your
audio with it, latency compensation across every path — and new delivery
options for the master. This cut also brings the console EQ on every channel
and bus onto the new-generation EQ core (a subtle, deliberate re-voicing —
see Changed) with a lighter CPU footprint, and fails with clear instructions
instead of a core dump when launched on Wayland without XWayland.

### Added

- **Native plugin hosting on Linux (CLAP / LV2 / VST3).** Effects and
  instruments load through Dusk Studio's own hosts (picker rows **CLAP**,
  **LV2-Native**, **VST3-Native**) instead of the standard hosting layer.
  Editors embed reliably on Wayland desktops, parameters are automatable and
  MIDI-learnable (including "learn last-touched knob"), state persists in the
  session, multi-plugin bundles resolve to the right plugin, and discovery is
  crash-safe (sandboxed scans; native LV2 discovery never executes plugin
  code). Native instruments get full MIDI delivery, a CC bridge, and the
  track-mode flow. LV2 instruments join in this release — if one doesn't
  appear in the instrument picker after updating, run **Scan plugins** once
  (older plugin caches predate LV2 instruments).
- **Session sample rate.** A session remembers the rate its audio was made
  at: opening it switches the device to that rate automatically, and Dusk
  Studio warns loudly when it can't — or when the device rate changes
  mid-session. No more silently detuned projects.
- **Mastering delivery presets.** Export master as 24-bit WAV at the session
  rate, **16-bit 44.1 kHz WAV with TPDF dither** (CD spec), or MP3 320 kbps.
  MP3s now carry a proper Info header so players report duration and seek
  correctly.
- **MIDI soft takeover (pickup).** Settings → General: a bound knob or fader
  stays dormant until it crosses the parameter's current position instead of
  snapping it on first touch. Covers all continuous mixer targets.
- **Autosave cadence.** Settings → General: 15 seconds to 5 minutes.
- **LV2 file-backed state.** A sampler's loaded bank or a convolution
  reverb's impulse response is snapshotted into the session's `state/lv2/`
  folder on save and travels with the session, including through Save As.
- **Send-effect latency compensation.** A latent plugin on an aux return
  (lookahead comp, linear-phase EQ) no longer flams the wet signal against
  the dry mix — the master stage aligns every path, metronome included.
- **Background imports.** DP-song import and SF2/SFZ soundfont loads run on
  worker threads behind progress dialogs with Cancel; the interface stays
  responsive through multi-minute imports and GM-bank loads.
- **DP import decodes time signature.** A song's time signature (numerator /
  denominator) is read from `song.sys` and applied on import, alongside the
  existing tempo / marker / mixer recall.
- **CLAP scan cache.** Scanned CLAP descriptions persist in a sidecar cache,
  so the picker is populated at launch without a re-scan.

### Fixed

- **Save As takes the audio along.** "Save As…" now copies every
  session-owned file (takes, take history, freeze renders, a session-local
  mastering source) into the new folder and rewrites the session to match.
  Previously the new session silently referenced the old folder — deleting
  it lost every region.
- **Seamless looping.** Loop playback no longer drops out at the top of each
  cycle, bleeds material from past the loop point, or skips the loop
  downbeat; the seam gets a short declick ramp.
- **Bounce correctness.** Mixdown length now accounts for MIDI regions and
  frozen tracks (a virtual-instrument song no longer renders as six
  seconds); a cancelled or failed bounce deletes its partial file instead of
  overwriting your last good one; disk-full during the MP3 flush fails the
  bounce instead of reporting success; freezing a track with a latent insert
  no longer bakes that latency in as a delay.
- **Mastering player pitch.** A mix whose sample rate differs from the
  device's is resampled — it used to preview and export fast and sharp.
- **MIDI clock stability.** The emitted clock no longer drifts at fractional
  samples-per-tick tempos; hardware slaved to Dusk Studio stays locked over
  a full song.
- **Undo coverage.** A MIDI overdub that replaces exactly one region is
  undoable; MIDI regions can be deleted from the arrangement (context menu
  and Delete key, both undoable).
- **Session load hardening.** A truncated or hand-edited `session.json` can
  no longer leave the previous session's tracks, buses or aux lanes playing
  under the new session's name; out-of-range automation times and
  non-finite values are clamped on load.
- **Import robustness.** Imports stream in bounded chunks (a long stem no
  longer allocates gigabytes), resample through a windowed-sinc kernel,
  land sample-aligned on the timeline, and no longer overwrite each other
  when two hit the same track within a second. Malformed or crafted `.sf2`
  files are rejected instead of exhausting memory.
- **DP import fader + pan recall.** Channel faders and pan are now read from
  the device's actual mixer array (0xC4) with a calibrated byte-to-dB curve,
  instead of the wrong record offset that recalled unity / centre for every
  track.
- **Transport taps.** REW/FFWD taps jump to the previous/next marker in any
  transport state (stopped taps used to rewind to zero).
- **Tuner pitch accuracy.** The pitch detector read consistently sharp
  (about a semitone at low notes); it now locks on the true period and reads
  within a few cents.
- **Freeze while rolling.** Freezing/unfreezing now requires the transport
  to be stopped — the render used to interrupt a live take.
- **Control surface readout.** The MCU bar/beat display is correct at
  sample rates other than 48 kHz.
- **Text fixes.** UTF-8 punctuation in the import windows no longer renders
  as mojibake; menu-bar tooltips that exceed one line wrap instead of
  clipping.
- **MTC.** SMPTE hours wrap at the 24-hour boundary (the emitted hours field
  stays within the 0-23 spec range).
- **No more popup flash while renaming.** Right-clicking inside a
  double-click rename field (track / bus / aux names, region gain and fades,
  the transport clock) no longer flashes a native menu window on
  X11/Wayland; the in-window behaviour now covers every text field.

### Changed

- **Console EQ engine.** The channel-strip and bus EQ moved to the
  new-generation parallel-summing console core (the engine behind 4K EQ
  version 2). Band interaction and the brown/black voicings track the
  hardware model more closely, and an engaged EQ carries a trace of
  transformer character even when flat. Existing sessions will sound
  slightly different on the strip EQ at the same settings; the compressors
  are bit-identical. The channel EQ/compressor path also costs roughly
  10-20% less CPU.
- **Launching without a display.** On a Wayland session without XWayland,
  Dusk Studio now prints instructions for enabling it and exits cleanly
  instead of crashing (#56). An X11 display (XWayland on Wayland desktops)
  remains a requirement — now stated in the manual.
- **MIDI engine internals.** Device enumeration, input routing and the
  clock/MTC output path were modernised. Behavior is intended to be
  identical; report any regression, especially external clock/MTC timing.
- **Dusk-native UI sweep.** The remaining stock-JUCE dialogs and pickers
  (including the last native alert on the stem-overwrite prompt) now use
  Dusk Studio's in-window equivalents — no more separate OS windows fighting
  Wayland stacking.
- **Update notice.** The "update available" badge moved from the startup
  dialog's sidebar to a banner above the Recent Sessions list.
- **Bus compressor.** Stereo link shares the sidechain across L/R with a
  continuous link amount, verified on both the native and oversampled paths.

## [0.11.1] - 2026-06-24

Patch release on the 0.11 Beta line: top-row layout, a playhead-chase option, an
editor grid, shortcut cleanup, and a batch of fixes.

### Added

- **Chase / Follow playhead.** The timeline and the audio / MIDI editors can
  scroll to keep the playhead in view during playback - a toolbar toggle plus a
  "Follow playhead by default" option in Settings.
- **Bar / beat grid in the region editors.** The audio and MIDI editors draw
  adaptive beat / sub-beat ticks on the ruler and a grid over the content, denser
  as you zoom in.
- **Raspberry Pi (arm64) Linux tarball.** Release builds now publish an aarch64
  tarball alongside x86_64.

### Changed

- **Top-row layout.** The stage selector (RECORDING / MIXING / MASTERING / AUX)
  moved into the menu row; the timeline controls (Snap / zoom / Chase) sit in the
  transport row, grouped with the bank buttons under the stage selector and shown
  only when the timeline is open.
- **Keyboard shortcuts.** Plain number keys 1-4 switch channel banks; Cmd/Ctrl+1
  to 4 switch stages. **T** shows / hides the timeline. **Cmd/Ctrl+E** is the one
  split shortcut everywhere (tape strip, audio editor, piano roll).

### Fixed

- **Audio device busy at startup.** When the saved device is in use or won't
  open, Dusk Studio now falls back to another available device with a notice
  instead of opening a silent session that can't play.
- **ALSA crash** on interfaces with fewer than 32 channels (an out-of-range
  channel index could corrupt the heap).
- **Bounce dialog freeze / crash.** Closing or cancelling a bounce no longer
  blocks the message thread or aborts on teardown.
- **Reopened sessions** no longer show leftover regions from a previously larger
  session.
- **Verbatim audio import.** Imported audio is copied byte-for-byte when its
  sample rate and channel count already match the session - no needless re-encode.
- **MIDI editor playhead** is smooth when zoomed in (fractional-tick positioning).
- **Tape-strip markers** no longer paint over the track-name column when the
  timeline is scrolled.
- **Tooltips** no longer cover the stage selector.
- **Linux app icon.** The desktop entry's StartupWMClass now matches the window
  class, so the dock / taskbar shows the Dusk Studio icon instead of a generic one.

## [0.11.0] - 2026-06-24

Built to a production-grade bar; shipped as a Beta. 1.0.0 is reserved for
the public stable declaration.

### Added

- **MP3 bounce / export.** Bounces and the mastering *Export master* can write
  320 kbps MP3 (CBR via libmp3lame) — name the output `.mp3` instead of `.wav`.
  WAV (stereo 24-bit) stays the default, and stems always stay WAV so they keep
  sample-accurate alignment for re-import. libmp3lame is auto-detected at build
  time; absent, the option is hidden and bounces stay WAV-only.
- **Import DP Song (experimental).** Reads a raw TASCAM DP-24 / DP-24SD / DP-32
  song folder off the SD card and reconstructs the session — each recorded
  fragment onto its own track with the right rate / bit-depth / stereo pairing,
  a confirmation dialog of what was found, and (where decodable from `song.sys`
  / a master mixdown) recovered clip positions, mixer fader/pan/EQ, tempo and
  markers. Marked experimental: parts of the format are reverse-engineered.
- **Plugin-scan validation.** A scan now drops dead entries — an empty / hollow
  `.vst3` bundle or an LV2 whose bundle was uninstalled — so the picker never
  offers a plugin that can't load. Filesystem + live-search-path checks only; no
  plugin is instantiated.
- **Dusk-native audio device selector.** Replaces JUCE's stock device-selector
  with a native backend / output / input / sample-rate / buffer-size picker that
  surfaces device-open errors as in-window alerts (no native popup), and opens
  every device channel so the main-output pair menu finds all active pairs.
- **Multicore DSP setting.** The Audio Settings panel exposes an in-app control
  for parallel strip DSP — Auto (uses spare cores), Off (serial), or a pinned
  worker count — persisted per machine. (`DUSKSTUDIO_AUDIO_WORKERS` still
  overrides it for CI / power users.)
- **Hardware-insert ping while stopped.** The insert chirp test runs with the
  transport stopped and IN off, sending the tone only to the insert's device
  output pair while master / bus / aux stay silent.
- **Transport tempo display.** The transport bar shows the tempo at the playhead
  (tempo-map aware) and edits it through the same undoable prompt as the ruler.
- **Marker rename on creation.** Adding a marker opens a rename prompt immediately.
- **Supporters credits panel.** A "Special Thanks" panel lists the project's
  Patreon / GitHub Sponsors supporters (built only when the credits header is
  present).
- **Mastering-EQ spectrum analyzer.** A real-time FFT overlay on the mastering
  EQ draws the post-EQ spectrum behind the response curve; toggle it with the
  FFT button in the panel's top-left.
- **Mastering-section visual pass.** Higher-contrast knobs (value-arc rings),
  raised-panel / recessed-well depth tiers, idle meter-well scales, a stronger
  loudness cluster, and a waveform with a time ruler, played/unplayed colouring,
  and a clearer playhead. The mastering-EQ low band now defaults to 50 Hz.
- **Optional out-of-process plugin sandbox.** Third-party plugins can run
  in a sandboxed child process on all three OSes (launch with
  `DUSKSTUDIO_USE_OOP_PLUGINS=1`) so a crashing plugin doesn't take the
  session down. In-process remains the default — the cross-process editor
  path added UI latency. Plugin scanning is always sandboxed, and plugin
  instantiation moved off-thread, so loading a heavy synth doesn't freeze
  the UI.
- **Automatic cross-track plugin delay compensation (PDC).** Tracks with
  latency-reporting inserts stay sample-aligned with the rest of the mix.
- **Undo, broadly.** Piano-roll note edits, automation breakpoint edits,
  tape-strip menu edits, tempo-map edits, and track / bus / aux rename +
  colour changes are all undoable.
- **Tempo editing from the ruler.** Right-click the bar ruler to add / edit
  tempo points, drag markers left/right to move them, wider hit zones, and
  the bar-1 anchor stays protected. The separate Grid edit mode is gone.
- **Timeline interaction pass.** Left-click moves the playhead (snapped to
  grid when Snap is on), drag on the ruler sets loop / punch ranges, and the
  Snap on/off + grid-resolution control is back in the tape-strip header.
- **MIDI Learn for per-track EQ frequency + Q** (band gain was already
  mappable).
- **Arrow-key focus across the 24 channel strips**, plus stage / bank
  keyboard shortcuts, tooltips, and an in-app shortcut reference.
- **Song-section display.** The transport shows the current section name
  (from markers) next to the clock.
- **Double-click the DSP readout to reset the xrun counters** after fixing
  whatever caused a dropout.
- **Update notice.** The startup dialog checks for a newer release and
  shows a flashing UPDATE badge in its sidebar when one exists; silent
  when up to date or offline.
- **Multi-output routing (Tascam-style cue/monitor sends).** The audio device
  can open more than two outputs (Audio settings). Each AUX return lane can be
  routed to its own physical output pair for an independent headphone / cue mix,
  fed by the existing per-channel pre-fader sends; the tap is taken before the
  return fader and mute, so those still govern only the fold into the master.
- **Selectable main output pair.** The master mix can be sent to any open output
  pair (Audio settings → Main output), freeing 1-2 for a control-room or cue
  feed. An aux routed to the master's pair sums in.
- **DP-24-style multiband compressor presets.** Nine genre starting points
  (Basic CD, Pop, Pop Rock 1/2, Rock 1/2, Classic, Dance, R&B Hip Hop) in the
  mastering comp header, mapping the three-band chart onto the multiband comp
  with the high-mid band disabled.
- **Piecewise tempo map (Grid edit mode).** Songs can change tempo over time:
  add / drag tempo points on the bar ruler in Grid mode; the bar ruler, grid
  snap, region-editor ruler, BPM control, piano roll, MIDI scheduler and
  metronome all follow the map.
- **Limiter Mode + Stereo-link.** Mastering limiter gains a Mode selector
  (Modern / Transparent / Punchy) shaping hold + release, and a Stereo-link
  toggle (linked GR vs per-channel).
- **Punch pre-roll / post-roll enable toggles** — arm playback before the
  in-point and auto-stop after the out-point.
- **Open a session from the command line / file manager.** Positional session
  path on launch and a running-instance hand-off, with the `.desktop` MIME
  association restored.
- **Portable Linux tarball + installer** replacing the AppImage: a
  run-in-place program directory plus an `install.sh` that registers the
  launcher / icon / MIME (the Reaper model).
- **Raspberry Pi (arm64) build-check CI.**

### Changed

- **Sessions are portable.** Audio file paths are stored relative to the
  session folder; loading re-roots stale absolute paths and lists any file
  it genuinely can't find instead of silently playing silence. Renaming the
  session folder or moving it between machines now just works.
- **Disengaged EQ / comp / tape sections skip their DSP** with click-free
  toggles — lighter CPU on mostly-clean mixes.
- **Take history capped at 8 per region** (was 20).
- **Stage tabs reordered** to RECORDING | MIXING | MASTERING | AUX.
- **Import... renamed to Import Audio or MIDI...**
- **Mastering limiter rebuilt as a true-peak brickwall limiter.** 4×
  oversampled lookahead limiting with a hard inter-sample-peak ceiling,
  monotonic-min gain envelope, and hold + smooth release — replaces the
  previous sample-peak design that pumped like a compressor.
- **Limiter "Threshold" control now makes sense.** Pulling the handle down
  drives more signal into the ceiling (louder, denser) instead of only
  attenuating.
- **Mastering layout rebalanced.** The starved limiter panel gets more width,
  the loudness readouts (TP / M / S / I) group into one cluster, and Export
  master moves up to the header beside Load latest mixdown.
- **Tape-strip track-label column auto-sizes** to the longest track name
  instead of clipping at a fixed width.
- Release CI (Linux / macOS / Windows) gates publishing on the test suite;
  version metadata corrected (24-channel, 0.11.0).

### Fixed

- **DP import offset-recovery works off macOS.** The fragment-to-mixdown onset
  aligner ran its FFT in place, which silently produced garbage on the non-vDSP
  backend used on Linux / Windows — so every fragment came back unplaced. Uses
  distinct in/out buffers now.
- **Shutdown / lifecycle.** The audio engine now removes its MIDI-input callback
  on teardown (a shutdown use-after-free if MIDI arrived mid-close); seven UI
  components stop their refresh timer in their destructor before members tear
  down.
- **Unsaved-changes prompt on more paths.** Open, Open Recent and New-from-
  Template now warn before discarding unsaved work, matching New Session.
- **Edit cursors on Linux/Wayland.** The piano-roll Grab hand and the tape-strip
  scissors now render reliably (the previous hide path failed on some window
  managers); the piano-roll "Key snap" toggle now actually snaps placed/dragged
  notes to the scale; the tape-strip Cut shows a dashed cut-preview line.
- **Session-load validation.** Out-of-range transport / master fields in a
  hand-edited or corrupt `session.json` (zero tempo / beats-per-bar, NaN gains)
  are clamped on load instead of dividing by zero or propagating NaN.
- **MP3 export edges.** A failed writer no longer leaves a 0-byte file behind;
  a typed `.mp3` falls back to WAV on a build without libmp3lame.
- **Metronome count-in clicks land on the beat.** Count-in (negative-playhead)
  pre-roll no longer mis-times its clicks crossing the bar line.
- **Tempo entry works on comma-decimal locales.** The BPM / tempo fields parse
  "127.6" the same on French / German systems as on English ones, and reject
  partial junk ("123abc") instead of silently taking the numeric prefix.
- **Data-integrity hardening.** Session save is now genuinely atomic (a
  crash mid-save can no longer lose both the old and new file); recovering
  from an autosave immediately persists the recovered state instead of
  silently discarding it on quit; a fast stop + re-record within the same
  second can no longer overwrite the previous take's audio.
- **Crash fixes.** Stopping the transport during dense playback could
  free disk readers under the audio callback; quitting with the audio
  settings, plugin scan, or a bounce in progress could tear down dialogs
  after the engine was gone. Both classes closed.
- **Real-time safety.** MIDI clock / MTC and per-track MIDI output are
  delivered through a dedicated thread instead of locking inside the audio
  callback (periodic xruns while rolling with MIDI out engaged); the
  Mastering player no longer reads from disk on the audio thread.
- **Send-effect tails.** Aux reverb / delay tails are no longer cut off
  the moment the dry signal stops.
- **Loading an older session over a newer one** no longer inherits the
  previous session's hardware-insert enable, automation mode, or markers.
- **The chosen audio device setup persists across restarts.**
- **Changing the oversampling factor actually re-prepares the DSP** without
  a full device restart.
- **Declining to save on quit discards the autosave**, so the next launch
  doesn't offer to "recover" the work you chose to drop.
- **Smoother playhead band repaint** (driven by the display vblank).
- **Windows MSI creates Start Menu + desktop shortcuts.**
- **AUX tab no longer flashes plugin-editor windows when opened.**
- **Region editor's Draw tool paints automation without moving the region**,
  and edit-tool cursor glyphs match the OS cursor size.
- **Non-fatal X11 errors raised by plugin editors are swallowed** instead
  of terminating the app (Linux).
- **Mastering comp display.** Gain-reduction meters are now live during
  playback, and applying a preset refreshes the band knobs / crossovers (the
  embedded panel previously only synced on a manual band-click).
- **Audio settings layout** no longer overlaps the Main-output row with the
  sample-rate / buffer controls when many outputs are enabled.
- **Packaging scripts hardened**: gated the destructive `rm -rf` install dir,
  empty-VERSION and missing-source guards, and a special-character-safe rewrite
  of the desktop `Exec=` line.
- **Tempo-map real-time safety**: tempo-map edits during playback are now
  thread-safe and can't corrupt playback, and the bar-1 anchor is protected
  from deletion.
- **File import** retries transient file-lock opens (Windows).

## [0.10.0-beta.1] - 2026-05-29 — first beta

Architectural-audit sprint: every "Working" cell in the README's
Status table is now also reviewed for real-time safety, lifecycle
correctness, cross-platform CI coverage, and documentation parity.
24 commits since 0.9.0, 148 Catch2 tests green across Linux
(amd64 + arm64), macOS, and Windows.

### Added

- **MIDI binding targets (H3, Phase 5a + 5b)** — 10 new mappable
  destinations covering parts of the mixer that previously had no
  MIDI Learn surface:
  - Per-track toggles: EQ on/off, compressor on/off, hardware
    insert bypass
  - Bus EQ band gain (packed bus × band, 3 bands per bus)
  - Master Pultec EQ: low boost, high boost
  - Master bus compressor: threshold, makeup, ratio
  Bank-relative variants of every per-track target keep an 8-fader
  controller useful for the 24-track surface.
- **Accessibility (H4, Phase 4a)** — every channel-strip slider
  now has an accessibility title + domain-formatted value text.
  VoiceOver and Orca read e.g. *"Track 1 Fader, -4.2 dB"* on
  focus. Static text-input dialogs (rename region, rename marker,
  edit label) render inside the main window via the EmbeddedModal
  framework — no native popups that escape the screen reader's
  focus tree.
- **Hot-unplug detection (H5, Phase 4b)** — AudioEngine
  subscribes to AudioDeviceManager change broadcasts; an
  unexpected device disappearance force-stops the transport,
  surfaces a UI alert ("device disconnected"), and leaves the
  session intact in memory + on disk. Reconnect or pick a new
  device and playback resumes.
- **macOS in-process plugin editor + OOP DSP (C6, Phase 3c)** —
  dual-load shell pattern: parent process loads the plugin's
  editor in-process while the OOP child keeps running the DSP.
  Eliminates cross-process NSView reparenting (a research
  problem) and gives Mac users a working OOP plugin pipeline
  without the fragile CARemoteLayer dance. Bidirectional
  parameter mirror over IPC keeps the in-process editor and the
  OOP DSP instance in sync; an atomic loop-breaker flag prevents
  echo storms.
- **Linux arm64 build** — CI now matrices Linux on amd64 +
  arm64 (Raspberry Pi 4 / 5 territory). JUCE's NEON-side
  SIMDNativeOps<long long> alias is patched the same way the SSE
  side is.
- **ThreadSanitizer CI** — Linux sanitizer workflow runs the
  Catch2 suite under TSan on every PR + push. Standing JUCE-
  internal patterns (WaitableEvent / TimeSliceThread /
  AbstractFifo) are suppressed via `tools/tsan_suppressions.txt`;
  Dusk Studio code stays fully instrumented.
- **Windows tests workflow** — Catch2 suite now builds + runs on
  Windows MSVC every push. The IPC-stub round-trip test is
  temporarily hidden on Windows pending a named-pipe timing
  investigation.
- **Schema versioning + migration** — `SessionSerializer`
  format-version is now 2 (was 1). Loaders read v1 + migrate
  forward; tests pin both the safety branch (unknown lower
  version refused) and the v1 → v2 advance.
- **Hash-based autosave dedup (M8)** — autosave timer compares a
  32-bit hash of the volatile-state-stripped JSON instead of a
  full juce::String equality scan. Memory + CPU win on 24-track
  sessions.
- **Patreon + GitHub Sponsors pricing tiers** documented in
  README + MANUAL: $3/$5/$10 Patreon recurring, $27 one-time
  (current major), $49 lifetime (current + next major).
- **First-time-launch walkthrough** in MANUAL for unsigned
  binaries — per-OS macOS Gatekeeper + Windows SmartScreen
  bypass steps.
- **Windows ASIO-first device selection** — the AudioEngine
  pre-registers Windows backends in preference order (ASIO →
  WASAPI exclusive → WASAPI shared → DirectSound), so the default
  device lands on the lowest-latency backend that actually has
  devices. A machine with no ASIO driver falls through to WASAPI
  exclusive instead of an empty device list — onboard-audio users
  still get a usable low-latency default.
- **Out-of-process plugin scanning** — third-party plugin
  discovery (VST3 / LV2 / AU) runs in the `dusk-studio-plugin-host`
  child via a new `--scan` mode, so a plugin that crashes or hangs
  during discovery takes down only the child. The parent times the
  scan out (30 s), kills a hung child, and blacklists the file, so
  a single bad plugin can no longer crash the app on first scan.
  Backed by a dead-man's-pedal that quarantines a culprit if the
  app itself dies mid-scan. Falls back to in-process scanning when
  the host binary is absent.

### Fixed

- **Audio-thread parameter writes (C1, Phase 2)** —
  `PluginSlot::setParamNormalised` no longer calls
  `param->setValue` directly from the audio thread. Real plugins
  observed in the wild (Diva, Massive X, Spitfire BBC SO) take
  std::mutex inside their parameter-change paths. The audio
  thread now pushes into a 256-deep SPSC FIFO; a 30 Hz
  juce::Timer on PluginSlot drains the queue and calls
  setValueNotifyingHost on the message thread. Eliminates a
  long-standing class of CPU-spike-under-MIDI bug.
- **Atomic memory-ordering pairs (C2)** — `syncOutputIdx` audio-
  thread reader switched from relaxed to acquire so it pairs
  cleanly with the message-thread release stores in
  rebuildMidiOutputBank + AudioSettingsPanel.
- **RecordManager UAF on forced teardown (C4 + C5, Phase 1)** —
  `stopRecording` now bails the writer / midiCapture teardown
  when the bounded audioInFlight drain exceeds 1000 yields; the
  next startRecording gates on audioInFlight == 0 to avoid
  overwriting an in-flight slot. Closes a stuck-audio-thread
  UAF.
- **PluginSlot lifetime races (Phase 4c)** — five findings
  closed: ParamWrite epoch-guarded for plugin replace, OOP
  fallback in applyParamWriteOnMessageThread, dtor leak path
  when shell wrapper is outstanding, stale shellInstance on
  plugin swap, ChildParamListener atomic per-param state, shared
  SinkState lifetime for callAsync lambdas. Plus
  ChannelStripComponent shellEditor leak on factory failure.
- **macOS Authenticode helper, Windows SmartScreen bypass docs**
  match shipping behaviour (binaries are unsigned).
- **CI cross-platform issues** — macOS missing JUCE include,
  arm64 GCC `juce::String += unsigned int` ambiguity, MSVC
  M_PI undeclared (Catch2 transitive `<cmath>`), MSVC strict
  lambda capture for constexpr locals, donor-version-dependent
  test assertions hidden via Catch2 `[.]` tag.
- **Gain-reduction meter on silent tracks** — the per-track
  silent-skip fast path now zeroes the GR meter atom on its early
  return, so a silent track reads 0 dB reduction instead of
  holding the last computed value (matches the comp-bypass path).
- **ALSA RT priority with unlimited RLIMIT_RTPRIO** —
  `RLIM_INFINITY` previously narrowed to -1 under a signed cast and
  selected the *lowest* SCHED_RR priority (the opposite of intent);
  it is now treated as the top priority, with unsigned comparison
  otherwise, and logged as "infinity"/"unknown" rather than a
  narrowed int.
- **Plugin editors hidden under modals (all platforms)** — the
  macOS shell-editor and Windows foreign-HWND editor wrappers are
  now tagged so a settings / quit / bounce modal hides them.
  EmbeddedModal's hide is nesting-safe via a per-editor token, so
  closing modals out of order no longer re-shows an editor still
  covered by another modal.
- **Global shortcuts while a modal is open** — Home
  (return-to-zero), `.` (stop + rewind) and F11 (fullscreen) now
  reach the transport from a focused modal or call-out popup,
  alongside the existing Space / R. Destructive edit keys (Delete,
  clipboard, split, nudge) remain gated so they can't act on the
  arrangement hidden behind the modal.

### Changed

- **MANUAL audited** for every user-visible Phase 4 + 5 change.
  New sections: Installing Dusk Studio (per-OS first-launch
  walkthrough), Audio device disconnected mid-session
  (Troubleshooting), Accessibility.
- **README rewritten** with tier table, first-launch link,
  GitHub Issues bug-reporting line. Status-table cleanup
  (mac-OOP-editor now "Working" via the in-process shell;
  notarised DMG still deferred).

### Build & packaging

- **Windows MSI codesigning** (H10, Phase 6) — `scripts/
  package-windows.ps1` learned `-SigningPfxPath` + `-Signing
  PfxPassword` params (alongside the legacy thumbprint path).
  Signs every produced `.exe` BEFORE cpack assembles the MSI so
  the embedded binaries are signed inside the installer payload,
  then signs the MSI itself. windows-build.yml decodes a
  SIGNING_CERT_BASE64 secret into a temp file under
  $env:RUNNER_TEMP, runs the script with the PFX + password,
  zero-overwrites + deletes the temp file on completion
  (success OR failure). Default timestamp authority switched to
  digicert. NOTE: project policy as of v0.10 is to ship
  unsigned — signing infrastructure exists in CI but does not
  fire until secrets are configured.
- **Linux arm64 matrix** added to linux-build.yml. Tests still
  amd64-only to keep CI runtime reasonable.
- **Windows ASIO required for release builds** —
  `DUSKSTUDIO_REQUIRE_ASIO` turns a missing ASIO SDK into a hard
  configure error for release-class builds (single-config
  CMAKE_BUILD_TYPE = Release/RelWithDebInfo/MinSizeRel, and the
  Visual Studio multi-config generator whenever a release config is
  buildable), so a shipping Windows binary can't silently fall back
  to WASAPI-only latency. SDK auto-discovered at `../asiosdk` /
  `$ASIOSDK_PATH` / `-DASIOSDK_PATH=`; override with
  `-DDUSKSTUDIO_REQUIRE_ASIO=OFF` for a deliberate ASIO-less dev
  build (warns + falls back to WASAPI).
- **Plugin-scan wire protocol** extracted to a shared header
  (`PluginScanProtocol.h`) consumed by both the host child and the
  parent, with 5 Catch2 round-trip / crash-path tests.

## [0.9.0] - 2026-05-21 — beta, road to 1.0

### Added
- Phase 4 MIDI tempo semantics
    - `MidiRegion.tempoLock` + `MidiRegion.recordedAtBPM` fields
    - `applyTempoChange(Session&, newBpm, sampleRate)` retimes
      tempo-locked MIDI regions on BPM change; float regions keep
      sample positions and rebuild musical length
    - BPM-change confirmation alert that summarises retime impact
      before applying (skips when no MIDI / no automation)
    - FileImporter anchors imported MIDI regions to the session's
      current BPM so subsequent retime is correct
- `FadeShape::RaisedCosine` + 64-sample raised-cosine punch
  crossfade — replaces the previous 10 ms linear fade for
  click-mask boundaries
- Piano roll: triplet quantize variants (1/8T, 1/16T, 1/32T)
- Transport: `.` (period) stops + rewinds playhead to 0
- `docs/KEYBINDINGS.md` — complete keyboard reference

### Build & packaging
- Version sourced from top-level `VERSION` file
- `scripts/bump-version.sh` writes version + AppStream `<release>`
- CPack generators for `deb`, `rpm`, `WIX`, `DragNDrop`
- `scripts/package-appimage.sh` — Linux AppImage one-shot
- `scripts/package-linux.sh deb|rpm` — distro packages
- `scripts/package-windows.ps1` — MSI via WIX, optional signtool
- `scripts/package-macos.sh` — signed + notarized DMG
- `packaging/macos/entitlements.plist` — Hardened Runtime config
  for OOP plugin host

### Fixed
- Audio thread reads `tempoBpm` with `memory_order_acquire` on the
  MIDI scheduling path; pairs with `applyTempoChange`'s release
  store so a new BPM observation implies new region positions

### Tests
- `tests/session_midi_tempolock_roundtrip.cpp` — serializer
  round-trip + legacy session migration anchor
- `tests/session_apply_tempo_change.cpp` — 120↔60 retime, round
  trip 120-96-120, clamp 30..300
- `tests/transport_state_machine.cpp` — defaults, state
  transitions, playhead arithmetic, loop/punch round-trip
- `FadeShape::RaisedCosine` zero-slope-at-endpoints + 0.5-at-midpoint

### Deferred to post-1.0
- Piano roll: keyboard-strip preview audition (needs RT-safe MIDI
  inject FIFO)
- Piano roll: explicit Overdub toggle (recording already
  history-stacks; toggle is UX)
- macOS cross-process NSView plugin editor embedding (Mac plugin
  windows float as separate native windows; spec-acknowledged
  limitation)

## Pre-0.9 — captured from git log

See `git log` for the full history. Phase milestones:

- **Phase 1a** (live mixer): 16 strips, 4 aux, master Pultec + bus
  comp + tape sat
- **Phase 1b** (send-bus plugin hosting): aux plugin slots, OOP
  scaffolding
- **Phase 2** (recording + session): multitrack, atomic JSON
  save/load, autosave, transport
- **Phase 3** (arrangement + automation): markers, region edits,
  take history, fader/pan/aux/mute automation (Off/Read/Write/Touch),
  bounce/export, undo, **OOP plugin audio path on all 3 OSes**,
  console automation parity (aux + master)
- **MTC** (Phases 1-3): MIDI Time Code receiver + emitter +
  transport chase
- **Rename**: Focal → Dusk Studio (5 phases across identifiers,
  packaging, CMake, namespaces)

---

[0.9.0]: https://github.com/dusk-audio/dusk-studio/releases/tag/v0.9.0
