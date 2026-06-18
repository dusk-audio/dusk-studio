#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../engine/AudioEngine.h"
#include "../session/Session.h"

namespace duskstudio
{
// Arrangement view: one row per visible track, regions painted as
// rounded blocks, playhead vertical line, time ruler at top.
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

    // Minimum label-column width. The actual width (labelColW) grows to fit the
    // longest track name, up to kTrackLabelWMax, recomputed on name changes.
    static constexpr int kTrackLabelW    = 44;
    static constexpr int kTrackLabelWMax = 200;
    // Ruler bands:
    //   y=0..tick band        — time labels + tick marks
    //   y=tick..pill band     — markers row + loop/punch pills
    // Loop/punch solid bar paints the bottom 4 px of the pill band.
    static constexpr int kRulerTickBandH = 14;
    static constexpr int kRulerPillBandH = 16;
    static constexpr int kRulerH         = kRulerTickBandH + kRulerPillBandH;
    // Track-row height is vertically zoomable (Cmd+Shift+wheel). Default
    // matches the original fixed 14 px; clamped to [kRowHMin, kRowHMax].
    static constexpr int kRowHMin     = 10;
    static constexpr int kRowHDefault = 14;
    static constexpr int kRowHMax     = 80;
    static constexpr int kRowGap      = 1;

    // Rows = armed ∪ has-content (or every track when SHOW ALL is on).
    int naturalHeight() const noexcept;
    // Upper bound for layout code that needs an estimate before any
    // TapeStrip instance exists.
    static int maxNaturalHeight() noexcept;

    void refreshModeCursor();

    // setMouseCursor + push the matching glyph into the CursorOverlay. When
    // c is NoCursor the overlay paints the Grab/Cut glyph at (x,y); any
    // other cursor clears it so only the native cursor shows.
    void setHoverCursor (const juce::MouseCursor& c, int x, int y, juce::Range<int> cutLine = {});

    // Called from MainComponent's keyboard handler. Returns true if the
    // op happened (caller decides whether to swallow the keypress).
    // All edits route through engine's UndoManager.
    bool copySelectedRegion();
    bool cutSelectedRegion();
    bool pasteAtPlayhead();
    bool deleteSelectedRegion();
    bool splitSelectedAtPlayhead();

    // Open the marker-name text input (current name pre-selected, Enter
    // commits, Escape keeps it). Used by the context-menu Rename item and
    // by the create paths so a fresh marker can be named in one shot.
    void promptRenameMarker (int markerIdx, const juce::String& title = "Rename marker");
    // Clone immediately after the original via PasteRegionAction.
    bool duplicateSelectedRegion();
    // Negative deltaSamples moves earlier. Clamped at zero.
    bool nudgeSelectedRegion (juce::int64 deltaSamples);
    // Forward: next previous take becomes live, old live moves to back.
    // Backward: last previous take becomes live, old moves to front.
    bool cycleSelectedTakeForward();
    bool cycleSelectedTakeBackward();

    // Single-click is reserved for direct manipulation. Double-click
    // opens the dedicated editor — one mental model across audio + MIDI.
    std::function<void (int trackIdx, int regionIdx)> onMidiRegionDoubleClicked;
    std::function<void (int trackIdx, int regionIdx)> onAudioRegionDoubleClicked;

    // CursorOverlay sink — MainComponent wires these so the strip can push
    // its local mouse position into the shared overlay (which can't poll
    // Desktop::getMousePosition reliably on Wayland). Grab/Cut mode hides
    // the native cursor with NoCursor; without these the overlay never gets
    // a position and the pointer simply vanishes over the lanes.
    std::function<void (juce::Component&, juce::Point<int>, EditMode,
                          juce::Range<int>)> onMouseMovedForCursor;
    std::function<void()> onMouseExitedForCursor;

    // On-screen rect of a region, in TapeStrip-local coords, for the
    // editor expand/collapse animation. Empty when the index is invalid
    // or the region is fully scrolled off the visible track area.
    juce::Rectangle<int> audioRegionScreenRect (int trackIdx, int regionIdx) const noexcept;
    juce::Rectangle<int> midiRegionScreenRect  (int trackIdx, int regionIdx) const noexcept;

    // trackHint = row under the drop, -1 if dropped on ruler / outside.
    // Host (batch-import) picks adjacent tracks for subsequent files.
    std::function<void (juce::Array<juce::File> files,
                         juce::int64 timelineStart,
                         int trackHint)> onFilesDropped;

    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void fileDragEnter (const juce::StringArray& files, int x, int y) override;
    void fileDragMove  (const juce::StringArray& files, int x, int y) override;
    void fileDragExit  (const juce::StringArray& files) override;
    void filesDropped  (const juce::StringArray& files, int x, int y) override;

    int  getSelectedTrack() const noexcept { return selectedTrack; }

    // From ChannelStripComponent click etc. Clears region selection
    // (gesture wasn't region-specific) and repaints.
    void setSelectedTrack (int t) noexcept;

    // anchorX >= 0 = zoom anchors on that pixel so the sample under
    // the cursor stays put.
    void zoomByFactor (float factor, int anchorX = -1);
    void zoomFit() noexcept;

private:
    void timerCallback() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    juce::Rectangle<int> labelColumnBounds() const noexcept;
    juce::Rectangle<int> rulerBounds() const noexcept;
    juce::Rectangle<int> tracksColumnBounds() const noexcept;

    // Width of the left label column, grown to fit the longest track name
    // (clamped [kTrackLabelW, kTrackLabelWMax]). Recomputed by
    // refreshLabelColumnWidth() on construction, layout, and name changes.
    int  labelColW { kTrackLabelW };
    void refreshLabelColumnWidth();
    // Empty rect when trackIdx is collapsed — callers iterating must
    // respect this so hit tests / painters skip hidden rows.
    juce::Rectangle<int> rowBounds (int trackIdx) const noexcept;

    // Cheap (24 iter). Called from resized(), timer poll, SHOW ALL
    // click. Relayouts + repaints only on actual change. relayoutParent
    // is false when the caller (MainComponent::resized) is already mid-
    // layout, to avoid re-entrant resized().
    void rebuildVisibleTrackOrder (bool relayoutParent = true);
    // -1 if collapsed.
    int  visualRowForTrack (int trackIdx) const noexcept;

    double pixelsPerSecond() const noexcept;
    juce::int64 sampleAtX (int x) const noexcept;
    int xForSample (juce::int64 s) const noexcept;

    // op = which sub-area (body / edges / fade handles / take badge).
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
    // True when (x,y) is inside a MIDI region's painted body on its lane.
    // hitTestRegion only covers audio regions; this is the MIDI counterpart
    // used for the Grab hand cursor over MIDI regions (move-only on the lane).
    bool overMidiRegionBody (int x, int y) const noexcept;
    // Tested only inside the ruler band.
    int hitTestMarker (int x, int y) const noexcept;
    // Tested only inside the ruler tick band (top), where tempo points live.
    // hitTestTempoPoint returns this for the bar-1 base handle shown when no
    // tempo map exists yet (lets the user set the starting tempo here).
    static constexpr int kTempoBaseHandle = -2;
    int hitTestTempoPoint (int x, int y) const noexcept;

    // Pills reposition that endpoint; bar drags translate the whole
    // range preserving length.
    enum class BracketHit
    {
        None,
        LoopIn, LoopOut, LoopBar,
        PunchIn, PunchOut, PunchBar,
    };
    BracketHit hitTestBracket (int x, int y) const noexcept;
    void rebuildPlaybackIfStopped();
    void showRegionContextMenu (const RegionHit&, juce::Point<int> screenPos);
    // Smaller than audio version — Rename + Color + mute/lock, all routed
    // through MidiRegionEditAction for undo.
    void showMidiRegionContextMenu (int trackIdx, int regionIdx,
                                       juce::Point<int> screenPos);

    // Undoable MIDI-region mute/lock toggle from the tape-strip menu.
    void commitMidiRegionToggle (int trackIdx, int regionIdx, const juce::String& name,
                                  std::function<void (duskstudio::MidiRegion&)> mutate);

    // Tempo edits, all driven from the ruler's right-click menu. Every edit
    // routes through commitTempoPoints so it's a single undoable transaction.
    void commitTempoPoints (std::vector<duskstudio::TempoPoint> after,
                             const juce::String& name);
    void promptAddTempoPoint (juce::int64 sample);   // prompt, add on accept
    void editTempoPointBpm (juce::int64 atSample);
    // Edit the song's starting tempo when no tempo map exists yet (the bar-1
    // base handle). hitTestTempoPoint returns kTempoBaseHandle for it.
    void editBaseTempo();
    void deleteTempoPoint (juce::int64 atSample);
    void paintTempoPoints (juce::Graphics&);

    Session& session;
    AudioEngine& engine;

    juce::int64 lastPlayhead = -1;

    // Playhead band repaint rides the display's vblank so motion steps
    // once per frame. A free-running 30 Hz Timer beats against the
    // compositor's refresh and reads as stutter. The timer keeps the
    // cheap state polling (names, colours, loop/punch, recording's
    // full-frame repaint).
    void updatePlayheadBand();
    juce::VBlankAttachment vBlankAttachment;

    // 1.0 = auto-fit-all. zoomFit resets to 1 + zeroes scroll.
    float userZoomFactor = 1.0f;
    // Leftmost visible sample when zoomed. 0 when factor == 1. Wheel +
    // zoom clamp it so the visible window stays inside content.
    juce::int64 scrollSamples = 0;

    // Bold-text LookAndFeel for the SHOW ALL pill (TextButton has no setFont).
    // Declared before showAllToggle so the button is destroyed first.
    struct PillButtonLnF : juce::LookAndFeel_V4
    {
        juce::Font getTextButtonFont (juce::TextButton&, int h) override
        {
            return juce::Font (juce::FontOptions (juce::jmin (12.0f, (float) h * 0.8f),
                                                   juce::Font::bold));
        }
    };
    PillButtonLnF pillButtonLnF;

    juce::TextButton zoomOutButton { "-" };
    juce::TextButton zoomInButton  { "+" };
    juce::TextButton zoomFitButton { "Fit" };
    juce::TextButton snapToggle    { "SNAP" };
    juce::TextButton showAllToggle { "ALL" };
    bool showAllTracks = false;

    // The track range the console currently shows (the active bank's
    // strips for this window width). MainComponent pushes this on every
    // resize + bank change. The timeline always shows these rows so it
    // mirrors the mixer bank, plus any track with content - so an empty
    // session on bank 7-12 shows tracks 7-12, not one row + tall faders.
    int consoleFirstTrack   = 0;
    int consoleVisibleCount = Session::kNumTracks;

public:
    // Tell the strip which tracks the console shows (active bank's
    // [firstTrack, firstTrack+count) for the current width). The timeline
    // mirrors this range. Safe to call every resize (no-op when same).
    void setConsoleVisibleRange (int firstTrack, int count);

private:
    // Session-track indices in display order. Index in this vector IS
    // the visual row; value is the Session track. Rebuilt by
    // rebuildVisibleTrackOrder().
    std::vector<int> visibleTrackOrder;

    // Vertical zoom: per-track row height (Cmd+Shift+wheel). When the
    // resulting content is taller than the strip's capped height, rows
    // scroll vertically by rowScrollY (plain wheel deltaY).
    int rowHeight  = kRowHDefault;
    int rowScrollY = 0;
    // Pixel height of all visible rows at the current rowHeight (content
    // extent below the ruler). Used to clamp rowScrollY + decide overflow.
    int rowsContentHeight() const noexcept;
    void clampRowScroll() noexcept;

    // Without these, the strip's only repaint trigger is playhead
    // motion — renaming a track wouldn't reflect until the next play.
    std::array<juce::String, Session::kNumTracks> lastNames;
    std::array<juce::Colour, Session::kNumTracks> lastColours;

    bool        lastLoopEnabled  = false;
    juce::int64 lastLoopStart    = -1;
    juce::int64 lastLoopEnd      = -1;
    bool        lastPunchEnabled = false;
    juce::int64 lastPunchIn      = -1;
    juce::int64 lastPunchOut     = -1;

    // Full-repaint on Stopped <-> Recording — the thin playhead band
    // isn't wide enough to cover the live-recording overlay's first
    // paint at Record-press.
    bool        lastIsRecording  = false;

    // One drag active at a time.
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

        // Captured at mouseDown by (track, regionIdx) — the latter can
        // be reordered between mouseDown and mouseUp by concurrent
        // record / undo, so the pair at capture time is the stable form.
        // Empty = single-region drag.
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

    // MIDI is move-only here (trim via piano roll edge handles).
    // Audio carries fade / gain / trim state in the bigger drag above.
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

    // Ruler drag-to-create-range. A drag on the ruler sweeps a neutral
    // highlight; on release a popup asks whether the range is a loop or a
    // punch. A drag shorter than ~1024 samples is treated as a click and
    // just seeks the playhead — so plain click-to-seek still works.
    struct RulerSelection
    {
        bool active     = false;
        juce::int64 originSample  = 0;
        juce::int64 currentSample = 0;
    };
    RulerSelection rulerSelection;

    // moved=false at release = click (seek); true = drag (update marker).
    struct MarkerDrag
    {
        bool active   = false;
        bool moved    = false;
        int  index    = -1;
        juce::int64 originSample = 0;
        juce::int64 mouseDownSample = 0;
    };
    MarkerDrag markerDrag;

    // origStart/End captured pre-drag so whole-bar moves translate by
    // delta without compounding rounding.
    struct BracketDrag
    {
        bool       active = false;
        BracketHit type   = BracketHit::None;
        juce::int64 mouseDownSample = 0;
        juce::int64 origStart = 0;
        juce::int64 origEnd   = 0;
    };
    BracketDrag bracketDrag;

    // Tempo-marker reposition drag. `orig` is the whole pre-drag map so the
    // dragged point keeps a stable index even as setPoints re-sorts the live
    // copy, and so mouseUp can push one before/after undo transaction. The
    // bar-1 anchor (timelineSamples == 0) is never dragged.
    struct TempoDrag
    {
        bool active = false;
        bool moved  = false;
        int  index  = -1;
        juce::int64 mouseDownSample = 0;
        std::vector<duskstudio::TempoPoint> orig;
    };
    TempoDrag tempoDrag;

    // Most-recently-clicked region. Single-region ops act on this;
    // group ops also include additionalSelections. Cleared on
    // undo/redo (action might have shifted indices).
    int selectedTrack    = -1;
    int selectedRegion   = -1;

    int  dropHoverTrack = -1;
    int  dropHoverX     = -1;
    bool dropAccepted   = false;

    // Audio + MIDI share a vector index space within a track but are
    // distinct types — separate selection slots avoid "which type is
    // index 3?" ambiguity.
    int selectedMidiTrack  = -1;
    int selectedMidiRegion = -1;

    // Primary NOT included. Sorted-deduped so group ops don't double-
    // iterate. Cleared when primary collapses to nothing.
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

    bool isRegionSelected (int track, int idx) const noexcept;
    std::vector<RegionId> allSelectedRegions() const;
    void clearAllSelections() noexcept;
    // Add or remove if already present — Shift / Cmd-click extends
    // without collapsing back to a single anchor.
    void toggleRegionSelected (int track, int idx);

    // Drives affordance visibility — fade handles only paint on
    // hovered / selected region.
    int hoveredTrack  = -1;
    int hoveredRegion = -1;
    void mouseExit (const juce::MouseEvent&) override;

    // Rubber-band box-select. Active during Shift / Cmd + drag from
    // empty track-row space. Audio regions whose painted rect intersects
    // get added; MIDI skipped (its click path is separate). Screen
    // coords so painter + intersection test share frame of reference.
    bool                  rubberBandActive = false;
    juce::Rectangle<int>  rubberBand;

    // Reaper-style vertical guide drawn at the fade boundary while the
    // user drags a fade-in or fade-out handle. -1 = inactive.
    int                   fadeGuideX = -1;
};
} // namespace duskstudio
