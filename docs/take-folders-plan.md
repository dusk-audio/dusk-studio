# Take folders and comping (replaces flat take history)

Status: product spec, draft, targeted at **1.1**. 1.0 ships the flat take history
with its limits documented. Supersedes the flat `previousTakes` model and the
take-stack design explored for #594/#595.

Delivery decisions (2026-09-15):
- Build order: audio take folders end to end first (record, playback, Delete,
  undo, timeline bar), then audio editor comping, Flatten / Flatten and Merge,
  then MIDI take folders, MIDI Merge and piano-roll take mode. All in 1.1.
- Comp edits apply while the transport is stopped in the first version. Live
  swipe comping during playback gets its own reviewed engine design afterwards.

## Why

People who have used Logic Pro, Pro Tools, Cubase, Reaper, Studio One or Ableton
Live expect recording over existing audio to work the same way everywhere: every
take stays complete, the takes are grouped on the track, and the user picks which
parts of which takes play. Dusk Studio's current model trims older takes when an
overdub partly covers them (#594) and loses the stack on Delete (#595). Patching
that model would be replaced later; this spec adopts the industry model instead.

Primary reference: Logic Pro take folders and Quick Swipe Comping. Where Logic's
exact behavior is not confirmed from Apple's documentation, the item is marked
**Dusk decision**.

## Terms

- **Take**: one complete recorded pass. Its audio file, source range and timeline
  position are never trimmed or destroyed by later recording.
- **Take folder**: a container on a track holding two or more takes that overlap
  in time. Its span is the union of its takes, unless the user has resized it.
- **Comp**: an ordered set of non-overlapping selections `[start, end) -> take`
  across the folder's span. What plays is the active comp. A range with no
  selection plays silence.
- **Lanes**: one row per take, shown only in the region editor modal.

## 1. Recording

1.1 Recording over one or more existing plain regions creates a take folder that
    contains each overlapping region as a take plus the new recording as a take.
    Regions that do not overlap the new recording are untouched. (Logic: with
    Cycle off and "Create Take Folder", the folder contains the existing region
    and the new one.)

1.2 **Dusk decision**: each overlapping plain region becomes its own take, oldest
    first by recording provenance, falling back to timeline order.

1.3 Recording over an existing take folder adds the new recording as the newest
    take. In the active comp, the new take is selected over its full range;
    outside its range the previous selections remain.

1.4 A recording that overlaps a folder and plain regions merges the plain regions
    into that folder as takes. A recording that overlaps two folders merges them
    into one folder, keeping every take and each folder's comp selections outside
    the new take's range.

1.5 Loop (cycle) recording: each pass becomes a take in one folder; the last pass
    is selected. This replaces the current loop-pass alternatives.

1.6 Punch in/out: the take is the punched range as recorded. Nothing outside the
    punch is trimmed from other takes.

1.7 Comp boundaries play with the existing 64-sample raised-cosine crossfade
    (DuskStudio.md §5b). No clicks at selection edges.

## 2. Display

Dusk Studio differs from Logic here. Logic edits takes inline in expandable
track lanes. Dusk Studio keeps the timeline compact (8 tracks at 1080p): a region
is a colored bar, and detailed editing happens in the region editor modal opened
by double-clicking a region. Take folders follow that split.

2.1 Timeline: a take folder is one colored bar on the track, drawn from the active
    comp as it plays, with the take badge showing the take count. Comp selection
    boundaries are marked subtly on the bar. There are no lanes in the timeline.

2.2 Double-clicking a take folder opens the region editor modal in take mode: one
    lane per take, newest at the top, each with its waveform. Selected sections are
    drawn at full contrast; unselected sections are dimmed. A comp row above the
    lanes shows the result.

2.3 The timeline bar and the editor show the same comp; neither view changes what
    plays by being opened or closed.

## 3. Comping (Quick Swipe Comping, in the region editor)

All comping happens in the region editor modal's take lanes.

3.1 Dragging across a take lane selects that range for the comp and deselects the
    same range in every other take.

3.2 Clicking a take's lane header selects the whole take (the comp becomes that
    take across its range).

3.3 Dragging the boundary between two adjacent selections moves it.

3.4 Clicking a selected section deselects it, leaving silence there. (**Dusk
    decision**, matches Logic's behavior as commonly used; confirm against Logic.)

3.5 Comps can be created, duplicated, renamed, deleted, and switched from the
    folder menu. A folder always has at least one comp.

3.6 Quick Swipe Comping can be switched off per folder. With it off, take regions
    in lanes can be trimmed and slipped in time like ordinary regions.

3.7 Every comp edit is one undo step.

## 4. Take folder menus

4.1 Timeline right-click on a take folder: Takes (select whole take) · Comps
    (switch) · Open in editor · Delete take · Flatten · Flatten and Merge. The
    existing next-take shortcut cycles whole takes. Quick take switching from the
    timeline does not require opening the editor.

4.2 Region editor (take mode): everything in 4.1 plus New comp · Duplicate comp ·
    Rename take/comp · Delete comp · Quick Swipe Comping on/off.

## 5. Delete

5.1 Delete with a take folder selected in the timeline deletes the whole folder
    (Logic behavior for a selected region). **Decided 2026-09-15**; supersedes the
    #595 decision ("Delete pops the top take").

5.2 Delete with a take lane selected in the region editor, or "Delete take" from
    either menu, removes that take from the folder. **Dusk decision**: comp sections that used it fall back
    to the newest remaining take that covers that time, rather than silence.

5.3 Deleting the last take removes the folder. A folder with one take stays a
    folder until the user flattens it. **Dusk decision**.

5.4 Audio files are never deleted from disk by these operations.

## 6. Editing folders

6.1 Moving a folder moves all its takes and comps together.

6.2 Resizing a folder edge changes the folder's visible span. Takes stay complete
    underneath; extending the edge again reveals them.

6.3 Splitting a folder at a position produces two folders, each holding every take
    and the comp selections on its side. Take audio stays complete.

6.4 Copy, paste and duplicate copy the folder with its takes and comps.

6.5 Fades and gain set on the folder in the timeline apply to the comp output.
    Take regions in the editor's lanes keep their own gain and fades when Quick
    Swipe Comping is off.

## 7. Flatten

7.1 Flatten replaces the folder with plain regions, one per comp selection, with
    the boundary crossfades kept as fades. Unused sections leave the session; their
    files stay on disk.

7.2 Flatten and Merge replaces the folder with a single region rendered to a new
    audio file.

## 8. Persistence and migration

8.1 Session format bump. A folder stores id, span, takes (file, source offset,
    length, timeline start, channels, gain, fades, name, colour, provenance),
    comps (name, selections), active comp, Quick Swipe flag.

8.2 Migration from the flat model: a region with `previousTakes` becomes a folder.
    The current region is the newest take; each previous take is placed at the
    region's timeline start (today's placement), in the existing order. The active
    comp selects the current region over its range. Loop-pass alternatives migrate
    the same way.

8.3 Older sessions load with identical playback.

## 9. Engine

9.1 What plays is derived from the active comp on the message thread and handed to
    the audio thread through the existing region hand-off. No allocation or locks
    on the audio thread.

## 10. MIDI take folders (same pass as audio)

10.1 MIDI recording over existing MIDI regions creates or extends a MIDI take
     folder with the same rules as §1: every take stays complete, newest take
     selected over its range.

10.2 Timeline and editor follow §2-§4: a colored bar in the timeline; take lanes in
     the MIDI region editor (piano roll) opened by double-click.

10.3 **Dusk decision**: MIDI comping selects time ranges per take like audio. A
     note belongs to the selection that contains its start; it plays its full
     length even past the selection end. CC and other events are taken from the
     selection that contains them. No crossfades.

10.4 Delete, editing, Flatten (§5-§7) apply. Flatten and Merge produces one MIDI
     region with the selected events (no audio render).

10.5 Migration: MIDI `previousTakes` migrate as in §8.2.

10.6 **MIDI Merge (overdub).** A transport toggle, "MIDI Merge", switches MIDI
     recording between two behaviors (Pro Tools "MIDI Merge", Cubase "Merge"
     record mode, Ableton Live "Arrangement Overdub", Logic "Overlapping MIDI
     recordings: Merge"):
     - Off (default): recording over MIDI creates or extends a take folder (§10.1).
     - On: recorded notes and CC are added to the existing MIDI region instead of
       creating a take. Record the snare on one pass, the kick on the next, and
       both end up in the same region.
     - Cycle recording with Merge on adds each pass to the same region, so a loop
       can be built up part by part.
     - If no region exists under the recording, a new region is created. If the
       recording runs past the region's end, the region extends to cover it.
     - **Dusk decision**: if the existing material is a take folder, notes merge
       into the take selected in the active comp at the record start position.
     - Each recording pass is one undo step, so a bad pass can be removed without
       touching earlier ones.
     - The toggle is saved with the session and shown clearly on the transport
       while armed tracks are MIDI.

## 11. Decisions (2026-09-15)

1. Delete on a take folder in the timeline deletes the whole folder (Logic).
   Removing one take is done from the menu or the editor. Supersedes #595.
2. MIDI take folders ship in the same pass as audio (§10).
3. Audio recording over existing audio always creates or extends a take folder;
   no replace or merge modes for audio. MIDI adds a "MIDI Merge" transport toggle
   (§10.6); with it off, MIDI behaves like audio.
4. Flatten and Merge ships with the first release of take folders.

## 12. Gaps resolved during codebase mapping (2026-09-15)

See `docs/take-folders-implementation.md` for the code-level plan.

1. MIDI Merge when the active comp has a gap at the record start position: create
   a new take in that folder holding the pass, selected over the pass range. An
   unselected take is never modified.
2. MIDI Merge with several plain MIDI regions at the record start: target the
   topmost (latest inserted) region, matching timeline hit-testing.
3. A recording that merges two folders with conflicting selections outside the new
   take's range: the later folder wins where they conflict; everything else
   survives.
4. MIDI folder time base: ticks for tempo-locked folders, samples for floating
   ones. When absorbing mixed modes, absolute event positions are kept and the
   folder adopts the new recording's mode.
5. Cycle recording audio and MIDI together with MIDI Merge on: one undo step per
   pass covers every armed track.
6. The MIDI Merge toggle cannot be changed while recording.
7. Timeline "Delete take" is a submenu listing the folder's takes, since a comp
   can use several.
8. Recording past a folder edge the user resized expands that edge to show the new
   recording.
9. No cap on takes or loop passes (replaces today's 8-take cap). If recording
   overflows, that pass fails with a visible error; older content is never
   evicted.
10. The next-take shortcut and badge set the active comp to one whole take.
11. The recorder today drops pitch bend, pressure and program change on MIDI
    commit; MIDI takes keep them.
