#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include "DuskComboBox.h"
#include "../session/Session.h"

namespace duskstudio
{
class AudioEngine;
class EditModeToolbar;
// Piano-roll editor anchored on (track, region). Validates indices each
// paint so a concurrent record / delete doesn't crash the UI.
//
// Concurrency: all mutation of session.track(t).midiRegions + inner
// notes/ccs happens on the message thread (this component's handlers +
// RecordManager::stopRecording). Audio thread reads lock-free during
// playback. A user edit while transport rolling can race a concurrent
// audio iteration; vector realloc on push_back would invalidate the
// audio's in-flight pointers. Edits are infrequent and audio iteration
// short, so the codebase accepts the same rare race AudioRegion edits
// have. Future hardening = gate mutation on transport-stopped, or
// swap-load atomic ptr.
//
// The roll is the one visible exception to DuskStudio's "everything
// visible" rule - modal overlay on top of TapeStrip, Esc / click-out
// to dismiss.
class PianoRollComponent final : public juce::Component,
                                    private juce::Timer,
                                    private juce::ScrollBar::Listener
{
public:
    PianoRollComponent (Session& session, AudioEngine& engine,
                          int trackIndex, int regionIndex);
    ~PianoRollComponent() override;

    // Cmd+]/Cmd+[ in-place region swap. Host re-opens the editor on
    // the requested (track, region). No-op at boundaries.
    std::function<void(int trackIdx, int newRegionIdx)> onNavigateToRegion;

    // VKB-driven step record. Each Note On lands a MidiNote at the
    // current playhead (or the start of the in-progress chord); the
    // playhead advances by one snap step when the chord clears.
    void stepRecordNoteOn  (int noteNumber, int velocity);
    void stepRecordNoteOff (int noteNumber);
    void resetStepRecordState() noexcept;

    // Host sets so Esc dismisses the overlay.
    std::function<void()> onCloseRequested;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp   (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    bool keyPressed (const juce::KeyPress&) override;
    void mouseWheelMove (const juce::MouseEvent&,
                          const juce::MouseWheelDetails&) override;

    // CursorOverlay sink - MainComponent wires these so the editor
    // pushes its local mouse position into the shared overlay (which
    // bypasses the platform cursor pipeline that fails on this hybrid
    // X11/Wayland setup). Same pattern as AudioRegionEditor.
    // The juce::Range<int> argument carries the cut-line Y span (unused
    // on the piano roll - Cut mode has no glyph here - but the
    // signature matches AudioRegionEditor's so MainComponent can use
    // a single forwarder lambda).
    std::function<void (juce::Component&, juce::Point<int>, EditMode,
                          juce::Range<int>)> onMouseMovedForCursor;
    std::function<void()> onMouseExitedForCursor;

    static constexpr int kKeyboardWidth     = 76;
    // Matches AudioRegionEditor kIconRowHeight for visual parity.
    static constexpr int kToolbarHeight     = 48;
    static constexpr int kHeaderHeight      = 28;
    static constexpr int kNoteHeight        = 16;
    static constexpr int kNumKeys           = 128;
    static constexpr int kFullGridHeight    = kNumKeys * kNoteHeight;
    // Strip heights are runtime-mutable (drag top edge / wheel zoom).
    static constexpr int kVelocityStripHDefault = 110;
    static constexpr int kVelocityStripHMin     = 56;
    static constexpr int kVelocityStripHMax     = 320;
    static constexpr int kCcStripHDefault       = 100;
    static constexpr int kCcStripHMin           = 48;
    static constexpr int kCcStripHMax           = 320;
    static constexpr int kStripResizeGrabPx     = 5;
    static constexpr int kStatusBarH = 30;
    static constexpr int kScrollBarH = 12;

    juce::ScrollBar horizontalScrollBar { false };
    void scrollBarMoved (juce::ScrollBar* bar, double newRangeStart) override;
    void syncScrollBarRange();

private:
    Session& session;
    AudioEngine& engine;
    int trackIdx;
    int regionIdx;

    // chordHadNotes flips true on the first Note On and back to false
    // when the count drops to zero. First Note On of the NEXT chord
    // advances the playhead before placing - chord notes share start.
    int  stepRecordHeld     = 0;
    bool stepRecordChordHad = false;

    // 24 px/quarter at 480 PPQN = 0.05 px/tick.
    float pixelsPerTick = 0.05f;

    // 0 = free positioning. 120 ticks = 1/16 at 480 PPQN. This is the chosen
    // RESOLUTION (grid combo); the MIDI editor's own snap-enable
    // (session.midiEditorSnap) decides whether it actually applies - see
    // effectiveSnapTicks(), which returns 0 (no snap) when the enable is off.
    std::int64_t snapTicks = 120;
    std::int64_t effectiveSnapTicks() const noexcept;

    int scrollY = (kNumKeys - 24) * kNoteHeight / 2;  // centre near middle C
    int scrollX = 0;

    // Sorted/dedup'd so set ops (toggle/contains) are O(log n). Mutating
    // ops iterate descending so earlier indices stay valid.
    std::vector<int> selectedNotes;

    // Note under the cursor at mouseDown. Drag deltas computed against
    // this; every selected note follows the same delta. Resize applies
    // only to the anchor (multi-resize ambiguous).
    int dragAnchor = -1;

    // Parallel to selectedNotes. Snapshot at mouseDown so MoveNote can
    // apply a consistent delta even after notes get clamped at the
    // region boundary.
    struct DragSnapshot
    {
        std::int64_t startTick     = 0;
        int         noteNumber    = 0;
        std::int64_t lengthInTicks = 0;
        int         velocity      = 100;
    };
    std::vector<DragSnapshot> dragSnapshots;

    juce::Rectangle<int> rubberBand;

    // App-wide so a Cmd+C in one piano-roll instance is visible to
    // Cmd+V in another (cross-region paste). Stored ticks are RELATIVE
    // to the earliest note's tick so paste lands at editCursorTick.
    static std::vector<MidiNote> sNoteClipboard;

    enum class DragMode { None, MoveNote, ResizeNote, CreateNote, EditVelocity, BoxSelect,
                           EditCcValue, EditNoteVelocity,
                           ResizeVelocityStrip, ResizeCcStrip, RangeSelect, Pan,
                           LoopIn, LoopOut, PunchIn, PunchOut };
    DragMode dragMode = DragMode::None;

    int panStartMouseX = 0;
    int panStartMouseY = 0;
    int panStartScrollX = 0;
    int panStartScrollY = 0;

    // Time-range drag in the bar/beat ruler. Notes whose start is in
    // [start, end) get auto-added to selectedNotes.
    std::int64_t rangeStartTick = 0;
    std::int64_t rangeEndTick   = 0;
    bool        rangeActive    = false;

    // In-component state only - reopening resets to defaults (editor
    // state, not session state).
    int velocityStripH = kVelocityStripHDefault;
    // Starts collapsed - "don't show what you're not using" portastudio
    // bias. Toggle-CC button or top-edge drag reveals.
    int ccStripH       = 0;

    int resizeStartStripH = 0;
    int resizeStartMouseY = 0;

    // CC 1 (mod wheel) by default. 'L' cycles common (1, 7, 11, 64, 74).
    // Events on other CCs stay in region->ccs and pass through to the
    // synth at playback.
    int activeCcController = 1;

    // When != Off, rows not in scale get a translucent wash so in-scale
    // notes pop. 'S' opens the picker.
    enum class Scale { Off, Major, Minor, Dorian, Phrygian, Lydian, Mixolydian, Locrian };
    Scale scale = Scale::Off;
    int   scaleRoot = 0;   // 0=C, ... 11=B
    int draggedCcIdx = -1;

    // Pitch = 12 distinct hues (chord shapes pop). Velocity = blue
    // tinted by 0..127. Channel = 16-way hue cycle. 'C' cycles.
    enum class ColorMode { Pitch, Velocity, Channel };
    ColorMode colorMode = ColorMode::Pitch;

    // Grid = use snapTicks verbatim. Free = ignore snap on creation
    // (move/resize still snap). Triplet = ×2/3. Dotted = ×3/2.
    enum class NoteEntryMode { Grid, Free, Triplet, Dotted };
    NoteEntryMode noteEntryMode = NoteEntryMode::Grid;
    // false = honour click's exact row without extra snap (the row IS
    // the noteNumber, so we still snap visually - just no constraint pass).
    bool keySnap = true;
    std::int64_t dragOriginTick    = 0;
    int         dragOriginNoteNum = 0;
    std::int64_t dragNoteStartTick = 0;
    std::int64_t dragNoteLenTicks  = 0;

    // Reaper-style gold cursor - "I am here" signal independent of
    // the transport.
    std::int64_t editCursorTick = 0;

    // Defensible when regionIdx out of range so paint never branches.
    MidiRegion*       region();
    const MidiRegion* region() const;

    // Note-edit undo. Drag gestures: beginNoteEdit() at mouseDown,
    // commitNoteEdit() at mouseUp. Discrete edits (key / menu mutators):
    // snapshot a local `before`, mutate, then pushNoteEdit(before, name).
    // All collapse to one MidiRegionEditAction, skipped if nothing changed.
    void beginNoteEdit();
    void commitNoteEdit (const juce::String& transactionName);
    void pushNoteEdit (const MidiRegion& before, const juce::String& transactionName);
    bool       noteEditActive = false;
    MidiRegion noteEditBefore;

    int  yForNoteNumber (int noteNumber) const;
    int  noteNumberForY (int y) const;
    int  xForTick (std::int64_t tick) const;
    std::int64_t tickForX (int x) const;

    // onRightEdge = click in the note's last few px (resize vs move
    // intent). Returns -1 on empty grid.
    int hitTestNote (int x, int y, bool& onRightEdge) const;

    void paintKeyboard      (juce::Graphics&, juce::Rectangle<int> area);
    void paintNoteGrid      (juce::Graphics&, juce::Rectangle<int> area);
    void paintToolbar       (juce::Graphics&, juce::Rectangle<int> area);
    void paintBeatRuler     (juce::Graphics&, juce::Rectangle<int> area);
    void paintLoopPunchBrackets (juce::Graphics&, juce::Rectangle<int> ruler,
                                 juce::Rectangle<int> grid);
    void paintNotes         (juce::Graphics&, juce::Rectangle<int> area);
    void paintVelocityStrip (juce::Graphics&, juce::Rectangle<int> area);
    void paintCcStrip       (juce::Graphics&, juce::Rectangle<int> area);
    void paintEditCursor    (juce::Graphics&, juce::Rectangle<int> gridArea);

    int hitTestCcBar (int x, juce::Rectangle<int> stripArea) const;
    int hitTestVelocityBar (int x, juce::Rectangle<int> stripArea) const;

    juce::Colour colourForNote (const MidiNote& n) const noexcept;

    bool isNoteSelected (int idx) const noexcept;
    void clearSelection();
    void selectOnly (int idx);
    void toggleSelected (int idx);
    void addToSelection (int idx);
    // Snapshot every selected note into dragSnapshots so MoveNote
    // applies a consistent group delta. dragAnchor = note under cursor.
    void beginGroupDrag (int anchorIdx);
    // Clamps delta against group bounding box so no note ends up out
    // of [0, lengthInTicks] or [0, 127].
    void applyGroupMove (std::int64_t deltaTicks, int deltaPitch);
    void transposeSelected (int semitones);

    // strength 1.0 = fully snap; 0.5 = move halfway from original to
    // snapped (humanise without losing original feel).
    void quantizeSelected (std::int64_t gridTicks, float strength);

    // Empty selection = whole region (mirrors quantize). humanize
    // adds [-range..+range]% of 127 per note; setVelocity sets a
    // fixed value. Both clamp [1, 127].
    void humanizeVelocity (int rangePercent);
    void setVelocityFor (int value);
    void showVelocityPopup();

    void setChannelForSelected (int channel);
    void setLengthTicksForSelected (std::int64_t ticks);
    // Right-click promotes clicked note to selected if it wasn't.
    void showNotePropertiesPopup (int hitNoteIdx, juce::Point<int> screenPos);

    // Two notes are contiguous when the second's startTick is <= the
    // first's endTick. Lower-index note absorbs the others; absorbed
    // erased in descending index order. Selection cleared on success
    // (indices stale).
    void glueSelectedNotes();

    // Clone selected shifted right by selection span. New notes
    // appended; selection becomes the clones so follow-up nudge /
    // transpose acts on the copy.
    void duplicateSelectedNotes();

    void nudgeSelectedTicks (std::int64_t deltaTicks);
    bool isInScale (int noteNumber) const noexcept;
    // Coerce a pitch to the nearest in-scale note when "Key snap" is armed.
    int  snapPitchToScale (int noteNumber) const noexcept;

    void showQuantizePopup();
    void showScalePopup();
    void showRegionPropertiesPopup();
    int  nextRegionIndex (int delta) const;
    void navigateRegion  (int delta);
    void timerCallback() override;
    // -1 if off-screen. Used for narrow repaint regions.
    int lastPlayheadX = -1;
    int  transportPlayheadX (juce::Rectangle<int> gridArea) const;
    void paintTransportPlayhead (juce::Graphics&, juce::Rectangle<int> gridArea);
    void showColorModePopup();
    void showCcControllerPopup();

    // Real widgets (not paint-only chips) so JUCE handles dispatch /
    // hover / focus / accessibility.
    juce::Label        positionLabel;
    juce::Label        valueLabel;
    juce::Label        trackLabel;
    DuskComboBox     gridCombo;
    DuskComboBox     notesCombo;
    DuskComboBox     colorCombo;
    juce::ToggleButton keySnapToggle;
    // Both submit via MidiRegionEditAction so Cmd+Z reverts.
    juce::ToggleButton muteToggle;
    juce::ToggleButton lockToggle;

    class IconButton final : public juce::Button
    {
    public:
        enum class Glyph { Undo, Redo, Split, Glue, Quantize, Properties, ZoomFit, ZoomIn, ZoomOut, ToggleCc };
        IconButton (const juce::String& name, Glyph g);
        void paintButton (juce::Graphics&, bool isMouseOver, bool isButtonDown) override;
    private:
        Glyph glyph;
    };
    juce::TextButton chaseToggle { "Chase" };
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

    // Shared with AudioRegionEditor + TapeStrip via session.editMode.
    // Only Grab and Draw modify roll behaviour; others are no-ops here.
    std::unique_ptr<EditModeToolbar> editModeToolbar;

public:
    // MainComponent calls when a global hotkey flips session.editMode
    // while the modal is open so the toolbar repaints.
    void syncEditModeToolbar();
private:
    // Note grid (excludes toolbar / ruler / keyboard column / velocity +
    // CC strips / scrollbar / status bar). Used to gate edit-mode cursor
    // overrides so the toolbar / status bar / etc. keep the normal arrow.
    juce::Rectangle<int> noteGridArea() const noexcept;

    // Push (x, y) into the shared CursorOverlay if it's inside the
    // note grid and the active mode wants a glyph (Grab / Draw).
    // Called from BOTH mouseMove and mouseDrag - drag fires only
    // mouseDrag, so without this the overlay freezes at the click
    // point while the note follows the pointer underneath.
    void pushCursorPosition (int x, int y);

    void layoutIconRow (juce::Rectangle<int> area);

    // Splits every selected note that straddles editCursorTick; if
    // nothing selected, splits whichever note straddles the cursor.
    void splitSelectedAtCursor();
    void zoomFit();
    // > 1 zoom in, < 1 zoom out. Anchored on visible-grid centre.
    void zoomByFactor (float factor);
    void toggleCcLane();

    void layoutStatusBar (juce::Rectangle<int> area);

    juce::String formatBarBeat (std::int64_t tick) const;

    // -1 if nothing selected.
    int activeVelocity() const noexcept;

    void refreshStatusBarReadouts();
};
} // namespace duskstudio
