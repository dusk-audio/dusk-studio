#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../engine/AudioEngine.h"
#include "../session/Session.h"
#include "../session/TrackMove.h"
#include "../foundation/MessageThread.h"
#include "WheelScroll.h"

#include <algorithm>
#include <optional>
#include <utility>

namespace duskstudio
{
// Arrangement view: one row per visible track, regions painted as
// rounded blocks, playhead vertical line, time ruler at top.
class TapeStrip final : public juce::Component,
                         public juce::FileDragAndDropTarget,
                         private dusk::Timer,
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
    //   y=0..tick band        - time labels + tick marks
    //   y=tick..pill band     - markers row + loop/punch pills
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
    bool nudgeSelectedRegion (std::int64_t deltaSamples);
    // Alt+T (forward) and Alt+Shift+T: see cycleTake.
    bool cycleSelectedTake (bool forward);

    // Single-click is reserved for direct manipulation. Double-click
    // opens the dedicated editor - one mental model across audio + MIDI.
    std::function<void (int trackIdx, int regionIdx)> onMidiRegionDoubleClicked;
    std::function<void (int trackIdx, int regionIdx)> onAudioRegionDoubleClicked;
    // A single click on a track's name in the label column, once it can no
    // longer turn into a double-click.
    std::function<void (int trackIdx)> onTrackLabelClicked;
    // A name dragged to another row: the move it asks for, and the track whose
    // name was dragged, by its slot before the move. The host commits it.
    std::function<void (const TrackMovePlan& plan, int draggedTrack)> onTrackMoveDropped;
    // Ends a name drag without moving anything; true when one was under way. A
    // press that has not dragged yet is left to end as a click.
    bool cancelTrackMoveDrag();
    // Around every track move, its undo and redo included. Before: the rename
    // and any name drag, which name tracks by slot, are dropped. After: the
    // picked tracks and any region drag follow their tracks, region picks are
    // cleared and the rows are rebuilt.
    void prepareForTrackMove();
    void followTrackMove (const TrackMovePlan& plan);

    // CursorOverlay sink - MainComponent wires these so the strip can push
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

    // timelineStart = the playhead, or the sample under the pointer when Alt
    // is held. trackHint = row under the drop, -1 if dropped on ruler /
    // outside. Host (batch-import) picks adjacent tracks for subsequent files.
    std::function<void (juce::Array<juce::File> files,
                         std::int64_t timelineStart,
                         int trackHint)> onFilesDropped;

    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void fileDragEnter (const juce::StringArray& files, int x, int y) override;
    void fileDragMove  (const juce::StringArray& files, int x, int y) override;
    void fileDragExit  (const juce::StringArray& files) override;
    void filesDropped  (const juce::StringArray& files, int x, int y) override;

    int  getSelectedTrack() const noexcept { return selectedTrack; }
    // The tracks picked in the name column, ascending; otherwise the selected
    // track alone, or nothing.
    std::vector<int> getSelectedTracks() const;

    // From ChannelStripComponent click etc. Clears region selection
    // (gesture wasn't region-specific) and repaints.
    void setSelectedTrack (int t);

    // anchorX >= 0 = zoom anchors on that pixel so the sample under
    // the cursor stays put.
    void zoomByFactor (float factor, int anchorX = -1);
    void zoomFit() noexcept;

    // Follow the playhead: when on, the strip scrolls during playback so the
    // playhead stays in view (no-op at fit-to-window zoom, where it always is).
    void setChaseEnabled (bool enabled) noexcept { chaseEnabled = enabled; }
    bool isChaseEnabled() const noexcept { return chaseEnabled; }

    // Explicit refresh for the session-load path. The strip otherwise relies on
    // indirect side effects (setConsoleVisibleRange / setBounds / the 30 Hz
    // timer) to repaint, and ALL of them no-op when a reopened session has the
    // same track layout + window size as the current view - so freshly-loaded
    // regions never get drawn. Rebuilds the visible rows, refits the horizontal
    // zoom/scroll to the loaded content (a session saved while zoomed-in must not
    // open with its regions scrolled off-screen), and repaints unconditionally.
    void refreshAfterSessionLoad();
    // Drops an open rename unapplied, and any name click still waiting to page
    // the console.
    void cancelTrackNameEdit();
    std::vector<double> viewForScenario() const
    {
        return { (double) userZoomFactor, (double) scrollSamples, (double) rowScrollY, (double) rowHeight,
                 showAllTracks ? 1.0 : 0.0, chaseEnabled ? 1.0 : 0.0 };
    }
    // Index 3 is the parent's tape-strip expansion and index 6 its Chase
    // toggle; the host owns both, so both are restored there.
    void restoreViewForScenario (const std::vector<double>& view)
    {
        if (view.size() != 7) return;
        userZoomFactor = (float) view[0];
        scrollSamples = (std::int64_t) view[1];
        rowScrollY = (int) view[2];
        rowHeight = (int) view[4];
        setShowAllTracksForScenario (view[5] > 0.5);
        repaint();
    }
    bool showsAllTracksForScenario() const noexcept { return showAllTracks; }
    // Through the pill's own click so the member, the button's lit state and
    // the row rebuild cannot drift apart. The click lands on a later tick.
    void setShowAllTracksForScenario (bool showAll)
    {
        if (showAllTracks != showAll) showAllToggle.triggerClick();
    }
    void clearSelectionsForScenario()
    {
        clearAllSelections();
        visibleTrackOrder.clear();
        rebuildVisibleTrackOrder();
    }
    auto dropPointForScenario (int track) const { return rowBounds (track).getCentre(); }
    std::int64_t dropPointSampleForScenario (int track) const { return sampleAtX (dropPointForScenario (track).x); }
    int dropLineXForScenario() const { return dropAccepted ? dropHoverX : -1; }
    int xForSampleForScenario (std::int64_t sample) const { return xForSample (sample); }
    // Stands in for the Alt key a drop reads; nullopt goes back to the key.
    void setDropAtMouseForScenario (std::optional<bool> atMouse) { dropAtMouseOverride = atMouse; }
    auto labelPointForScenario (int track) const { return rowBounds (track).getCentre().withX (labelColW / 2); }
    bool nameEditorOpenForScenario() const { return nameEditor.isBeingEdited(); }
    // The track whose row the open name editor sits on, -1 when it is closed
    // or off that row.
    int nameEditorTrackForScenario() const
    {
        const auto row = rowBounds (nameEditTrack);
        const int y = nameEditor.getBounds().getCentreY();
        return nameEditor.isBeingEdited() && y >= row.getY() && y < row.getBottom() ? nameEditTrack : -1;
    }
    auto rulerPointForScenario (float fraction) const { return rulerBounds().getRelativePoint (fraction, 0.25f); }
    std::int64_t rulerSampleForScenario (float fraction) const { return sampleAtX (rulerPointForScenario (fraction).x); }
    std::uint32_t regionAccentForScenario (int track, int region) const;
    // On the take badge but clear of the fade-in handle, which wins the
    // badge's top-left corner in hitTestRegion.
    auto takeBadgePointForScenario (int track, int region) const
    {
        return audioRegionScreenRect (track, region).getTopLeft().translated (kFadeHitPx + 5, kFadeHandleH + 1);
    }
    // The flag's left edge, in the pill band, by the same placement
    // hitTestMarker uses. The index must name an existing marker.
    auto markerPointForScenario (int index) const
    {
        const auto& marker = session.getMarkers()[(size_t) index];
        const int flagW = std::clamp (marker.name.length() * 8 + 12, 28, 160);
        const int flagX = std::min (xForSample (marker.timelineSamples), getWidth() - flagW - 2);
        const auto ruler = rulerBounds();
        return ruler.getTopLeft().withX (flagX + 6)
                                 .withY ((ruler.getY() + kRulerTickBandH + ruler.getBottom()) / 2);
    }

private:
    void timerCallback() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    // A plain press on a name arms a track move; this much vertical travel
    // starts it. gap counts the shown rows whose centre is above the pointer.
    static constexpr int kTrackMoveStartPx = 4;
    struct TrackMoveDrag
    {
        int pressed = -1;
        int pressY = 0;
        int pointerY = 0;
        bool active = false;
        // The pressed name is one of several picked. A click still narrows the
        // pick to it, but on release, so that a drag carries all of them.
        bool narrowOnRelease = false;
        std::vector<int> moving;
        int gap = 0;
    };
    TrackMoveDrag trackMove;
    void startTrackMove();
    void trackMovePointerAt (int y);
    void autoScrollTrackMove();
    TrackMovePlan trackMovePlanFor (const TrackMoveDrag& moveDrag) const;
    // The drop line's y, -1 when a drop there would move nothing.
    int trackMoveLineY() const;

    juce::Rectangle<int> labelColumnBounds() const noexcept;
    juce::Rectangle<int> rulerBounds() const noexcept;
    juce::Rectangle<int> tracksColumnBounds() const noexcept;

    // Width of the left label column, grown to fit the longest track name
    // (clamped [kTrackLabelW, kTrackLabelWMax]). Recomputed by
    // refreshLabelColumnWidth() on construction, layout, and name changes.
    int  labelColW { kTrackLabelW };
    void refreshLabelColumnWidth();
    // -1 when y falls in no row, including the gaps between rows.
    int trackAtLabelY (int y) const noexcept;

    // In-place rename over a row's name cell. Only visible while editing.
    juce::Label nameEditor;
    int nameEditTrack = -1;
    // The track under the last single click in the label column, and when it
    // landed. A double-click renames this track without hit-testing its second
    // click, and only while that click came within the double-click timeout.
    int labelPressTrack = -1;
    std::int64_t labelPressMs = 0;
    // Paging the console relayouts the window and can shrink this strip, so a
    // name click pages only once the double-click timeout has passed. Paging
    // any sooner would move the rows under a double-click's second press.
    struct LabelClickTimer final : dusk::Timer
    {
        std::function<void()> onExpired;
        void timerCallback() override { stopTimer(); onExpired(); }
    };
    LabelClickTimer labelClickTimer;
    void showNameEditor (int track);
    // Follows the edited row through scrolling and relayout, and commits the
    // edit once that row is scrolled fully out of view.
    void placeNameEditor();
    void commitTrackName();
    // Empty rect when trackIdx is collapsed - callers iterating must
    // respect this so hit tests / painters skip hidden rows.
    juce::Rectangle<int> rowBounds (int trackIdx) const noexcept;

    // Cheap (24 iter). Called from resized(), timer poll, SHOW ALL
    // click. Relayouts + repaints only on actual change. relayoutParent
    // is false when the caller (MainComponent::resized) is already mid-
    // layout, to avoid re-entrant resized().
    void rebuildVisibleTrackOrder (bool relayoutParent = true);
    // -1 if collapsed.
    int  visualRowForTrack (int trackIdx) const noexcept;

    // Rightmost sample any content occupies (audio + MIDI regions + playhead).
    // Shared by pixelsPerSecond() and zoomFit() so their extent can't diverge.
    std::int64_t rightmostContentSample() const noexcept;

    double pixelsPerSecond() const noexcept;
    std::int64_t sampleAtX (int x) const noexcept;
    int xForSample (std::int64_t s) const noexcept;

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
    // Smaller than audio version - Rename + Color + mute/lock, all routed
    // through MidiRegionEditAction for undo.
    void showMidiRegionContextMenu (int trackIdx, int regionIdx,
                                       juce::Point<int> screenPos);
    // The name column's menu: every region, audio and MIDI, on these tracks.
    void showTrackContextMenu (std::vector<int> tracks, juce::Point<int> screenPos);

    // Tempo edits, all driven from the ruler's right-click menu. Every edit
    // routes through commitTempoPoints so it's a single undoable transaction.
    void commitTempoPoints (std::vector<duskstudio::TempoPoint> after,
                             const juce::String& name);
    void promptAddTempoPoint (std::int64_t sample);   // prompt, add on accept
    void editTempoPointBpm (std::int64_t atSample);
    // Edit the song's starting tempo when no tempo map exists yet (the bar-1
    // base handle). hitTestTempoPoint returns kTempoBaseHandle for it.
    void editBaseTempo();
    void deleteTempoPoint (std::int64_t atSample);
    void paintTempoPoints (juce::Graphics&);

    Session& session;
    AudioEngine& engine;

    std::int64_t lastPlayhead = -1;

    // Playhead band repaint rides the display's vblank so motion steps
    // once per frame. A free-running 30 Hz Timer beats against the
    // compositor's refresh and reads as stutter. The timer keeps the
    // cheap state polling (names, colours, loop/punch, recording's
    // full-frame repaint).
    void updatePlayheadBand();
    juce::VBlankAttachment vBlankAttachment;

    // Handles the Stopped <-> Recording transition. Called from both the
    // timer and the vblank, so whichever runs first after it wins.
    void syncRecordingState();
    // Turns the page when the playhead nears the edge of the view, keeping
    // the zoom. True when the view scrolled.
    bool followPlayhead (std::int64_t playhead) noexcept;

    // 1.0 = auto-fit-all. zoomFit resets to 1 + zeroes scroll.
    float userZoomFactor = 1.0f;
    // Leftmost visible sample when zoomed. 0 when factor == 1. Wheel +
    // zoom clamp it so the visible window stays inside content.
    std::int64_t scrollSamples = 0;

    bool chaseEnabled = false;

    // Bold-text LookAndFeel for the SHOW ALL pill (TextButton has no setFont).
    // Declared before showAllToggle so the button is destroyed first.
    struct PillButtonLnF : juce::LookAndFeel_V4
    {
        juce::Font getTextButtonFont (juce::TextButton&, int h) override
        {
            return juce::Font (juce::FontOptions (std::min (12.0f, (float) h * 0.8f),
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
    wheel::Accumulator rowHeightWheel, rowScrollWheel;
    // Pixel height of all visible rows at the current rowHeight (content
    // extent below the ruler). Used to clamp rowScrollY + decide overflow.
    int rowsContentHeight() const noexcept;
    void clampRowScroll();

    // Without these, the strip's only repaint trigger is playhead
    // motion - renaming a track wouldn't reflect until the next play.
    std::array<juce::String, Session::kNumTracks> lastNames;
    std::array<juce::Colour, Session::kNumTracks> lastColours;

    bool        lastLoopEnabled  = false;
    std::int64_t lastLoopStart    = -1;
    std::int64_t lastLoopEnd      = -1;
    bool        lastPunchEnabled = false;
    std::int64_t lastPunchIn      = -1;
    std::int64_t lastPunchOut     = -1;

    // Full-repaint on Stopped <-> Recording - the thin playhead band
    // isn't wide enough to cover the live-recording overlay's first
    // paint at Record-press.
    bool        lastIsRecording  = false;

    // One drag active at a time.
    struct ActiveDrag
    {
        int track     = -1;
        int regionIdx = -1;
        RegionOp op   = RegionOp::None;
        std::int64_t mouseDownSample = 0;
        std::int64_t origTimelineStart = 0;
        std::int64_t origLength        = 0;
        std::int64_t origSourceOffset  = 0;
        std::int64_t origFadeIn        = 0;
        std::int64_t origFadeOut       = 0;
        float       origGainDb        = 0.0f;

        // Captured at mouseDown by (track, regionIdx) - the latter can
        // be reordered between mouseDown and mouseUp by concurrent
        // record / undo, so the pair at capture time is the stable form.
        // Empty = single-region drag.
        struct AdditionalOrig
        {
            int track;
            int regionIdx;
            std::int64_t origTimelineStart;
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
        std::int64_t mouseDownSample   = 0;
        std::int64_t origTimelineStart = 0;
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
    // just seeks the playhead - so plain click-to-seek still works.
    struct RulerSelection
    {
        bool active     = false;
        std::int64_t originSample  = 0;
        std::int64_t currentSample = 0;
    };
    RulerSelection rulerSelection;

    // moved=false at release = click (seek); true = drag (update marker).
    struct MarkerDrag
    {
        bool active   = false;
        bool moved    = false;
        int  index    = -1;
        std::int64_t originSample = 0;
        std::int64_t mouseDownSample = 0;
    };
    MarkerDrag markerDrag;

    // origStart/End captured pre-drag so whole-bar moves translate by
    // delta without compounding rounding.
    struct BracketDrag
    {
        bool       active = false;
        BracketHit type   = BracketHit::None;
        std::int64_t mouseDownSample = 0;
        std::int64_t origStart = 0;
        std::int64_t origEnd   = 0;
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
        std::int64_t mouseDownSample = 0;
        std::vector<duskstudio::TempoPoint> orig;
    };
    TempoDrag tempoDrag;

    // Most-recently-clicked region. Single-region ops act on this;
    // group ops also include additionalSelections. Cleared on
    // undo/redo (action might have shifted indices).
    int selectedTrack    = -1;
    int selectedRegion   = -1;

    // Name-column picks, sorted, and the row a Shift range starts from. The
    // set counts only while no region is picked and it holds selectedTrack;
    // any other gesture that moves the selection leaves it stale, and
    // selectedTrack alone is then the selection.
    std::vector<int> selectedTracks;
    int trackAnchor = -1;
    bool trackSetHolds() const noexcept;
    void selectOnlyTrack (int t);
    // Cmd/Ctrl toggles the row, Shift takes every shown row from the anchor.
    void extendTrackSelection (int t, bool range);

    int  dropHoverTrack = -1;
    int  dropHoverX     = -1;
    bool dropAccepted   = false;
    // Drops land at the playhead, or at the pointer while Alt is held.
    bool dropAtMouse() const;
    std::optional<bool> dropAtMouseOverride;

    // Audio + MIDI share a vector index space within a track but are
    // distinct types - separate selection slots avoid "which type is
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
    // The whole selection when the right-clicked region is part of it,
    // otherwise that region alone.
    std::vector<RegionId> regionMenuTargets (int track, int idx) const;

    // The region edits the menus and the edit keys share. None of them opens
    // an undo transaction; the caller names one per gesture. Each rebuilds
    // playback once however many regions it edits, and puts a track's MIDI
    // regions in one action, which publishes them to the audio thread once.
    // Sets one field on every target, skipping regions that already hold it.
    template <typename Field, typename Value>
    void setAudioRegionField (const std::vector<RegionId>& targets, Field AudioRegion::* field, const Value& value);
    template <typename Field, typename Value>
    void setMidiRegionField (const std::vector<RegionId>& targets, Field MidiRegion::* field, const Value& value);
    // The unlocked targets that keep accepts, ordered track ascending and
    // index descending, so deleting or splitting them in that order leaves
    // every index still to come valid.
    std::vector<RegionId> editableAudioRegions (std::vector<RegionId> targets,
                                                const std::function<bool (const AudioRegion&)>& keep = {}) const;
    std::vector<RegionId> editableMidiRegions (std::vector<RegionId> targets) const;
    // Unlocked and cut strictly inside by at: a split at an edge changes nothing.
    std::vector<RegionId> splittableAudioRegions (std::vector<RegionId> targets, std::int64_t at) const;
    // Every region on the tracks, leaving out frozen tracks: every region
    // action refuses one, since its regions are baked into the file it plays.
    std::vector<RegionId> regionsOnTracks (const std::vector<int>& tracks, bool midi) const;
    // The earliest start and latest end of those regions; end <= start with none.
    std::pair<std::int64_t, std::int64_t> regionSpan (const std::vector<int>& tracks) const;
    void deleteAudioRegions (const std::vector<RegionId>& ordered);
    void deleteMidiRegions (const std::vector<RegionId>& ordered);
    void splitAudioRegions (const std::vector<RegionId>& ordered, std::int64_t at);

    // A region's own colour when it has one, otherwise its track's. An unset
    // customColour is transparent.
    template <typename Region>
    auto regionAccent (int track, const Region& region) const
    {
        return region.customColour.isTransparent() ? session.track (track).colour : region.customColour;
    }

    // For an edit that leaves every region at its index. The undo history as
    // it stands afterwards is kept: while the change listener still finds it
    // unchanged, nothing else has touched the regions and the selection holds.
    // Change messages arrive later, so an edit the listener has not seen yet
    // may already be in the history; the hold is taken only when the history
    // before this edit is one the listener or the last hold accounted for.
    template <typename Action>
    void performInPlace (Action* action)
    {
        const auto before = undoHistory();
        const bool accounted = before == observedHistory
                            || (! heldSelectionHistory.empty() && before == heldSelectionHistory);
        engine.getUndoManager().perform (action);
        if (accounted) heldSelectionHistory = undoHistory();
        else           heldSelectionHistory.clear();
    }
    std::vector<std::string> undoHistory() const;
    std::vector<std::string> heldSelectionHistory;
    std::vector<std::string> observedHistory;

    // Add or remove if already present - Shift / Cmd-click extends
    // without collapsing back to a single anchor.
    void toggleRegionSelected (int track, int idx);

    // Drives affordance visibility - fade handles only paint on
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
