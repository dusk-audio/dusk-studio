#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
// juce_dsp must precede juce_audio_utils so the explicit
// SIMDNativeOps<int64> specialisation is visible before
// juce_audio_processors (transitively pulled by juce_audio_utils)
// instantiates SIMDRegister<int64> via its private dependencies.
#include <juce_dsp/juce_dsp.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <functional>
#include "../session/Session.h"

namespace duskstudio
{
class AudioEngine;
class EditModeToolbar;

// Modal editor for one AudioRegion. Sister to PianoRollComponent. Shows
// the slice's waveform via juce::AudioThumbnail and overlays the fade
// envelopes + edit cursor. Phase 1 is display-only; later phases add
// drag-to-edit, the icon row, the bottom status bar, and destructive
// ops (normalize / reverse).
//
// Lifecycle contract: instances are owned by MainComponent. Construction
// happens on the message thread; the underlying AudioRegion may be
// mutated by other UI code (or by RecordManager on the message thread)
// while the editor is open. region() validates the (track, region)
// indices on every access; a stale view paints nothing rather than
// crashing.
class AudioRegionEditor final : public juce::Component,
                                  private juce::ChangeListener,
                                  private juce::Timer,
                                  private juce::ScrollBar::Listener
{
public:
    AudioRegionEditor (Session& session, AudioEngine& engine,
                          int trackIndex, int regionIndex);
    ~AudioRegionEditor() override;

    // Esc-to-close hook. The host (MainComponent) sets this so the
    // user can dismiss the overlay without reaching for the mouse.
    std::function<void()> onCloseRequested;

    // Cmd+]/Cmd+[ in-place region swap. The host re-opens the editor
    // on the requested (track, region) so editor state (zoom, scroll,
    // edit cursor) resets to fit the new region.
    std::function<void(int trackIdx, int newRegionIdx)> onNavigateToRegion;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseMove      (const juce::MouseEvent&) override;
    void mouseDown      (const juce::MouseEvent&) override;
    void mouseDrag      (const juce::MouseEvent&) override;
    void mouseUp        (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&,
                            const juce::MouseWheelDetails&) override;
    bool keyPressed     (const juce::KeyPress&) override;

    // Visual constants exposed so the host overlay can size itself.
    static constexpr int kIconRowHeight = 48;   // top - action icons + edit-mode palette
    static constexpr int kRulerHeight   = 28;   // bar.beat ruler under icon row
    static constexpr int kStatusBarH    = 30;   // bottom - readouts + props
    static constexpr int kScrollBarH    = 12;   // horizontal scrollbar above status bar

    juce::ScrollBar horizontalScrollBar { false };   // false = horizontal
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

    // Cheap to construct (just registers basic file formats). A
    // dedicated manager per editor instance avoids cross-component
    // coupling - matches MasteringView's pattern in
    // [src/ui/MasteringView.h](src/ui/MasteringView.h).
    juce::AudioFormatManager formatManager;
    // Cache size of 8 thumbs is plenty - we only ever show one
    // region at a time, but leaving a few cached entries keeps
    // back-and-forth take cycling snappy.
    juce::AudioThumbnailCache thumbCache { 8 };
    std::unique_ptr<juce::AudioThumbnail> thumb;
    juce::File loadedFile;

    // Neighborhood view: the editor is anchored to a FIXED timeline
    // range captured at open time (or after Cmd+]/Cmd+[ navigation).
    // After splits the anchor stays put, so the waveform doesn't
    // shift/zoom and the new slices show up in the same on-screen
    // place. pixelsPerSample + scrollSamples now scale TIMELINE
    // samples (not file samples) within this range.
    juce::int64 anchorTimelineStart  = 0;
    juce::int64 anchorTimelineLength = 0;
    float pixelsPerSample = 0.0f;        // timeline samples -> pixels
    juce::int64 scrollSamples = 0;       // timeline-sample offset within the anchor range
    juce::int64 editCursorSample = 0;    // absolute file sample inside the focused slice

    // Drag state. Mirrors TapeStrip's pattern in [src/ui/TapeStrip.cpp]:
    // mouseDown captures `regionAtDragStart` so mouseUp can submit a
    // RegionEditAction(before=regionAtDragStart, after=current). MoveCursor
    // is not undoable; the rest are.
    // MoveRegion (Phase 2) drags a slice's timelineStart so the user
    // can reposition a chunk after splitting it out.
    enum class DragMode { None, FadeIn, FadeOut, Gain, TrimStart, TrimEnd, MoveCursor, Range, MoveRegion, Pan };
    DragMode dragMode = DragMode::None;
    juce::int64 dragOriginTimelineSample = 0;  // anchor for MoveRegion drag
    // Range selection on the waveform - inclusive [start, end) in
    // absolute file samples. Active when end > start; range ops
    // (Split / Delete chunk / Fade-fit) operate on this band.
    juce::int64 rangeStartSample = 0;
    juce::int64 rangeEndSample   = 0;
    bool        rangeActive      = false;
    AudioRegion regionAtDragStart;        // before-state for RegionEditAction
    juce::int64 dragOriginSample  = 0;    // edit-cursor anchor for relative drags
    int         dragOriginMouseY  = 0;    // for the gain drag (vertical)
    float       dragOriginGainDb  = 0.0f;
    // Pan-drag state. Middle-mouse-drag in the wave area pans the view;
    // panStartMouseX captures the down-x in component coords, panStartScroll
    // is the scrollSamples value at drag start so mouseDrag can compute an
    // absolute delta without accumulating float errors. Cursor flips to
    // DraggingHandCursor while DragMode::Pan is active.
    int         panStartMouseX    = 0;
    juce::int64 panStartScroll    = 0;

    // Ctrl/Cmd+click on a neighborhood region toggles it into this set.
    // The focused regionIdx is the "primary" selection and is always
    // implicitly included; this vector holds additional same-track region
    // indices. Multi-select drives Delete (removes all), drag-move
    // (translates the whole set by the drag delta), and arrow-nudge.
    // Empty by default; cleared on plain (no-mod) click.
    std::vector<int> additionalSelectedRegions;
    // Per-region timelineStart snapshots captured at drag start so the
    // mouseDrag handler can translate every selected region by the same
    // delta. Same length + order as the union (focused first, additional
    // after). Resized in mouseDown's MoveRegion-prep paths.
    std::vector<juce::int64> dragMultiOriginStarts;
    // When >= 0, paint a 1-px vertical guide at this TIMELINE sample so
    // the user can see exactly where the active drag will snap. Cleared
    // on mouseUp. Driven by the snap helpers in mouseDrag.
    juce::int64 snapGuideTimelineSample = -1;

    // Hit-test helpers - return the rect of each draggable handle within
    // the waveform area, or empty when the handle isn't currently visible
    // (e.g. fade handles when the region is locked / too narrow). Each
    // handle has a generous grab slop wider than the painted glyph.
    juce::Rectangle<int> fadeInHandleRect  (juce::Rectangle<int> waveArea) const;
    juce::Rectangle<int> fadeOutHandleRect (juce::Rectangle<int> waveArea) const;
    juce::Rectangle<int> trimStartRect     (juce::Rectangle<int> waveArea) const;
    juce::Rectangle<int> trimEndRect       (juce::Rectangle<int> waveArea) const;
    int                   gainLineY        (juce::Rectangle<int> waveArea) const;

    // Right-click context menu (split / reset gain / reset fades / mute /
    // lock / colour / label). All actions submitted through the engine's
    // UndoManager so Cmd+Z reverts. screenPos anchors the popup at the
    // click location (mouseDown forwards it from the MouseEvent).
    void showContextMenu (juce::Point<int> screenPos);

    // Right-click on a fade handle: pick the in / out fade curve shape.
    // `isFadeIn` selects which shape field the chosen value writes to.
    void showFadeShapeMenu (juce::Point<int> screenPos, bool isFadeIn);

    // Properties button → real region inspector popup (rename / colour /
    // mute / lock / delete). Replaces the old behaviour where the button
    // aliased into showContextMenu(). Right-click still routes there;
    // this is a parallel mouse surface.
    void showRegionPropertiesPopup();

    // Cmd+]/Cmd+[ helpers. nextRegionIndex returns the storage index of
    // the neighbouring region by timelineStart (delta = +1 → later,
    // -1 → earlier). -1 means "no neighbour in that direction" — the
    // caller no-ops at boundaries (no wrap).
    int  nextRegionIndex (int delta) const;
    void navigateRegion  (int delta);

    // 30 Hz playhead poll. Reads engine.transport().getPlayhead() and
    // repaints just the strip the playhead crossed since last tick.
    // Inherited from juce::Timer.
    void timerCallback() override;

    // Last x-pixel where the transport playhead was painted, or -1 if
    // it wasn't visible. Used to drive narrow repaint regions instead
    // of a full-component invalidate at 30 Hz.
    int lastPlayheadX = -1;
    // Map current transport sample to a wave-area x-pixel, or -1 if the
    // playhead is outside the open region's bounds. Caller passes the
    // current waveArea since paint and timer compute it identically.
    int  transportPlayheadX (juce::Rectangle<int> waveArea) const;
    void paintTransportPlayhead (juce::Graphics&, juce::Rectangle<int> waveArea);

    // Reaper-style top icon row. Compact circular buttons mirroring
    // TransportIconButton / PianoRollComponent::IconButton aesthetic.
    class IconButton final : public juce::Button
    {
    public:
        enum class Glyph { Undo, Redo, Split, Normalize, ZoomFit, ZoomIn, ZoomOut, Properties };
        IconButton (const juce::String& name, Glyph g);
        void paintButton (juce::Graphics&, bool isMouseOver, bool isButtonDown) override;
    private:
        Glyph glyph;
    };
    juce::TextButton chaseToggle { "Chase" };   // auto-scroll view to follow the transport playhead
    IconButton undoButton       { "Undo",       IconButton::Glyph::Undo };
    IconButton redoButton       { "Redo",       IconButton::Glyph::Redo };
    IconButton splitButton      { "Split",      IconButton::Glyph::Split };
    IconButton normalizeButton  { "Normalize",  IconButton::Glyph::Normalize };
    IconButton propertiesButton { "Properties", IconButton::Glyph::Properties };
    IconButton zoomOutButton    { "Zoom out",   IconButton::Glyph::ZoomOut };
    IconButton zoomInButton     { "Zoom in",    IconButton::Glyph::ZoomIn };
    IconButton zoomFitButton    { "Zoom fit",   IconButton::Glyph::ZoomFit };

    // Ardour-style edit-mode palette + snap controls. Lives inline in
    // the modal's icon row so the user picks the active tool mode while
    // editing a region. session.editMode persists session-wide, so a
    // mode set here also drives TapeStrip's mouse dispatch.
    std::unique_ptr<EditModeToolbar> editModeToolbar;

public:
    // Called by MainComponent when a global hotkey (e.g. 'G') flips
    // session.editMode while the modal is open, so the toolbar repaints
    // with the new active state.
    void syncEditModeToolbar();

    // Walk the track's regions and update every auto-managed fade so it
    // matches the current overlap with its neighbours. Two-way: a fresh
    // overlap creates / widens; an overlap that vanished retracts a
    // previously-auto fade back to zero. User-pinned fades (fadeInAuto /
    // fadeOutAuto == false with non-zero length) stay untouched. Each
    // changed region commits as its own RegionEditAction inside the
    // caller's undo transaction. Called after every geometry mutation —
    // MoveRegion, TrimStart, TrimEnd — so left-side and right-side
    // overlaps are both handled uniformly.
    void syncAutoCrossfades();
private:

    // Reaper-style bottom status-bar children. Real interactive widgets,
    // not paint-only - JUCE handles dispatch / hover / focus.
    juce::Label        positionLabel;
    juce::Label        gainLabel;
    juce::Label        fadeLabel;
    juce::Label        infoLabel;
    // Region label / source file name at the top-left of the modal,
    // above the bar ruler. Drives "what am I editing?" identification
    // without forcing the user to close the modal to check the timeline.
    juce::Label        titleLabel;
    juce::ToggleButton muteToggle;
    juce::ToggleButton lockToggle;

    // Layout helpers.
    void layoutIconRow   (juce::Rectangle<int>);
    void layoutStatusBar (juce::Rectangle<int>);
    void refreshStatusBarReadouts();

    // Action handlers. Each finalises through engine.getUndoManager()
    // so Cmd+Z reverts cleanly. normalize is non-destructive (gainDb
    // adjustment); reverse is destructive (rewrites the source file).
    void normalizeRegion();
    void zoomFit();
    // Multiplicative zoom around the edit cursor — same math the '=' / '-'
    // keypresses use. factor > 1 = zoom in, < 1 = zoom out.
    void zoomByFactor (float factor);
    void splitAtCursor();

    void rebuildThumbIfNeeded();
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    // Paint helpers - each takes the band's screen rect.
    void paintRuler         (juce::Graphics&, juce::Rectangle<int> area);
    void paintWaveform      (juce::Graphics&, juce::Rectangle<int> area);
    void paintFadeEnvelopes (juce::Graphics&, juce::Rectangle<int> area);
    void paintEditCursor    (juce::Graphics&, juce::Rectangle<int> area);

    // Coordinate mapping. The editor operates in TIMELINE samples
    // anchored on [anchorTimelineStart, anchorTimelineStart + length).
    //  • xForTimelineSample / timelineSampleForX are the primitives.
    //  • xForSample / sampleForX wrap them in file-sample form
    //    (file sample → timeline sample via the focused region's
    //    sourceOffset → timelineStart mapping) so existing callers
    //    (fade discs, trim strips, edit cursor) keep working.
    int         xForTimelineSample (juce::int64 timelineSample,
                                      juce::Rectangle<int> area) const;
    juce::int64 timelineSampleForX (int x, juce::Rectangle<int> area) const;
    int  xForSample (juce::int64 absSample, juce::Rectangle<int> area) const;
    juce::int64 sampleForX (int x, juce::Rectangle<int> area) const;

    // Snapshot the focused region's bounds into anchorTimeline* +
    // recompute pixelsPerSample so the anchor range fills the wave
    // area. Called once on ctor / nav; splits do NOT re-call this.
    void zoomFitToArea (juce::Rectangle<int> area);

    // Hit-test the slice under x. Returns the regionIdx of the
    // intersected region, or -1 if x falls in a gap. Used by
    // mouseDown to swap focus when the user clicks a non-focused
    // slice rendered by the neighborhood-view paint pass.
    int regionIndexAtX (int x, juce::Rectangle<int> area) const;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioRegionEditor)
};
} // namespace duskstudio
