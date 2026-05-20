#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include "../session/Session.h"

namespace duskstudio
{
class AudioEngine;
class EditModeToolbar;
// Piano-roll editor for one MidiRegion. Anchored on a (track, region) pair
// at construction; the component validates indices each paint so a
// concurrent record / delete that mutates the regions vector doesn't crash
// the UI - a stale view just shows nothing until the parent reopens it.
//
// Concurrency contract (matches the AudioRegion path used by TapeStrip):
//   • All mutation of session.track(t).midiRegions and the inner notes /
//     ccs vectors happens on the MESSAGE THREAD (this component's mouse
//     and keyboard handlers, plus RecordManager::stopRecording).
//   • The AUDIO THREAD reads those vectors lock-free during playback in
//     AudioEngine's per-block MIDI scheduler.
//   • A user edit while transport is rolling can race a concurrent
//     audio-thread iteration; vector reallocation on push_back would
//     invalidate the audio-thread's in-flight pointers. In practice
//     edits are infrequent enough (single user gesture per ~hundred ms)
//     and audio-thread iteration is short enough that the codebase
//     accepts this as the same "rare-but-possible" race that affects
//     AudioRegion edits. A future hardening pass would either (a) gate
//     mutations on transport-stopped, or (b) introduce a swap-load
//     atomic-pointer pattern. Not done yet.
//
// The roll is the ONE visible exception to Focal's "everything visible"
// rule (per Focal.md): a modal overlay on top of the tape-strip, dismissed
// with Esc or by clicking outside. The overlay-host is parent code; this
// component just paints the keyboard + note grid and handles edit input.
//
// Editing surface (lands across 4c-3):
//   • Click empty grid space → create a 1/4-note at that pitch+tick
//   • Click on a note → select; Backspace → delete
//   • Drag a selected note's body → move (snaps to grid)
//   • Drag a note's right edge → resize
//   • Mouse wheel → vertical scroll across the 128-key range
//   • Cmd/Ctrl + wheel → horizontal zoom (same as the main timeline)
class PianoRollComponent final : public juce::Component,
                                    private juce::Timer,
                                    private juce::ScrollBar::Listener
{
public:
    PianoRollComponent (Session& session, AudioEngine& engine,
                          int trackIndex, int regionIndex);
    ~PianoRollComponent() override;

    // Cmd+]/Cmd+[ in-place region swap. The host re-opens the editor
    // on the requested (track, region). No-op at boundaries (no wrap).
    std::function<void(int trackIdx, int newRegionIdx)> onNavigateToRegion;

    // Step-record entry points. MainComponent wires these to the
    // VirtualKeyboardComponent's onNoteOn / onNoteOff callbacks
    // when both modals are open. Each VKB Note On lands as a
    // MidiNote at the current playhead (or the start of the
    // currently-in-progress chord); the playhead advances by one
    // snap step when the chord clears.
    void stepRecordNoteOn  (int noteNumber, int velocity);
    void stepRecordNoteOff (int noteNumber);
    // Reset step-record bookkeeping. Called when the VKB closes -
    // any in-flight chord state would be stale next time.
    void resetStepRecordState() noexcept;

    // Esc-to-close hook. The host (MainComponent) sets this so the user
    // can dismiss the overlay without reaching for the mouse. The roll
    // itself just calls it when it sees an Esc keypress.
    std::function<void()> onCloseRequested;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp   (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    bool keyPressed (const juce::KeyPress&) override;
    void mouseWheelMove (const juce::MouseEvent&,
                          const juce::MouseWheelDetails&) override;

    // Visual constants exposed so the host overlay can size itself.
    // Sized for daily-use legibility - earlier 10 pt / 12 px values
    // read as cramped on 1080p+ displays.
    static constexpr int kKeyboardWidth     = 76;
    static constexpr int kToolbarHeight     = 48;   // mode indicators + hotkey legend
                                                     // (matches AudioRegionEditor kIconRowHeight for visual parity)
    static constexpr int kHeaderHeight      = 28;   // bar/beat ruler (below toolbar)
    static constexpr int kNoteHeight        = 16;
    static constexpr int kNumKeys           = 128;
    static constexpr int kFullGridHeight    = kNumKeys * kNoteHeight;
    // Strip heights are runtime-mutable so the user can drag the top
    // edge of either lane to resize, or scroll-wheel inside the lane
    // to zoom the velocity axis. The kDefault values are the initial
    // sizes; kMin / kMax bound the resize gesture.
    static constexpr int kVelocityStripHDefault = 110;
    static constexpr int kVelocityStripHMin     = 56;
    static constexpr int kVelocityStripHMax     = 320;
    static constexpr int kCcStripHDefault       = 100;
    static constexpr int kCcStripHMin           = 48;
    static constexpr int kCcStripHMax           = 320;
    static constexpr int kStripResizeGrabPx     = 5;   // hit zone above each strip's top edge
    // Reaper-style status bar at the very bottom of the modal.
    // Hosts position / value readouts + grid / notes / color combos
    // + key-snap toggle + track readout. Child widgets (juce::Label,
    // ComboBox, ToggleButton) take their own clicks; the band is
    // never paint-handled directly by mouseDown.
    static constexpr int kStatusBarH = 30;
    static constexpr int kScrollBarH = 12;

    juce::ScrollBar horizontalScrollBar { false };   // false = horizontal
    void scrollBarMoved (juce::ScrollBar* bar, double newRangeStart) override;
    void syncScrollBarRange();

private:
    Session& session;
    AudioEngine& engine;
    int trackIdx;
    int regionIdx;

    // Step-record (VKB-driven) state. Held-counter tracks how many
    // VKB keys are currently down; chordHadNotes flips true on the
    // first Note On of a chord and back to false when the count
    // drops to zero. The first Note On of the NEXT chord advances
    // the playhead before placing - chord notes themselves share a
    // start position.
    int  stepRecordHeld     = 0;
    bool stepRecordChordHad = false;

    // Pixels per tick: drives horizontal scale. Default 24 px/quarter at
    // 480 ticks/quarter = 0.05 px/tick.
    float pixelsPerTick = 0.05f;

    // Snap resolution in ticks. 0 = no snap (free-positioning); positive =
    // round each create/move/resize tick to the nearest multiple. Default
    // 120 ticks = 1/16 note at 480 PPQN, which matches the most common
    // pop / rock entry grid. The user can change this with a number-key
    // shortcut later (Phase 5 keyboard pass).
    juce::int64 snapTicks = 120;

    // Vertical scroll. 0 = top of the 128-key range visible. Bounded so
    // we never scroll past either end. Mouse wheel adjusts.
    int scrollY = (kNumKeys - 24) * kNoteHeight / 2;  // centre near middle C

    // Horizontal scroll in pixels. 0 = region origin sits at the left of
    // the grid. Adjusted by Shift+wheel and by trackpad delta-X. Bounded
    // at zero on the left; right bound is open (a user editing past the
    // region's end is welcome - 4d's punch-stack will resize the region).
    int scrollX = 0;

    // Selected notes. Indices into region->notes; sorted ascending and
    // deduplicated. Operations that mutate notes (delete) iterate in
    // descending order so earlier indices stay valid. Empty = nothing
    // selected.
    std::vector<int> selectedNotes;

    // Anchor note for a multi-note drag - the note actually under the
    // cursor at mouseDown. Drag deltas are computed against this note;
    // every other selected note follows the same delta. Resize applies
    // only to the anchor (multi-resize is ambiguous).
    int dragAnchor = -1;

    // Per-selected-note snapshot taken at mouseDown so MoveNote can
    // apply a consistent delta to every selected note even after some
    // of them get clamped against the region boundary. Parallel to
    // selectedNotes (one entry per selected note in the same order).
    struct DragSnapshot
    {
        juce::int64 startTick     = 0;
        int         noteNumber    = 0;
        juce::int64 lengthInTicks = 0;
        int         velocity      = 100;   // baseline for EditNoteVelocity drag
    };
    std::vector<DragSnapshot> dragSnapshots;

    // Active rubber-band rectangle in screen coords during BoxSelect.
    // Empty rect = no box-select in flight (paint skips the overlay).
    juce::Rectangle<int> rubberBand;

    // App-wide note clipboard. Static so a Cmd+C in one piano-roll
    // instance is visible to Cmd+V in another (cross-region paste is
    // the headline use case). Stored startTicks are RELATIVE to the
    // earliest note's tick, so paste lands the cluster at the target
    // region's editCursorTick regardless of the source's anchor.
    static std::vector<MidiNote> sNoteClipboard;

    enum class DragMode { None, MoveNote, ResizeNote, CreateNote, EditVelocity, BoxSelect,
                           EditCcValue, EditNoteVelocity,
                           ResizeVelocityStrip, ResizeCcStrip, RangeSelect, Pan };
    DragMode dragMode = DragMode::None;

    // Pan-drag state. Middle-mouse-drag in the grid pans the timeline +
    // the keyboard range together so an off-screen note can be reached
    // without flipping between scroll modifiers.
    int panStartMouseX = 0;
    int panStartMouseY = 0;
    int panStartScrollX = 0;
    int panStartScrollY = 0;

    // Time-range selection driven by drag in the bar/beat ruler. When
    // active, paints a translucent yellow band over the grid and all
    // notes whose start falls in [start, end) get added to
    // selectedNotes automatically. Cleared on Esc or by clicking the
    // ruler again without dragging.
    juce::int64 rangeStartTick = 0;
    juce::int64 rangeEndTick   = 0;
    bool        rangeActive    = false;

    // Runtime strip heights. Mutated by the resize gesture (drag the
    // top edge) and the in-strip wheel-zoom. Both are persisted only
    // in-component; reopening the roll resets to defaults, which is
    // intentional - lane sizing is editor-state, not session-state.
    int velocityStripH = kVelocityStripHDefault;
    // CC lane starts collapsed. User can reveal it with the Toggle-CC
    // toolbar button or by dragging the strip's top edge; the default
    // matches the "don't show what you're not using" portastudio bias.
    int ccStripH       = 0;

    // mouseDown anchor for the strip-resize drag: the strip height at
    // gesture start, so mouseDrag can compute a stable delta against
    // the cursor's vertical motion.
    int resizeStartStripH = 0;
    int resizeStartMouseY = 0;

    // Active CC controller shown in the CC lane. Starts on CC 1 (mod
    // wheel) - the most-used continuous controller. 'L' cycles through
    // common ones (1, 7, 11, 64, 74). The lane only displays CCs whose
    // controller matches this; events on other controllers stay in the
    // region's ccs vector and pass through to the synth at playback.
    int activeCcController = 1;

    // Scale-highlight overlay. When `scale != Scale::Off`, rows whose
    // note isn't in the selected scale get a translucent dark wash so
    // in-scale notes pop visually. 'S' opens the picker popup.
    enum class Scale { Off, Major, Minor, Dorian, Phrygian, Lydian, Mixolydian, Locrian };
    Scale scale = Scale::Off;
    int   scaleRoot = 0;   // 0=C, 1=C#, ... 11=B
    // Index into region->ccs of the CC event currently being dragged in
    // the CC lane (set during EditCcValue, -1 otherwise). The vector
    // doesn't get mutated mid-drag, so the index stays stable.
    int draggedCcIdx = -1;

    // Note coloring. Pitch maps 12 semitones to 12 distinct hues (chord
    // shapes pop at a glance); Velocity is the legacy single-blue tinted
    // by 0..127; Channel is a 16-way hue cycle (useful for multi-channel
    // captures from a controller). 'C' cycles. Default Pitch matches
    // commercial DAWs.
    enum class ColorMode { Pitch, Velocity, Channel };
    ColorMode colorMode = ColorMode::Pitch;

    // Note-entry-mode multiplier for snapTicks. Grid = use snapTicks
    // verbatim; Free = ignore snap on note creation only (move /
    // resize still snap unless snap is also off); Triplet = step *
    // 2/3 (so 3 notes fit in 2 grid steps); Dotted = step * 3/2.
    enum class NoteEntryMode { Grid, Free, Triplet, Dotted };
    NoteEntryMode noteEntryMode = NoteEntryMode::Grid;
    // Vertical pitch-row snap on note creation. When false, the new
    // note's pitch is the row pointed at exactly (so a click between
    // row centres still creates a single note - we still snap to the
    // nearest row visually since the row IS the noteNumber, but we
    // honour the click's row without an extra constraint pass).
    bool keySnap = true;
    juce::int64 dragOriginTick    = 0;
    int         dragOriginNoteNum = 0;
    juce::int64 dragNoteStartTick = 0;
    juce::int64 dragNoteLenTicks  = 0;

    // Edit cursor - the vertical-line marker at the last "anchor"
    // gesture (grid click / note creation / note move). Step-record
    // and future click-to-place actions key off this; visually it
    // matches Reaper's gold playhead so users have an "I am here"
    // signal independent of the transport.
    juce::int64 editCursorTick = 0;

    // Helpers - all return defensible values when the region pointer is
    // stale (regionIdx out of range), so paint code never branches on it.
    MidiRegion*       region();
    const MidiRegion* region() const;

    // Coordinate maps - keyboard column on the left is fixed, the note
    // grid fills the rest. Y wraps around the kNumKeys range with scrollY
    // applied; X anchors at the keyboard's right edge, ticks measured
    // from region.timelineStart (== region-local tick 0).
    int  yForNoteNumber (int noteNumber) const;
    int  noteNumberForY (int y) const;
    int  xForTick (juce::int64 tick) const;
    juce::int64 tickForX (int x) const;

    // Hit test - returns the index into region->notes hit by (x, y), or
    // -1 if the click was on empty grid space. Out-param `onRightEdge`
    // tells the caller the click was within the note's last few px (for
    // resize-vs-move drag intent).
    int hitTestNote (int x, int y, bool& onRightEdge) const;

    // Drawing helpers.
    void paintKeyboard      (juce::Graphics&, juce::Rectangle<int> area);
    void paintNoteGrid      (juce::Graphics&, juce::Rectangle<int> area);
    void paintToolbar       (juce::Graphics&, juce::Rectangle<int> area);
    void paintBeatRuler     (juce::Graphics&, juce::Rectangle<int> area);
    void paintNotes         (juce::Graphics&, juce::Rectangle<int> area);
    void paintVelocityStrip (juce::Graphics&, juce::Rectangle<int> area);
    void paintCcStrip       (juce::Graphics&, juce::Rectangle<int> area);
    void paintEditCursor    (juce::Graphics&, juce::Rectangle<int> gridArea);

    // Hit-test for CC bars in the CC lane. Returns the index into
    // region->ccs whose tick maps to a screen-x within kHitSlopPx
    // of the click, restricted to events on activeCcController.
    int hitTestCcBar (int x, juce::Rectangle<int> stripArea) const;

    // Hit-test helper for the velocity strip. Returns the index into
    // notes whose vertical bar contains x; -1 if no note matches.
    int hitTestVelocityBar (int x, juce::Rectangle<int> stripArea) const;

    // Resolve the fill colour for a note under the active colorMode.
    // Selection state is the caller's concern (paint applies the
    // selection highlight on top); this only returns the base fill.
    juce::Colour colourForNote (const MidiNote& n) const noexcept;

    // Selection helpers. Selection is stored sorted/deduped so set
    // operations (toggle / contains) are O(log n) on a vector of ints.
    bool isNoteSelected (int idx) const noexcept;
    void clearSelection();
    void selectOnly (int idx);
    void toggleSelected (int idx);
    void addToSelection (int idx);
    // Snapshot every selected note's start / pitch / length into
    // dragSnapshots so MoveNote can apply a consistent group delta.
    // Set dragAnchor to `anchorIdx` (the note under the cursor).
    void beginGroupDrag (int anchorIdx);
    // Shift every selected note by (deltaTicks, deltaPitch). Clamps
    // the delta against the group's bounding box so no note ends up
    // out of [0, lengthInTicks] or [0, 127].
    void applyGroupMove (juce::int64 deltaTicks, int deltaPitch);
    // Transpose every selected note by `semitones`, clamped per note
    // to [0, 127]. Used by Up/Down arrow handlers.
    void transposeSelected (int semitones);

    // Quantize selected notes (or all notes if nothing selected) to
    // `gridTicks`, blended at `strength` 0..1. strength=1.0 fully
    // snaps to grid; strength=0.5 moves halfway from original tick to
    // snapped tick (humanises without losing the original feel).
    void quantizeSelected (juce::int64 gridTicks, float strength);

    // Velocity helpers for selected notes (or all notes if nothing
    // selected, mirroring quantizeSelected's "no-selection = whole-
    // region" rule). humanizeVelocity adds a random offset in
    // [-rangePercent..+rangePercent] of full-scale (127) per note;
    // setVelocityFor clamps every note to a fixed value. Both clamp
    // the result to [1, 127].
    void humanizeVelocity (int rangePercent);
    void setVelocityFor (int value);
    void showVelocityPopup();

    // Per-note property setters. Same "selected notes" rule as the
    // velocity helpers: empty selection = whole region. Channel
    // clamped to [1, 16]; length clamped to [1, region length].
    void setChannelForSelected (int channel);
    void setLengthTicksForSelected (juce::int64 ticks);
    // Right-click context menu on a note. Three submenus: channel,
    // length, velocity. Acts on the current selection (the right-
    // click promotes the clicked note to selected first if it
    // wasn't already). screenPos anchors the popup at the click.
    void showNotePropertiesPopup (int hitNoteIdx, juce::Point<int> screenPos);

    // Glue every selected same-pitch + contiguous note into one. Two
    // notes are "contiguous" when the second's startTick falls at or
    // before the first's endTick. The lower-index note absorbs the
    // others; absorbed notes are erased in descending index order so
    // earlier indices stay valid. Selection is cleared on success
    // because the indices we held are now stale.
    void glueSelectedNotes();

    // Clone every selected note shifted right by the selection's
    // span (max-end - min-start), so the duplicate sits cleanly
    // after the original block. New notes are appended to
    // region->notes; selection is replaced with the freshly-cloned
    // indices so a follow-up nudge / transpose acts on the copy.
    void duplicateSelectedNotes();

    // Shift every selected note's startTick by deltaTicks, clamped so
    // no note ends up out of [0, lengthInTicks - lengthInTicks_of_note].
    // Used by Left/Right arrow nudge handlers.
    void nudgeSelectedTicks (juce::int64 deltaTicks);

    // True if `noteNumber` is in the active scale (or scale is Off).
    bool isInScale (int noteNumber) const noexcept;

    // 'Q' / 'S' popup launchers - show the menu and apply the chosen
    // action. Both run on the message thread inside keyPressed.
    void showQuantizePopup();
    void showScalePopup();
    // Region-level inspector. Rename / colour / mute / lock / delete.
    // Bound to the toolbar's Properties button; right-click on a note
    // still routes to showNotePropertiesPopup (note-level).
    void showRegionPropertiesPopup();
    // Cmd+]/Cmd+[ navigation helpers. nextRegionIndex returns the
    // storage index of the neighbouring region by timelineStart, or
    // -1 at the boundary.
    int  nextRegionIndex (int delta) const;
    void navigateRegion  (int delta);
    // 30 Hz playhead poll, inherited from juce::Timer.
    void timerCallback() override;
    // Last x-pixel where the transport playhead was painted; -1 if
    // it was off-screen. Used for narrow repaint regions.
    int lastPlayheadX = -1;
    int  transportPlayheadX (juce::Rectangle<int> gridArea) const;
    void paintTransportPlayhead (juce::Graphics&, juce::Rectangle<int> gridArea);
    // Click-target popups for the toolbar's Color / CC chips. The
    // hotkeys ('C', 'L') still cycle through the choices; click is
    // an alternative discoverable surface.
    void showColorModePopup();
    void showCcControllerPopup();

    // Reaper-style bottom status-bar children. Real interactive
    // widgets (not paint-only chips) so JUCE handles dispatch /
    // hover / focus / accessibility.
    juce::Label        positionLabel;
    juce::Label        valueLabel;
    juce::Label        trackLabel;
    juce::ComboBox     gridCombo;
    juce::ComboBox     notesCombo;
    juce::ComboBox     colorCombo;
    juce::ToggleButton keySnapToggle;
    // Region-level mute / lock - parity with AudioRegionEditor's
    // status-bar checkboxes. Both submit via MidiRegionEditAction so
    // Cmd+Z reverts.
    juce::ToggleButton muteToggle;
    juce::ToggleButton lockToggle;

    // Reaper-style top icon row. Compact (~28 px) circular icon buttons
    // mirroring TransportIconButton's visual language. Hotkeys still
    // do the heavy lifting; these are a discoverability surface for
    // mouse-first editing.
    class IconButton final : public juce::Button
    {
    public:
        enum class Glyph { Undo, Redo, Split, Glue, Quantize, Properties, ZoomFit, ZoomIn, ZoomOut, ToggleCc };
        IconButton (const juce::String& name, Glyph g);
        void paintButton (juce::Graphics&, bool isMouseOver, bool isButtonDown) override;
    private:
        Glyph glyph;
    };
    juce::TextButton chaseToggle { "Chase" };   // auto-scroll view to follow the transport playhead
    IconButton undoButton       { "Undo",       IconButton::Glyph::Undo };
    IconButton redoButton       { "Redo",       IconButton::Glyph::Redo };
    IconButton splitButton      { "Split",      IconButton::Glyph::Split };
    IconButton glueButton       { "Glue",       IconButton::Glyph::Glue };
    IconButton quantizeButton   { "Quantize",   IconButton::Glyph::Quantize };
    IconButton propertiesButton { "Properties", IconButton::Glyph::Properties };
    IconButton toggleCcButton   { "Toggle CC",  IconButton::Glyph::ToggleCc };
    IconButton zoomOutButton    { "Zoom out",   IconButton::Glyph::ZoomOut };
    IconButton zoomInButton     { "Zoom in",    IconButton::Glyph::ZoomIn };
    IconButton zoomFitButton    { "Zoom fit",   IconButton::Glyph::ZoomFit };

    // Ardour-style edit-mode palette (Grab / Range / Cut / Grid / Draw +
    // Snap + denomination). Shared with AudioRegionEditor + TapeStrip
    // via session.editMode. Only Grab and Draw modify PianoRoll
    // behavior today; the other modes are no-ops on the roll surface.
    std::unique_ptr<EditModeToolbar> editModeToolbar;

public:
    // Called by MainComponent when a global hotkey flips
    // session.editMode while the modal is open, so the toolbar
    // repaints with the new active state.
    void syncEditModeToolbar();
private:

    // Lay the icon row out in the kToolbarHeight band at the top.
    void layoutIconRow (juce::Rectangle<int> area);

    // New action handlers (icon-only; not currently bound to a hotkey).
    // splitSelectedAtCursor cuts every selected note that straddles
    // editCursorTick into two notes at the cursor; if no notes are
    // selected, splits any note straddling the cursor.
    void splitSelectedAtCursor();
    // zoomFit rescales pixelsPerTick so the entire region length fits
    // the visible grid width.
    void zoomFit();
    // Multiplicative zoom anchored on the centre of the visible grid.
    // factor > 1 zooms in, < 1 zooms out. Shared by the icon buttons
    // and the '=' / '-' keypress shortcuts.
    void zoomByFactor (float factor);
    // toggleCcLane flips ccStripH between 0 (lane hidden) and
    // kCcStripHDefault (lane shown).
    void toggleCcLane();

    // Lay the status bar's children out in the kStatusBarH band at
    // `area`'s y. Called from resized().
    void layoutStatusBar (juce::Rectangle<int> area);

    // Format a region-local tick as a Reaper-style "bar.beat.tick"
    // string. Bar / beat are 1-based (so bar 1 = first bar); the
    // remainder is the offset in ticks from the beat.
    juce::String formatBarBeat (juce::int64 tick) const;

    // Velocity of the most-recently-selected note, or -1 if no note
    // is selected. Used by the value readout in the status bar.
    int activeVelocity() const noexcept;

    // Refresh the position / value / track readouts. Called any time
    // editCursorTick or selectedNotes mutates.
    void refreshStatusBarReadouts();
};
} // namespace duskstudio
