#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
// juce_dsp must precede juce_audio_utils so SIMDNativeOps<int64> is
// visible before juce_audio_processors (transitively pulled by
// juce_audio_utils) instantiates SIMDRegister<int64>.
#include <juce_dsp/juce_dsp.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <functional>
#include "../session/Session.h"

namespace duskstudio
{
class AudioEngine;
class EditModeToolbar;

// Modal editor for one AudioRegion. Sister to PianoRollComponent.
// AudioThumbnail waveform + fade envelopes + edit cursor.
//
// Owned by MainComponent. Constructed on the message thread; the
// underlying AudioRegion may be mutated by other UI / RecordManager
// while open. region() validates indices on every access — a stale
// view paints nothing rather than crashing.
class AudioRegionEditor final : public juce::Component,
                                  private juce::ChangeListener,
                                  private juce::Timer,
                                  private juce::ScrollBar::Listener
{
public:
    AudioRegionEditor (Session& session, AudioEngine& engine,
                          int trackIndex, int regionIndex);
    ~AudioRegionEditor() override;

    std::function<void()> onCloseRequested;

    // Cmd+]/Cmd+[ in-place swap. Host re-opens; editor state (zoom,
    // scroll, cursor) resets to fit the new region.
    std::function<void(int trackIdx, int newRegionIdx)> onNavigateToRegion;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseMove      (const juce::MouseEvent&) override;
    void mouseExit      (const juce::MouseEvent&) override;
    void mouseDown      (const juce::MouseEvent&) override;
    void mouseDrag      (const juce::MouseEvent&) override;
    void mouseUp        (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&,
                            const juce::MouseWheelDetails&) override;
    bool keyPressed     (const juce::KeyPress&) override;

    static constexpr int kIconRowHeight = 48;
    static constexpr int kRulerHeight   = 28;
    static constexpr int kStatusBarH    = 30;
    static constexpr int kScrollBarH    = 12;

    juce::ScrollBar horizontalScrollBar { false };
    void scrollBarMoved (juce::ScrollBar* bar, double newRangeStart) override;
    void syncScrollBarRange();
    static constexpr int kKeyboardWidth = 0;

private:
    Session& session;
    AudioEngine& engine;
    int trackIdx;
    int regionIdx;

    AudioRegion*       region();
    const AudioRegion* region() const;

    // Crossfade neighbours of the focused region (sorted by timelineStart).
    // Sets overlapPrev / overlapNext to the overlap length in samples (0
    // when none) and the neighbour's complementary fade shape used to
    // draw the crossing "X" curve. Const + cheap (few regions per track).
    void overlapNeighbours (juce::int64& overlapPrev, juce::int64& overlapNext,
                             FadeShape& prevOutShape, FadeShape& nextInShape) const;

    juce::AudioFormatManager formatManager;
    // 8 is plenty — we show one region at a time, but cached entries
    // keep take cycling snappy.
    juce::AudioThumbnailCache thumbCache { 8 };
    std::unique_ptr<juce::AudioThumbnail> thumb;
    juce::File loadedFile;
    // Cached from the AudioFormatReader so the bar/beat grid is
    // computed in the WAV's own time domain regardless of the device's
    // current SR. Without this, the grid drifts when the user hot-
    // swaps to a different-rate device. 0 = unknown (fall back to
    // engine SR).
    double loadedFileSampleRate = 0.0;

    // Anchor captured at open (and Cmd+]/[ navigation). After splits
    // the anchor stays put so the waveform doesn't shift/zoom and the
    // new slices appear in the same on-screen place. pixelsPerSample
    // + scrollSamples scale TIMELINE samples within this range.
    juce::int64 anchorTimelineStart  = 0;
    juce::int64 anchorTimelineLength = 0;
    float pixelsPerSample = 0.0f;
    juce::int64 scrollSamples = 0;
    juce::int64 editCursorSample = 0;

    // mouseDown captures regionAtDragStart so mouseUp submits a
    // RegionEditAction(before, after). MoveCursor is not undoable;
    // the rest are.
    enum class DragMode { None, FadeIn, FadeOut, Gain, TrimStart, TrimEnd, MoveCursor, Range, MoveRegion, Pan, AutomationPoint,
                          LoopIn, LoopOut, PunchIn, PunchOut };
    DragMode dragMode = DragMode::None;
    juce::int64 dragOriginTimelineSample = 0;
    // [start, end) in absolute file samples. Active when end > start;
    // range ops (Split / Delete / Fade-fit) operate on this band.
    juce::int64 rangeStartSample = 0;
    juce::int64 rangeEndSample   = 0;
    bool        rangeActive      = false;
    AudioRegion regionAtDragStart;
    juce::int64 dragOriginSample  = 0;
    int         dragOriginMouseY  = 0;
    float       dragOriginGainDb  = 0.0f;
    // panStartScroll = scrollSamples at drag start so mouseDrag
    // computes absolute delta without accumulating float errors.
    int         panStartMouseX    = 0;
    juce::int64 panStartScroll    = 0;

    // Focused regionIdx is the primary selection and always implicit.
    // Drives Delete (removes all), drag-move (translates the set), and
    // arrow-nudge. Cleared on plain (no-mod) click.
    std::vector<int> additionalSelectedRegions;
    // Same length + order as focused-first + additional. Resized in
    // mouseDown's MoveRegion-prep paths.
    std::vector<juce::int64> dragMultiOriginStarts;
    // -1 = no guide. Drawn as 1-px vertical line at the snap target
    // during a drag; cleared on mouseUp.
    juce::int64 snapGuideTimelineSample = -1;

    // Generous grab slop wider than the painted glyph. Empty rect when
    // the handle isn't visible (locked region, too narrow).
    juce::Rectangle<int> fadeInHandleRect  (juce::Rectangle<int> waveArea) const;
    juce::Rectangle<int> fadeOutHandleRect (juce::Rectangle<int> waveArea) const;
    juce::Rectangle<int> trimStartRect     (juce::Rectangle<int> waveArea) const;
    juce::Rectangle<int> trimEndRect       (juce::Rectangle<int> waveArea) const;
    int                   gainLineY        (juce::Rectangle<int> waveArea) const;

    // Split / reset gain / reset fades / mute / lock / colour / label.
    // All actions through the engine's UndoManager.
    void showContextMenu (juce::Point<int> screenPos);

    void showFadeShapeMenu (juce::Point<int> screenPos, bool isFadeIn);

    // Parallel mouse surface to right-click — used to alias into
    // showContextMenu; now real region inspector (rename / colour /
    // mute / lock / delete).
    void showRegionPropertiesPopup();

    // delta +1 = later, -1 = earlier. -1 return = no neighbour (caller
    // no-ops at boundaries; no wrap).
    int  nextRegionIndex (int delta) const;
    void navigateRegion  (int delta);

    void timerCallback() override;

    // -1 if off-screen. Drives narrow repaint instead of full
    // invalidate at 30 Hz.
    int lastPlayheadX = -1;
    // -1 if playhead is outside the region's bounds.
    int  transportPlayheadX (juce::Rectangle<int> waveArea) const;
    void paintTransportPlayhead (juce::Graphics&, juce::Rectangle<int> waveArea);

    class IconButton final : public juce::Button
    {
    public:
        enum class Glyph { Undo, Redo, Split, Normalize, ZoomFit, ZoomIn, ZoomOut, Properties };
        IconButton (const juce::String& name, Glyph g);
        void paintButton (juce::Graphics&, bool isMouseOver, bool isButtonDown) override;
    private:
        Glyph glyph;
    };
    juce::TextButton chaseToggle { "Chase" };
    IconButton undoButton       { "Undo",       IconButton::Glyph::Undo };
    IconButton redoButton       { "Redo",       IconButton::Glyph::Redo };
    IconButton splitButton      { "Split",      IconButton::Glyph::Split };
    IconButton normalizeButton  { "Normalize",  IconButton::Glyph::Normalize };
    IconButton propertiesButton { "Properties", IconButton::Glyph::Properties };
    IconButton zoomOutButton    { "Zoom out",   IconButton::Glyph::ZoomOut };
    IconButton zoomInButton     { "Zoom in",    IconButton::Glyph::ZoomIn };
    IconButton zoomFitButton    { "Zoom fit",   IconButton::Glyph::ZoomFit };

    // session.editMode persists session-wide so a mode set here also
    // drives TapeStrip mouse dispatch.
    std::unique_ptr<EditModeToolbar> editModeToolbar;

public:
    // MainComponent calls when global hotkey ('G') flips
    // session.editMode while the modal is open.
    void syncEditModeToolbar();

    // Set the mouse cursor to match the active edit mode (hand / scissors
    // / pencil / crosshair / I-beam). Called on mode change + as the
    // mouseMove fallback when no handle is under the pointer.
    void updateModeCursor();

    // CursorOverlay sink — MainComponent wires this so the editor can
    // push its local mouse position into the shared overlay (instead
    // of the overlay polling Desktop::getMousePosition, which is
    // broken on Wayland).
    // The juce::Range<int> argument carries the cut-line Y span (in
    // editor-local coords) used by CursorOverlay's half-scissor variant
    // in Cut mode. Empty range = no cut line, full-scissor fallback.
    std::function<void (juce::Component&, juce::Point<int>, EditMode,
                          juce::Range<int>)> onMouseMovedForCursor;
    std::function<void()> onMouseExitedForCursor;

    // Push (x, y) into the shared CursorOverlay if it's inside the
    // waveArea and the active mode wants a glyph (Grab / Cut / Draw).
    // Called from BOTH mouseMove and mouseDrag — drag fires only
    // mouseDrag (not mouseMove), so without this the overlay glyph
    // freezes at the click point while the region drags out from
    // underneath it.
    void pushCursorPosition (int x, int y);

    // Resolve the cursor for a given pointer position by consulting the
    // local hotspot rectangles (fade / trim / gain) FIRST, then falling
    // back to the edit-mode cursor when inside the waveform, and the
    // normal arrow elsewhere. Shared between mouseMove and
    // updateModeCursor so toolbar-driven mode flips can't overwrite a
    // resize cursor sitting on a handle.
    juce::MouseCursor cursorForPoint (int x, int y) const;

    // True when x falls inside a region's painted body (hovered slice, else
    // the focused region). Drives the Grab cursor: hand glyph over a body,
    // plain arrow over empty timeline between / past regions.
    bool pointOverRegionBody (int x, juce::Rectangle<int> waveArea) const;

    // Two-way: fresh overlap creates/widens auto-fades, vanishing
    // overlap retracts a previously-auto fade to zero. User-pinned
    // fades (fadeInAuto=false with non-zero length) untouched. Each
    // changed region commits as its own RegionEditAction in the
    // caller's undo transaction. Called after every geometry mutation
    // — MoveRegion, TrimStart, TrimEnd.
    void syncAutoCrossfades();
private:

    juce::Label        positionLabel;
    juce::Label        gainLabel;
    juce::Label        fadeLabel;
    juce::Label        infoLabel;
    // "What am I editing?" without forcing the user to close to check.
    // trackNameLabel = which TRACK (non-editable, tinted with the track
    // colour); titleLabel = the region label/filename (editable rename).
    juce::Label        trackNameLabel;
    juce::Label        titleLabel;
    juce::ToggleButton muteToggle;
    juce::ToggleButton lockToggle;

    // Click empty area to add, drag a dot to move, right-click to
    // delete. Edit only when transport stopped — audio reads lane
    // lock-free, mid-play mutation would race.
    juce::TextButton automationParamButton { "Auto: Off" };
    int  automationParam = -1;        // -1 = overlay disabled; else AutomationParam enum
    int  draggedPointIdx = -1;
    void showAutomationParamMenu();
    void paintAutomationOverlay (juce::Graphics&, juce::Rectangle<int> waveArea);
    int  hitTestAutomationPoint (int x, int y, juce::Rectangle<int> waveArea) const;
    juce::Rectangle<int> automationLaneArea (juce::Rectangle<int> waveArea) const;
    float automationValueForY (int y, juce::Rectangle<int> waveArea) const;
    int   automationYForValue (float v01, juce::Rectangle<int> waveArea) const;

    void layoutIconRow   (juce::Rectangle<int>);
    void layoutStatusBar (juce::Rectangle<int>);
    void refreshStatusBarReadouts();

    // Honours session.snapToGrid + active snapResolution. bypass
    // short-circuits to input (Cmd-bypass during drags). Shared so
    // every gesture lands on the same grid.
    juce::int64 snapFileSampleToGrid (juce::int64 fileSample, bool bypass) const noexcept;
    juce::int64 snapTimelineSampleToGrid (juce::int64 timelineSample, bool bypass) const noexcept;

    // All finalise through engine.getUndoManager. normalize is
    // non-destructive (gainDb adjust); reverse rewrites the source file.
    void normalizeRegion();
    void zoomFit();
    // > 1 zoom in, < 1 zoom out. Same math as '=' / '-' keypresses.
    void zoomByFactor (float factor);
    void splitAtCursor();

    void rebuildThumbIfNeeded();
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    void paintRuler         (juce::Graphics&, juce::Rectangle<int> area);
    void paintWaveform      (juce::Graphics&, juce::Rectangle<int> area);
    void paintFadeEnvelopes (juce::Graphics&, juce::Rectangle<int> area);
    // Loop (green) + punch (red) brackets over the ruler + waveform, read
    // from the transport. Dimmed when the matching mode is disabled.
    void paintLoopPunchBrackets (juce::Graphics&, juce::Rectangle<int> ruler,
                                  juce::Rectangle<int> wave);
    void paintEditCursor    (juce::Graphics&, juce::Rectangle<int> area);

    // Editor operates in TIMELINE samples on
    // [anchorTimelineStart, anchor + length). xForTimeline* are the
    // primitives; xForSample wraps in file-sample form (file -> timeline
    // via the focused region's sourceOffset + timelineStart) so existing
    // callers (fade discs, trim strips, edit cursor) keep working.
    int         xForTimelineSample (juce::int64 timelineSample,
                                      juce::Rectangle<int> area) const;
    juce::int64 timelineSampleForX (int x, juce::Rectangle<int> area) const;
    int  xForSample (juce::int64 absSample, juce::Rectangle<int> area) const;
    juce::int64 sampleForX (int x, juce::Rectangle<int> area) const;

    // Called on ctor / nav. Splits do NOT re-call.
    void zoomFitToArea (juce::Rectangle<int> area);

    // -1 in a gap. Used by mouseDown to swap focus when the user
    // clicks a non-focused slice rendered by the neighborhood-view.
    int regionIndexAtX (int x, juce::Rectangle<int> area) const;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioRegionEditor)
};
} // namespace duskstudio
