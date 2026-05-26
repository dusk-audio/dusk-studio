#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../engine/AudioEngine.h"
#include "../session/Session.h"

namespace duskstudio
{
// Phase 2 minimum tape strip:
//   - 16 horizontal rows (one per track), color-coded with track accent
//   - Recorded regions drawn as rounded blocks per row
//   - Vertical playhead line that follows the transport
//   - Time ruler at the top (seconds)
//   - Click anywhere on a row to seek the playhead to that position
// Region drag/split/trim, markers, loop brackets, and horizontal scroll all
// arrive in later phases per the spec; this component focuses on visibility.
class TapeStrip final : public juce::Component,
                         public juce::FileDragAndDropTarget,
                         private juce::Timer,
                         private juce::ChangeListener
{
public:
    TapeStrip (Session& sessionRef, AudioEngine& engineRef);
    ~TapeStrip() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    void mouseDown        (const juce::MouseEvent&) override;
    void mouseDrag        (const juce::MouseEvent&) override;
    void mouseUp          (const juce::MouseEvent&) override;
    void mouseMove        (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove   (const juce::MouseEvent&,
                              const juce::MouseWheelDetails&) override;

    static constexpr int kTrackLabelW = 44;
    // Ruler is split into three vertical bands:
    //   y=0..kRulerTickBandH        - time-label row + tick marks
    //   y=kRulerTickBandH..kRulerH  - markers row + loop/punch pills
    // Loop/punch's solid bracket bar paints the bottom 4 px of the lower
    // band so it visually attaches the pills to the timeline.
    static constexpr int kRulerTickBandH = 14;
    static constexpr int kRulerPillBandH = 16;
    static constexpr int kRulerH         = kRulerTickBandH + kRulerPillBandH;  // 30
    static constexpr int kTrackRowH   = 14;
    static constexpr int kRowGap      = 1;

    // Total pixel height of the strip sized to the CURRENT row count.
    // Rows visible = armed tracks ∪ tracks with audio/midi content (plus
    // every track when SHOW ALL is on). Empty unarmed tracks collapse so
    // a 24-track session doesn't burn half the screen on empty rows.
    int naturalHeight() const noexcept;
    // Conservative upper bound: every track visible. Used by layout code
    // that needs a static size estimate before a TapeStrip instance
    // exists.
    static int maxNaturalHeight() noexcept;

    // Clipboard / selection-driven editing surface, called from
    // MainComponent's keyboard handler. Each returns true if the operation
    // happened (so the caller can decide whether to swallow the keypress).
    // All edits go through the engine's UndoManager.
    bool copySelectedRegion();
    bool cutSelectedRegion();
    bool pasteAtPlayhead();
    bool deleteSelectedRegion();
    // Split the selected audio region at the current transport playhead.
    // No-op (returns false) when nothing is selected, the playhead is
    // outside the region's range, or the region is too short to split.
    // Same SplitRegionAction the right-click menu uses, so undo/redo
    // behaviour matches.
    bool splitSelectedAtPlayhead();
    // Duplicate the selected region. The clone is positioned right
    // after the original (timelineStart + lengthInSamples). Routes
    // through PasteRegionAction so undo/redo work. Returns false if
    // nothing is selected.
    bool duplicateSelectedRegion();
    // Nudge the selected region's timelineStart by deltaSamples,
    // routed through RegionEditAction so undo/redo work. Clamps so
    // the region can't move past the timeline origin. deltaSamples is
    // signed - negative values move the region earlier.
    bool nudgeSelectedRegion (juce::int64 deltaSamples);
    // Cycle the selected region's take stack. Forward = the next
    // previous take becomes live, the old live moves to the back of
    // previousTakes. Backward = the last previous take becomes live,
    // the old live moves to the front. Returns false when nothing is
    // selected or the region has no take history. Routes through
    // RegionEditAction / MidiRegionEditAction so undo/redo work.
    bool cycleSelectedTakeForward();
    bool cycleSelectedTakeBackward();

    // Double-click hooks. Single-click is reserved for direct
    // manipulation (move / trim / fade / gain on audio; future drag
    // for MIDI). Double-click opens the dedicated editor modal:
    // PianoRollComponent for MIDI, AudioRegionEditor for audio. Both
    // share the same gesture so users can rely on one mental model
    // ("double-click any region to edit it").
    std::function<void (int trackIdx, int regionIdx)> onMidiRegionDoubleClicked;
    std::function<void (int trackIdx, int regionIdx)> onAudioRegionDoubleClicked;

    // File drag-and-drop callback. Fires when the user drops an audio /
    // MIDI files onto the strip. trackHint is the row under the drop
    // (0..15) or -1 if dropped on the ruler / outside any row; the host
    // can use it as the FIRST file's pre-selection. timelineStart is
    // the timeline sample the drop X-coordinate maps to. The host (a
    // batch-import chain) is responsible for picking adjacent tracks
    // for subsequent files in the array.
    std::function<void (juce::Array<juce::File> files,
                         juce::int64 timelineStart,
                         int trackHint)> onFilesDropped;

    // juce::FileDragAndDropTarget. Accept any file whose extension is
    // in the WAV / AIFF / FLAC / MID set; filesDropped routes to
    // onFilesDropped after computing timelineStart + trackHint.
    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void fileDragEnter (const juce::StringArray& files, int x, int y) override;
    void fileDragMove  (const juce::StringArray& files, int x, int y) override;
    void fileDragExit  (const juce::StringArray& files) override;
    void filesDropped  (const juce::StringArray& files, int x, int y) override;

    // Selected track index (the track that owns the most-recently-clicked
    // region) or -1 if nothing is selected. Used by keyboard shortcuts in
    // MainComponent to target arm / solo / mute toggles at "the focus
    // track". Returning the internal selection state avoids inventing a
    // separate selection model.
    int  getSelectedTrack() const noexcept { return selectedTrack; }

    // Set track focus from outside (e.g. ChannelStripComponent click).
    // Clears any region selection since the user's gesture wasn't
    // region-specific. Repaints so the highlighted row updates.
    void setSelectedTrack (int t) noexcept;

    // User-controlled horizontal zoom.
    //   • zoomByFactor multiplies the current factor. If `anchorX` is
    //     supplied (>= 0) the zoom anchors on that x-pixel so the
    //     sample currently under that cursor stays put.
    //   • zoomFit resets factor to 1.0 + scroll to 0 so the whole
    //     timeline auto-fits.
    void zoomByFactor (float factor, int anchorX = -1);
    void zoomFit() noexcept;

private:
    void timerCallback() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    juce::Rectangle<int> labelColumnBounds() const noexcept;
    juce::Rectangle<int> rulerBounds() const noexcept;
    juce::Rectangle<int> tracksColumnBounds() const noexcept;
    // Returns the row rect for `trackIdx` (a Session-track index 0..N-1)
    // when that track is currently visible, or an empty rect when the
    // track is collapsed out of view. Callers that iterate must respect
    // the empty-rect contract so hit tests / painters skip hidden rows.
    juce::Rectangle<int> rowBounds (int trackIdx) const noexcept;

    // Rebuilds `visibleTrackOrder` from session state (armed flag + region
    // counts) and the SHOW ALL override. Cheap (24 iterations). Called
    // from resized(), timer-driven state polling, and the SHOW ALL click
    // handler. If the visible set actually changed we relayout + repaint.
    void rebuildVisibleTrackOrder();
    // Visual position (0..visibleTrackOrder.size()-1) of a Session-track
    // index, or -1 if the track is collapsed.
    int  visualRowForTrack (int trackIdx) const noexcept;

    double pixelsPerSecond() const noexcept;
    juce::int64 sampleAtX (int x) const noexcept;
    int xForSample (juce::int64 s) const noexcept;

    // Region hit-testing for the editing surface. `op` reports which
    // sub-area of the region the cursor lies over (body / left edge /
    // right edge / top-corner fade handles); kEdgeHitPx controls how
    // wide the resize gutter is, kFadeHandleH the height of the top
    // band reserved for fade handles.
    enum class RegionOp { None, Move, TrimStart, TrimEnd, TakeBadge, FadeIn, FadeOut, AdjustGain };
    static constexpr int kFadeHandleH = 6;
    static constexpr int kFadeHitPx   = 5;
    static constexpr int kEdgeHitPx = 6;
    struct RegionHit
    {
        int track    = -1;
        int regionIdx = -1;
        RegionOp op  = RegionOp::None;
    };
    RegionHit hitTestRegion (int x, int y) const noexcept;
    // Marker flag hit-test. Returns the index of the marker whose flag
    // sits under (x, y), or -1. Tested only inside the ruler band.
    int hitTestMarker (int x, int y) const noexcept;

    // Loop / punch bracket hit-test. Pills (in/out) and the bar between
    // them are draggable: pill drags reposition that endpoint; bar drags
    // translate the whole range while preserving its length.
    enum class BracketHit
    {
        None,
        LoopIn, LoopOut, LoopBar,
        PunchIn, PunchOut, PunchBar,
    };
    BracketHit hitTestBracket (int x, int y) const noexcept;
    void rebuildPlaybackIfStopped();
    void showRegionContextMenu (const RegionHit&, juce::Point<int> screenPos);
    // Right-click context for MIDI regions. Smaller than the audio
    // version (no edge gutters, no fade handles, no take badge yet)
    // - just Rename and Color. Mutates via midiRegions.currentMutable()
    // since MIDI regions don't currently have an undoable edit-action
    // surface; matches how the piano roll handles per-note edits.
    void showMidiRegionContextMenu (int trackIdx, int regionIdx,
                                       juce::Point<int> screenPos);

    Session& session;
    AudioEngine& engine;

    juce::int64 lastPlayhead = -1;

    // Horizontal zoom factor. 1.0 = auto-fit-all-content (legacy behaviour).
    // > 1 magnifies. zoomFit resets to 1.0 and zeroes scroll.
    float userZoomFactor = 1.0f;
    // Horizontal scroll offset in samples. The leftmost visible sample
    // when zoomed in. Always 0 when zoom = 1 (full content visible).
    // Wheel + zoom interactions clamp it so the visible window stays
    // inside the content range.
    juce::int64 scrollSamples = 0;

    // Tiny zoom HUD glued to the top-right of the ruler band. Snap is
    // co-located here (moved from the transport bar) so the gesture
    // controls that drive arrangement-surface behaviour all live on the
    // arrangement surface itself.
    juce::TextButton zoomOutButton { "-" };
    juce::TextButton zoomInButton  { "+" };
    juce::TextButton zoomFitButton { "Fit" };
    juce::TextButton snapToggle    { "SNAP" };
    // SHOW ALL toggle in the ruler band — when on, every Session track
    // gets a row regardless of armed / content state. Off by default
    // (rows collapse to armed ∪ has-content so 24-track sessions don't
    // burn vertical space on empty rows).
    juce::TextButton showAllToggle { "ALL" };
    bool showAllTracks = false;
    // Current visible row order. Session-track indices in display order
    // (top → bottom). Empty entries do NOT appear here — index in this
    // vector IS the visual row, the value at that index is the actual
    // Session track. Rebuilt by rebuildVisibleTrackOrder().
    std::vector<int> visibleTrackOrder;
    // Cache of the last computed armed-or-has-content set + show-all
    // flag, so timer-driven re-checks can skip the relayout when nothing
    // changed. Compare-and-rebuild pattern.
    std::array<bool, Session::kNumTracks> lastTrackHadContent {};
    std::array<bool, Session::kNumTracks> lastTrackArmed      {};

    // Caches so the timer can detect track color / name changes and repaint.
    // Without these, the strip's only repaint trigger is playhead motion, so
    // renaming a track or changing its color in the mixer wouldn't reflect
    // in the summary until the next play.
    std::array<juce::String, Session::kNumTracks> lastNames;
    std::array<juce::Colour, Session::kNumTracks> lastColours;

    // Loop / punch state caches - same pattern, so a TransportBar toggle
    // (or a session load) repaints the brackets without the user having to
    // move the playhead.
    bool        lastLoopEnabled  = false;
    juce::int64 lastLoopStart    = -1;
    juce::int64 lastLoopEnd      = -1;
    bool        lastPunchEnabled = false;
    juce::int64 lastPunchIn      = -1;
    juce::int64 lastPunchOut     = -1;

    // Transport-state cache so the timer can full-repaint on
    // Stopped <-> Recording transitions - the existing thin-band
    // playhead repaint isn't wide enough to cover the live-recording
    // overlay's initial paint at the moment of Record-press.
    bool        lastIsRecording  = false;

    // In-flight region drag. Captured on mouseDown when the click hits a
    // region's body or edge gutter; updated on mouseDrag; finalised on
    // mouseUp. Only one drag can be active at a time.
    struct ActiveDrag
    {
        int track     = -1;
        int regionIdx = -1;
        RegionOp op   = RegionOp::None;
        juce::int64 mouseDownSample = 0;
        juce::int64 origTimelineStart = 0;
        juce::int64 origLength        = 0;
        juce::int64 origSourceOffset  = 0;
        juce::int64 origFadeIn        = 0;
        juce::int64 origFadeOut       = 0;
        float       origGainDb        = 0.0f;

        // Per-additional-selection orig state captured at mouseDown
        // for group Move / AdjustGain drags. Empty vector = single-
        // region drag (only the anchor). track / regionIdx point
        // back into Session, NOT into additionalSelections - the
        // latter can be reordered between mouseDown and mouseUp by
        // a concurrent record / undo, so identifying by (track,
        // regionIdx) at capture time is the stable form.
        struct AdditionalOrig
        {
            int track;
            int regionIdx;
            juce::int64 origTimelineStart;
            float       origGainDb;
        };
        std::vector<AdditionalOrig> additional;
    };
    ActiveDrag drag;

    // In-flight MIDI region drag. Single-click on a MIDI region body
    // captures origState + origTimelineStart; mouseDrag moves the
    // region by the cursor delta (snap-to-beat when session.snapToGrid
    // is on); mouseUp finalises through MidiRegionEditAction so Cmd+Z
    // reverts. Audio regions go through the bigger `drag` machinery
    // above which carries fade / gain / trim state; MIDI is move-only
    // for now (trim is via piano roll edge handles).
    struct MidiActiveDrag
    {
        int track     = -1;
        int regionIdx = -1;
        juce::int64 mouseDownSample   = 0;
        juce::int64 origTimelineStart = 0;
        MidiRegion  origState;
        bool active() const noexcept { return track >= 0 && regionIdx >= 0; }
        void clear() noexcept
        {
            track = -1;
            regionIdx = -1;
            mouseDownSample = 0;
            origTimelineStart = 0;
            origState = MidiRegion{};
        }
    };
    MidiActiveDrag midiDrag;

    // In-flight ruler selection. Click+drag on the ruler defines a
    // candidate range that's painted as a neutral highlight. On mouseUp
    // we offer a menu ("Set loop here" / "Set punch in/out here") so the
    // user picks what the range is for - dragging itself doesn't
    // commit anything to transport state.
    struct RulerSelection
    {
        bool active     = false;
        juce::int64 originSample  = 0;
        juce::int64 currentSample = 0;
    };
    RulerSelection rulerSelection;

    // In-flight marker drag. Click on a flag captures the marker; if the
    // user moves before releasing, we treat it as a drag (update marker
    // position); otherwise it's a click (seek to marker on mouseUp).
    struct MarkerDrag
    {
        bool active   = false;
        bool moved    = false;     // true once drag delta exceeds threshold
        int  index    = -1;
        juce::int64 originSample = 0;
        juce::int64 mouseDownSample = 0;
    };
    MarkerDrag markerDrag;

    // In-flight loop / punch bracket drag. type identifies which endpoint
    // (or whole-bar) is being moved; origStart/origEnd are the bracket's
    // pre-drag bounds, captured at mouseDown so a "move whole bar" drag
    // can translate by delta without compounding rounding.
    struct BracketDrag
    {
        bool       active = false;
        BracketHit type   = BracketHit::None;
        juce::int64 mouseDownSample = 0;
        juce::int64 origStart = 0;
        juce::int64 origEnd   = 0;
    };
    BracketDrag bracketDrag;

    // Primary / anchor selection - the most-recently-clicked region.
    // Single-region operations (clipboard, paste, anchor for drag deltas)
    // act on this; group operations also include `additionalSelections`.
    // -1 / -1 means nothing selected. Cleared on undo/redo because the
    // action might have shifted indices.
    int selectedTrack    = -1;
    int selectedRegion   = -1;

    // File-drop visual feedback. Highlighted row repaints with a soft
    // accent so the user sees which track + timeline position the drop
    // would land at. Reset on fileDragExit / filesDropped.
    int  dropHoverTrack = -1;
    int  dropHoverX     = -1;
    bool dropAccepted   = false;

    // MIDI region selection. Mirrors selectedTrack / selectedRegion but
    // for the MIDI side, so clicking a MIDI region paints it highlighted
    // the same way an audio region does. Held separately because audio
    // and MIDI regions share the same vector index space within a track
    // but are distinct types — folding them into one slot would make
    // "which type is index 3?" ambiguous. -1 / -1 = nothing selected.
    int selectedMidiTrack  = -1;
    int selectedMidiRegion = -1;

    // Extra regions selected via Shift / Cmd-click on top of the
    // primary. The primary is NOT included in this vector - the full
    // selection set is `(selectedTrack, selectedRegion)` ∪ this.
    // Sorted-and-deduped to keep group ops (delete, nudge, gain) from
    // double-iterating the same region. Cleared whenever the primary
    // collapses to "nothing" (plain-click on empty space, undo, etc.).
    struct RegionId
    {
        int track;
        int regionIdx;
        bool operator== (const RegionId& other) const noexcept
        {
            return track == other.track && regionIdx == other.regionIdx;
        }
        bool operator< (const RegionId& other) const noexcept
        {
            return track < other.track
                || (track == other.track && regionIdx < other.regionIdx);
        }
    };
    std::vector<RegionId> additionalSelections;

    // True if (track, idx) is the primary OR appears in additional.
    bool isRegionSelected (int track, int idx) const noexcept;
    // Full selection list (primary first if set, then additional).
    std::vector<RegionId> allSelectedRegions() const;
    // Reset both primary and additional. Used by changeListenerCallback
    // (undo/redo may have shifted indices) and by plain-click on empty
    // timeline.
    void clearAllSelections() noexcept;
    // Add (or remove if already present) a region to the selection.
    // Used by Shift / Cmd-click to extend the selection without
    // collapsing back to a single anchor.
    void toggleRegionSelected (int track, int idx);

    // Hover state - set in mouseMove, cleared in mouseExit. Drives
    // affordance visibility (fade handles only paint on the hovered
    // or selected region, otherwise the timeline reads cluttered).
    int hoveredTrack  = -1;
    int hoveredRegion = -1;
    void mouseExit (const juce::MouseEvent&) override;

    // Rubber-band box-select state. Active during a Shift / Cmd +
    // drag from empty track-row space. Origin is captured on
    // mouseDown; current is updated each mouseDrag; on mouseUp every
    // audio region whose painted rect intersects the box gets added
    // to the multi-selection. MIDI regions are skipped (their
    // click-to-open-roll path is separate). Rectangle stored in
    // screen coords so the painter and intersection test share the
    // same frame of reference.
    bool                  rubberBandActive = false;
    juce::Rectangle<int>  rubberBand;
};
} // namespace duskstudio
