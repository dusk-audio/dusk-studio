# Take folders: implementation mapping (draft v1, 2026-09-15)

Read-only mapping of `docs/take-folders-plan.md` onto the codebase. Target: 1.1.

**Superseded ordering:** the phase table in §8 builds all model/engine layers for
audio and MIDI before any UI. The approved order is audio end to end first, then
the audio comping editor, Flatten, then MIDI folders + Merge + piano roll. Phases
0A/0B are dropped unless a concrete need is shown. Re-cut the phase table before
implementation; the architecture, data model, risks and reuse points still apply.

The decisions in §5.1, §10 and §11 of the spec are treated as binding, including the MIDI Merge behavior.

## 1. Current architecture

- Authoritative audio regions are `Track::regions`; MIDI regions use `AtomicSnapshot<std::vector<MidiRegion>>` because that collection currently doubles as editable state and the RT playback hand-off (`src/session/Session.h:800-852`).
- Flat take history is payload rotation:
  - `TakeRef` stores only file/source offset/length/provenance, not independent placement, channels, gain, fades, name, or colour (`src/session/Session.h:317-335`).
  - `MidiTakeRef` stores ticks, notes, CCs, and provenance (`src/session/Session.h:636-655`).
  - Swap helpers replace only those payload fields (`src/session/Session.h:673-693`, `:777-797`).
- Audio playback is prepared off the audio thread. `PlaybackEngine::preparePlayback()` opens readers, allocates streams/caches, copies region metadata, sorts, calculates overlaps, then publishes availability with release ordering (`src/engine/PlaybackEngine.cpp:88-219`). `readForTrack()` reads only prepared streams and pre-sized scratch without allocation or locks (`src/engine/PlaybackEngine.cpp:301-495`).
- MIDI playback instead acquire-loads `track.midiRegions` directly once per callback block (`src/engine/AudioEngine.cpp:5483-5489`). This must be separated from editable MIDI-folder state.
- Recording allocates writers/FIFOs and publishes fixed capture masks before starting (`src/engine/RecordManager.cpp:256-398`). The callback pushes audio to the threaded writer and MIDI POD events to the pre-sized FIFO (`src/engine/RecordManager.cpp:1257-1347`, `:1358-1432`). `stopRecording()` clears capture state, drains in-flight callbacks, then performs all model commits on the message thread (`src/engine/RecordManager.cpp:481-560`).
- Current audio overdub handling absorbs fully covered regions, but destructively splits/trims partial overlaps—the #594 path (`src/engine/RecordManager.cpp:1042-1205`). Take history and loop descriptors are capped at eight previous/nine total passes (`src/engine/RecordManager.cpp:14-27`, `src/engine/RecordManager.h:157-176`).
- Current MIDI loop capture already tags raw events with `passOrdinal`, reconstructs separate pass regions, and closes held notes at seams (`src/engine/RecordManager.cpp:612-779`, `:1341-1346`). It then turns passes into `previousTakes`; normal MIDI overdub absorbs only fully contained regions (`src/engine/RecordManager.cpp:793-844`, `:850-965`). This is close to MIDI Merge capture-wise, but not commit-wise.
- Recording undo uses full before/after region snapshots. `RecordCommitAction::perform()` is a no-op on first registration because the recorder already applied the change; later perform is redo (`src/session/RegionEditActions.h:313-343`, `src/session/RegionEditActions.cpp:1404-1444`). `AudioEngine::stop()` currently registers one `"Record"` transaction for the entire gesture (`src/engine/AudioEngine.cpp:1531-1563`).
- `TapeStrip` paints and hit-tests indexed regions directly (`src/ui/TapeStrip.cpp:590-665`, `:3168-3400`). Its take badge rotates `previousTakes` (`src/ui/TapeStrip.cpp:1224-1249`), and its menus do the same (`src/ui/TapeStrip.cpp:2686-2732`).
- Double-click chooses the newest overlapping indexed audio or MIDI region (`src/ui/TapeStrip.cpp:2120-2160`). `AudioRegionEditor` and `PianoRollComponent` both hold mutable `(trackIdx, regionIdx)` targets (`src/ui/AudioRegionEditor.h:14-34`, `src/ui/PianoRollComponent.cpp:466-510`).
- The audio editor is not an `EmbeddedModal`: `MainComponent` directly owns the editor and dim overlay, handles focus, and animates from the timeline rectangle (`src/ui/MainComponent.h:466-473`, `src/ui/MainComponent.cpp:5760-5921`). `EmbeddedModal` is used by in-window menus and other dialogs.

## 2. Data model

Use session-wide, nonzero 64-bit IDs for folders, takes, and comps. IDs are stable across edits and undo; copy/paste and split allocate fresh IDs. On load, scan all IDs and seed the next allocator above the maximum.

The types should live beside the region model in `src/session/Session.h`, but use `std::string`, `std::filesystem::path`, integer ARGB colours, and existing foundation types so the change adds no JUCE coupling.

```cpp
using TakeFolderId = std::uint64_t;
using TakeId       = std::uint64_t;
using TakeCompId   = std::uint64_t;

struct AudioCompSelection {
    TakeId takeId;
    std::int64_t startSample, endSample;
};

struct MidiCompSelection {
    TakeId takeId;
    std::int64_t start, end;  // interpreted using MidiTakeFolder::timeBase
};

enum class MidiFolderTimeBase { MusicalTicks, AbsoluteSamples };
```

Shared folder fields:

```cpp
struct TakeFolderHeader {
    TakeFolderId id;
    std::uint64_t creationOrder;
    std::int64_t timelineStartSamples;
    std::int64_t lengthInSamples;
    bool spanWasUserResized;
    std::string label;
    std::uint32_t colourArgb;
    bool muted, locked;
    TakeCompId activeCompId;
    bool quickSwipeEnabled;
    std::uint64_t revision;   // bumped by every mutation of the folder, its takes or comps
};
```

Audio-specific types:

```cpp
struct AudioTake {
    TakeId id;
    std::uint64_t recordingOrder;
    std::filesystem::path file;
    std::int64_t timelineStartSamples;
    std::int64_t lengthInSamples;
    std::int64_t sourceOffset;
    int numChannels;
    float gainDb;
    std::int64_t fadeInSamples, fadeOutSamples;
    FadeShape fadeInShape, fadeOutShape;
    bool fadeInAuto, fadeOutAuto;
    std::string name;
    std::uint32_t colourArgb;
    TakeProvenance provenance;
};

struct AudioTakeComp {
    TakeCompId id;
    std::string name;
    std::vector<AudioCompSelection> selections;
};

struct AudioTakeFolder {
    TakeFolderHeader header;
    std::vector<AudioTake> takes;
    std::vector<AudioTakeComp> comps;

    float outputGainDb;
    std::int64_t outputFadeInSamples, outputFadeOutSamples;
    FadeShape outputFadeInShape, outputFadeOutShape;
    bool outputFadeInAuto, outputFadeOutAuto;
};
```

MIDI must remain separate because it has musical time, note-tail rules, no fades, and event payloads:

```cpp
struct MidiTake {
    TakeId id;
    std::uint64_t recordingOrder;
    std::int64_t timelineStartSamples, lengthInSamples;
    std::int64_t timelineStartTicks, lengthInTicks;
    bool tempoLock;
    double recordedAtBpm;
    std::vector<MidiNote> notes;
    std::vector<MidiCc> ccs;
    std::vector<MidiDiscreteEvent> otherEvents;
    std::string name;
    std::uint32_t colourArgb;
    TakeProvenance provenance;
};

struct MidiTakeComp {
    TakeCompId id;
    std::string name;
    std::vector<MidiCompSelection> selections;
};

struct MidiTakeFolder {
    TakeFolderHeader header;
    MidiFolderTimeBase timeBase;
    std::int64_t timelineStartTicks, lengthInTicks;
    bool tempoLock;
    double recordedAtBpm;
    std::vector<MidiTake> takes;
    std::vector<MidiTakeComp> comps;
};
```

`MidiDiscreteEvent` is necessary because the recorder accepts pitch bend, pressure, and program messages but currently drops them during commit (`src/engine/RecordManager.cpp:1280-1291`, `:904-907`). It should store kind/status, channel, data bytes, stable insertion order, and its position in both units: the tick in the folder's time base and the absolute sample at which it was recorded. Tempo-locked folders play, merge and persist by tick; floating folders by sample. When a merge converts a folder between modes, each event's position is recomputed from the unit that was canonical before the change through the tempo map, so the change of `timeBase` never moves an event.

`Track` gains:

```cpp
std::vector<AudioTakeFolder> audioTakeFolders;
std::vector<MidiTakeFolder> midiTakeFolders;
AtomicSnapshot<std::vector<MidiRegion>> midiPlaybackRegions;
```

Keep `midiRegions` as the authoring collection initially to avoid a repository-wide rename; the audio thread stops reading it and reads `midiPlaybackRegions` instead.

Core invariants:

- A folder has at least one take and one comp; `activeCompId` always resolves.
- Selections are positive, sorted, half-open, non-overlapping, and reference an existing take.
- Adjacent selections of the same take are coalesced. Gaps are preserved as silence.
- Stored selections and complete takes are not clipped when the folder is resized; only playback/display clips them to the visible span.
- Take vectors are ordered oldest-to-newest by `recordingOrder`; lanes display reverse order.
- New recordings never modify an existing take’s extent or source range.
- Folder move translates folder, takes, and comp selections. Resize changes only the visible span.
- Deleting a take replaces only its selected intervals with the newest remaining take covering those intervals.
- Quick Swipe off enables take-local trim/slip/gain/fades. Folder gain/fades always apply once to final output.

### Playback derivation

Add JUCE-free operations in `src/session/TakeFolderOperations.{h,cpp}`.

For audio, materialize plain regions and active folder selections into ephemeral `PlaybackRegion` records on the message thread before `PlaybackEngine::preparePlayback()` opens readers. A playback record carries:

- clipped timeline/source range;
- take and folder gain;
- independent absolute envelope anchors for take fades and folder fades;
- explicit comp-boundary fade data.

This is more accurate than forcing every envelope into today’s single `AudioRegion` fade pair. `PlaybackEngine::readForTrack()` remains allocation- and lock-free.

At a take boundary, create the existing 64-sample raised-cosine overlap used by punch recording (`src/engine/RecordManager.cpp:1056-1064`). Prefer extending the outgoing take 64 samples into the incoming selection where both sources cover it; otherwise move/clamp the overlap to the available common coverage. A boundary against silence gets a one-sided 64-sample fade.

For MIDI, materialize plain MIDI regions plus one derived `MidiRegion` per active folder comp. Include a note when its absolute note-on belongs to the selection, but retain its complete duration. Include CC/other events according to their timestamp. Set the derived region’s scheduler extent through the latest selected note-off; otherwise the current scheduler can skip the region before that note-off (`src/engine/AudioEngine.cpp:5678-5681`). One region per folder also avoids multiplying the fixed per-block MIDI scheduling budget (`src/engine/AudioEngine.cpp:5305-5325`).

Both materializers run on the message thread. Comp/cardinality edits should initially require stopped transport: current structural edits are intentionally deferred until Stop+Play (`src/session/RegionEditActions.cpp:25-40`), and `AtomicSnapshot` retains only one prior generation. Live swipe audition would require an epoch/retirement hand-off, not rapid mutation of the current snapshots.

## 3. Behavior mapping, §1–§10

| Spec | Code changes and resolution |
|---|---|
| §1 Recording | Replace both commit algorithms in `RecordManager.cpp:612-1205` with shared folder operations. Finalize complete takes first; find all positively overlapping plain regions/folders; create, extend, or merge one folder; preserve every old take; overlay the new take only over its recorded range. Remove history/pass caps and all Pass-2 destructive trimming. Audio has no mode switch: overlap always creates/extends a folder. |
| §1.5 loop | Current raw MIDI pass ordinals and audio spool are reusable. Audio full-pass offsets can be derived from ordinal × capture length; arbitrary partial/failure cases need an RT-safe descriptor FIFO drained off-thread. Never silently evict old pass descriptors. MIDI Merge-off retains empty passes as empty takes; Merge-on empty passes are no-ops. FIFO/writer overflow must invalidate the affected pass and surface an error rather than commit a silently incomplete take. |
| §1.6 punch | Keep the punch-window capture gate, but commit the punched take as a complete take and update only the comp selection. Delete today’s outer-fragment construction at `RecordManager.cpp:1090-1192`. |
| §1.7 fades | Share one boundary-envelope routine among playback, Flatten, and Flatten-and-Merge. Test exact 64-sample raised-cosine behavior. |
| §2 Display | `TapeStrip` paints one folder bar over its visible span, colours it by active-comp selection, darkens gaps, adds subtle boundaries, and shows `T N` rather than the false current `T 1/N`. No timeline lanes. Double-click routes by stable item ID to folder mode. |
| §3 Comping | Pure operations normalize swipe replacement, whole-take selection, boundary movement, deselection-to-silence, comp creation/duplication/rename/delete/switch, and Quick Swipe state. One mouse gesture produces one before/after action on mouse-up. |
| §4 Menus | Branch existing `TapeStrip` and editor context-menu paths. A timeline comp may use several takes, so bare “Delete take” is ambiguous: make it a submenu listing take names/ordinals; in the editor it targets the selected lane. Next-take replaces the active comp with one whole-take selection instead of rotating payloads. |
| §5 Delete | Timeline Delete dispatches `DeleteTakeFolderAction`, fixing #595 by deleting/restoring the complete folder. Lane/menu Delete dispatches `DeleteTakeAction`; deleting the last take removes the folder. No action deletes source files. |
| §6 Editing | Folder-aware move/resize/split/copy/paste/duplicate actions operate on persisted folder state, never derived playback regions. Split duplicates complete take metadata into two newly identified folders and partitions comps only. Folder resize never trims stored takes/selections. A recording extending beyond a manually resized edge should expand that edge enough to expose the new recording; otherwise newly recorded material would be hidden. |
| §7 Flatten | See section 7 below. Audio Flatten produces one plain region per selection and retains editable boundary fades. MIDI Flatten produces one region per selection; MIDI Flatten-and-Merge produces one sorted region. |
| §8 Persistence | Format v8 with explicit folder/take/comp/selection records and `transport.midi_merge`. Migrate both flat audio and MIDI histories before normal restore. |
| §9 Engine | Materialize on the message thread, then use prepared audio streams and a dedicated MIDI playback snapshot. No file open, allocation, locking, model traversal, or comp normalization in the callback. |
| §10 MIDI | Add equivalent folder creation, timeline bar, piano-roll take mode, comp operations, deletion, flattening, and migration. Extend the retained MIDI model to preserve non-note/non-CC channel events that current commit drops. |
| §10.6 MIDI Merge | Sample `Session::midiMergeEnabled` into an immutable recording plan at Record start. Pick the target at the record-start position: topmost plain MIDI region, or the take referenced by the active folder comp there. All cycle passes merge into that same target. Group captured events by existing `passOrdinal`, union notes/events into the target, extend its start/end as needed, stable-sort by time then pass order, and emit one incremental undo diff per pass. |

### Remaining specification gaps

- **Merge into a folder comp gap:** no take is selected at record start. Recommended behavior: create a new take in that folder containing the pass and select it over the pass range. This matches “no region exists under the recording” without modifying a deliberately unselected take.
- **Several plain MIDI regions at record start:** choose the topmost/latest inserted region, matching current hit-test and overlap precedence.
- **Two folders with conflicting outside selections:** later/topmost folder wins only in overlapping outside ranges; non-conflicting selections from both survive.
- **MIDI time base:** use ticks for tempo-locked folders and samples for floating folders, with an explicit `timeBase`. When absorbing mixed lock modes, adopt the new recording’s mode for the resulting folder and convert the existing folder span, every `MidiCompSelection` start/end and every event position through the tempo map before changing `timeBase`, so nothing audible moves. Each `MidiTake` carries both sample and tick placement; the unit named by the folder’s current `timeBase` is canonical, and the merge recomputes only the other unit from it through the tempo map, never converting both independently. If a session’s tempo map makes that conversion lossy for a selection boundary, reject the merge with a message rather than approximate it.
- **Mixed audio and MIDI loop recording with Merge:** make each ordinal one transaction across all armed tracks. That makes undo remove the last musical pass coherently instead of removing MIDI while leaving its simultaneously recorded audio take.
- **Changing MIDI Merge while recording:** disable the toggle until Stop; the captured recording plan remains immutable.

## 4. Undo

Add:

- `TakeFolderEditAction`: generic stable-ID before/after snapshot for move, resize, gain/fades, Quick Swipe, comp edits, renames, active-comp changes, and whole-take selection.
- `DeleteTakeFolderAction`: remove/reinsert a complete folder.
- `DeleteTakeAction`: remove one take and apply deterministic fallback; if last, remove the folder.
- `SplitTakeFolderAction`: one folder to two, restoring exact pre-split state on undo.
- `PasteTakeFolderAction`: deep copy with fresh IDs.
- `FlattenTakeFolderAction`: folder to derived plain regions; undo restores the folder exactly.
- `FlattenMergeTakeFolderAction`: swaps a folder for an already rendered audio region, or for one merged MIDI region. Redo reuses the output file; undo never deletes it.
- `MidiMergePassAction`, or pass-grouped `RecordCommitAction`: before/after snapshot for one loop ordinal.

Change:

- `RecordManager::TrackCommitDiff` and `RecordCommitAction::TrackDiff` gain before/after audio-folder and MIDI-folder vectors.
- Emit incremental pass diffs for MIDI Merge cycle recording. The existing first-perform-no-op design supports registering several already-applied pass actions: undoing the last restores the state after the previous pass; redo reapplies it.
- Every folder action resolves by stable ID, never vector index.
- Every action republishes derived MIDI playback state and rebuilds audio playback when stopped.

## 5. Serialization and migration

Bump `kFormatVersion` from 7 to 8 (`src/session/SessionSerializer.cpp:74-77`).

Track JSON gains `audio_take_folders` and `midi_take_folders`. Persist all model fields above, including:

- folder ID/order/span/`span_user_resized`, label/colour/mute/lock;
- folder output gain/fades for audio;
- take IDs/order and complete payload/placement/edit metadata/provenance;
- comp ID/name/selections;
- active comp ID and Quick Swipe flag;
- MIDI time base, tempo-lock anchors, notes, CCs, other events;
- `transport.midi_merge`, default `false`.

Continue to route every take file through portable-path helpers used by ordinary audio regions. Save As currently calls `SessionSerializer::consolidateInto()` (`src/ui/MainComponent.cpp:3083-3111`); its traversal currently covers live files and `previousTakes` only (`src/session/SessionSerializer.cpp:2785-2796`, `:2853-2869`). Replace history traversal with every audio take, including unused takes and shared loop-spool files, deduplicated by source path.

### v7 → v8 migration

For each audio region with `previous_takes`:

1. Create one folder at the region’s existing timeline span.
2. Add previous entries in their serialized order, placed at the current region’s timeline start, inheriting current channels because old `TakeRef` did not store them.
3. Append the current region as the newest take.
4. Put the legacy region gain/fades/mute/lock/label/colour on the folder output, not both folder and take; legacy payload swapping kept those fields common.
5. Create `Comp 1` selecting only the current take over its current range.

For MIDI, do the same. Previous MIDI takes inherit the current placement/style/tempo mode; derive their sample lengths from their stored ticks. The active comp selects the current MIDI payload.

Loop-pass alternatives require no separate migration path: today they are serialized through the same `previous_takes` arrays with provenance.

Playback parity is required only for data v7 still contains. Migration cannot recover takes already discarded by the cap or portions already destructively sliced by #594. The regression test must render the pre-migration current region and the migrated active comp and compare samples/events exactly.

Loader validation should repair duplicate/zero IDs deterministically, keeping one old-to-new mapping per domain (`TakeFolderId`, `TakeId`, `TakeCompId`): the first occurrence in serialized order keeps its ID, later duplicates and every zero ID receive fresh IDs, and a reference to a duplicated ID resolves to the first occurrence. Rewrite `activeCompId` through the comp mapping and every `AudioCompSelection::takeId` / `MidiCompSelection::takeId` through the take mapping before checking references, reject only references that remain unknown afterwards (a zero reference is always unknown), coalesce selections, clamp numeric ranges, preserve silence gaps, and ensure at least one valid comp.

## 6. UI plan

### Timeline

`TapeStrip` remains compact at its existing row height; this aligns with `docs/fit-8-tracks-1080p-plan.md` and `TapeStrip.h:45-49`.

Replace index-only selection/hits with a discriminated stable target:

```cpp
TimelineItemId { kind: AudioRegion|AudioFolder|MidiRegion|MidiFolder,
                 trackIndex, entityId }
```

Use it for hit-testing, multi-selection, clipboard, edit dispatch, double-click callbacks, and editor animation rectangles.

Folder painting:

- one bar covering the visible span;
- per-selection take colour and dark silence gaps;
- low-alpha boundary marks;
- folder label/mute/lock/gain/fades;
- `T N` badge;
- badge/Alt+T cycles whole takes by changing the active comp.

### Audio region editor

Retarget `AudioRegionEditor` from `(trackIdx, regionIdx)` to `TimelineItemId`. In folder mode:

1. Existing toolbar/ruler.
2. Comp result row.
3. Vertically scrollable take lanes, newest first.
4. Existing shared horizontal scrollbar/status area.

Gesture priority:

1. Shared selection boundary.
2. Lane header.
3. Already selected section.
4. Swipe body.

Swipe/boundary changes preview locally and commit one action on mouse-up. Clicking a selected section removes exactly that interval. Existing editor snap applies, with the existing modifier bypass.

`WaveformSource` owns a worker thread per instance, so do not create an unbounded source per take. Maintain a bounded pool for visible lanes and reassign sources while scrolling.

### MIDI piano-roll editor

The full piano roll already uses vertical space for 128 pitch rows, velocity, and CC lanes (`src/ui/PianoRollComponent.cpp:658-665`, `:782-844`). Use two modes inside the same double-click modal:

- **Comp view:** comp row plus vertically stacked compact mini-piano-roll take lanes; horizontal swipe selects time.
- **Take edit view / Quick Swipe off:** existing full piano roll bound to the selected take, preserving note, velocity, CC, quantize, and step-edit behavior.

### MIDI Merge transport toggle

Add `Session::midiMergeEnabled`, persisted under transport. Add a clearly labelled `MIDI MERGE`/compact `M-MRG` text toggle to `TransportBar`, adjacent to the existing count-in/metronome mode controls (`src/ui/TransportBar.cpp:516-586`, `:1119-1202`).

At the 20 Hz refresh, show it whenever at least one armed track has `Track::Mode::Midi`; mixed audio/MIDI arming still shows it. Keep it visible while recording and disable interaction until Stop. Off is the default.

### JUCE gate

`TapeStrip`, `AudioRegionEditor`, `PianoRollComponent`, `TransportBar`, and `MainComponent` are existing JUCE surfaces; this feature should stay there. ImGui is used only for selected native panels and would require an unnecessary new child-window/focus architecture.

New model, comp geometry, and rendering helpers remain STL/foundation-only. UI files are already allowlisted, but their current ratchet ceilings still apply. Reuse existing component types/helpers without introducing extra literal JUCE references, and use `DuskContextMenu`, `DuskComboBox`, and Dusk modal helpers rather than new raw popup/file-chooser calls.

## 7. Flatten and Flatten-and-Merge

### Flatten

Audio:

- Use the same materializer as playback.
- Produce one plain region per comp selection.
- Preserve source offsets and 64-sample boundary fades as editable region fades.
- Combine folder/take dB additively.
- If the compounded take, folder and boundary envelope cannot be represented exactly by one ordinary region envelope, render that selection to a new file with the compounded envelope baked into the samples (the same renderer Flatten and Merge uses, applied to one selection) instead of leaving folder or boundary fades as metadata that plain-region playback does not apply. Playback parity takes precedence over a forced zero-copy implementation.

MIDI:

- Flatten produces selection-derived plain MIDI regions.
- Flatten-and-Merge produces the same selected notes/events as one sorted region; note tails remain complete.

### Flatten and Merge audio renderer

Do not call current `JoinRegionsAction` wholesale. Its slow path:

- allocates the entire result in RAM;
- runs synchronously from the action path;
- applies region gain but omits full fade-envelope behavior (`src/session/RegionEditActions.cpp:1278-1369`).

Instead add a bounded-block renderer that consumes the exact materialized playback regions, reads with `dusk::audio::FileReader`, and writes through `dusk::audio::FileWriter` (`src/engine/audiofile/FileWriter.h:12-41`). Render to a unique 32-bit-float WAV in `takes/`, using the session sample rate and mono only if every selected source is mono.

Reuse points:

- `JoinRegionsAction`’s source-path, reader, output-name, and model-replacement logic.
- `BounceEngine`’s worker-thread lifecycle, progress/cancel handling, partial-file cleanup, and message-thread completion pattern (`src/engine/BounceEngine.cpp:298-365`, `src/engine/BounceEngine.h:227-250`).
- `SessionSerializer::consolidateInto()` only for later Save As copying/repointing; it is not a renderer (`src/session/SessionSerializer.cpp:2741-2872`).

This render must not drive the full mix engine: Flatten-and-Merge should bake the folder/take/comp envelopes, not channel-strip plugins, buses, or master processing. It runs on a dedicated worker, never the audio callback or message thread. The render request captures the folder ID and `header.revision` when it is queued; every folder mutation (take added or removed, selection edited, span or envelope changed, comp switched) increments `revision` on the message thread. On completion, return to the message thread and register `FlattenMergeTakeFolderAction` only if both the ID and the captured revision still match; otherwise discard the rendered file and report the stale render. A session switch cancels every pending render before `finishLoadingSessionFrom` replaces the model, the same way a bounce in progress is cancelled, so a render can never complete against a folder from a different session.

## 8. Phased implementation plan

`V` means:

```bash
CCACHE_DIR=/tmp/duskstudio-ccache \
CCACHE_TEMPDIR=/tmp/duskstudio-ccache-tmp \
cmake -S . -B build-tests -DCMAKE_BUILD_TYPE=Release \
  -DDUSKSTUDIO_BUILD_TESTS=ON

CCACHE_DIR=/tmp/duskstudio-ccache \
CCACHE_TEMPDIR=/tmp/duskstudio-ccache-tmp \
cmake --build build-tests --target dusk-studio-tests -j6

ctest --test-dir build-tests --output-on-failure
cmake --build build -j6
tools/juce-gate.sh
```

For audio-path phases, also run `scripts/run-selftest-xvfb.sh` where practical, otherwise launch the binary and verify no immediate crash. For every added assertion: deliberately break its guarded behavior, capture the failing targeted test, restore, capture the pass, then run `V`. The known sandbox-only baseline remains 1061/1064 with the three failures named in the brief.

| Phase | Label and goal | Files, maximum 5 | Tests | Visible result | Estimate |
|---|---|---|---|---|---:|
| 0A | Prerequisite clean-first: model/engine | `Session.h`, `RecordManager.cpp`, `RegionEditActions.cpp`, `SessionSerializer.cpp`, `PlaybackEngine.cpp` | No new cases; existing full suite via `V` | None; separate human commit boundary | 100–250 |
| 0B | Prerequisite clean-first: UI/render | `TapeStrip.cpp`, `AudioRegionEditor.cpp`, `PianoRollComponent.cpp`, `TransportBar.cpp`, `BounceEngine.cpp` | Full suite plus existing screenshot capture | None; separate human commit boundary | 100–250 |
| 1 | **Audio core + MIDI core:** model and invariants | `Session.h`, `Session.cpp`, new `TakeFolders.h`, `tests/session_region_bounds.cpp`, `tests/session_apply_tempo_change.cpp` | `folder resize preserves hidden take content`; `MIDI folder tempo change preserves its time-base invariant` | None | 450–650 |
| 2 | **Audio core + MIDI core:** pure operations | new `TakeFolderOperations.{h,cpp}`, root `CMakeLists.txt`, new `tests/take_folder_model.cpp`, `tests/CMakeLists.txt` | `record overlay preserves complete takes`; `folder merge preserves outside selections`; `take deletion repairs only selected intervals`; `split preserves complete takes`; `comp normalization preserves silence gaps` | Core semantics callable | 900–1,200 |
| 3 | **Audio core + MIDI core + Merge:** v8 persistence/migration/Save As | `SessionSerializer.{h,cpp}`, `tests/session_schema_migration.cpp`, `tests/session_format_version.cpp`, `tests/session_save_as_consolidation.cpp` | `v7 audio history migrates with identical playback`; `v7 MIDI history migrates with identical events`; `MIDI Merge defaults off and round trips`; `consolidation copies every folder take once` | Old sessions load and new state saves portably | 700–1,000 |
| 4 | **Audio core + MIDI core:** derived playback hand-off | `PlaybackEngine.{h,cpp}`, `AudioEngine.cpp`, `TakeFolderOperations.cpp`, `tests/playback_loop_read.cpp` | `active comp preserves source offsets and silence gaps`; `comp boundary renders a 64-sample raised-cosine overlap`; `MIDI comp preserves note tail past boundary`; `MIDI folder costs one scheduler region` | Fixture-created folders play correctly | 450–700 |
| 5 | **Audio core / #594:** audio record commits | `RecordManager.{h,cpp}`, `TakeFolderOperations.{h,cpp}`, `tests/record_loop_take_stacking.cpp` | `partial audio overdub preserves the complete covered take`; `recording merges plain material and two folders without take loss`; `loop audio retains more than nine passes`; `punch changes comp only inside punch` | Audio recording creates/extends folders; #594 fixed | 650–900 |
| 6 | **MIDI core + Merge:** MIDI folder and merge commit | `RecordManager.{h,cpp}`, `TakeFolderOperations.cpp`, `tests/record_midi_overdub_diff.cpp`, `tests/session_apply_tempo_change.cpp` | `MIDI Merge off creates a folder`; `MIDI Merge cycle accumulates every pass in one target`; `folder target is the take selected at record start`; `comp-gap Merge creates a new take`; `other channel events survive`; `empty non-Merge cycle pass remains a take` | MIDI folder/Merge model behavior complete | 700–1,000 |
| 7 | **Audio core + MIDI core + Merge / #595:** undo and folder actions | `RegionEditActions.{h,cpp}`, `AudioEngine.cpp`, `TakeFolderOperations.cpp`, `tests/take_folder_model.cpp` | `timeline delete undo restores exact folder IDs`; `delete last take removes folder`; `record commit snapshots audio and MIDI folders`; `each MIDI Merge pass undoes independently`; `redo restores pass order` | Correct Delete and undo/redo; #595 fixed by design | 650–900 |
| 8 | **Flatten:** bounded renderer | new `TakeFolderRenderer.{h,cpp}`, root `CMakeLists.txt`, new `tests/take_folder_renderer.cpp`, `tests/CMakeLists.txt` | `rendered comp matches prepared playback sample for sample`; `renderer keeps silence gaps and crossfades`; `render failure removes partial output`; `large comp uses bounded buffers` | Rendering backend complete | 600–900 |
| 9 | **Flatten:** model actions and async completion | `RegionEditActions.{h,cpp}`, `TakeFolderRenderer.{h,cpp}`, `tests/take_folder_model.cpp` | `Flatten preserves boundary fades`; `Flatten and Merge undo retains rendered file`; `redo reuses rendered file`; `MIDI Flatten and Merge preserves selected events exactly` | Both Flatten operations complete behind API | 450–700 |
| 10A | **UI:** compact timeline, stable targets, menus | `TapeStrip.{h,cpp}`, `MainComponent.{h,cpp}`, `ScreenshotCapture.cpp` | Core actions from phase 7; manual/screenshot cases `audio folder timeline` and `MIDI folder timeline` | Folder bars, badge, menus, whole-folder Delete, editor routing | 600–850 |
| 10B | **Merge + UI:** transport toggle | `TransportBar.{h,cpp}`, `ScreenshotCapture.cpp`, `tests/session_transport_bounds.cpp` | `MIDI Merge transport state defaults off and survives reload`; manual mixed-arm/recording-disabled checks | Clearly visible MIDI Merge toggle | 180–280 |
| 11 | **Audio UI:** Quick Swipe take editor | `AudioRegionEditor.{h,cpp}`, `MainComponent.{h,cpp}`, `ScreenshotCapture.cpp` | Pure hit cases in `tests/take_folder_model.cpp`: `boundary hit wins over swipe`; `swipe replaces exactly its interval`; `mouse-up produces one comp state` | Audio take lanes and comp row | 1,100–1,500 |
| 12 | **MIDI core + UI:** piano-roll take mode | `PianoRollComponent.{h,cpp}`, `MainComponent.{h,cpp}`, `ScreenshotCapture.cpp` | `MIDI comp uses half-open note-on ownership`; `CC and other events use event time`; `take editor mutates selected take only` | MIDI comp lanes plus detailed take editing | 1,000–1,400 |
| 13 | **Audio/MIDI integration:** freeze, bounce length, cleanup | `AudioEngine.cpp`, `BounceEngine.{h,cpp}`, `MainComponent.cpp`, `tests/session_region_bounds.cpp` | `freeze length includes folder content and MIDI note tails`; `bounce length includes folder spans`; `clean-out retains unused folder takes` | Freeze/bounce/clean-out understand folders | 250–400 |
| 14 | **UI/docs:** final documentation | `MANUAL.md`, `DuskStudio.md`, `README.md`, `ScreenshotCapture.cpp` | `V`; screenshot checklist for audio/MIDI folders and MIDI Merge | Documented first-release feature | 200–350 |

Approval should pause between every phase. Phases 1–7 and 10A are required for #594/#595; phases 6, 10B, and 12 are the binding MIDI/Merge work; phases 8–9 are the binding first-release Flatten work.

## 9. Risks and guarding tests

- **Migration changes what plays:** compare actual audio buffers and MIDI event streams before and after the v7 → v8 migration, not just model fields.
- **Another silent cap replaces the old cap:** record more than nine audio/MIDI passes and assert every ordinal remains. Overflow must fail a pass visibly, never evict its oldest content.
- **Boundary clicks or gain bumps:** sample-check all 64 fade samples, selection near take edges, take-to-silence, and selection shorter than 128 samples.
- **Folder merges lose outside selections:** test plain+folder and two-folder recording with overlap on both sides of the new pass.
- **Take fallback fills intentional silence:** test that only intervals formerly using the deleted take are repaired.
- **Stale vector indices:** all actions/editor targets use stable IDs; test deletion/undo while an editor target is open.
- **MIDI note-offs disappear:** include a note whose start is selected and whose end lies beyond both selection and folder span.
- **MIDI scheduler budget grows with take count:** a folder with more than 32 takes must still publish one scheduler region.
- **MIDI Merge targets the wrong take:** change the active comp after capture starts and verify commit still uses the start-time target.
- **Cycle Merge undo is not pass-granular:** three passes must require exactly three undos and restore the exact intermediate states.
- **Duplicate-time MIDI events reorder:** stable sort by timestamp, event type, and pass/insertion order; assert same-tick controller precedence.
- **RT regression:** source-contract tests plus self-test must show no allocation, lock, file open, JSON work, or model normalization in the callback.
- **Rapid comp publication lifetime:** stopped-only comp editing for the first implementation. Live audition requires a separately reviewed epoch-retirement design.
- **Flatten does not match playback:** compare rendered WAV samples to `PlaybackEngine` output using the same materialized comp.
- **Render blocks UI/audio or leaves debris:** worker-thread test, cancellation test, partial-file cleanup, folder-revision check before commit.
- **Save As or Clean Out loses unused takes:** include inactive comps, unused takes, and several takes sharing one spool file.
- **Waveform worker explosion:** visible-lane source pool; stress a folder with hundreds of takes and repeated scrolling.
- **JUCE ratchet grows:** run `tools/juce-gate.sh` every phase and keep new model/operations/render code framework-free.

## 10. Size estimate

Estimated changed/new lines, including tests and documentation:

- Cleanup prerequisites: 200–500
- Model and pure operations: 1,350–1,850
- Serialization/migration: 700–1,000
- Playback hand-off: 450–700
- Audio recording: 650–900
- MIDI folders and Merge: 700–1,000
- Undo/actions: 650–900
- Flatten/rendering: 1,050–1,600
- Timeline/transport UI: 780–1,130
- Audio editor: 1,100–1,500
- MIDI editor: 1,000–1,400
- Integration/docs: 450–750

Overall: approximately **8,600–12,300 changed/new lines across roughly 30–35 unique files**, delivered in approval-gated slices of no more than five files.

