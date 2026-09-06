# Release checklist coverage

What automates each item of the pre-release checklist, and what still needs a
person. Buckets: **unit** (Catch2, every ctest run), **scenario** (in-app
suite, `DUSKSTUDIO_RUN_SCENARIOS`), **bb** (black-box leg in
`scripts/regress/scenarios.sh`), **windows** / **mac** (a phase of that
platform's runner), **manual** (with the reason). Run everything with
`scripts/regress.sh all --gui-scenarios --msi <installer>`.

## Launch, sessions, single instance

| # | Item | Bucket | Covered by |
|---|---|---|---|
| 1 | Opening a session hands off to the running window | bb | `bb-handoff` |
| 2 | Windows: the handed-off window comes to the foreground | windows | `phase2-handoff` (`GetForegroundWindow` check) |
| 3 | Two launches after a crash leave exactly one primary | bb | `bb-crash-relaunch` (run ten times before trusting a change to the handoff) |
| 4 | An unusable runtime dir is reported, app still starts | bb | `bb-no-runtime-dir`; unit twin in `single_instance_socket_lifecycle.cpp` |
| 5 | An interrupted handoff frame is discarded | unit | `single_instance_socket_lifecycle.cpp` |
| 6 | Windows: foreign pipe owner / second desktop | unit | `single_instance_windows_contract.cpp` |
| 7 | Windows: non-ASCII session paths | manual | the VM channel types through a US keymap and cannot produce one; the failure lives in the shell/Explorer association layer |
| 8 | A damaged recent session does not take the picker down | bb | `bb-damaged-recent` |
| 9 | Windows: quit twice is clean | windows | `phase3-session-close` (two `WM_CLOSE`, `re-entry ignored` required) |
| 10 | macOS: quit twice is clean | manual | the node cannot open a window over ssh; `bb-quit-twice` proves the latch on Linux |

## Plug-in state and inserts

| # | Item | Bucket | Covered by |
|---|---|---|---|
| 11 | Unreadable saved state is preserved and reported | scenario | `clap.state_roundtrip` |
| 12 | An offline insert adds no PDC | scenario | `plugin.offline_insert_no_pdc` |
| 13 | Clone Track keeps native plug-in state | scenario | `session.clone_track_keeps_native_state` (CLAP, LV2, VST3; undo/redo) |
| 14 | VST3 bus activation | unit | `vst3_instance_process.cpp`, `vst3_native_slot.cpp` |
| 15 | Cancel stops an LV2 scan | scenario | `scan.cancel_lv2` |
| 16 | Cancel stops an AU scan | scenario | `scan.cancel_au` (runs on the mac leg, skips elsewhere) |
| 17 | LV2 editor shows the plug-in's current settings | manual | `gui.lv2_editor_reflects_state` exists but skips: no fixture ships an LV2 UI. Check with a real LV2 plug-in that has one |
| 18 | LV2 parameter numbering is stable across reloads | scenario | `lv2.params_numbering_stable` |
| 19 | LV2 file state lives inside the session folder | scenario + unit | `lv2.file_state_inside_session`; `lv2_file_state.cpp`, `lv2_state_paths.cpp` |
| 20 | A large LV2 file store saves quickly | scenario | `lv2.large_store_timing_bound` (tag `slow`) |
| 21 | A CLAP editor that never shows a window says so | scenario (gui) | `gui.clap_no_window_message` |
| 22 | A failed aux editor attach logs, never dialogs | scenario (gui) + bb | `gui.aux_attach_failure_no_modal`; the `[Dusk Studio/native editor]` line is pinned by `stderr_marker_contract.cpp` |
| 23 | Closing an editor on Linux cannot crash | scenario (gui) | `gui.editor_open_close_loop` |
| 24 | Muted empty MIDI tracks skip the chain | scenario + unit | `engine.muted_midi_track_skips_chain`; `channel_strip_midi_gate.cpp` |

## MIDI and transport

| # | Item | Bucket | Covered by |
|---|---|---|---|
| 25 | Panic reaches every path (open, muted, soloed-out) | scenario | `midi.panic_all_paths` |
| 26 | CLAP-only voices are choked on stop, loop wrap, jump | scenario | `clap.choke_on_stop_loop_jump` |
| 27 | Irregular SMF files import | scenario + unit | `import.smf_irregular`; `midi_file_reader.cpp`, `file_importer_midi.cpp` |
| 28 | A dense MIDI output block keeps every event | scenario + unit | `midi.out_dense_block`; `generated_midi_budget.cpp`, `midi_fifo_overflow.cpp` |
| 29 | Callback add/remove never stalls | unit | `midi_device_layer.cpp` under the TSan job |

## Sandboxed plug-ins

| # | Item | Bucket | Covered by |
|---|---|---|---|
| 30 | A sandboxed load does not freeze the UI | scenario | `oop.load_does_not_block_message_thread` |
| 31 | Quit during a load exits fast | scenario + bb | `oop.quit_during_load_bounded`; `bb-oop-quit-during-load` |
| 32 | A killed child drops out fast | scenario + bb | `oop.killed_child_bypasses`; `bb-oop-child-kill` |
| 33 | Rapid plug-in switching is safe | scenario | `oop.rapid_switching_safe` (tag `slow`) |
| 34 | The editor closes before the plug-in | manual | `gui.oop_editor_closes_before_child` skips until #508 is understood (the child exits on its own when sandboxing is enabled at runtime) |
| 35 | Windows: editor responsiveness and alignment | manual | visual only |
| 36 | macOS: editor responsiveness and alignment | manual | visual only, and no console session on the node |
| 37 | Editor failures do not strand the window | scenario (gui) | `gui.oop_editor_failure_no_strand` |
| 38 | macOS: one editor window | manual | NSWindow count needs a console session |
| 39 | macOS: SHM name collision | unit | `ipc_mac_backend_contract.cpp` |
| 40 | macOS: first-launch stall | manual | Gatekeeper timing on a fresh binary; not reproducible over ssh |
| 41 | Windows: console diagnostics, exit once | windows | phase 2 and 3 stderr assertions |
| 42 | Windows: pipe and poison handling | unit | `ipc_windows_backend_contract.cpp` |

## Release artifacts

| # | Item | Bucket | Covered by |
|---|---|---|---|
| 43 | The DMG carries the sandbox helper | unit | `release.yml` asset check, `scripts/verify-release-assets.sh` |
| 44 | Donor and sfizz audio unchanged | unit | the `*_ab.cpp` null tests |

Manual items: 7, 10, 17, 34, 35, 36, 38, 40. Every other row runs on every
`scripts/regress.sh` pass, and the headless scenarios also run in CI.
