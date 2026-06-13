#include "TapeStrip.h"
#include "../session/MarkerEditActions.h"
#include "../session/ParamEditAction.h"
#include "../session/RegionEditActions.h"
#include "../session/SnapHelpers.h"
#include "DuskAlerts.h"
#include "DuskContextMenu.h"
#include "EditCursors.h"
#include "EmbeddedModal.h"
#include "FadeCurve.h"
#include <cmath>
#include <string>

namespace duskstudio
{
namespace
{
// Process-wide static modal for the tape strip's text-input dialogs
// (marker rename, audio region label, MIDI region label). One in-
// window EmbeddedModal at a time across all three sites; close() is
// idempotent so re-entry is safe.
EmbeddedModal& sharedTapeStripTextInputModal()
{
    static EmbeddedModal m;
    return m;
}

// Strict, full-string float parse. containsOnly() lets malformed input like
// "+", "-", or "1..2" through, and getFloatValue() turns those into 0 — which
// jlimit then silently clamps to the minimum (a bogus 30 BPM entry). Require the
// ENTIRE trimmed string to parse as one number; reject partial / junk input.
bool parseFullFloat (const juce::String& s, float& out) noexcept
{
    const auto t = s.trim().toStdString();
    if (t.empty()) return false;
    try
    {
        std::size_t consumed = 0;
        const float v = std::stof (t, &consumed);
        if (consumed != t.size() || ! std::isfinite (v)) return false;   // trailing junk / inf / nan
        out = v;
        return true;
    }
    catch (...) { return false; }
}

// Dusk-styled text-input panel — title + prompt + TextEditor + OK /
// Cancel. Replaces juce::AlertWindow + addTextEditor for every
// rename flow that lived on this strip so the dialog renders inside
// the main window's component tree (no native popups on the
// X11 / Wayland stack). Duplicated body across TapeStrip /
// PianoRollComponent rather than centralised because per the H2
// scope we touch only these two files; the duplication is ~70 LOC
// and worth promoting to a shared header in a follow-up pass.
class TextInputPanel final : public juce::Component
{
public:
    TextInputPanel (juce::String title, juce::String prompt,
                      juce::String initial, juce::String acceptLabel,
                      std::function<void (juce::String)> onAccept)
        : titleStr (std::move (title)),
          promptStr (std::move (prompt)),
          onAcceptFn (std::move (onAccept))
    {
        setOpaque (true);
        editor.setText (initial, false);
        editor.setSelectAllWhenFocused (true);
        editor.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff15151c));
        editor.setColour (juce::TextEditor::outlineColourId,    juce::Colour (0xff3a3a42));
        editor.setColour (juce::TextEditor::textColourId,       juce::Colours::white);
        editor.setColour (juce::TextEditor::highlightColourId,  juce::Colour (0xff5a4880));
        editor.onReturnKey = [this] { commit(); };
        editor.onEscapeKey = [this] { dismiss(); };
        addAndMakeVisible (editor);

        auto style = [] (juce::TextButton& b, juce::Colour fill)
        {
            b.setColour (juce::TextButton::buttonColourId,   fill);
            b.setColour (juce::TextButton::buttonOnColourId, fill.brighter (0.15f));
            b.setColour (juce::TextButton::textColourOffId,  juce::Colours::white);
            b.setColour (juce::TextButton::textColourOnId,   juce::Colours::white);
            b.setMouseClickGrabsKeyboardFocus (false);
        };
        okBtn.setButtonText (acceptLabel.isEmpty() ? juce::String ("OK") : acceptLabel);
        style (okBtn,     juce::Colour (0xff5a4880));
        style (cancelBtn, juce::Colour (0xff262630));
        okBtn.onClick     = [this] { commit(); };
        cancelBtn.onClick = [this] { dismiss(); };
        addAndMakeVisible (okBtn);
        addAndMakeVisible (cancelBtn);

        setSize (420, 160);
        setWantsKeyboardFocus (true);
    }

    std::function<void()> onDismissFn;

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff1a1a22));
        auto r = getLocalBounds().reduced (18);
        g.setColour (juce::Colours::white);
        g.setFont (juce::Font (juce::FontOptions (14.5f, juce::Font::bold)));
        g.drawText (titleStr, r.removeFromTop (22),
                     juce::Justification::topLeft, false);
        r.removeFromTop (6);
        g.setColour (juce::Colour (0xffc0c0c8));
        g.setFont (juce::Font (juce::FontOptions (12.0f)));
        g.drawText (promptStr, r.removeFromTop (18),
                     juce::Justification::topLeft, false);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (18);
        r.removeFromTop (22 + 6 + 18 + 8);
        editor.setBounds (r.removeFromTop (26));
        r.removeFromTop (14);
        auto buttons = r.removeFromTop (28);
        cancelBtn.setBounds (buttons.removeFromRight (90));
        buttons.removeFromRight (8);
        okBtn    .setBounds (buttons.removeFromRight (90));
    }

    void visibilityChanged() override
    {
        if (! isVisible()) return;
        // Deferred: at this point the modal host may not have finished
        // showing, and a synchronous grab silently fails — the user then
        // has to click into the field before typing.
        juce::Component::SafePointer<TextInputPanel> safe (this);
        juce::MessageManager::callAsync ([safe]
        {
            if (safe != nullptr) safe->editor.grabKeyboardFocus();
        });
    }

private:
    void commit()
    {
        if (onAcceptFn) onAcceptFn (editor.getText());
        dismiss();
    }
    void dismiss() { if (onDismissFn) onDismissFn(); }

    juce::String titleStr, promptStr;
    juce::TextEditor editor;
    juce::TextButton okBtn { "OK" };
    juce::TextButton cancelBtn { "Cancel" };
    std::function<void (juce::String)> onAcceptFn;
};

void showEmbeddedTextInput (juce::Component& parent,
                              juce::String title, juce::String prompt,
                              juce::String initialValue,
                              juce::String acceptLabel,
                              std::function<void (juce::String)> onAccept)
{
    auto panel = std::make_unique<TextInputPanel> (std::move (title),
                                                      std::move (prompt),
                                                      std::move (initialValue),
                                                      std::move (acceptLabel),
                                                      std::move (onAccept));
    panel->onDismissFn = [] { sharedTapeStripTextInputModal().close(); };
    sharedTapeStripTextInputModal().show (parent, std::move (panel),
        /*onDismiss*/ [] { sharedTapeStripTextInputModal().close(); });
}
} // namespace
void TapeStrip::promptRenameMarker (int markerIdx, const juce::String& title)
{
    const auto& markers = session.getMarkers();
    if (markerIdx < 0 || markerIdx >= (int) markers.size()) return;

    auto* host = getTopLevelComponent();
    if (host == nullptr) host = this;
    juce::Component::SafePointer<TapeStrip> safeThis (this);
    showEmbeddedTextInput (*host, title, "New name:",
        markers[(size_t) markerIdx].name, "Rename",
        [safeThis, markerIdx] (juce::String newName)
        {
            if (safeThis == nullptr) return;
            const auto trimmed = newName.trim();
            if (trimmed.isEmpty()) return;
            auto& s = safeThis->session;
            const auto& m = s.getMarkers();
            if (markerIdx < 0 || markerIdx >= (int) m.size()) return;
            const auto oldName = m[(size_t) markerIdx].name;
            if (oldName == trimmed) return;
            auto& um = safeThis->engine.getUndoManager();
            um.beginNewTransaction ("Rename marker");
            um.perform (new ParamEditAction (
                [&s, idx = markerIdx, trimmed] { s.renameMarker (idx, trimmed); },
                [&s, idx = markerIdx, oldName] { s.renameMarker (idx, oldName); }));
            safeThis->repaint();
        });
}

TapeStrip::TapeStrip (Session& s, AudioEngine& e)
    : session (s), engine (e),
      vBlankAttachment (this, [this] { updatePlayheadBand(); })
{
    setOpaque (true);
    startTimerHz (30);
    engine.getUndoManager().addChangeListener (this);
    refreshModeCursor();

    auto wireZoom = [this] (juce::TextButton& b, const juce::String& tip,
                              std::function<void()> click)
    {
        b.setTooltip (tip);
        b.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff282830));
        b.setColour (juce::TextButton::textColourOffId, juce::Colour (0xffd0d0d8));
        b.setMouseClickGrabsKeyboardFocus (false);
        b.setWantsKeyboardFocus (false);
        b.onClick = std::move (click);
        addAndMakeVisible (b);
    };
    wireZoom (zoomOutButton, "Zoom out (-)",        [this] { zoomByFactor (1.0f / 1.25f); });
    wireZoom (zoomInButton,  "Zoom in (=)",         [this] { zoomByFactor (1.25f); });
    wireZoom (zoomFitButton, "Zoom to fit (Cmd+0)", [this] { zoomFit(); });

    // SNAP: lives alongside the zoom HUD so the user can toggle grid
    // snapping without having to look up at the transport bar.
    snapToggle.setClickingTogglesState (true);
    snapToggle.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff282830));
    snapToggle.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff5e4a18));   // dim amber when on
    snapToggle.setColour (juce::TextButton::textColourOffId,  juce::Colour (0xff909094));
    snapToggle.setColour (juce::TextButton::textColourOnId,   juce::Colour (0xffe0c050));
    snapToggle.setMouseClickGrabsKeyboardFocus (false);
    snapToggle.setWantsKeyboardFocus (false);
    snapToggle.setTooltip ("Snap region drags to the grid.");
    snapToggle.setToggleState (engine.getSession().snapToGrid, juce::dontSendNotification);
    snapToggle.onClick = [this]
    {
        engine.getSession().snapToGrid = snapToggle.getToggleState();
    };
    addAndMakeVisible (snapToggle);

    showAllToggle.setClickingTogglesState (true);
    showAllToggle.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff282830));
    showAllToggle.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff1f3a52));
    showAllToggle.setColour (juce::TextButton::textColourOffId,  juce::Colour (0xff909094));
    showAllToggle.setColour (juce::TextButton::textColourOnId,   juce::Colour (0xff7fb6e6));
    showAllToggle.setMouseClickGrabsKeyboardFocus (false);
    showAllToggle.setWantsKeyboardFocus (false);
    showAllToggle.setTooltip ("Show every track row, including empty unarmed tracks.");
    showAllToggle.setToggleState (showAllTracks, juce::dontSendNotification);
    showAllToggle.onClick = [this]
    {
        showAllTracks = showAllToggle.getToggleState();
        // Force a rebuild — content/armed bitmasks haven't changed but
        // the show-all override has.
        visibleTrackOrder.clear();
        rebuildVisibleTrackOrder();
    };
    addAndMakeVisible (showAllToggle);

    // Seed the visible list so naturalHeight() returns something sane
    // before the first resized()/timer tick.
    rebuildVisibleTrackOrder();

    // Force default-cursored child labels / buttons to defer to this
    // strip's stored cursor — see EditCursors.h for the
    // JUCE-default-NormalCursor-shadows-parent bug.
    inheritCursorOnDescendants (*this);
}

TapeStrip::~TapeStrip()
{
    engine.getUndoManager().removeChangeListener (this);
}

void TapeStrip::changeListenerCallback (juce::ChangeBroadcaster*)
{
    // Undo / redo just mutated the regions - repaint to reflect the swap.
    // The action itself called preparePlayback if stopped, so audio is
    // already aligned with what we'll draw. Every selection index might
    // now point at a region that has been deleted or shifted, so clear
    // both primary and additional.
    clearAllSelections();
    // Region count may have changed (paste/cut/undo/redo) — recompute
    // the visible row set so tracks that just gained or lost content
    // appear / disappear without the user toggling SHOW ALL.
    rebuildVisibleTrackOrder();
    repaint();
}

int TapeStrip::rowsContentHeight() const noexcept
{
    const int rows = juce::jmax (1, (int) visibleTrackOrder.size());
    return rows * (rowHeight + kRowGap);
}

int TapeStrip::naturalHeight() const noexcept
{
    // Layout height = bank-fit at the DEFAULT row height, independent of
    // the vertical-zoom row height. Zooming taller rows does NOT grow the
    // strip (so the mixer faders never shrink) - the extra content scrolls
    // inside this fixed band instead.
    const int rows = juce::jmax (1, (int) visibleTrackOrder.size());
    return kRulerH + rows * (kRowHDefault + kRowGap) + 6;
}

void TapeStrip::clampRowScroll() noexcept
{
    const int band = juce::jmax (0, getHeight() - kRulerH);
    const int maxScroll = juce::jmax (0, rowsContentHeight() - band);
    rowScrollY = juce::jlimit (0, maxScroll, rowScrollY);
}

int TapeStrip::maxNaturalHeight() noexcept
{
    return kRulerH + Session::kNumTracks * (kRowHDefault + kRowGap) + 6;
}

void TapeStrip::setConsoleVisibleRange (int firstTrack, int count)
{
    firstTrack = juce::jlimit (0, Session::kNumTracks - 1, firstTrack);
    count      = juce::jlimit (1, Session::kNumTracks - firstTrack, count);
    if (firstTrack == consoleFirstTrack && count == consoleVisibleCount) return;
    consoleFirstTrack   = firstTrack;
    consoleVisibleCount = count;
    // MainComponent calls this from inside its own resized(); don't
    // re-enter the parent layout - it sizes us via naturalHeight() right
    // after this returns.
    rebuildVisibleTrackOrder (/*relayoutParent*/ false);
}

void TapeStrip::rebuildVisibleTrackOrder (bool relayoutParent)
{
    // Membership rule:
    //   • ALL toggle  -> every track (0..23).
    //   • otherwise   -> the console's active bank range (so the timeline
    //                    mirrors the visible mixer strips - switch to
    //                    bank 7-12 and the timeline shows tracks 7-12),
    //                    UNIONed with any track that has audio/MIDI
    //                    content or is record-armed (so recorded material
    //                    is never hidden). Empty session => just the bank
    //                    range, filling the same row count the mixer shows.
    std::vector<int> next;
    if (showAllTracks)
    {
        for (int t = 0; t < Session::kNumTracks; ++t) next.push_back (t);
    }
    else
    {
        std::array<bool, Session::kNumTracks> keep {};
        for (int t = 0; t < Session::kNumTracks; ++t)
        {
            const auto& tr = session.track (t);
            keep[(size_t) t] = ! tr.regions.empty()
                             || ! tr.midiRegions.current().empty()
                             || tr.recordArmed.load (std::memory_order_relaxed);
        }
        // Mirror the console's visible bank range.
        const int last = juce::jmin (Session::kNumTracks,
                                      consoleFirstTrack + consoleVisibleCount);
        for (int t = consoleFirstTrack; t < last; ++t)
            keep[(size_t) t] = true;

        for (int t = 0; t < Session::kNumTracks; ++t)
            if (keep[(size_t) t]) next.push_back (t);
    }
    if (next.empty()) next.push_back (0);

    if (next == visibleTrackOrder) return;   // nothing changed
    visibleTrackOrder = std::move (next);

    // Fewer rows now (bank flip / content removed) can leave rowScrollY
    // pointing past the shrunken content — pull it back into range.
    clampRowScroll();

    // Grow / shrink the strip's own height to match the row count.
    if (relayoutParent)
        if (auto* p = getParentComponent()) p->resized();
    repaint();
}

int TapeStrip::visualRowForTrack (int trackIdx) const noexcept
{
    for (size_t i = 0; i < visibleTrackOrder.size(); ++i)
        if (visibleTrackOrder[i] == trackIdx) return (int) i;
    return -1;
}

juce::Rectangle<int> TapeStrip::labelColumnBounds() const noexcept
{
    return getLocalBounds().withTrimmedTop (kRulerH).withWidth (labelColW);
}

juce::Rectangle<int> TapeStrip::rulerBounds() const noexcept
{
    return juce::Rectangle<int> (labelColW, 0,
                                  juce::jmax (0, getWidth() - labelColW),
                                  kRulerH);
}

juce::Rectangle<int> TapeStrip::tracksColumnBounds() const noexcept
{
    return juce::Rectangle<int> (labelColW, kRulerH,
                                  juce::jmax (0, getWidth() - labelColW),
                                  juce::jmax (0, getHeight() - kRulerH));
}

void TapeStrip::refreshLabelColumnWidth()
{
    // Measure every row's drawn text (name, or the 1-based number fallback) in
    // the same 10 pt bold the painter uses, and widen the column to the longest
    // plus the stripe + insets, clamped so a stray long name can't swallow the
    // timeline.
    const juce::Font font (juce::FontOptions (10.0f, juce::Font::bold));
    float maxText = 0.0f;
    for (int t = 0; t < Session::kNumTracks; ++t)
    {
        const auto& nm = session.track (t).name;
        const juce::String s = nm.isNotEmpty() ? nm : juce::String (t + 1);
        juce::GlyphArrangement ga;
        ga.addLineOfText (font, s, 0.0f, 0.0f);
        maxText = juce::jmax (maxText, ga.getBoundingBox (0, -1, true).getWidth());
    }
    // 3 px colour stripe + 4 px left inset (see paint) + text + 6 px right pad.
    const int want = 3 + 4 + (int) maxText + 1 + 6;
    labelColW = juce::jlimit (kTrackLabelW, kTrackLabelWMax, want);
}

juce::Rectangle<int> TapeStrip::rowBounds (int trackIdx) const noexcept
{
    const int visualRow = visualRowForTrack (trackIdx);
    if (visualRow < 0) return {};
    auto col = tracksColumnBounds();
    const int y = col.getY() + visualRow * (rowHeight + kRowGap) - rowScrollY;
    return juce::Rectangle<int> (col.getX(), y, col.getWidth(), rowHeight);
}

double TapeStrip::pixelsPerSecond() const noexcept
{
    const double sr = engine.getCurrentSampleRate();
    if (sr <= 0.0) return 0.0;

    // Find the rightmost sample we need to show - either the longest region
    // or the current playhead - then add a margin so there's always blank
    // tape past the last recorded thing.
    juce::int64 maxSample = engine.getTransport().getPlayhead();
    for (int t = 0; t < Session::kNumTracks; ++t)
        for (auto& r : session.track (t).regions)
            maxSample = juce::jmax (maxSample, r.timelineStart + r.lengthInSamples);

    const double maxSeconds = juce::jmax (60.0, (double) maxSample / sr * 1.20);

    auto col = tracksColumnBounds();
    if (col.getWidth() <= 0) return 0.0;
    const double autoFit = (double) col.getWidth() / maxSeconds;

    // While recording, force fit-to-window so the growing playhead +
    // freshly-captured audio stay on screen instead of running off the
    // right edge at whatever userZoomFactor was selected pre-record.
    // User's zoom factor is preserved and re-applied the moment STOP
    // fires (recording flag clears).
    if (engine.getTransport().isRecording())
        return autoFit;

    return autoFit * (double) juce::jlimit (0.1f, 32.0f, userZoomFactor);
}

void TapeStrip::zoomByFactor (float factor, int anchorX)
{
    // Capture the sample currently under anchorX BEFORE changing zoom
    // so we can adjust scroll afterwards to keep that sample pinned.
    const juce::int64 anchorSampleBefore = (anchorX >= 0) ? sampleAtX (anchorX) : 0;
    userZoomFactor = juce::jlimit (0.1f, 32.0f, userZoomFactor * factor);
    if (anchorX >= 0)
    {
        const double sr = engine.getCurrentSampleRate();
        const double px = pixelsPerSecond();
        auto col = tracksColumnBounds();
        if (sr > 0.0 && px > 0.0)
        {
            // After zoom: anchorX should resolve to anchorSampleBefore.
            // Rearranged: scrollSamples = anchorSampleBefore - (anchorX - col.x) * sr / px
            const double pixelsFromLeft = (double) (anchorX - col.getX());
            const auto desiredScroll = anchorSampleBefore
                                          - (juce::int64) (pixelsFromLeft / px * sr);
            scrollSamples = juce::jmax<juce::int64> (0, desiredScroll);
        }
    }
    // Reset scroll to 0 once we drop back to fit-all (zoom <= 1).
    if (userZoomFactor <= 1.0f)
        scrollSamples = 0;
    repaint();
}

void TapeStrip::zoomFit() noexcept
{
    scrollSamples = 0;

    // The legacy pixelsPerSecond() builds in a 60-second minimum + 20 %
    // headroom margin, so just resetting userZoomFactor to 1.0 leaves
    // short content squished into the left half of the strip. Compute
    // the zoom that makes the rightmost content sample land at the
    // column's right edge — that's the user's "fit" expectation.
    const double sr = engine.getCurrentSampleRate();
    if (sr <= 0.0)
    {
        userZoomFactor = 1.0f;
        repaint();
        return;
    }

    juce::int64 maxSample = engine.getTransport().getPlayhead();
    for (int t = 0; t < Session::kNumTracks; ++t)
        for (auto& r : session.track (t).regions)
            maxSample = juce::jmax (maxSample, r.timelineStart + r.lengthInSamples);

    const double contentSec    = (double) juce::jmax<juce::int64> (1, maxSample) / sr;
    const double autoFitBudget = juce::jmax (60.0, contentSec * 1.20);
    // pxPerSec at zoom 1 = col.width / autoFitBudget. We want the
    // visible window to equal contentSec exactly, so
    // zoom = autoFitBudget / contentSec.
    const float fitZoom = (float) (autoFitBudget / juce::jmax (0.001, contentSec));
    userZoomFactor = juce::jlimit (0.1f, 32.0f, fitZoom);
    repaint();
}

juce::int64 TapeStrip::sampleAtX (int x) const noexcept
{
    const double sr = engine.getCurrentSampleRate();
    const double px = pixelsPerSecond();
    if (sr <= 0.0 || px <= 0.0) return 0;
    auto col = tracksColumnBounds();
    const double seconds = (double) (x - col.getX()) / px;
    return scrollSamples + (juce::int64) juce::jmax (0.0, seconds * sr);
}

int TapeStrip::xForSample (juce::int64 s) const noexcept
{
    const double sr = engine.getCurrentSampleRate();
    const double px = pixelsPerSecond();
    if (sr <= 0.0 || px <= 0.0) return tracksColumnBounds().getX();
    const auto rel = s - scrollSamples;
    return tracksColumnBounds().getX() + (int) ((double) rel / sr * px);
}

juce::Rectangle<int> TapeStrip::audioRegionScreenRect (int trackIdx, int regionIdx) const noexcept
{
    if (trackIdx < 0 || trackIdx >= Session::kNumTracks) return {};
    const auto& ar = session.track (trackIdx).regions;
    if (regionIdx < 0 || regionIdx >= (int) ar.size()) return {};
    const auto& r = ar[(size_t) regionIdx];
    const auto row = rowBounds (trackIdx);
    if (row.isEmpty()) return {};
    const int x0 = xForSample (r.timelineStart);
    const int x1 = xForSample (r.timelineStart + r.lengthInSamples);
    juce::Rectangle<int> rect (x0, row.getY() + 1, juce::jmax (2, x1 - x0), row.getHeight() - 2);
    return rect.getIntersection (tracksColumnBounds());
}

juce::Rectangle<int> TapeStrip::midiRegionScreenRect (int trackIdx, int regionIdx) const noexcept
{
    if (trackIdx < 0 || trackIdx >= Session::kNumTracks) return {};
    const auto& mr = session.track (trackIdx).midiRegions.current();
    if (regionIdx < 0 || regionIdx >= (int) mr.size()) return {};
    const auto& r = mr[(size_t) regionIdx];
    const auto row = rowBounds (trackIdx);
    if (row.isEmpty()) return {};
    const int x0 = xForSample (r.timelineStart);
    const int x1 = xForSample (r.timelineStart + r.lengthInSamples);
    juce::Rectangle<int> rect (x0, row.getY() + 1, juce::jmax (2, x1 - x0), row.getHeight() - 2);
    return rect.getIntersection (tracksColumnBounds());
}

TapeStrip::RegionHit TapeStrip::hitTestRegion (int x, int y) const noexcept
{
    auto col = tracksColumnBounds();
    if (! col.contains (x, y)) return {};

    for (int t = 0; t < Session::kNumTracks; ++t)
    {
        const auto row = rowBounds (t);
        if (! row.contains (x, y)) continue;

        const auto& regions = session.track (t).regions;
        // Iterate from most-recent (last) to first so the topmost region on
        // overlap wins the hit, matching the painter's order.
        for (int i = (int) regions.size() - 1; i >= 0; --i)
        {
            const auto& r = regions[(size_t) i];
            const int x0 = xForSample (r.timelineStart);
            const int x1 = xForSample (r.timelineStart + r.lengthInSamples);
            if (x < x0 || x > x1) continue;

            RegionHit hit;
            hit.track = t;
            hit.regionIdx = i;

            // Fade-handle hit zone: top kFadeHandleH px of the region.
            // Within this band, the cursor's distance to either fade-end
            // x-position determines which handle is grabbed. Outside
            // the band, falls through to the existing edge / move
            // logic. Fade handles take priority over the take badge so
            // the user can still adjust fade-in even on regions with
            // alternate takes (the badge sits below the fade band).
            const int yTopBand = row.getY() + 1;
            const bool inFadeBand = (y >= yTopBand && y < yTopBand + kFadeHandleH);
            if (inFadeBand)
            {
                const auto& reg = regions[(size_t) i];
                const auto fadeInSamples  = juce::jmax ((juce::int64) 0, reg.fadeInSamples);
                const auto fadeOutSamples = juce::jmax ((juce::int64) 0, reg.fadeOutSamples);
                const double pxPerSample = (double) (x1 - x0)
                    / (double) juce::jmax ((juce::int64) 1, reg.lengthInSamples);
                const int fadeInEndX  = x0 + (int) std::round ((double) fadeInSamples  * pxPerSample);
                const int fadeOutBegX = x1 - (int) std::round ((double) fadeOutSamples * pxPerSample);
                if (std::abs (x - fadeInEndX)  <= kFadeHitPx) { hit.op = RegionOp::FadeIn;  return hit; }
                if (std::abs (x - fadeOutBegX) <= kFadeHitPx) { hit.op = RegionOp::FadeOut; return hit; }
            }

            // Take-history badge takes precedence over the trim-start gutter
            // since the two share screen area at the region's top-left. Same
            // bounds as the painter so the click target visibly lines up.
            if (! r.previousTakes.empty())
            {
                const int regionWidth = juce::jmax (2, x1 - x0);
                const int badgeW = juce::jmin (regionWidth, 16);
                const int badgeH = juce::jmin (row.getHeight() - 2, 10);
                const int badgeYTop = row.getY() + 1;
                if (badgeW >= 8 && badgeH >= 6
                    && x >= x0 && x <= x0 + badgeW
                    && y >= badgeYTop && y <= badgeYTop + badgeH)
                {
                    hit.op = RegionOp::TakeBadge;
                    return hit;
                }
            }

            // Edge gutters at the ends - only when the region is wide enough
            // that body + two gutters still leaves a body to drag.
            const int edgeBudget = juce::jmax (1, (x1 - x0) / 4);
            const int gutter     = juce::jmin (kEdgeHitPx, edgeBudget);
            if      (x <= x0 + gutter)  hit.op = RegionOp::TrimStart;
            else if (x >= x1 - gutter)  hit.op = RegionOp::TrimEnd;
            else                        hit.op = RegionOp::Move;
            return hit;
        }
    }
    return {};
}

bool TapeStrip::isRegionSelected (int track, int idx) const noexcept
{
    if (track == selectedTrack && idx == selectedRegion) return true;
    const RegionId q { track, idx };
    return std::binary_search (additionalSelections.begin(),
                                  additionalSelections.end(), q);
}

std::vector<TapeStrip::RegionId> TapeStrip::allSelectedRegions() const
{
    std::vector<RegionId> result;
    if (selectedTrack >= 0 && selectedRegion >= 0)
        result.push_back ({ selectedTrack, selectedRegion });
    for (auto& id : additionalSelections)
        result.push_back (id);
    return result;
}

void TapeStrip::clearAllSelections() noexcept
{
    selectedTrack  = -1;
    selectedRegion = -1;
    selectedMidiTrack  = -1;
    selectedMidiRegion = -1;
    additionalSelections.clear();
}

void TapeStrip::toggleRegionSelected (int track, int idx)
{
    if (track < 0 || idx < 0) return;
    // Toggle the primary itself.
    if (track == selectedTrack && idx == selectedRegion)
    {
        // Collapsing the primary: promote first-additional to
        // primary if any, else clear.
        if (additionalSelections.empty())
        {
            selectedTrack = -1;
            selectedRegion = -1;
        }
        else
        {
            const auto promoted = additionalSelections.front();
            additionalSelections.erase (additionalSelections.begin());
            selectedTrack  = promoted.track;
            selectedRegion = promoted.regionIdx;
        }
        return;
    }
    // Toggle within additional.
    const RegionId id { track, idx };
    auto it = std::lower_bound (additionalSelections.begin(),
                                  additionalSelections.end(), id);
    if (it != additionalSelections.end() && *it == id)
    {
        additionalSelections.erase (it);
        return;
    }
    // Not currently selected. If there is no primary, become primary;
    // otherwise add to additional.
    if (selectedTrack < 0)
    {
        selectedTrack  = track;
        selectedRegion = idx;
        return;
    }
    additionalSelections.insert (it, id);
}

void TapeStrip::setSelectedTrack (int t) noexcept
{
    if (t < 0 || t >= Session::kNumTracks) t = -1;
    if (selectedTrack == t && selectedRegion == -1) return;
    selectedTrack  = t;
    selectedRegion = -1;
    repaint();
}

void TapeStrip::rebuildPlaybackIfStopped()
{
    // Re-prep the playback engine so the next play picks up the edited
    // regions. We avoid this during play/record because preparePlayback
    // closes and re-opens audio readers - momentary I/O on the message
    // thread is safe at rest but risks an xrun mid-transport.
    auto& transport = engine.getTransport();
    if (transport.getState() == Transport::State::Stopped)
        engine.getPlaybackEngine().preparePlayback();
}

void TapeStrip::resized()
{
    // SNAP + zoom buttons moved to MainComponent's header row so they
    // sit between the bank tabs and the tuner button. The TapeStrip
    // retains the button members + onClick wiring (MainComponent
    // forwards through TapeStrip's public zoom/snap helpers) but hides
    // them here so the ruler band reads cleanly.
    snapToggle    .setVisible (false);
    zoomOutButton .setVisible (false);
    zoomInButton  .setVisible (false);
    zoomFitButton .setVisible (false);

    // Recompute the visible row set in case session content changed
    // since the last layout pass (file drop, take commit, arm toggle).
    // relayoutParent=false: we're already inside resized(), so a parent
    // relayout here would re-enter this method.
    rebuildVisibleTrackOrder (/*relayoutParent*/ false);

    // Size the label column to the current track names before placing the
    // SHOW ALL toggle (which is pinned to that column's width).
    refreshLabelColumnWidth();

    // SHOW ALL toggle pinned to the right edge of the label column at
    // the top — lives in unused real estate above the row labels, fits
    // in the kRulerH band so it doesn't compete with the time ruler.
    constexpr int kShowAllH = 14;
    constexpr int kShowAllW = 38;
    constexpr int kShowAllPad = 2;
    showAllToggle.setBounds (kShowAllPad,
                              kShowAllPad,
                              juce::jmin (kShowAllW, labelColW - 2 * kShowAllPad),
                              kShowAllH);
    inheritCursorOnDescendants (*this);
}

void TapeStrip::timerCallback()
{
    // Detect track color / name changes and repaint the whole strip if
    // anything changed. Cheap - there are 16 tracks and we just compare
    // a String + a Colour each tick.
    bool stateChanged = false;
    bool namesChanged = false;
    for (int t = 0; t < Session::kNumTracks; ++t)
    {
        const auto& tr = session.track (t);
        if (lastNames[(size_t) t]   != tr.name)   { lastNames[(size_t) t]   = tr.name;   stateChanged = true; namesChanged = true; }
        if (lastColours[(size_t) t] != tr.colour) { lastColours[(size_t) t] = tr.colour; stateChanged = true; }
    }

    // A rename can change the column width; resize repositions the SHOW ALL
    // toggle and recomputes labelColW, and the repaint below redraws.
    if (namesChanged)
        resized();

    auto& transport = engine.getTransport();
    const bool        loopOn = transport.isLoopEnabled();
    const juce::int64 loopS  = transport.getLoopStart();
    const juce::int64 loopE  = transport.getLoopEnd();
    const bool        pOn    = transport.isPunchEnabled();
    const juce::int64 pIn    = transport.getPunchIn();
    const juce::int64 pOut   = transport.getPunchOut();
    if (loopOn != lastLoopEnabled || loopS != lastLoopStart || loopE != lastLoopEnd
        || pOn != lastPunchEnabled || pIn != lastPunchIn || pOut != lastPunchOut)
    {
        lastLoopEnabled = loopOn;
        lastLoopStart   = loopS;
        lastLoopEnd     = loopE;
        lastPunchEnabled = pOn;
        lastPunchIn     = pIn;
        lastPunchOut    = pOut;
        stateChanged = true;
    }

    // Force a full repaint on the Stopped <-> Recording transition so
    // the live-recording overlay paints / clears the moment the user
    // presses Record / Stop, not a few frames later when the playhead
    // band-repaint catches up.
    const bool nowRec = transport.isRecording();
    if (nowRec != lastIsRecording)
    {
        lastIsRecording = nowRec;
        stateChanged = true;
    }

    // Poll arm-flag changes — the user can toggle ARM from the channel
    // strip at any time, and that flips a row into / out of the visible
    // set. rebuildVisibleTrackOrder() compares against cached state and
    // only triggers a relayout when something actually changed.
    rebuildVisibleTrackOrder();

    if (stateChanged) repaint();

    // During recording, pixelsPerSecond auto-shrinks every frame as the
    // playhead grows, so the bar grid + existing regions also shift —
    // a thin-band playhead repaint isn't enough. Full repaint keeps
    // ruler labels + region waveforms aligned with the live playhead.
    // Stays at the timer's 30 Hz; a full-strip repaint per vblank would
    // be needlessly heavy. Playback's band repaint lives on the vblank
    // (updatePlayheadBand).
    if (nowRec)
    {
        const auto now = transport.getPlayhead();
        if (now != lastPlayhead)
        {
            lastPlayhead = now;
            repaint();
        }
    }
}

void TapeStrip::updatePlayheadBand()
{
    if (engine.getTransport().isRecording()) return; // timer's full repaint covers it

    const auto now = engine.getTransport().getPlayhead();
    if (now == lastPlayhead) return;

    const int oldX = xForSample (lastPlayhead < 0 ? 0 : lastPlayhead);
    const int newX = xForSample (now);
    lastPlayhead = now;

    // Repaint a thin vertical band covering both the old and new playhead
    // positions plus a few pixels of margin so we don't see ghosting.
    const int x = juce::jmin (oldX, newX) - 2;
    const int w = std::abs (newX - oldX) + 4;
    repaint (x, 0, w, getHeight());
}

void TapeStrip::mouseDown (const juce::MouseEvent& e)
{
    auto col = tracksColumnBounds();
    auto ruler = rulerBounds();

    // Left-click on a marker flag → start a marker drag. We DON'T seek
    // here - that's deferred to mouseUp so a click-without-movement seeks
    // and a click-with-movement repositions the marker.
    if (! e.mods.isRightButtonDown())
    {
        if (const int markerIdx = hitTestMarker (e.x, e.y); markerIdx >= 0)
        {
            markerDrag.active           = true;
            markerDrag.moved            = false;
            markerDrag.index            = markerIdx;
            markerDrag.originSample     = session.getMarkers()[(size_t) markerIdx]
                                                .timelineSamples;
            markerDrag.mouseDownSample  = sampleAtX (e.x);
            return;
        }
    }

    // Left-click on an existing loop / punch pill or bar → start a
    // bracket drag. Endpoint pills move that endpoint; the bar in the
    // middle translates the whole range by the drag delta. Tested
    // before ruler-selection so dragging on top of an existing bracket
    // doesn't accidentally start a new selection.
    if (! e.mods.isRightButtonDown())
    {
        if (const auto bh = hitTestBracket (e.x, e.y); bh != BracketHit::None)
        {
            auto& transport = engine.getTransport();
            bracketDrag.active = true;
            bracketDrag.type   = bh;
            bracketDrag.mouseDownSample = sampleAtX (e.x);
            const bool isPunch = (bh == BracketHit::PunchIn
                                || bh == BracketHit::PunchOut
                                || bh == BracketHit::PunchBar);
            bracketDrag.origStart = isPunch ? transport.getPunchIn()  : transport.getLoopStart();
            bracketDrag.origEnd   = isPunch ? transport.getPunchOut() : transport.getLoopEnd();
            return;
        }
    }

    // Left-click on a tempo marker → start a reposition drag. Checked before
    // the ruler seek so grabbing a marker doesn't also move the playhead. The
    // bar-1 anchor (sample 0) is the starting tempo and stays put. Edit /
    // delete stay on the right-click menu; double-click still edits the BPM.
    if (! e.mods.isRightButtonDown())
    {
        const int tIdx = hitTestTempoPoint (e.x, e.y);
        if (tIdx >= 0)
        {
            const auto& pts = session.tempoMap.points();
            if (tIdx < (int) pts.size() && pts[(size_t) tIdx].timelineSamples != 0)
            {
                tempoDrag.active          = true;
                tempoDrag.moved           = false;
                tempoDrag.index           = tIdx;
                tempoDrag.mouseDownSample = sampleAtX (e.x);
                tempoDrag.orig            = pts;
                return;
            }
        }
    }

    // Tempo is edited via the ruler's right-click menu (any edit mode) — see
    // the "Tempo" section in the loop/punch context menu below.

    // Left-click on the ruler (not a marker / bracket) → start a neutral
    // selection drag. The range is painted as a translucent highlight while
    // dragging; on mouseUp a short drag (≈ click) seeks the playhead, a real
    // drag pops a menu asking loop vs punch. Transport state is untouched
    // until release.
    if (! e.mods.isRightButtonDown() && ruler.contains (e.x, e.y))
    {
        rulerSelection.active        = true;
        rulerSelection.originSample  = sampleAtX (e.x);
        rulerSelection.currentSample = rulerSelection.originSample;
        repaint();
        return;
    }

    // Right-click on a region opens a region-specific menu (delete, split).
    // Right-click on empty timeline / ruler opens the loop+punch menu.
    if (e.mods.isRightButtonDown())
    {
        const auto hit = hitTestRegion (e.x, e.y);
        if (hit.op != RegionOp::None)
        {
            showRegionContextMenu (hit, e.getScreenPosition());
            return;
        }
        // No audio hit - try MIDI. MIDI regions don't have edge gutters
        // / fade handles / take badges, so we just check whether the
        // cursor sits inside any region's painted rect.
        for (int t = 0; t < Session::kNumTracks; ++t)
        {
            const auto row = rowBounds (t);
            if (! row.contains (e.x, e.y)) continue;
            const auto& mr = session.track (t).midiRegions.current();
            for (int i = (int) mr.size() - 1; i >= 0; --i)
            {
                const auto& r = mr[(size_t) i];
                const int x0 = xForSample (r.timelineStart);
                const int x1 = xForSample (r.timelineStart + r.lengthInSamples);
                if (e.x < x0 || e.x > x1) continue;
                showMidiRegionContextMenu (t, i, e.getScreenPosition());
                return;
            }
            break;
        }
    }

    // Right-click anywhere over the ruler or track area opens a context menu
    // for setting loop / punch points and seeking. The clicked sample is
    // captured here so the menu items act on a fixed timeline position even
    // if the user moves the mouse before picking.
    if (e.mods.isRightButtonDown()
        && (col.contains (e.x, e.y) || ruler.contains (e.x, e.y)))
    {
        const auto clickedSample = sampleAtX (e.x);
        auto& transport = engine.getTransport();

        juce::PopupMenu m;
        m.addSectionHeader ("Loop");
        m.addItem ("Set loop in here",  [&transport, clickedSample]
        {
            const auto end = transport.getLoopEnd();
            transport.setLoopRange (clickedSample,
                                     end > clickedSample ? end : clickedSample);
        });
        m.addItem ("Set loop out here", [&transport, clickedSample]
        {
            const auto start = transport.getLoopStart();
            transport.setLoopRange (start < clickedSample ? start : clickedSample,
                                     clickedSample);
        });
        m.addItem ("Clear loop", [&transport]
        {
            transport.setLoopRange (0, 0);
            transport.setLoopEnabled (false);
        });
        m.addSeparator();
        m.addSectionHeader ("Punch");
        m.addItem ("Set punch in here",  [&transport, clickedSample]
        {
            const auto end = transport.getPunchOut();
            transport.setPunchRange (clickedSample,
                                      end > clickedSample ? end : clickedSample);
        });
        m.addItem ("Set punch out here", [&transport, clickedSample]
        {
            const auto start = transport.getPunchIn();
            transport.setPunchRange (start < clickedSample ? start : clickedSample,
                                      clickedSample);
        });
        m.addItem ("Clear punch", [&transport]
        {
            transport.setPunchRange (0, 0);
            transport.setPunchEnabled (false);
        });
        m.addSeparator();
        m.addSectionHeader ("Markers");
        const int hoveredMarkerIdx = hitTestMarker (e.x, e.y);
        if (hoveredMarkerIdx >= 0)
        {
            const auto& mk = session.getMarkers()[(size_t) hoveredMarkerIdx];
            m.addItem ("Rename \"" + mk.name + "\"...",
                        [safeThis = juce::Component::SafePointer<TapeStrip> (this),
                         hoveredMarkerIdx]
                        {
                            if (safeThis != nullptr)
                                safeThis->promptRenameMarker (hoveredMarkerIdx);
                        });
            m.addItem ("Delete \"" + mk.name + "\"",
                        [safeThis = juce::Component::SafePointer<TapeStrip> (this),
                         hoveredMarkerIdx]
                        {
                            if (safeThis == nullptr) return;
                            auto& um = safeThis->engine.getUndoManager();
                            um.beginNewTransaction ("Delete marker");
                            um.perform (new RemoveMarkerAction (
                                safeThis->session, hoveredMarkerIdx));
                            safeThis->repaint();
                        });
        }
        m.addItem ("Add marker here",
                    [safeThis = juce::Component::SafePointer<TapeStrip> (this),
                     clickedSample]
                    {
                        if (safeThis == nullptr) return;
                        // Snap the spawn position to the grid when SNAP
                        // is on so newly-added markers land on bar/beat
                        // lines (mirrors the drag-snap path below).
                        const auto sr = safeThis->engine.getCurrentSampleRate();
                        const auto spawn = snap::snapAbsoluteToGrid (
                            clickedSample, safeThis->session, sr);
                        auto& um = safeThis->engine.getUndoManager();
                        um.beginNewTransaction ("Add marker");
                        auto* add = new AddMarkerAction (safeThis->session, spawn);
                        // The UndoManager owns the action — and DELETES it
                        // if perform() fails — so only dereference `add`
                        // after a successful perform.
                        const bool added = um.perform (add);
                        safeThis->repaint();
                        // Name-on-create: open the rename input with the
                        // auto-generated name pre-selected so one typing
                        // pass names the new marker. Escape keeps it.
                        if (added)
                            safeThis->promptRenameMarker (add->insertedIndex(), "Name marker");
                    });
        if (! session.getMarkers().empty())
        {
            const double sr = engine.getCurrentSampleRate();
            juce::PopupMenu jumpSub;
            for (int i = 0; i < (int) session.getMarkers().size(); ++i)
            {
                const auto& mk = session.getMarkers()[(size_t) i];
                const int secs = sr > 0.0
                                   ? (int) ((double) mk.timelineSamples / sr)
                                   : 0;
                jumpSub.addItem (mk.name + "  (" + juce::String (secs) + "s)",
                                  [safeThis = juce::Component::SafePointer<TapeStrip> (this),
                                   i]
                                  {
                                      if (safeThis == nullptr) return;
                                      const auto& m = safeThis->session.getMarkers();
                                      if (i < 0 || i >= (int) m.size()) return;
                                      safeThis->engine.getTransport()
                                          .setPlayhead (m[(size_t) i].timelineSamples);
                                      safeThis->repaint();
                                  });
            }
            m.addSubMenu ("Jump to marker", jumpSub);
        }
        m.addSeparator();
        m.addItem ("Move playhead here", [&transport, clickedSample]
        {
            transport.setPlayhead (clickedSample);
        });

        // ── Tempo (ruler only) ── add / edit / delete a tempo-map point at the
        // clicked position. Right-click is the only tempo edit surface.
        if (ruler.contains (e.x, e.y))
        {
            using SP = juce::Component::SafePointer<TapeStrip>;
            m.addSeparator();
            m.addSectionHeader ("Tempo");
            const int tIdx = hitTestTempoPoint (e.x, e.y);
            if (tIdx == kTempoBaseHandle)        // bar-1 base, no map yet
            {
                m.addItem ("Set starting tempo...", [safeThis = SP (this)]
                    { if (safeThis != nullptr) safeThis->editBaseTempo(); });
            }
            else if (tIdx >= 0)                  // an existing tempo marker
            {
                const auto sample = session.tempoMap.points()[(size_t) tIdx].timelineSamples;
                m.addItem ("Set tempo...", [safeThis = SP (this), sample]
                    { if (safeThis != nullptr) safeThis->editTempoPointBpm (sample); });
                // The bar-1 anchor (sample 0) is the starting tempo — can't delete it.
                m.addItem ("Delete tempo", sample != 0, false, [safeThis = SP (this), sample]
                    { if (safeThis != nullptr) safeThis->deleteTempoPoint (sample); });
            }
            else                                  // empty spot -> add a change
            {
                const auto snapped = snap::snapAbsoluteToGrid (
                    clickedSample, session, engine.getCurrentSampleRate());
                m.addItem ("Set tempo here...", [safeThis = SP (this), snapped]
                    { if (safeThis != nullptr) safeThis->promptAddTempoPoint (snapped); });
            }
        }

        // Anchor the menu at the cursor instead of the TapeStrip's
        // top-left corner. Same fix as the plugin picker.
        const auto cursor = e.getScreenPosition();
        showContextMenu (m, *this, cursor,
                          [safeThis = juce::Component::SafePointer<TapeStrip> (this)] (int)
                          {
                              if (safeThis != nullptr) safeThis->repaint();
                          });
        return;
    }

    // Left-click on the track label (the strip on the far left of each
    // row, before the timeline column starts) selects that track without
    // picking a region. Lets keyboard shortcuts (A / S / X) target a
    // track that has no recorded regions yet.
    {
        const auto labelCol = labelColumnBounds();
        if (labelCol.contains (e.x, e.y) && ! e.mods.isRightButtonDown())
        {
            for (int t = 0; t < Session::kNumTracks; ++t)
            {
                if (rowBounds (t).contains (e.x, e.y))
                {
                    selectedTrack  = t;
                    selectedRegion = -1;
                    repaint();
                    return;
                }
            }
        }
    }

    if (! col.contains (e.x, e.y)) return;

    // Left-click on a region body or edge starts a drag (move / trim) AND
    // marks that region as selected - keyboard copy/cut/delete then act on
    // it without needing a separate selection step.
    const auto hit = hitTestRegion (e.x, e.y);

    // NOTE: the empty-timeline seek lives at the END of this method, AFTER the
    // MIDI-region click and the Shift/Cmd rubber-band handlers. Seeking here on
    // hit.op == None (audio-only hit test) would swallow clicks on MIDI regions
    // and box-select drags.

    // Take-history badge: rotate to the next take (FIFO — front of
    // previousTakes surfaces, current goes to the back). Wrapped in a
    // RegionEditAction so Cmd+Z reverts the rotation, like the Takes menu.
    if (hit.op == RegionOp::TakeBadge)
    {
        const auto& cur = session.track (hit.track).regions[(size_t) hit.regionIdx];
        if (! cur.previousTakes.empty())
        {
            AudioRegion before = cur;
            AudioRegion after  = cur;
            const TakeRef current { after.file, after.sourceOffset, after.lengthInSamples };
            const TakeRef next = after.previousTakes.front();
            after.previousTakes.erase (after.previousTakes.begin());
            after.previousTakes.push_back (current);
            after.file            = next.file;
            after.sourceOffset    = next.sourceOffset;
            after.lengthInSamples = next.lengthInSamples;

            selectedTrack  = hit.track;
            selectedRegion = hit.regionIdx;
            selectedMidiTrack  = -1;
            selectedMidiRegion = -1;

            auto& um = engine.getUndoManager();
            um.beginNewTransaction ("Cycle take");
            um.perform (new RegionEditAction (session, engine,
                                                hit.track, hit.regionIdx, before, after));
            repaint();
        }
        return;
    }

    if (hit.op != RegionOp::None)
    {
        // Locked regions reject drag / resize / fade / gain. The
        // take-badge rotation is still allowed - it's reversible by
        // another click and doesn't change geometry. Shift-click
        // for selection is also allowed so the user can pick up
        // locked regions to copy / paste somewhere else.
        const auto& clickedRegion = session.track (hit.track).regions[(size_t) hit.regionIdx];
        if (clickedRegion.locked && hit.op != RegionOp::TakeBadge
            && ! (e.mods.isShiftDown() || e.mods.isCommandDown()))
        {
            // Just select the region so the user knows what they
            // clicked on, but don't enter drag mode.
            clearAllSelections();
            selectedTrack  = hit.track;
            selectedRegion = hit.regionIdx;
            drag.op = RegionOp::None;
            repaint();
            return;
        }

        // Shift / Cmd-click on a region body extends the multi-selection
        // without starting a drag. Edge ops (trim, fade, take badge)
        // ignore the modifier - those are still single-region operations
        // even when other regions are selected. Group drag begins from a
        // plain (no-modifier) click on an already-selected region body.
        //   • Cmd-click  → toggle the clicked region.
        //   • Shift-click on the same track as the primary → fill every
        //     region between the primary and the clicked one (by
        //     timelineStart order) into the selection.
        //   • Shift-click on a different track → fall back to toggle.
        const bool isBodyHit = (hit.op == RegionOp::Move
                                 || hit.op == RegionOp::TrimStart
                                 || hit.op == RegionOp::TrimEnd);
        if (e.mods.isShiftDown() && isBodyHit
            && selectedTrack == hit.track && selectedRegion >= 0)
        {
            const auto& regs = session.track (hit.track).regions;
            if (selectedRegion < (int) regs.size()
                && hit.regionIdx < (int) regs.size())
            {
                const auto anchorTL = regs[(size_t) selectedRegion].timelineStart;
                const auto clickTL  = regs[(size_t) hit.regionIdx].timelineStart;
                const auto lo = juce::jmin (anchorTL, clickTL);
                const auto hi = juce::jmax (anchorTL, clickTL);
                for (int i = 0; i < (int) regs.size(); ++i)
                {
                    const auto t = regs[(size_t) i].timelineStart;
                    if (t < lo || t > hi) continue;
                    if (i == selectedRegion) continue;
                    if (! isRegionSelected (hit.track, i))
                        toggleRegionSelected (hit.track, i);
                }
                drag.op = RegionOp::None;
                repaint();
                return;
            }
        }
        if ((e.mods.isShiftDown() || e.mods.isCommandDown()) && isBodyHit)
        {
            toggleRegionSelected (hit.track, hit.regionIdx);
            drag.op = RegionOp::None;   // no drag mode entered
            repaint();
            return;
        }

        const auto& region = session.track (hit.track).regions[(size_t) hit.regionIdx];
        drag.track             = hit.track;
        drag.regionIdx         = hit.regionIdx;
        drag.op                = hit.op;
        // Alt held while clicking the body promotes the drag to gain
        // adjust - vertical drag changes dB instead of horizontal
        // motion changing the timeline start. Doesn't affect the
        // edge gutters or fade handles (which already do something
        // useful with Alt held).
        if (drag.op == RegionOp::Move && e.mods.isAltDown())
            drag.op = RegionOp::AdjustGain;
        drag.mouseDownSample   = sampleAtX (e.x);
        drag.origTimelineStart = region.timelineStart;
        drag.origLength        = region.lengthInSamples;
        drag.origSourceOffset  = region.sourceOffset;
        drag.origFadeIn        = region.fadeInSamples;
        drag.origFadeOut       = region.fadeOutSamples;
        drag.origGainDb        = region.gainDb;
        drag.additional.clear();

        // Selection logic: if the clicked region was already selected
        // (in the multi-set), keep the existing selection so group
        // drag works. Otherwise collapse to single-region. Same
        // pattern as the piano roll's note multi-select.
        const bool wasSelected = isRegionSelected (hit.track, hit.regionIdx);
        if (! wasSelected)
        {
            clearAllSelections();
            selectedTrack  = hit.track;
            selectedRegion = hit.regionIdx;
        }
        else
        {
            // Promote the clicked region to primary if it was an
            // additional. Drag delta is computed against primary's
            // origs; per-additional origs are captured below.
            if (hit.track != selectedTrack || hit.regionIdx != selectedRegion)
            {
                // Find + erase from additional, then make it primary.
                const RegionId clickedId { hit.track, hit.regionIdx };
                auto it = std::lower_bound (additionalSelections.begin(),
                                              additionalSelections.end(), clickedId);
                if (it != additionalSelections.end() && *it == clickedId)
                    additionalSelections.erase (it);
                // Demote former primary into additional.
                if (selectedTrack >= 0 && selectedRegion >= 0)
                {
                    const RegionId formerPrimary { selectedTrack, selectedRegion };
                    auto pos = std::lower_bound (additionalSelections.begin(),
                                                    additionalSelections.end(),
                                                    formerPrimary);
                    additionalSelections.insert (pos, formerPrimary);
                }
                selectedTrack  = hit.track;
                selectedRegion = hit.regionIdx;
            }
        }

        // Capture per-additional origs for group Move / AdjustGain.
        // Trim / Fade are anchor-only - skip the work.
        if (drag.op == RegionOp::Move || drag.op == RegionOp::AdjustGain)
        {
            for (auto& add : additionalSelections)
            {
                const auto& addRegions = session.track (add.track).regions;
                if (add.regionIdx < 0 || add.regionIdx >= (int) addRegions.size())
                    continue;
                const auto& ar = addRegions[(size_t) add.regionIdx];
                drag.additional.push_back ({ add.track, add.regionIdx,
                                                ar.timelineStart, ar.gainDb });
            }
        }

        repaint();
        return;
    }

    // MIDI single-click: capture move-drag origin. Move-only on the
    // tape lane; fade / gain / trim happen inside the piano roll.
    // Double-click opens the editor (handled in mouseDoubleClick).
    {
        for (int t = 0; t < Session::kNumTracks; ++t)
        {
            const auto row = rowBounds (t);
            if (! row.contains (e.x, e.y)) continue;
            const auto& mr = session.track (t).midiRegions.current();
            for (int i = (int) mr.size() - 1; i >= 0; --i)
            {
                const auto& r = mr[(size_t) i];
                const int x0 = xForSample (r.timelineStart);
                const int x1 = xForSample (r.timelineStart + r.lengthInSamples);
                if (e.x < x0 || e.x > x1) continue;
                midiDrag.track             = t;
                midiDrag.regionIdx         = i;
                midiDrag.mouseDownSample   = sampleAtX (e.x);
                midiDrag.origTimelineStart = r.timelineStart;
                midiDrag.origState         = r;
                selectedMidiTrack  = t;
                selectedMidiRegion = i;
                // Plain MIDI click clears the audio selection so the two
                // selection sets don't both light up. Modifier-extend on
                // MIDI regions isn't supported yet; that path stays
                // single-region for now.
                selectedTrack  = -1;
                selectedRegion = -1;
                additionalSelections.clear();
                repaint();
                return;
            }
            break;
        }
    }

    // Shift / Cmd + click on empty track-row space starts a rubber-
    // band box-select. Skip the seek + clear so the existing
    // selection is preserved; mouseUp intersects each region's rect
    // with the box and adds matches to additionalSelections. Outside
    // the tracks column (e.g. on the left-edge label gutter) the
    // rubber band would have nothing to hit, so we only enter this
    // mode inside tracksColumnBounds.
    if ((e.mods.isShiftDown() || e.mods.isCommandDown())
        && tracksColumnBounds().contains (e.x, e.y))
    {
        rubberBandActive = true;
        rubberBand = juce::Rectangle<int> (e.x, e.y, 0, 0);
        repaint();
        return;
    }

    // Plain click on empty timeline → seek the playhead AND clear
    // every selection (primary + additional). Runs only after the MIDI and
    // rubber-band handlers above have declined the click.
    clearAllSelections();

    const auto sample = snap::snapAbsoluteToGrid (
        sampleAtX (e.x), session, engine.getCurrentSampleRate());
    engine.getTransport().setPlayhead (sample);
    // Remember this click so a future Stop in "Return to last clicked" mode
    // lands here.
    session.lastClickedTimelineSample.store (sample, std::memory_order_relaxed);
    repaint();
}

void TapeStrip::mouseDrag (const juce::MouseEvent& e)
{
    // Keep the Grab glyph following a region MOVE drag - mouseDrag fires but
    // mouseMove doesn't, so without this the glyph freezes at the click point
    // while the region slides out from under it. Trim/fade/gain drags keep
    // the native resize cursor mouseMove last set.
    if (onMouseMovedForCursor && session.editMode == EditMode::Grab
        && (drag.op == RegionOp::Move || midiDrag.active()))
        onMouseMovedForCursor (*this, { e.x, e.y }, EditMode::Grab, {});

    // MIDI region move-drag. In-place mutation of timelineStart on
    // the AtomicSnapshot's mutable copy so the audio thread's
    // scheduler picks it up without a publish (no reallocation, no
    // shifting, structural integrity preserved per AtomicSnapshot's
    // contract). mouseUp finalises through MidiRegionEditAction.
    if (midiDrag.active())
    {
        auto& v = session.track (midiDrag.track).midiRegions.currentMutable();
        if (midiDrag.regionIdx < 0 || midiDrag.regionIdx >= (int) v.size()) return;

        juce::int64 deltaSamples = sampleAtX (e.x) - midiDrag.mouseDownSample;
        // Snap-to-beat - same model as the audio drag below.
        deltaSamples = snap::snapDeltaToGrid (deltaSamples, session, engine.getCurrentSampleRate());
        const auto newStart = juce::jmax<juce::int64> (
            0, midiDrag.origTimelineStart + deltaSamples);
        v[(size_t) midiDrag.regionIdx].timelineStart = newStart;
        repaint();
        return;
    }

    // Rubber-band drag - update the screen-space rectangle from the
    // mouseDown origin to the current point. mouseUp finalises the
    // selection.
    if (rubberBandActive)
    {
        const int x0 = e.getMouseDownX();
        const int y0 = e.getMouseDownY();
        rubberBand = juce::Rectangle<int> (juce::jmin (x0, e.x), juce::jmin (y0, e.y),
                                              std::abs (e.x - x0), std::abs (e.y - y0));
        repaint();
        return;
    }

    // Marker drag - reposition a marker once the cursor moves more than
    // a small threshold from the click. Below threshold, it's still a
    // click (mouseUp will seek). The marker's array index stays valid
    // throughout because we re-sort only on mouseUp.
    if (markerDrag.active)
    {
        const auto cur = sampleAtX (e.x);
        constexpr juce::int64 kDragThresholdSamples = 256;   // ~5 ms @ 48k
        if (! markerDrag.moved
            && std::abs (cur - markerDrag.mouseDownSample) > kDragThresholdSamples)
        {
            markerDrag.moved = true;
        }
        if (markerDrag.moved
            && markerDrag.index >= 0
            && markerDrag.index < (int) session.getMarkers().size())
        {
            // Snap to absolute grid lines when SNAP is on. Markers don't
            // carry a meaningful "mid-beat origin" semantic (unlike a
            // region's fade-in offset) so delta-against-origin snap left
            // an off-grid marker stuck at off-grid positions forever.
            // Absolute snap means the marker lands ON the bar / beat the
            // user is dragging it toward.
            juce::int64 newPos = juce::jmax ((juce::int64) 0, cur);
            newPos = snap::snapAbsoluteToGrid (newPos, session,
                                                engine.getCurrentSampleRate());
            session.getMarkers()[(size_t) markerDrag.index].timelineSamples = newPos;
            repaint();
        }
        return;
    }

    // Tempo-marker drag — same threshold + absolute-snap model as markers.
    // Rebuild the full point set from the captured original (fixed index),
    // move the dragged point, and republish so the bar grid + audio map
    // follow live. Clamp to >= 1 sample so it never collides with the bar-1
    // anchor (which setPoints would dedup away).
    if (tempoDrag.active)
    {
        const auto cur = sampleAtX (e.x);
        constexpr juce::int64 kDragThresholdSamples = 256;   // ~5 ms @ 48k
        if (! tempoDrag.moved
            && std::abs (cur - tempoDrag.mouseDownSample) > kDragThresholdSamples)
            tempoDrag.moved = true;

        if (tempoDrag.moved
            && tempoDrag.index >= 0
            && tempoDrag.index < (int) tempoDrag.orig.size())
        {
            juce::int64 newPos = juce::jmax ((juce::int64) 1, cur);
            newPos = snap::snapAbsoluteToGrid (newPos, session,
                                                engine.getCurrentSampleRate());
            newPos = juce::jmax ((juce::int64) 1, newPos);

            auto working = tempoDrag.orig;
            working[(size_t) tempoDrag.index].timelineSamples = newPos;
            engine.setTempoPoints (std::move (working));
            repaint();
        }
        return;
    }

    // Bracket drag - reposition loop/punch endpoints or translate the
    // whole range. Endpoint drags clamp to keep start ≤ end - 1024
    // samples (the same useful-range floor we use elsewhere) so the
    // user can't accidentally collapse a bracket to zero length by
    // dragging one end through the other.
    if (bracketDrag.active)
    {
        constexpr juce::int64 kMinUsefulRangeSamples = 1024;
        const auto cur = juce::jmax ((juce::int64) 0, sampleAtX (e.x));
        const auto delta = cur - bracketDrag.mouseDownSample;
        auto& transport = engine.getTransport();
        switch (bracketDrag.type)
        {
            case BracketHit::LoopIn:
            {
                const auto newStart = juce::jlimit ((juce::int64) 0,
                                                       bracketDrag.origEnd - kMinUsefulRangeSamples,
                                                       cur);
                transport.setLoopRange (newStart, bracketDrag.origEnd);
                break;
            }
            case BracketHit::LoopOut:
            {
                const auto newEnd = juce::jmax (bracketDrag.origStart + kMinUsefulRangeSamples, cur);
                transport.setLoopRange (bracketDrag.origStart, newEnd);
                break;
            }
            case BracketHit::LoopBar:
            {
                const auto newStart = juce::jmax ((juce::int64) 0,
                                                    bracketDrag.origStart + delta);
                const auto length   = bracketDrag.origEnd - bracketDrag.origStart;
                transport.setLoopRange (newStart, newStart + length);
                break;
            }
            case BracketHit::PunchIn:
            {
                const auto newStart = juce::jlimit ((juce::int64) 0,
                                                       bracketDrag.origEnd - kMinUsefulRangeSamples,
                                                       cur);
                transport.setPunchRange (newStart, bracketDrag.origEnd);
                break;
            }
            case BracketHit::PunchOut:
            {
                const auto newEnd = juce::jmax (bracketDrag.origStart + kMinUsefulRangeSamples, cur);
                transport.setPunchRange (bracketDrag.origStart, newEnd);
                break;
            }
            case BracketHit::PunchBar:
            {
                const auto newStart = juce::jmax ((juce::int64) 0,
                                                    bracketDrag.origStart + delta);
                const auto length   = bracketDrag.origEnd - bracketDrag.origStart;
                transport.setPunchRange (newStart, newStart + length);
                break;
            }
            case BracketHit::None: break;
        }
        repaint();
        return;
    }

    // Ruler selection — sweep the highlight range as the user drags. Range
    // start/end via min/max so dragging either direction works.
    if (rulerSelection.active)
    {
        rulerSelection.currentSample = juce::jmax ((juce::int64) 0, sampleAtX (e.x));
        repaint();
        return;
    }

    if (drag.op == RegionOp::None) return;

    auto& regions = session.track (drag.track).regions;
    if (drag.regionIdx < 0 || drag.regionIdx >= (int) regions.size()) return;

    const auto current = sampleAtX (e.x);
    juce::int64 deltaSamples = current - drag.mouseDownSample;

    // Snap-to-grid: round the *delta* to a whole-step value so the drag
    // motion still feels continuous but the destination lands on a tick.
    // Step is one beat when tempo > 0 (gives the user beat-aligned drags
    // even when their tempo is unusual), or one second otherwise.
    // The delta is rounded (not the absolute target) so a region whose
    // origin is mid-step stays mid-step on small drags - only large
    // drags re-align it to the grid.
    deltaSamples = snap::snapDeltaToGrid (deltaSamples, session, engine.getCurrentSampleRate());

    constexpr juce::int64 kMinLengthSamples = 1024;  // ~21 ms @ 48k
    auto& r = regions[(size_t) drag.regionIdx];

    switch (drag.op)
    {
        case RegionOp::Move:
        {
            // Group-clamp: the smallest origTimelineStart across the
            // anchor + every additional sets the floor on -delta so
            // no region in the group falls off the timeline.
            juce::int64 minOrig = drag.origTimelineStart;
            for (const auto& a : drag.additional)
                minOrig = juce::jmin (minOrig, a.origTimelineStart);
            const juce::int64 clampedDelta = juce::jmax (deltaSamples, -minOrig);
            r.timelineStart = drag.origTimelineStart + clampedDelta;
            for (const auto& a : drag.additional)
            {
                auto& addRegions = session.track (a.track).regions;
                if (a.regionIdx < 0 || a.regionIdx >= (int) addRegions.size())
                    continue;
                addRegions[(size_t) a.regionIdx].timelineStart =
                    a.origTimelineStart + clampedDelta;
            }
            break;
        }
        case RegionOp::TrimStart:
        {
            // Bound delta so newSourceOffset >= 0 and length stays above the
            // floor. Equivalent constraints in sample space:
            //   delta >= -origSourceOffset            (don't expose negative source)
            //   delta <=  origLength - kMinLength     (don't shrink below the floor)
            //   timelineStart + delta >= 0            (don't fall off the timeline)
            juce::int64 d = deltaSamples;
            d = juce::jmax (d, -drag.origSourceOffset);
            d = juce::jmax (d, -drag.origTimelineStart);
            d = juce::jmin (d, drag.origLength - kMinLengthSamples);
            r.timelineStart   = drag.origTimelineStart + d;
            r.lengthInSamples = drag.origLength        - d;
            r.sourceOffset    = drag.origSourceOffset  + d;
            break;
        }
        case RegionOp::TrimEnd:
        {
            const juce::int64 newLen = juce::jmax (kMinLengthSamples,
                                                    drag.origLength + deltaSamples);
            r.lengthInSamples = newLen;
            break;
        }
        case RegionOp::FadeIn:
        {
            // Drag right grows the fade-in. Clamp so fadeIn + fadeOut
            // never exceeds the region's length (the renderer assumes
            // the two ramps don't overlap; an overlapping pair would
            // attenuate the middle to a value below 1.0 unintentionally).
            const auto maxFadeIn = juce::jmax ((juce::int64) 0,
                                                  r.lengthInSamples - drag.origFadeOut);
            r.fadeInSamples = juce::jlimit ((juce::int64) 0,
                                              maxFadeIn,
                                              drag.origFadeIn + deltaSamples);
            fadeGuideX = xForSample (r.timelineStart + r.fadeInSamples);
            break;
        }
        case RegionOp::FadeOut:
        {
            // Mirror of FadeIn. Drag LEFT (negative delta) grows the
            // fade-out, so subtract delta from the original length.
            const auto maxFadeOut = juce::jmax ((juce::int64) 0,
                                                   r.lengthInSamples - drag.origFadeIn);
            r.fadeOutSamples = juce::jlimit ((juce::int64) 0,
                                               maxFadeOut,
                                               drag.origFadeOut - deltaSamples);
            fadeGuideX = xForSample (r.timelineStart
                                      + r.lengthInSamples - r.fadeOutSamples);
            break;
        }
        case RegionOp::AdjustGain:
        {
            // Vertical pixels -> dB. Up = louder, down = quieter.
            // 0.1 dB per pixel gives a comfortable range without the
            // user having to drag wildly: 60 px = 6 dB, 120 px = the
            // full +12 dB ceiling. Apply the same dB delta to every
            // selected region (anchor + additional); per-region clamp
            // to [-24, +12] is fine even when origs differ - the
            // group goes "out of lockstep" only at the boundaries.
            const float deltaPx = (float) (e.getMouseDownY() - e.y);
            const float deltaDb = deltaPx * 0.10f;
            r.gainDb = juce::jlimit (-24.0f, 12.0f,
                                       drag.origGainDb + deltaDb);
            for (const auto& a : drag.additional)
            {
                auto& addRegions = session.track (a.track).regions;
                if (a.regionIdx < 0 || a.regionIdx >= (int) addRegions.size())
                    continue;
                addRegions[(size_t) a.regionIdx].gainDb =
                    juce::jlimit (-24.0f, 12.0f, a.origGainDb + deltaDb);
            }
            break;
        }
        case RegionOp::None:
        case RegionOp::TakeBadge:
            break;  // mouseDown handled the rotation; no drag semantics
    }
    repaint();
}

void TapeStrip::mouseUp (const juce::MouseEvent& e)
{
    // Ruler-selection finalisation. A drag shorter than ≈1024 samples is a
    // click → seek the playhead. A real drag pops a menu asking loop vs
    // punch — dragging itself never auto-commits transport state.
    //
    // We DON'T clear rulerSelection before the menu shows: keeping it active
    // leaves the grey highlight painted while the menu is open; each lambda
    // clears it as part of the same setLoop/setPunch call so the next paint
    // jumps straight to the committed green/red range — no grey blink.
    if (rulerSelection.active)
    {
        const auto a = juce::jmin (rulerSelection.originSample,
                                     rulerSelection.currentSample);
        const auto b = juce::jmax (rulerSelection.originSample,
                                     rulerSelection.currentSample);

        constexpr juce::int64 kMinUsefulRangeSamples = 1024;
        if (b - a <= kMinUsefulRangeSamples)
        {
            const auto sample = snap::snapAbsoluteToGrid (
                juce::jmax ((juce::int64) 0, a), session, engine.getCurrentSampleRate());
            engine.getTransport().setPlayhead (sample);
            // Remember this click so a future Stop in "Return to last
            // clicked" mode lands here.
            session.lastClickedTimelineSample.store (sample, std::memory_order_relaxed);
            rulerSelection = {};
            repaint();
            return;
        }

        const auto cursor = e.getScreenPosition();
        juce::PopupMenu m;
        m.addItem ("Set loop here",
                    [safeThis = juce::Component::SafePointer<TapeStrip> (this), a, b]
                    {
                        if (safeThis == nullptr) return;
                        auto& transport = safeThis->engine.getTransport();
                        transport.setLoopRange (a, b);
                        transport.setLoopEnabled (true);
                        safeThis->rulerSelection = {};
                        safeThis->repaint();
                    });
        m.addItem ("Set punch in / out here",
                    [safeThis = juce::Component::SafePointer<TapeStrip> (this), a, b]
                    {
                        if (safeThis == nullptr) return;
                        auto& transport = safeThis->engine.getTransport();
                        transport.setPunchRange (a, b);
                        transport.setPunchEnabled (true);
                        safeThis->rulerSelection = {};
                        safeThis->repaint();
                    });
        m.addSeparator();
        m.addItem ("Cancel",
                    [safeThis = juce::Component::SafePointer<TapeStrip> (this)]
                    {
                        if (safeThis == nullptr) return;
                        safeThis->rulerSelection = {};
                        safeThis->repaint();
                    });
        // Dismiss callback catches escape / click-outside (no item chosen)
        // so the grey highlight still clears in those paths.
        showContextMenu (m, *this, cursor,
                          [safeThis = juce::Component::SafePointer<TapeStrip> (this)] (int)
                          {
                              if (safeThis == nullptr) return;
                              if (safeThis->rulerSelection.active)
                              {
                                  safeThis->rulerSelection = {};
                                  safeThis->repaint();
                              }
                          });
        return;
    }

    // MIDI region drag finalise. If the cursor moved at all from the
    // origin, submit a MidiRegionEditAction (before/after) to the
    // engine's UndoManager so Cmd+Z reverts. mouseDrag has already
    // mutated the live region; here we roll it back to the captured
    // origState and let perform() re-apply afterState through the
    // action so the recorded transaction is the canonical one.
    if (midiDrag.active())
    {
        auto& v = session.track (midiDrag.track).midiRegions.currentMutable();
        if (midiDrag.regionIdx >= 0 && midiDrag.regionIdx < (int) v.size())
        {
            const auto afterState = v[(size_t) midiDrag.regionIdx];
            if (afterState.timelineStart != midiDrag.origState.timelineStart)
            {
                v[(size_t) midiDrag.regionIdx] = midiDrag.origState;
                auto& um = engine.getUndoManager();
                um.beginNewTransaction ("Move MIDI region");
                um.perform (new MidiRegionEditAction (
                    session, engine, midiDrag.track, midiDrag.regionIdx,
                    midiDrag.origState, afterState));
            }
        }
        midiDrag.clear();
        repaint();
        return;
    }

    // Rubber-band finalisation. Walk every audio region; any whose
    // painted rect intersects the box gets added to additional-
    // Selections. We don't replace the existing selection - the
    // user's modifier was Shift/Cmd, which is "extend", not
    // "replace". Empty box (a click without drag) is a no-op so the
    // user's existing selection stays intact.
    if (rubberBandActive)
    {
        if (! rubberBand.isEmpty())
        {
            const auto col = tracksColumnBounds();
            for (int t = 0; t < Session::kNumTracks; ++t)
            {
                const auto row = rowBounds (t);
                if (row.isEmpty()) continue;
                if (rubberBand.getBottom() < row.getY()) continue;
                if (rubberBand.getY() > row.getBottom()) continue;
                const auto& regions = session.track (t).regions;
                for (int i = 0; i < (int) regions.size(); ++i)
                {
                    const auto& r = regions[(size_t) i];
                    const int x0 = xForSample (r.timelineStart);
                    const int x1 = xForSample (r.timelineStart + r.lengthInSamples);
                    auto regionRect = juce::Rectangle<int> (
                            x0, row.getY() + 1,
                            juce::jmax (2, x1 - x0),
                            row.getHeight() - 2)
                        .getIntersection (col.withTrimmedTop (1).withTrimmedBottom (1));
                    if (regionRect.isEmpty()) continue;
                    if (! rubberBand.intersects (regionRect)) continue;

                    // Promote the first hit to primary if no primary
                    // exists; otherwise add to the additional set.
                    if (selectedTrack < 0 || selectedRegion < 0)
                    {
                        selectedTrack  = t;
                        selectedRegion = i;
                    }
                    else if (! isRegionSelected (t, i))
                    {
                        const RegionId id { t, i };
                        auto pos = std::lower_bound (additionalSelections.begin(),
                                                        additionalSelections.end(), id);
                        additionalSelections.insert (pos, id);
                    }
                }
            }
        }
        rubberBandActive = false;
        rubberBand = {};
        repaint();
        return;
    }

    // Bracket drag finalisation. Transport state is already up-to-date
    // from each mouseDrag call; nothing to do beyond clearing the drag
    // so future mouseDrags route through the right path.
    if (bracketDrag.active)
    {
        bracketDrag = {};
        return;
    }


    // Marker drag finalisation. A click without movement seeks; a real
    // drag commits the new marker position (and re-sorts the vector so
    // hit-testing stays consistent next time).
    if (markerDrag.active)
    {
        if (markerDrag.moved
            && markerDrag.index >= 0
            && markerDrag.index < (int) session.getMarkers().size())
        {
            // Capture the drag's after-state, build the matching
            // MoveMarkerAction so the swap is one undo transaction. The
            // action's perform() finds the marker at toSamples and is
            // a no-op (drag already mutated it); undo() flips it back.
            auto& mks = session.getMarkers();
            const auto& m = mks[(size_t) markerDrag.index];
            const auto toSamples = m.timelineSamples;
            const auto name      = m.name;

            // Re-sort by timelineSamples so the painter still iterates
            // left-to-right after the drag.
            std::stable_sort (mks.begin(), mks.end(),
                [] (const Marker& a, const Marker& b)
                { return a.timelineSamples < b.timelineSamples; });

            auto& um = engine.getUndoManager();
            um.beginNewTransaction ("Move marker");
            um.perform (new MoveMarkerAction (session, name,
                                                markerDrag.originSample,
                                                toSamples));
        }
        else if (! markerDrag.moved
                 && markerDrag.index >= 0
                 && markerDrag.index < (int) session.getMarkers().size())
        {
            // Pure click on a flag → seek to the marker.
            const auto& m = session.getMarkers()[(size_t) markerDrag.index];
            engine.getTransport().setPlayhead (m.timelineSamples);
        }
        markerDrag = {};
        repaint();
        return;
    }

    if (tempoDrag.active)
    {
        if (tempoDrag.moved)
        {
            // Live drag already published the moved map; wrap the
            // original -> final swap in one SetTempoMapAction for undo.
            auto& um = engine.getUndoManager();
            um.beginNewTransaction ("Move tempo");
            um.perform (new SetTempoMapAction (engine, tempoDrag.orig,
                                                 session.tempoMap.points()));
        }
        else if (tempoDrag.index >= 0
                 && tempoDrag.index < (int) tempoDrag.orig.size())
        {
            // Pure click on a tempo marker → seek to it.
            engine.getTransport().setPlayhead (
                tempoDrag.orig[(size_t) tempoDrag.index].timelineSamples);
        }
        tempoDrag = {};
        repaint();
        return;
    }

    if (drag.op == RegionOp::None) return;

    auto& regions = session.track (drag.track).regions;
    if (drag.regionIdx >= 0 && drag.regionIdx < (int) regions.size())
    {
        // The drag mutated `regions[idx]` in-place for live feedback; capture
        // the final (after) state, rebuild the (before) state from the
        // captured originals, and route the swap through UndoManager so it
        // becomes one undoable transaction. perform() re-applies `after` to
        // the region, which is idempotent - the user sees no glitch.
        AudioRegion afterState  = regions[(size_t) drag.regionIdx];
        AudioRegion beforeState = afterState;
        beforeState.timelineStart   = drag.origTimelineStart;
        beforeState.lengthInSamples = drag.origLength;
        beforeState.sourceOffset    = drag.origSourceOffset;
        beforeState.fadeInSamples   = drag.origFadeIn;
        beforeState.fadeOutSamples  = drag.origFadeOut;
        beforeState.gainDb          = drag.origGainDb;

        // Skip if nothing actually moved (a click without a drag).
        if (beforeState.timelineStart   != afterState.timelineStart
            || beforeState.lengthInSamples != afterState.lengthInSamples
            || beforeState.sourceOffset    != afterState.sourceOffset
            || beforeState.fadeInSamples   != afterState.fadeInSamples
            || beforeState.fadeOutSamples  != afterState.fadeOutSamples
            || beforeState.gainDb          != afterState.gainDb)
        {
            const bool isGroup = ! drag.additional.empty()
                && (drag.op == RegionOp::Move || drag.op == RegionOp::AdjustGain);
            const char* label =
                drag.op == RegionOp::Move        ? (isGroup ? "Move regions" : "Move region") :
                drag.op == RegionOp::FadeIn      ? "Adjust fade-in" :
                drag.op == RegionOp::FadeOut     ? "Adjust fade-out" :
                drag.op == RegionOp::AdjustGain  ? (isGroup ? "Adjust regions gain" : "Adjust region gain") :
                                                    "Trim region";
            auto& um = engine.getUndoManager();
            um.beginNewTransaction (label);
            um.perform (new RegionEditAction (session, engine,
                                                drag.track, drag.regionIdx,
                                                beforeState, afterState));
            // Group drag: emit one RegionEditAction per additional
            // selection, all bundled into the transaction we just
            // started so undo reverts the whole group at once.
            for (const auto& a : drag.additional)
            {
                auto& addRegions = session.track (a.track).regions;
                if (a.regionIdx < 0 || a.regionIdx >= (int) addRegions.size())
                    continue;
                AudioRegion addAfter  = addRegions[(size_t) a.regionIdx];
                AudioRegion addBefore = addAfter;
                addBefore.timelineStart = a.origTimelineStart;
                addBefore.gainDb        = a.origGainDb;
                if (addBefore.timelineStart == addAfter.timelineStart
                    && addBefore.gainDb == addAfter.gainDb)
                    continue;
                um.perform (new RegionEditAction (session, engine,
                                                    a.track, a.regionIdx,
                                                    addBefore, addAfter));
            }
        }
    }

    drag = {};
    fadeGuideX = -1;
    rebuildPlaybackIfStopped();
    repaint();
}

void TapeStrip::mouseDoubleClick (const juce::MouseEvent& e)
{
    // Right-click double doesn't make sense for create-region.
    if (e.mods.isRightButtonDown()) return;

    // Marker double-click: rename. Mirrors the right-click "Rename" item —
    // same modal and the same undoable ParamEditAction path so Cmd+Z reverts.
    if (const int markerIdx = hitTestMarker (e.x, e.y); markerIdx >= 0)
    {
        const auto& markers = session.getMarkers();
        if (markerIdx >= (int) markers.size()) return;
        const juce::String current = markers[(size_t) markerIdx].name;
        auto* host = getTopLevelComponent();
        if (host == nullptr) host = this;
        juce::Component::SafePointer<TapeStrip> safeThis (this);
        showDuskTextInput (*host, "Rename marker", "New name:", current,
            [safeThis, markerIdx] (const juce::String& newName)
            {
                if (safeThis == nullptr) return;
                const auto trimmed = newName.trim();
                if (trimmed.isEmpty()) return;
                auto& s = safeThis->session;
                const auto& m2 = s.getMarkers();
                if (markerIdx < 0 || markerIdx >= (int) m2.size()) return;
                const auto oldName = m2[(size_t) markerIdx].name;
                if (oldName == trimmed) return;
                auto& um = safeThis->engine.getUndoManager();
                um.beginNewTransaction ("Rename marker");
                um.perform (new ParamEditAction (
                    [&s, idx = markerIdx, trimmed] { s.renameMarker (idx, trimmed); },
                    [&s, idx = markerIdx, oldName] { s.renameMarker (idx, oldName); }));
                safeThis->repaint();
            });
        return;
    }

    // Tempo-point double-click: edit its BPM. The discoverable path for
    // changing tempo — right-click still offers add / delete. Checked after
    // markers so a marker pill double-click always wins on shared columns.
    if (const int tIdx = hitTestTempoPoint (e.x, e.y); tIdx != -1)
    {
        if (tIdx == kTempoBaseHandle)
            editBaseTempo();
        else
            editTempoPointBpm (session.tempoMap.points()[(size_t) tIdx].timelineSamples);
        return;
    }

    auto col = tracksColumnBounds();
    if (! col.contains (e.x, e.y)) return;

    // Find which track row was double-clicked.
    int trackIdx = -1;
    for (int t = 0; t < Session::kNumTracks; ++t)
    {
        if (rowBounds (t).contains (e.x, e.y))
        {
            trackIdx = t;
            break;
        }
    }
    if (trackIdx < 0) return;

    // Audio-region hit: open the AudioRegionEditor modal. Walk newest-
    // first so overlaid regions prefer the most-recently-added (matches
    // the painter's draw order).
    {
        const auto& ar = session.track (trackIdx).regions;
        for (int i = (int) ar.size() - 1; i >= 0; --i)
        {
            const auto& r = ar[(size_t) i];
            const int x0 = xForSample (r.timelineStart);
            const int x1 = xForSample (r.timelineStart + r.lengthInSamples);
            if (e.x < x0 || e.x > x1) continue;
            if (onAudioRegionDoubleClicked) onAudioRegionDoubleClicked (trackIdx, i);
            return;
        }
    }

    // MIDI-region hit: open the PianoRollComponent modal. Same gesture
    // shape as audio so users only have to remember one rule
    // ("double-click any region to edit it").
    {
        const auto& mr = session.track (trackIdx).midiRegions.current();
        for (int i = (int) mr.size() - 1; i >= 0; --i)
        {
            const auto& r = mr[(size_t) i];
            const int x0 = xForSample (r.timelineStart);
            const int x1 = xForSample (r.timelineStart + r.lengthInSamples);
            if (e.x < x0 || e.x > x1) continue;
            if (onMidiRegionDoubleClicked) onMidiRegionDoubleClicked (trackIdx, i);
            return;
        }
    }

    // Only MIDI-mode tracks can host the create-region-on-empty-grid
    // gesture. Audio tracks with no region under the cursor fall
    // through to here and are no-op.
    if (session.track (trackIdx).mode.load (std::memory_order_relaxed)
        != (int) Track::Mode::Midi)
        return;

    // Don't create on top of an existing region - that's the click-to-
    // edit path. mouseDown's MIDI hit-test would have already opened
    // the piano roll for that region, so the second click of the
    // double-click won't reach here unless the user clicked empty.
    {
        const auto& mr = session.track (trackIdx).midiRegions.current();
        for (const auto& r : mr)
        {
            const int x0 = xForSample (r.timelineStart);
            const int x1 = xForSample (r.timelineStart + r.lengthInSamples);
            if (e.x >= x0 && e.x <= x1) return;
        }
    }

    // Create an empty 4-bar region at the click position. The piano
    // roll is region-driven (no "open empty piano roll on a track" path
    // exists yet) so this doubles as a manual entry point for users
    // whose recording captures aren't reaching the lane.
    //
    // Wrapped in a CreateMidiRegionAction so undo / redo work the same
    // way they do for every other timeline mutation (paste, delete,
    // split, marker add). Without this the user could create a region
    // and then have no way to undo if they wanted to redo a recording
    // capture into the same slot.
    const auto sr  = engine.getCurrentSampleRate();
    const float bpm = session.tempoBpm.load (std::memory_order_relaxed);
    const int beatsBar = juce::jmax (1, session.beatsPerBar.load (std::memory_order_relaxed));
    if (sr <= 0.0 || bpm <= 0.0f) return;

    juce::int64 startSample = juce::jmax ((juce::int64) 0, sampleAtX (e.x));

    // Snap start to the nearest beat when snap-to-grid is on. Mirrors
    // the drag-snap pattern at line ~641 but operates on an absolute
    // position rather than a delta - new regions have no prior origin
    // to be "mid-step" relative to. The user can override by toggling
    // snap off before the double-click.
    startSample = snap::snapAbsoluteToGrid (startSample, session, sr);

    const juce::int64 fourBarsSamples =
        (juce::int64) (sr * 60.0 / (double) bpm * (double) beatsBar * 4.0);
    const juce::int64 fourBarsTicks = samplesToTicks (fourBarsSamples, sr, bpm);

    auto action = std::make_unique<CreateMidiRegionAction> (
        session, trackIdx, startSample, fourBarsSamples, fourBarsTicks);
    auto* actionRaw = action.get();

    auto& um = engine.getUndoManager();
    if (! um.perform (action.release(), "Create MIDI region"))
        return;

    repaint();
    const int newRegionIdx = actionRaw->getInsertedIndex();
    if (onMidiRegionDoubleClicked && newRegionIdx >= 0)
        onMidiRegionDoubleClicked (trackIdx, newRegionIdx);
}

void TapeStrip::mouseMove (const juce::MouseEvent& e)
{
    const auto mode = session.editMode;
    const auto hit  = hitTestRegion (e.x, e.y);

    // Update hover state so paint() can show the fade handles only
    // for the region under the cursor (or the selected region). Skip
    // the repaint when nothing changed - mouseMove fires per pixel
    // of cursor motion.
    if (hit.track != hoveredTrack || hit.regionIdx != hoveredRegion)
    {
        hoveredTrack  = hit.track;
        hoveredRegion = hit.regionIdx;
        repaint();
    }

    const bool overMidi = overMidiRegionBody (e.x, e.y);

    // Cut: scissors glyph only over an AUDIO region body (the only thing the
    // cut tool splits on the lane); empty lane and MIDI regions get the plain
    // arrow so the cursor never advertises a split that won't happen. Over the
    // ruler / label column fall through so marker + loop/punch affordances work.
    if (mode == EditMode::Cut && tracksColumnBounds().contains (e.x, e.y))
    {
        setHoverCursor (hit.regionIdx >= 0 ? juce::MouseCursor::NoCursor
                                           : juce::MouseCursor::NormalCursor,
                        e.x, e.y);
        return;
    }

    // Tempo markers are drag-to-reposition (left/right) — a horizontal-resize
    // cursor advertises it. The bar-1 anchor (sample 0) doesn't move, so it
    // keeps the plain cursor.
    if (const int tIdx = hitTestTempoPoint (e.x, e.y); tIdx >= 0)
    {
        const auto& pts = session.tempoMap.points();
        if (tIdx < (int) pts.size() && pts[(size_t) tIdx].timelineSamples != 0)
        {
            setHoverCursor (juce::MouseCursor::LeftRightResizeCursor, e.x, e.y);
            return;
        }
    }

    // Cursor feedback so the user can tell where edges/body are without
    // clicking blindly.
    if (hitTestMarker (e.x, e.y) >= 0)
    {
        // Markers are click-to-seek and drag-to-reposition - "dragging
        // hand" reads as both clickable and draggable.
        setHoverCursor (juce::MouseCursor::DraggingHandCursor, e.x, e.y);
        return;
    }

    if (const auto bh = hitTestBracket (e.x, e.y); bh != BracketHit::None)
    {
        // Endpoint pills get a horizontal-resize cursor; bar drags get
        // a "moving" hand. Same pattern as region trim vs region move.
        const bool isBar = (bh == BracketHit::LoopBar || bh == BracketHit::PunchBar);
        setHoverCursor (isBar ? juce::MouseCursor::DraggingHandCursor
                              : juce::MouseCursor::LeftRightResizeCursor, e.x, e.y);
        return;
    }

    // MIDI regions are move-only on the lane (trim happens in the piano roll):
    // hand glyph in Grab, plain arrow otherwise. Audio region ops fall through
    // to the switch below.
    if (overMidi)
    {
        setHoverCursor (mode == EditMode::Grab ? juce::MouseCursor::NoCursor
                                                : juce::MouseCursor::NormalCursor, e.x, e.y);
        return;
    }

    switch (hit.op)
    {
        case RegionOp::TrimStart:
        case RegionOp::TrimEnd:
        case RegionOp::FadeIn:
        case RegionOp::FadeOut:
            // Same horizontal-resize cursor for both edge trim and fade
            // handle - the user sees they can drag horizontally either
            // way. The y-position (top band vs full edge) tells them
            // which mode they're in; we don't need a separate cursor.
            setHoverCursor (juce::MouseCursor::LeftRightResizeCursor, e.x, e.y);
            break;
        case RegionOp::Move:
            // Alt over the body promotes Move -> AdjustGain on click;
            // signal that with an up/down resize cursor so the user
            // doesn't drag horizontally and accidentally move. In Grab
            // mode the plain hover shows the hand cursor so the active
            // tool is always visible; other modes stay on the normal
            // arrow so a dragging-hand cursor doesn't compete with the
            // actual drag-in-progress cursor.
            setHoverCursor (e.mods.isAltDown()
                              ? juce::MouseCursor::UpDownResizeCursor
                              : (mode == EditMode::Grab
                                    ? juce::MouseCursor::NoCursor
                                    : juce::MouseCursor::NormalCursor), e.x, e.y);
            break;
        case RegionOp::AdjustGain:
            setHoverCursor (juce::MouseCursor::UpDownResizeCursor, e.x, e.y);
            break;
        case RegionOp::TakeBadge:
            setHoverCursor (juce::MouseCursor::PointingHandCursor, e.x, e.y);
            break;
        case RegionOp::None:
            // Empty lane = plain arrow even in Grab. The hand glyph is
            // reserved for hovering a region body (RegionOp::Move).
            setHoverCursor (juce::MouseCursor::NormalCursor, e.x, e.y);
            break;
    }
}

void TapeStrip::setHoverCursor (const juce::MouseCursor& c, int x, int y)
{
    setMouseCursor (c);
    if (! (onMouseMovedForCursor && onMouseExitedForCursor))
        return;
    if (c == juce::MouseCursor::NoCursor)
        onMouseMovedForCursor (*this, { x, y }, session.editMode, {});
    else
        onMouseExitedForCursor();
}

void TapeStrip::refreshModeCursor()
{
    const auto mode = session.editMode;
    const auto p = getMouseXYRelative();
    // A glyph shows only over the lanes, and only where mouseMove would hide
    // the native cursor: Cut anywhere on a lane (scissors), Grab over a
    // region body (hand). Empty lane / ruler / label column = plain arrow -
    // hiding the whole component would leave an invisible pointer there until
    // the next move (on platforms where JUCE's NoCursor does the hiding).
    const bool overLanes = isMouseOver (true) && tracksColumnBounds().contains (p.x, p.y);
    bool wantGlyph = false;
    if (overLanes)
    {
        const auto hit      = hitTestRegion (p.x, p.y);
        const bool overMidi = overMidiRegionBody (p.x, p.y);
        if (mode == EditMode::Cut)
            wantGlyph = (hit.regionIdx >= 0);                        // scissors over audio (splittable) only
        else if (mode == EditMode::Grab)
            wantGlyph = (hit.op == RegionOp::Move || overMidi);      // hand over a body
    }
    setMouseCursor (wantGlyph ? juce::MouseCursor::NoCursor
                              : juce::MouseCursor::NormalCursor);
    // Mode flipped via toolbar / hotkey with no mouse event to follow -
    // seed the overlay glyph from the current pointer so it appears
    // immediately instead of waiting for the next move.
    if (onMouseMovedForCursor && onMouseExitedForCursor)
    {
        if (wantGlyph) onMouseMovedForCursor (*this, { p.x, p.y }, mode, {});
        else           onMouseExitedForCursor();
    }
}

void TapeStrip::mouseExit (const juce::MouseEvent&)
{
    if (onMouseExitedForCursor) onMouseExitedForCursor();
    if (hoveredTrack != -1 || hoveredRegion != -1)
    {
        hoveredTrack  = -1;
        hoveredRegion = -1;
        repaint();
    }
}

void TapeStrip::mouseWheelMove (const juce::MouseEvent& e,
                                  const juce::MouseWheelDetails& w)
{
    // Cmd/Ctrl + Shift + wheel = VERTICAL zoom (track row height). Cmd
    // alone is already horizontal zoom (matches the audio / piano-roll
    // editors), so vertical takes the Shift modifier.
    if ((e.mods.isCommandDown() || e.mods.isCtrlDown()) && e.mods.isShiftDown())
    {
        const int delta = (w.deltaY > 0.0f) ? 4 : -4;
        const int nh    = juce::jlimit (kRowHMin, kRowHMax, rowHeight + delta);
        if (nh != rowHeight)
        {
            rowHeight = nh;
            // The strip's band height is fixed (bank-fit at default row
            // height) so the mixer faders never move; taller rows just
            // scroll inside it. No parent relayout needed.
            clampRowScroll();
            repaint();
        }
        return;
    }
    // Cmd/Ctrl + wheel = horizontal zoom anchored on the cursor sample.
    if (e.mods.isCommandDown() || e.mods.isCtrlDown())
    {
        const float factor = w.deltaY > 0.0f ? 1.15f : (1.0f / 1.15f);
        zoomByFactor (factor, e.x);
        return;
    }
    // Plain wheel: vertical scroll first when rows overflow the (capped)
    // strip height, else horizontal scroll when zoomed in. Absorb either
    // way so the console behind us doesn't scroll.
    const int band = juce::jmax (0, getHeight() - kRulerH);
    if (rowsContentHeight() > band && std::abs (w.deltaY) > 0.001f)
    {
        rowScrollY -= (int) std::round (w.deltaY * (float) (rowHeight + kRowGap) * 3.0f);
        clampRowScroll();
        repaint();
        return;
    }
    if (userZoomFactor > 1.0f)
    {
        const double sr = engine.getCurrentSampleRate();
        if (sr > 0.0)
        {
            // ~half a second per wheel-notch. Sign: positive deltaY (away
            // from user) = scroll left = decrease scrollSamples.
            const auto step = (juce::int64) (sr * 0.5);
            const double dx = std::abs (w.deltaX) > 0.001f ? w.deltaX
                                                            : w.deltaY;
            const auto delta = (juce::int64) (dx * (double) step);
            scrollSamples = juce::jmax<juce::int64> (0, scrollSamples - delta);
            repaint();
        }
    }
}

void TapeStrip::showRegionContextMenu (const RegionHit& hit, juce::Point<int> screenPos)
{
    auto& transport = engine.getTransport();
    const auto playhead = transport.getPlayhead();

    const auto& region = session.track (hit.track).regions[(size_t) hit.regionIdx];
    const auto regionEnd = region.timelineStart + region.lengthInSamples;
    const bool playheadInside =
        playhead > region.timelineStart && playhead < regionEnd;

    juce::PopupMenu m;
    m.addSectionHeader (juce::String::formatted ("Track %d region %d",
                                                  hit.track + 1, hit.regionIdx + 1));
    m.addItem ("Loop region",
                [safeThis = juce::Component::SafePointer<TapeStrip> (this),
                 regionStart = region.timelineStart,
                 regionEnd]
                {
                    if (safeThis == nullptr) return;
                    auto& tr = safeThis->engine.getTransport();
                    tr.setLoopRange (regionStart, regionEnd);
                    tr.setLoopEnabled (true);
                    tr.setPlayhead (regionStart);
                    safeThis->repaint();
                });
    m.addItem ("Split at playhead", playheadInside,
                false /*ticked*/,
                [safeThis = juce::Component::SafePointer<TapeStrip> (this),
                 hitCopy = hit, playhead]
                {
                    if (safeThis == nullptr) return;
                    auto& um = safeThis->engine.getUndoManager();
                    um.beginNewTransaction ("Split region");
                    um.perform (new SplitRegionAction (safeThis->session, safeThis->engine,
                                                         hitCopy.track,
                                                         hitCopy.regionIdx,
                                                         playhead));
                    safeThis->repaint();
                });

    // Join regions: enabled when at least two regions on this track are
    // selected (counting the primary + additional). Same-source abutting
    // selections collapse cheaply; everything else renders a glued WAV.
    int joinCount = 0;
    std::vector<int> joinIdxs;
    if (selectedTrack == hit.track && selectedRegion >= 0)
    {
        joinIdxs.push_back (selectedRegion);
        ++joinCount;
    }
    for (const auto& id : additionalSelections)
        if (id.track == hit.track)
        {
            joinIdxs.push_back (id.regionIdx);
            ++joinCount;
        }
    m.addItem ("Join selected regions", joinCount >= 2, false,
                [safeThis = juce::Component::SafePointer<TapeStrip> (this),
                 track = hit.track, joinIdxs]
                {
                    if (safeThis == nullptr) return;
                    auto& um = safeThis->engine.getUndoManager();
                    um.beginNewTransaction (
                        juce::String ("Join ") + juce::String ((int) joinIdxs.size())
                        + " regions");
                    um.perform (new JoinRegionsAction (
                        safeThis->session, safeThis->engine, track, joinIdxs));
                    safeThis->clearAllSelections();
                    safeThis->repaint();
                });
    m.addSeparator();

    // Rename / clear-label action. Single-region only - giving every
    // member of a multi-selection the same name is rarely useful;
    // users rename one at a time.
    const auto currentLabel = region.label;
    const juce::String renameLabel = currentLabel.isEmpty()
        ? juce::String ("Add label...")
        : juce::String ("Rename label...");
    m.addItem (renameLabel,
                [safeThis = juce::Component::SafePointer<TapeStrip> (this),
                 hitCopy = hit, currentLabel]
                {
                    if (safeThis == nullptr) return;
                    auto* host = safeThis->getTopLevelComponent();
                    if (host == nullptr) host = safeThis.getComponent();
                    showEmbeddedTextInput (*host, "Region label",
                        "Type a label for this region:",
                        currentLabel, "OK",
                        [safeThis, hitCopy] (juce::String newLabel)
                        {
                            if (safeThis == nullptr) return;
                            auto& regs = safeThis->session.track (hitCopy.track).regions;
                            if (hitCopy.regionIdx < 0
                                || hitCopy.regionIdx >= (int) regs.size()) return;
                            const auto& current = regs[(size_t) hitCopy.regionIdx];
                            if (current.label == newLabel) return;
                            AudioRegion afterState  = current;
                            AudioRegion beforeState = current;
                            afterState.label = std::move (newLabel);
                            auto& um = safeThis->engine.getUndoManager();
                            um.beginNewTransaction (afterState.label.isEmpty()
                                                       ? "Clear region label"
                                                       : "Rename region");
                            um.perform (new RegionEditAction (
                                safeThis->session, safeThis->engine,
                                hitCopy.track, hitCopy.regionIdx,
                                beforeState, afterState));
                            safeThis->repaint();
                        });
                });

    m.addSeparator();

    // Mute toggle. Single-region action - giving every member of a
    // multi-selection the same mute state IS useful (mute all, then
    // unmute one), so we fold it into a group op when the right-
    // clicked region is part of the multi-selection. Same idiom as
    // the Color submenu below.
    m.addItem (region.muted ? "Unmute region" : "Mute region",
                [safeThis = juce::Component::SafePointer<TapeStrip> (this),
                 hitCopy = hit, currentlyMuted = region.muted]
                {
                    if (safeThis == nullptr) return;
                    const bool target = ! currentlyMuted;
                    std::vector<RegionId> targets;
                    if (safeThis->isRegionSelected (hitCopy.track, hitCopy.regionIdx))
                        targets = safeThis->allSelectedRegions();
                    else
                        targets.push_back ({ hitCopy.track, hitCopy.regionIdx });
                    auto& um = safeThis->engine.getUndoManager();
                    um.beginNewTransaction (target ? "Mute regions" : "Unmute regions");
                    for (const auto& id : targets)
                    {
                        const auto& regs = safeThis->session.track (id.track).regions;
                        if (id.regionIdx < 0 || id.regionIdx >= (int) regs.size()) continue;
                        const auto& current = regs[(size_t) id.regionIdx];
                        if (current.muted == target) continue;
                        AudioRegion afterState  = current;
                        AudioRegion beforeState = current;
                        afterState.muted = target;
                        um.perform (new RegionEditAction (
                            safeThis->session, safeThis->engine,
                            id.track, id.regionIdx, beforeState, afterState));
                    }
                    safeThis->rebuildPlaybackIfStopped();
                    safeThis->repaint();
                });

    // Lock toggle. Same multi-aware pattern as Mute. Locked regions
    // reject move / trim / fade / gain / split / nudge / delete -
    // playback unaffected. Toggle is the only operation that works
    // on a locked region, so the menu item itself is always
    // clickable.
    m.addItem (region.locked ? "Unlock region" : "Lock region",
                [safeThis = juce::Component::SafePointer<TapeStrip> (this),
                 hitCopy = hit, currentlyLocked = region.locked]
                {
                    if (safeThis == nullptr) return;
                    const bool target = ! currentlyLocked;
                    std::vector<RegionId> targets;
                    if (safeThis->isRegionSelected (hitCopy.track, hitCopy.regionIdx))
                        targets = safeThis->allSelectedRegions();
                    else
                        targets.push_back ({ hitCopy.track, hitCopy.regionIdx });
                    auto& um = safeThis->engine.getUndoManager();
                    um.beginNewTransaction (target ? "Lock regions" : "Unlock regions");
                    for (const auto& id : targets)
                    {
                        const auto& regs = safeThis->session.track (id.track).regions;
                        if (id.regionIdx < 0 || id.regionIdx >= (int) regs.size()) continue;
                        const auto& current = regs[(size_t) id.regionIdx];
                        if (current.locked == target) continue;
                        AudioRegion afterState  = current;
                        AudioRegion beforeState = current;
                        afterState.locked = target;
                        um.perform (new RegionEditAction (
                            safeThis->session, safeThis->engine,
                            id.track, id.regionIdx, beforeState, afterState));
                    }
                    safeThis->repaint();
                });

    m.addSeparator();

    // Takes submenu (audio). Shown when the recorder has absorbed older
    // takes into previousTakes. Selecting a previous take swaps its
    // payload (file / sourceOffset / lengthInSamples) into the live
    // region; the displaced live take drops back into the slot the
    // chosen take came from so the cycle is reversible.
    if (! region.previousTakes.empty())
    {
        juce::PopupMenu takesSub;
        const int numTakes = (int) region.previousTakes.size() + 1;
        takesSub.addSectionHeader (juce::String::formatted ("%d takes", numTakes));
        takesSub.addItem ("Take 1 (current)", false, true, [] {});
        for (int i = 0; i < (int) region.previousTakes.size(); ++i)
        {
            const int takeNumber = i + 2;
            takesSub.addItem (
                juce::String::formatted ("Take %d", takeNumber),
                [safeThis = juce::Component::SafePointer<TapeStrip> (this),
                 hitCopy = hit, takeIdx = i]
                {
                    if (safeThis == nullptr) return;
                    // The region (or whole track) may have been deleted
                    // between menu open + click - bounds-check before
                    // indexing. Matches the MIDI variant's guard.
                    if (hitCopy.track < 0 || hitCopy.track >= Session::kNumTracks) return;
                    const auto& regs = safeThis->session.track (hitCopy.track).regions;
                    if (hitCopy.regionIdx < 0
                        || hitCopy.regionIdx >= (int) regs.size()) return;
                    const auto& cur = regs[(size_t) hitCopy.regionIdx];
                    if (takeIdx < 0 || takeIdx >= (int) cur.previousTakes.size()) return;

                    AudioRegion before = cur;
                    AudioRegion after  = cur;
                    TakeRef oldLive;
                    oldLive.file            = after.file;
                    oldLive.sourceOffset    = after.sourceOffset;
                    oldLive.lengthInSamples = after.lengthInSamples;

                    const TakeRef chosen = after.previousTakes[(size_t) takeIdx];
                    after.file            = chosen.file;
                    after.sourceOffset    = chosen.sourceOffset;
                    after.lengthInSamples = chosen.lengthInSamples;
                    after.previousTakes[(size_t) takeIdx] = oldLive;

                    auto& um = safeThis->engine.getUndoManager();
                    um.beginNewTransaction (
                        juce::String::formatted ("Swap to take %d", takeIdx + 2));
                    um.perform (new RegionEditAction (
                        safeThis->session, safeThis->engine,
                        hitCopy.track, hitCopy.regionIdx,
                        before, after));
                    safeThis->repaint();
                });
        }
        m.addSubMenu ("Takes", takesSub);
        m.addSeparator();
    }

    // Color submenu - 8 curated accent options + "Reset to track
    // colour". Setting goes through RegionEditAction so undo/redo
    // round-trip cleanly. Acts on every selected region (the user's
    // multi-selection) when the right-clicked region is part of one;
    // single-region otherwise. Same logic as note-properties popup.
    juce::PopupMenu colourSub;
    struct PaletteEntry { const char* label; juce::uint32 argb; };
    static const PaletteEntry kPalette[] = {
        { "Reset to track colour", 0x00000000 },   // 0 alpha = transparent = unset
        { "Red",          0xffd05f5f },
        { "Orange",       0xffd09060 },
        { "Yellow",       0xffd0c060 },
        { "Green",        0xff60c070 },
        { "Cyan",         0xff60c0c0 },
        { "Blue",         0xff6090d0 },
        { "Purple",       0xff9070c0 },
        { "Magenta",      0xffc060a0 },
    };
    for (int i = 0; i < (int) (sizeof (kPalette) / sizeof (kPalette[0])); ++i)
    {
        const bool isReset = (i == 0);
        colourSub.addItem (5000 + i,
                            kPalette[i].label,
                            true,
                            isReset ? region.customColour.isTransparent()
                                    : region.customColour.getARGB() == kPalette[i].argb);
    }
    m.addSubMenu ("Color", colourSub);

    m.addSeparator();
    m.addItem ("Delete region",
                [safeThis = juce::Component::SafePointer<TapeStrip> (this),
                 hitCopy = hit]
                {
                    if (safeThis == nullptr) return;
                    auto& um = safeThis->engine.getUndoManager();
                    um.beginNewTransaction ("Delete region");
                    um.perform (new DeleteRegionAction (safeThis->session, safeThis->engine,
                                                          hitCopy.track,
                                                          hitCopy.regionIdx));
                    safeThis->repaint();
                });

    showContextMenu (m, *this, screenPos,
        [safeThis = juce::Component::SafePointer<TapeStrip> (this), hitCopy = hit]
        (int chosen)
        {
            if (safeThis == nullptr) return;
            // Color choices land in the 5000-range. Other items handle
            // themselves via their per-item lambdas above; we only act
            // on the colour-submenu IDs here.
            if (chosen < 5000 || chosen >= 5000 + (int) (sizeof (kPalette) / sizeof (kPalette[0])))
                return;
            const juce::uint32 newArgb = kPalette[chosen - 5000].argb;
            const juce::Colour newColour (newArgb);

            // Pick the target list - the multi-selection if the
            // right-clicked region is part of one, otherwise just
            // the right-clicked region.
            std::vector<RegionId> targets;
            if (safeThis->isRegionSelected (hitCopy.track, hitCopy.regionIdx))
                targets = safeThis->allSelectedRegions();
            else
                targets.push_back ({ hitCopy.track, hitCopy.regionIdx });

            auto& um = safeThis->engine.getUndoManager();
            um.beginNewTransaction (targets.size() == 1 ? "Set region colour"
                                                          : "Set regions colour");
            for (const auto& id : targets)
            {
                const auto& regs = safeThis->session.track (id.track).regions;
                if (id.regionIdx < 0 || id.regionIdx >= (int) regs.size()) continue;
                const auto& current = regs[(size_t) id.regionIdx];
                if (current.customColour == newColour) continue;
                AudioRegion afterState  = current;
                AudioRegion beforeState = current;
                afterState.customColour = newColour;
                um.perform (new RegionEditAction (safeThis->session, safeThis->engine,
                                                    id.track, id.regionIdx,
                                                    beforeState, afterState));
            }
            safeThis->repaint();
        });
}

void TapeStrip::showMidiRegionContextMenu (int trackIdx, int regionIdx,
                                              juce::Point<int> screenPos)
{
    const auto& mr = session.track (trackIdx).midiRegions.current();
    if (regionIdx < 0 || regionIdx >= (int) mr.size()) return;
    const auto& region = mr[(size_t) regionIdx];

    juce::PopupMenu m;
    m.addSectionHeader (juce::String::formatted ("Track %d MIDI region %d",
                                                  trackIdx + 1, regionIdx + 1));

    m.addItem ("Loop region",
                [safeThis = juce::Component::SafePointer<TapeStrip> (this),
                 regionStart = region.timelineStart,
                 regionEnd   = region.timelineStart + region.lengthInSamples]
                {
                    if (safeThis == nullptr) return;
                    auto& tr = safeThis->engine.getTransport();
                    tr.setLoopRange (regionStart, regionEnd);
                    tr.setLoopEnabled (true);
                    tr.setPlayhead (regionStart);
                    safeThis->repaint();
                });

    m.addSeparator();

    // Rename / clear-label. AlertWindow modal text input, same flow
    // as the audio version. Mutation goes through
    // midiRegions.currentMutable() - the message thread is the only
    // mutator and the audio thread reads via its acquire-loaded
    // pointer, same race profile the existing per-note PianoRoll
    // edits already accept.
    const auto currentLabel = region.label;
    const juce::String renameLabel = currentLabel.isEmpty()
        ? juce::String ("Add label...")
        : juce::String ("Rename label...");
    m.addItem (renameLabel,
                [safeThis = juce::Component::SafePointer<TapeStrip> (this),
                 trackIdx, regionIdx, currentLabel]
                {
                    if (safeThis == nullptr) return;
                    auto* host = safeThis->getTopLevelComponent();
                    if (host == nullptr) host = safeThis.getComponent();
                    showEmbeddedTextInput (*host, "MIDI region label",
                        "Type a label for this region:",
                        currentLabel, "OK",
                        [safeThis, trackIdx, regionIdx] (juce::String newLabel)
                        {
                            if (safeThis == nullptr) return;
                            const auto& live = safeThis->session.track (trackIdx)
                                                  .midiRegions.current();
                            if (regionIdx < 0 || regionIdx >= (int) live.size()) return;
                            MidiRegion before = live[(size_t) regionIdx];
                            MidiRegion after  = before;
                            after.label = std::move (newLabel);
                            auto& um = safeThis->engine.getUndoManager();
                            um.beginNewTransaction ("Label MIDI region");
                            um.perform (new MidiRegionEditAction (safeThis->session,
                                safeThis->engine, trackIdx, regionIdx, before, after));
                            safeThis->repaint();
                        });
                });

    m.addSeparator();

    // Mute / lock toggles — wrapped in MidiRegionEditAction so Cmd+Z reverts.
    m.addItem (region.muted ? "Unmute region" : "Mute region",
                [safeThis = juce::Component::SafePointer<TapeStrip> (this), trackIdx, regionIdx]
                {
                    if (safeThis != nullptr)
                        safeThis->commitMidiRegionToggle (trackIdx, regionIdx, "Mute MIDI region",
                            [] (MidiRegion& r) { r.muted = ! r.muted; });
                });
    m.addItem (region.locked ? "Unlock region" : "Lock region",
                [safeThis = juce::Component::SafePointer<TapeStrip> (this), trackIdx, regionIdx]
                {
                    if (safeThis != nullptr)
                        safeThis->commitMidiRegionToggle (trackIdx, regionIdx, "Lock MIDI region",
                            [] (MidiRegion& r) { r.locked = ! r.locked; });
                });

    m.addSeparator();

    // Takes submenu (MIDI). Same shape as the audio version - swap the
    // live region's musical payload (notes / ccs / lengthInTicks) with
    // a previously-absorbed take. lengthInSamples is recomputed from
    // lengthInTicks at the session's current BPM so the rendered span
    // stays correct after the swap.
    if (! region.previousTakes.empty())
    {
        juce::PopupMenu takesSub;
        const int numTakes = (int) region.previousTakes.size() + 1;
        takesSub.addSectionHeader (juce::String::formatted ("%d takes", numTakes));
        takesSub.addItem ("Take 1 (current)", false, true, [] {});
        for (int i = 0; i < (int) region.previousTakes.size(); ++i)
        {
            const int takeNumber = i + 2;
            takesSub.addItem (
                juce::String::formatted ("Take %d", takeNumber),
                [safeThis = juce::Component::SafePointer<TapeStrip> (this),
                 trackIdx, regionIdx, takeIdx = i]
                {
                    if (safeThis == nullptr) return;
                    const auto& curList = safeThis->session.track (trackIdx)
                                                .midiRegions.current();
                    if (regionIdx < 0 || regionIdx >= (int) curList.size()) return;
                    const auto& cur = curList[(size_t) regionIdx];
                    if (takeIdx < 0 || takeIdx >= (int) cur.previousTakes.size()) return;

                    MidiRegion before = cur;
                    MidiRegion after  = cur;
                    MidiTakeRef oldLive;
                    oldLive.lengthInTicks = after.lengthInTicks;
                    oldLive.notes         = std::move (after.notes);
                    oldLive.ccs           = std::move (after.ccs);

                    after.lengthInTicks = after.previousTakes[(size_t) takeIdx].lengthInTicks;
                    after.notes         = std::move (after.previousTakes[(size_t) takeIdx].notes);
                    after.ccs           = std::move (after.previousTakes[(size_t) takeIdx].ccs);
                    after.previousTakes[(size_t) takeIdx] = std::move (oldLive);

                    // Recompute lengthInSamples from the new lengthInTicks
                    // at the session's current tempo. PlaybackEngine will
                    // also rebuild on the next preparePlayback if tempo
                    // changes later.
                    const double sr = safeThis->engine.getCurrentSampleRate();
                    const float bpm = safeThis->session.tempoBpm.load (std::memory_order_relaxed);
                    if (sr > 0.0 && bpm > 0.0f)
                    {
                        const double samplesPerTick =
                            (sr * 60.0) / ((double) bpm * (double) kMidiTicksPerQuarter);
                        after.lengthInSamples = (juce::int64) std::llround (
                            (double) after.lengthInTicks * samplesPerTick);
                    }

                    auto& um = safeThis->engine.getUndoManager();
                    um.beginNewTransaction (
                        juce::String::formatted ("Swap to take %d", takeIdx + 2));
                    um.perform (new MidiRegionEditAction (
                        safeThis->session, safeThis->engine,
                        trackIdx, regionIdx, before, after));
                    safeThis->repaint();
                });
        }
        m.addSubMenu ("Takes", takesSub);
        m.addSeparator();
    }

    // Same 8-colour palette + Reset as the audio menu.
    juce::PopupMenu colourSub;
    struct PaletteEntry { const char* label; juce::uint32 argb; };
    static const PaletteEntry kPalette[] = {
        { "Reset to track colour", 0x00000000 },
        { "Red",     0xffd05f5f }, { "Orange",  0xffd09060 },
        { "Yellow",  0xffd0c060 }, { "Green",   0xff60c070 },
        { "Cyan",    0xff60c0c0 }, { "Blue",    0xff6090d0 },
        { "Purple",  0xff9070c0 }, { "Magenta", 0xffc060a0 },
    };
    for (int i = 0; i < (int) (sizeof (kPalette) / sizeof (kPalette[0])); ++i)
    {
        const bool isReset = (i == 0);
        colourSub.addItem (5000 + i,
                            kPalette[i].label,
                            true,
                            isReset ? region.customColour.isTransparent()
                                    : region.customColour.getARGB() == kPalette[i].argb);
    }
    m.addSubMenu ("Color", colourSub);

    showContextMenu (m, *this, screenPos,
        [safeThis = juce::Component::SafePointer<TapeStrip> (this),
         trackIdx, regionIdx]
        (int chosen)
        {
            if (safeThis == nullptr) return;
            if (chosen < 5000
                || chosen >= 5000 + (int) (sizeof (kPalette) / sizeof (kPalette[0])))
                return;
            const juce::Colour newColour { kPalette[chosen - 5000].argb };
            safeThis->commitMidiRegionToggle (trackIdx, regionIdx, "Color MIDI region",
                [newColour] (MidiRegion& r) { r.customColour = newColour; });
        });
}

void TapeStrip::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff0e0e10));

    auto label = labelColumnBounds();
    auto col   = tracksColumnBounds();

    // ── Time ruler at the top of the tracks column ──
    auto ruler = rulerBounds();
    g.setColour (juce::Colour (0xff181820));
    g.fillRect (ruler);
    g.setColour (juce::Colour (0xff2a2a30));
    g.drawHorizontalLine (ruler.getBottom() - 1, (float) ruler.getX(), (float) ruler.getRight());

    const double px = pixelsPerSecond();
    const double sr = engine.getCurrentSampleRate();
    if (px > 0.0 && sr > 0.0)
    {
        g.setColour (juce::Colour (0xff707074));
        g.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                                                    9.5f, juce::Font::plain)));

        const auto mode = (TimeDisplayMode) session.timeDisplayMode.load (std::memory_order_relaxed);
        if (mode == TimeDisplayMode::Bars)
        {
            // Bar grid - bar/beat boundaries are musical-time positions resolved
            // through the session tempo map (constant tempoBpm when the map is
            // empty), so the grid follows tempo changes instead of assuming one
            // uniform bar width.
            const int     bpb = juce::jmax (1, session.beatsPerBar.load (std::memory_order_relaxed));
            const juce::int64 ticksPerBar = (juce::int64) bpb * kMidiTicksPerQuarter;

            // Step + sub-tick density are sized from the tempo at the left edge.
            const float  bpmEst = juce::jmax (1.0f, session.bpmAt (scrollSamples));
            const double pxPerBar = px * (double) bpb * 60.0 / (double) bpmEst;
            int barStep = 1;
            if      (pxPerBar < 8.0)  barStep = 16;
            else if (pxPerBar < 16.0) barStep = 8;
            else if (pxPerBar < 32.0) barStep = 4;
            else if (pxPerBar < 64.0) barStep = 2;

            const juce::int64 startTick = session.samplesToTicks (scrollSamples, sr);
            int firstBar = (int) (startTick / ticksPerBar);
            firstBar -= (firstBar % barStep);
            firstBar = juce::jmax (0, firstBar);

            auto barX = [&] (int bar)
            {
                return xForSample (session.ticksToSamples ((juce::int64) bar * ticksPerBar, sr));
            };

            // Beat sub-ticks: only when each beat renders at least ~6 px apart,
            // so they don't smear into a grey band at low zoom. Drawn first
            // (dimmer, shorter) so bar ticks overpaint the first beat of a bar.
            const double pxPerBeat = pxPerBar / (double) bpb;
            if (pxPerBeat >= 6.0 && barStep == 1)
            {
                g.setColour (juce::Colour (0xff3a3a40));
                const float beatY0 = (float) ruler.getY() + (float) kRulerTickBandH * 0.55f;
                const float beatY1 = (float) ruler.getY() + (float) kRulerTickBandH;
                for (int bar = firstBar; barX (bar) <= col.getRight(); ++bar)
                {
                    for (int beat = 1; beat < bpb; ++beat)
                    {
                        const auto tick = (juce::int64) bar * ticksPerBar
                                            + (juce::int64) beat * kMidiTicksPerQuarter;
                        const int x = xForSample (session.ticksToSamples (tick, sr));
                        if (x < col.getX() || x > col.getRight()) continue;
                        g.drawVerticalLine (x, beatY0, beatY1);
                    }
                    if (bar - firstBar > 100000) break;   // safety
                }
                g.setColour (juce::Colour (0xff707074));
            }

            for (int bar = firstBar; ; bar += barStep)
            {
                const int x = barX (bar);
                if (x > col.getRight()) break;
                if (x >= col.getX())
                {
                    g.drawVerticalLine (x, (float) ruler.getY() + 6.0f,
                                          (float) ruler.getY() + (float) kRulerTickBandH);
                    g.drawText (juce::String (bar + 1),
                                 x + 3, ruler.getY(), 60, kRulerTickBandH - 1,
                                 juce::Justification::centredLeft, false);
                }
                if (bar - firstBar > 200000) break;       // safety
            }
        }
        else
        {
            // Time grid - same cadence the bar used to use, kept as the
            // exact previous behaviour.
            double tickEverySec = 1.0;
            if (px < 6.0)       tickEverySec = 30.0;
            else if (px < 16.0) tickEverySec = 10.0;
            else if (px < 40.0) tickEverySec = 5.0;

            const double endSec = (double) col.getWidth() / px;
            for (double sec = 0.0; sec <= endSec; sec += tickEverySec)
            {
                const int x = col.getX() + (int) (sec * px);
                g.drawVerticalLine (x, (float) ruler.getY() + 6.0f,
                                      (float) ruler.getY() + (float) kRulerTickBandH);
                const int mins = (int) (sec / 60.0);
                const int secs = (int) sec % 60;
                const auto timeLabel = juce::String::formatted ("%d:%02d", mins, secs);
                g.drawText (timeLabel, x + 3, ruler.getY(), 60, kRulerTickBandH - 1,
                             juce::Justification::centredLeft, false);
            }
        }
    }

    // Clip the row/region drawing below the ruler so vertically-scrolled
    // rows (rowScrollY > 0) can't overpaint the time ruler. Scoped to the
    // track-content block ONLY — the separator, loop/punch pills + marker
    // flags below it live in the ruler band and must stay unclipped.
    {
    juce::Graphics::ScopedSaveState rowClip (g);
    g.reduceClipRegion (0, kRulerH, getWidth(),
                         juce::jmax (0, getHeight() - kRulerH));

    // ── Track rows ──
    for (int t = 0; t < Session::kNumTracks; ++t)
    {
        auto row = rowBounds (t);

        // Row background - slightly darker every other row. Shade by the
        // VISUAL row, not the track index: when tracks are hidden the
        // visible rows pack together, and t % 2 would give two adjacent
        // visible rows the same shade. visualRowForTrack returns -1 for
        // hidden tracks (row is then empty, so fillRect is a no-op).
        const int visualRow = visualRowForTrack (t);
        g.setColour (visualRow % 2 == 0 ? juce::Colour (0xff141418) : juce::Colour (0xff101014));
        g.fillRect (row);

        // Track label on the left (color stripe + name from session, falling
        // back to the 1-based track number if no name has been set).
        auto labelRow = juce::Rectangle<int> (label.getX(), row.getY(),
                                                label.getWidth(), row.getHeight());
        g.setColour (session.track (t).colour.withAlpha (0.85f));
        g.fillRect (labelRow.removeFromLeft (3));
        g.setColour (juce::Colour (0xffd0d0d0));
        g.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
        const auto& trackName = session.track (t).name;
        const juce::String displayLabel = trackName.isNotEmpty() ? trackName
                                                                  : juce::String (t + 1);
        g.drawText (displayLabel, labelRow.withTrimmedLeft (4),
                     juce::Justification::centredLeft, false);

        // Recorded regions for this track.
        const auto& regions = session.track (t).regions;
        for (int ri = 0; ri < (int) regions.size(); ++ri)
        {
            const auto& region = regions[(size_t) ri];
            const int x0 = xForSample (region.timelineStart);
            const int x1 = xForSample (region.timelineStart + region.lengthInSamples);
            if (x1 <= col.getX() || x0 >= col.getRight()) continue;

            auto regionRect = juce::Rectangle<int> (x0, row.getY() + 1,
                                                     juce::jmax (2, x1 - x0),
                                                     row.getHeight() - 2)
                                  .getIntersection (col.withTrimmedTop (1).withTrimmedBottom (1));
            if (regionRect.isEmpty()) continue;

            const bool isSelected = isRegionSelected (t, ri);

            // Region colour - per-region override when set, otherwise
            // the parent track's colour. customColour defaults to
            // transparent so the test below cleanly distinguishes
            // "user picked a colour" from "leave it on track default".
            const auto regionAccent = region.customColour.isTransparent()
                ? session.track (t).colour
                : region.customColour;

            // Block fill - region colour, slightly darker so the row label
            // still pops. Selected regions get a brighter mix so they read
            // as the focused thing. Muted regions get desaturated + half
            // alpha so they read as "skipped" without disappearing.
            auto fillColour = regionAccent.withMultipliedSaturation (0.85f)
                                            .withMultipliedBrightness (0.65f);
            if (isSelected) fillColour = fillColour.brighter (0.30f);
            if (region.muted)
                fillColour = fillColour.withMultipliedSaturation (0.20f)
                                       .withMultipliedAlpha (0.45f);
            g.setColour (fillColour);
            g.fillRoundedRectangle (regionRect.toFloat(), 2.0f);

            // Outline - thicker + white-ish when selected.
            if (isSelected)
            {
                g.setColour (juce::Colours::white.withAlpha (0.85f));
                g.drawRoundedRectangle (regionRect.toFloat().reduced (0.5f), 2.0f, 1.6f);
            }
            else
            {
                g.setColour (regionAccent.withAlpha (0.9f));
                g.drawRoundedRectangle (regionRect.toFloat().reduced (0.5f), 2.0f, 0.8f);
            }

            // Tiny waveform-stub stripe along the centre - not real audio, just
            // a visual cue that the block holds something.
            g.setColour (session.track (t).colour.brighter (0.4f).withAlpha (0.6f));
            const float midY = (float) regionRect.getCentreY();
            g.drawHorizontalLine ((int) midY, (float) regionRect.getX() + 2.0f,
                                   (float) regionRect.getRight() - 2.0f);

            // User-supplied region label, drawn over the body's
            // top-left. Only when set + the region is wide enough
            // for a few characters; auto-truncated by the renderer
            // otherwise. Take-history badge sits in the same corner;
            // the label paints on top so the user can see what they
            // typed. Skip when the row is too short to fit text.
            if (region.label.isNotEmpty()
                && regionRect.getHeight() >= 10
                && regionRect.getWidth()  >= 24)
            {
                const int padX = 4;
                // Badge is 36 px wide (see take-badge paint below) plus
                // 2 px breathing room before the label starts.
                const int badgeOffset = ! region.previousTakes.empty() ? 38 : 0;
                auto labelArea = regionRect.withTrimmedLeft (padX + badgeOffset)
                                            .withTrimmedRight (padX);
                if (labelArea.getWidth() > 0)
                {
                    g.setColour (juce::Colours::black.withAlpha (0.55f));
                    g.setFont (juce::Font (juce::FontOptions (9.0f, juce::Font::bold)));
                    // Single-pixel shadow at +1,+1 makes the label
                    // legible regardless of region accent brightness.
                    g.drawText (region.label,
                                 labelArea.translated (1, 1),
                                 juce::Justification::topLeft, true);
                    g.setColour (juce::Colours::white.withAlpha (0.92f));
                    g.drawText (region.label, labelArea,
                                 juce::Justification::topLeft, true);
                }
            }

            // Lock indicator: a small amber filled circle at the
            // bottom-right of the region body, far from the top-
            // right gain badge and top-left label / take badge.
            // The shape doesn't have to be a literal padlock - at
            // 14 px row height nothing more elaborate would read
            // anyway. The accent colour is the same amber used on
            // the gain-up badge so the user sees "this is a state
            // indicator", not a fade artefact.
            if (region.locked
                && regionRect.getHeight() >= 8
                && regionRect.getWidth()  >= 10)
            {
                const float cx = (float) regionRect.getRight() - 5.0f;
                const float cy = (float) regionRect.getBottom() - 5.0f;
                g.setColour (juce::Colours::black.withAlpha (0.6f));
                g.fillEllipse (cx - 3.0f, cy - 3.0f, 6.0f, 6.0f);
                g.setColour (juce::Colour (0xffe0c060));
                g.fillEllipse (cx - 2.0f, cy - 2.0f, 4.0f, 4.0f);
            }

            // Take badge: "T 1/3" at the top-left of the region body
            // when previousTakes is non-empty. Tells the user at a
            // glance that the region has alternates, and which slot is
            // currently live. Hidden on very narrow / short regions so
            // the badge doesn't visually crowd a thin slice.
            if (! region.previousTakes.empty()
                && regionRect.getWidth()  >= 40
                && regionRect.getHeight() >= 12)
            {
                const int total = (int) region.previousTakes.size() + 1;
                const auto badgeLabel = juce::String::formatted ("T 1/%d", total);
                const int badgeW = 36;
                const int badgeH = 10;
                juce::Rectangle<int> badge (regionRect.getX() + 2,
                                              regionRect.getY() + 1,
                                              badgeW, badgeH);
                g.setColour (juce::Colours::black.withAlpha (0.55f));
                g.fillRoundedRectangle (badge.toFloat(), 1.5f);
                g.setColour (juce::Colour (0xffa0c0e0));
                g.setFont (juce::Font (juce::FontOptions (8.0f, juce::Font::bold)));
                g.drawText (badgeLabel, badge, juce::Justification::centred, false);
            }

            // Region-gain badge: small "+3.0 dB" / "-6.0 dB" readout
            // at the top-right of the region body. Only painted when
            // gain is meaningfully off unity so an unedited timeline
            // reads clean. Alt-drag adjusts.
            if (std::abs (region.gainDb) >= 0.05f)
            {
                const auto label = juce::String::formatted (
                    "%+.1f dB", region.gainDb);
                const int badgeW = juce::jmin (regionRect.getWidth(), 56);
                const int badgeH = juce::jmin (regionRect.getHeight() - 2, 10);
                if (badgeW >= 28 && badgeH >= 6)
                {
                    juce::Rectangle<int> badge (regionRect.getRight() - badgeW,
                                                  regionRect.getY() + 1,
                                                  badgeW, badgeH);
                    g.setColour (juce::Colours::black.withAlpha (0.55f));
                    g.fillRoundedRectangle (badge.toFloat(), 1.5f);
                    g.setColour (region.gainDb > 0 ? juce::Colour (0xffffd060)
                                                     : juce::Colour (0xff70c0ff));
                    g.setFont (juce::Font (juce::FontOptions (8.0f, juce::Font::bold)));
                    g.drawText (label, badge, juce::Justification::centred, false);
                }
            }

            // Fade-in / fade-out visualisation. Slope line is always
            // drawn so non-zero fades read at a glance; the grab
            // handles only paint for the hovered or selected region
            // so the timeline stays uncluttered when you're not
            // actively editing fades.
            if (region.lengthInSamples > 0)
            {
                const auto fadeInSamples  = juce::jmax ((juce::int64) 0, region.fadeInSamples);
                const auto fadeOutSamples = juce::jmax ((juce::int64) 0, region.fadeOutSamples);
                const double pxPerSample = (double) regionRect.getWidth()
                    / (double) region.lengthInSamples;
                const auto fadeCol = juce::Colours::yellow.withAlpha (0.85f);
                const float topY    = (float) regionRect.getY();
                const float bottomY = (float) regionRect.getBottom();

                if (fadeInSamples > 0)
                {
                    const float xEnd = (float) regionRect.getX()
                        + (float) std::round ((double) fadeInSamples * pxPerSample);
                    g.setColour (fadeCol);
                    g.drawLine ((float) regionRect.getX(), bottomY, xEnd, topY, 1.2f);
                }
                if (fadeOutSamples > 0)
                {
                    const float xStart = (float) regionRect.getRight()
                        - (float) std::round ((double) fadeOutSamples * pxPerSample);
                    g.setColour (fadeCol);
                    g.drawLine (xStart, topY,
                                 (float) regionRect.getRight(), bottomY, 1.2f);
                }

                const bool isHovered = (t == hoveredTrack && ri == hoveredRegion);
                if (isHovered || isSelected)
                {
                    // 6 px square grab handles at each fade end-point,
                    // outlined in black so they pop against any track
                    // colour. The hit zone (kFadeHitPx = 5) stays the
                    // same size; this just makes the visible target
                    // bigger when the user is interacting with this
                    // region.
                    const float fadeInEndX  = (float) regionRect.getX()
                        + (float) std::round ((double) fadeInSamples * pxPerSample);
                    const float fadeOutBegX = (float) regionRect.getRight()
                        - (float) std::round ((double) fadeOutSamples * pxPerSample);
                    auto drawHandle = [&] (float cx)
                    {
                        const auto outer = juce::Rectangle<float> (cx - 4.0f, topY - 1.0f, 8.0f, 8.0f);
                        const auto inner = juce::Rectangle<float> (cx - 3.0f, topY,         6.0f, 6.0f);
                        g.setColour (juce::Colours::black.withAlpha (0.85f));
                        g.fillRect (outer);
                        g.setColour (juce::Colours::yellow);
                        g.fillRect (inner);
                    };
                    drawHandle (fadeInEndX);
                    drawHandle (fadeOutBegX);
                }
            }

            // Take-history badge. Shows total take count (current + prior)
            // anchored to the region's top-left when there's at least one
            // prior take. Clicking it rotates to the next prior take; the
            // rotation is hit-tested via hitTestRegion's TakeBadge op.
            if (! region.previousTakes.empty())
            {
                const int badgeW = juce::jmin (regionRect.getWidth(), 16);
                const int badgeH = juce::jmin (regionRect.getHeight(), 10);
                if (badgeW >= 8 && badgeH >= 6)
                {
                    juce::Rectangle<int> badge (regionRect.getX(), regionRect.getY(),
                                                  badgeW, badgeH);
                    g.setColour (juce::Colours::black.withAlpha (0.6f));
                    g.fillRoundedRectangle (badge.toFloat(), 1.5f);
                    g.setColour (juce::Colours::white);
                    g.setFont (juce::Font (juce::FontOptions (8.0f, juce::Font::bold)));
                    g.drawText (juce::String ((int) region.previousTakes.size() + 1),
                                 badge, juce::Justification::centred, false);
                }
            }
        }

        // Crossfade overlay pass. Region bodies are opaque, so an
        // earlier region's fade-out is occluded by the later region that
        // overlaps it - the "X" never shows. Here, after all bodies on
        // the track are painted, walk time-sorted adjacent pairs and, for
        // each overlap, shade the zone + draw the outgoing region's
        // fade-out crossing the incoming region's fade-in.
        {
            std::vector<int> order;
            order.reserve (regions.size());
            for (int i = 0; i < (int) regions.size(); ++i) order.push_back (i);
            std::sort (order.begin(), order.end(), [&] (int a, int b)
            {
                return regions[(size_t) a].timelineStart
                     < regions[(size_t) b].timelineStart;
            });

            const auto xfFill  = juce::Colour (0x22ffffff);
            const auto xfCurve = juce::Colours::yellow.withAlpha (0.9f);

            for (size_t i = 0; i + 1 < order.size(); ++i)
            {
                const auto& a = regions[(size_t) order[i]];       // outgoing
                const auto& b = regions[(size_t) order[i + 1]];   // incoming
                const auto aEnd = a.timelineStart + a.lengthInSamples;
                if (aEnd <= b.timelineStart) continue;            // no overlap

                const auto ovStart = b.timelineStart;
                const auto ovEnd   = juce::jmin (aEnd, b.timelineStart + b.lengthInSamples);
                if (ovEnd <= ovStart) continue;

                const int zx0 = xForSample (ovStart);
                const int zx1 = xForSample (ovEnd);
                juce::Rectangle<int> zoneI (zx0, row.getY() + 1,
                                             juce::jmax (1, zx1 - zx0), row.getHeight() - 2);
                zoneI = zoneI.getIntersection (col.withTrimmedTop (1).withTrimmedBottom (1));
                if (zoneI.isEmpty()) continue;

                fadeviz::drawCrossfade (g, zoneI.toFloat(),
                                         a.fadeOutShape, b.fadeInShape,
                                         xfCurve, xfFill, 1.2f);
            }
        }

        // MIDI regions on this track. Same anchor + bounds math as audio
        // regions; the inside paints a stylised note-pile (small dots at
        // each note's start position, vertically distributed by pitch)
        // so the user can read note density at a glance from the timeline.
        const auto& midiRegions = session.track (t).midiRegions.current();
        for (int mri = 0; mri < (int) midiRegions.size(); ++mri)
        {
            const auto& region = midiRegions[(size_t) mri];
            const int x0 = xForSample (region.timelineStart);
            const int x1 = xForSample (region.timelineStart + region.lengthInSamples);
            if (x1 <= col.getX() || x0 >= col.getRight()) continue;

            auto regionRect = juce::Rectangle<int> (x0, row.getY() + 1,
                                                     juce::jmax (2, x1 - x0),
                                                     row.getHeight() - 2)
                                  .getIntersection (col.withTrimmedTop (1).withTrimmedBottom (1));
            if (regionRect.isEmpty()) continue;

            const bool isMidiSelected = (t == selectedMidiTrack
                                          && mri == selectedMidiRegion);

            // Block fill - desaturated region accent (custom colour
            // when set, otherwise track) with a subtle inset so it
            // reads "MIDI" vs the brighter audio block colour. Selected
            // regions brighten the fill + paint a white outline,
            // mirroring the audio selection look. Muted regions get
            // further desaturated + half alpha to read as "skipped".
            const auto base = region.customColour.isTransparent()
                ? session.track (t).colour
                : region.customColour;
            auto midiFill = base.withMultipliedSaturation (0.5f).withMultipliedBrightness (0.55f);
            if (isMidiSelected) midiFill = midiFill.brighter (0.30f);
            if (region.muted)
                midiFill = midiFill.withMultipliedSaturation (0.20f).withMultipliedAlpha (0.45f);
            g.setColour (midiFill);
            g.fillRoundedRectangle (regionRect.toFloat(), 2.0f);
            if (isMidiSelected)
            {
                g.setColour (juce::Colours::white.withAlpha (0.85f));
                g.drawRoundedRectangle (regionRect.toFloat().reduced (0.5f), 2.0f, 1.6f);
            }
            else
            {
                g.setColour (base.withAlpha (region.muted ? 0.45f : 0.85f));
                g.drawRoundedRectangle (regionRect.toFloat().reduced (0.5f), 2.0f, 0.8f);
            }

            // User-supplied label, top-left of the region body. Same
            // shadow + white-text combo the audio painter uses. Shift
            // right by the take-badge width when previousTakes is non-
            // empty so the label and the "T 1/N" pill don't overlap;
            // matches the audio painter's badgeOffset.
            if (region.label.isNotEmpty()
                && regionRect.getHeight() >= 10
                && regionRect.getWidth()  >= 24)
            {
                // 36 px badge + 2 px breathing room.
                const int badgeOffset = ! region.previousTakes.empty() ? 38 : 0;
                auto labelArea = regionRect.withTrimmedLeft (4 + badgeOffset)
                                            .withTrimmedRight (4);
                if (labelArea.getWidth() > 0)
                {
                    g.setColour (juce::Colours::black.withAlpha (0.55f));
                    g.setFont (juce::Font (juce::FontOptions (9.0f, juce::Font::bold)));
                    g.drawText (region.label, labelArea.translated (1, 1),
                                 juce::Justification::topLeft, true);
                    g.setColour (juce::Colours::white.withAlpha (0.92f));
                    g.drawText (region.label, labelArea,
                                 juce::Justification::topLeft, true);
                }
            }

            // Lock indicator (same idiom as the audio painter).
            if (region.locked
                && regionRect.getHeight() >= 8
                && regionRect.getWidth()  >= 10)
            {
                const float cx = (float) regionRect.getRight() - 5.0f;
                const float cy = (float) regionRect.getBottom() - 5.0f;
                g.setColour (juce::Colours::black.withAlpha (0.6f));
                g.fillEllipse (cx - 3.0f, cy - 3.0f, 6.0f, 6.0f);
                g.setColour (juce::Colour (0xffe0c060));
                g.fillEllipse (cx - 2.0f, cy - 2.0f, 4.0f, 4.0f);
            }

            // Take badge - matches the audio painter so MIDI take cycles
            // surface the same affordance.
            if (! region.previousTakes.empty()
                && regionRect.getWidth()  >= 40
                && regionRect.getHeight() >= 12)
            {
                const int total = (int) region.previousTakes.size() + 1;
                const auto badgeLabel = juce::String::formatted ("T 1/%d", total);
                const int badgeW = 36;
                const int badgeH = 10;
                juce::Rectangle<int> badge (regionRect.getX() + 2,
                                              regionRect.getY() + 1,
                                              badgeW, badgeH);
                g.setColour (juce::Colours::black.withAlpha (0.55f));
                g.fillRoundedRectangle (badge.toFloat(), 1.5f);
                g.setColour (juce::Colour (0xffa0c0e0));
                g.setFont (juce::Font (juce::FontOptions (8.0f, juce::Font::bold)));
                g.drawText (badgeLabel, badge, juce::Justification::centred, false);
            }

            // Note dots. Walk the region's notes; each note's start tick
            // converted back to a fraction of region length gives an X
            // position; pitch (0..127) gives a Y position inside the
            // region rect. Caps the per-region dot count to keep paint
            // cheap on dense regions.
            if (region.lengthInTicks > 0 && ! region.notes.empty())
            {
                const float rx = (float) regionRect.getX();
                const float ry = (float) regionRect.getY();
                const float rw = (float) regionRect.getWidth();
                const float rh = (float) regionRect.getHeight();
                const float dotSize = juce::jmax (1.0f, rh * 0.07f);
                g.setColour (base.brighter (0.4f).withAlpha (0.85f));
                const int maxDots = juce::jmin ((int) region.notes.size(), 256);
                for (int i = 0; i < maxDots; ++i)
                {
                    const auto& n = region.notes[(size_t) i];
                    const float fx = (float) n.startTick / (float) region.lengthInTicks;
                    const float fy = 1.0f - juce::jlimit (0.0f, 1.0f, (float) n.noteNumber / 127.0f);
                    g.fillRect (rx + fx * rw - dotSize * 0.5f,
                                 ry + fy * rh - dotSize * 0.5f,
                                 dotSize, dotSize);
                }
            }
        }

        // ── Automation ribbon (overlay) ──────────────────────────────
        // Plots the FaderDb lane's normalised points across the row as a
        // thin polyline. Drawn AFTER region blocks so it reads on top of
        // them. Empty lane = nothing rendered (zero-cost when the user
        // hasn't recorded automation). Future: per-track param picker so
        // pan / sends / mute / solo can swap in here.
        const auto& fadeLane = session.track (t)
                                   .automationLanes[(size_t) AutomationParam::FaderDb];
        if (! fadeLane.pointsConst().empty())
        {
            const auto rowF = row.toFloat();
            const auto leftX  = (float) col.getX();
            const auto rightX = (float) col.getRight();
            const auto pointPx = [&] (const AutomationPoint& p) -> juce::Point<float>
            {
                const float x = (float) xForSample (p.timeSamples);
                // Normalised value 0..1 → row top..bottom (inverted: 1.0
                // sits at the top so a higher fader reads as "up").
                const float y = rowF.getBottom()
                              - juce::jlimit (0.0f, 1.0f, p.value) * rowF.getHeight();
                return { x, y };
            };

            juce::Path curve;
            const auto first = pointPx (fadeLane.pointsConst().front());
            // Hold from row-left edge to the first point so the curve
            // doesn't appear to fade in from nowhere.
            curve.startNewSubPath (leftX, first.y);
            curve.lineTo (first.x, first.y);
            for (size_t i = 1; i < fadeLane.pointsConst().size(); ++i)
                curve.lineTo (pointPx (fadeLane.pointsConst()[i]));
            // Hold the final value out to the row's right edge.
            const auto last = pointPx (fadeLane.pointsConst().back());
            curve.lineTo (rightX, last.y);

            g.setColour (juce::Colour (0xff7fdfff).withAlpha (0.85f));  // soft cyan
            g.strokePath (curve, juce::PathStrokeType (1.2f));
        }

        // ── Live-recording overlay ─────────────────────────────────
        // Translucent red block from record-start to current playhead
        // on tracks that are armed while the transport is recording.
        // Until full waveform / note rendering during capture is in
        // place this gives the user a visible "yes, recording is
        // happening, here's how much you've captured" indicator
        // without waiting for stopRecording to publish a region.
        if (engine.getTransport().isRecording()
            && session.track (t).recordArmed.load (std::memory_order_relaxed))
        {
            const auto recStart = session.lastRecordPointSamples.load (
                                      std::memory_order_relaxed);
            const auto playhead = engine.getTransport().getPlayhead();
            if (playhead > recStart)
            {
                const int x0 = xForSample (recStart);
                const int x1 = xForSample (playhead);
                if (x1 > col.getX() && x0 < col.getRight())
                {
                    auto liveRect = juce::Rectangle<int> (x0, row.getY() + 1,
                                                            juce::jmax (2, x1 - x0),
                                                            row.getHeight() - 2)
                                      .getIntersection (col.withTrimmedTop (1)
                                                            .withTrimmedBottom (1));
                    if (! liveRect.isEmpty())
                    {
                        // Recording-red wash so it reads "active capture"
                        // even when the row is otherwise empty.
                        g.setColour (juce::Colour (0xffd03030).withAlpha (0.30f));
                        g.fillRoundedRectangle (liveRect.toFloat(), 2.0f);
                        g.setColour (juce::Colour (0xffd03030).withAlpha (0.95f));
                        g.drawRoundedRectangle (liveRect.toFloat().reduced (0.5f),
                                                  2.0f, 1.0f);
                        // "REC" pill at the left edge of the active block,
                        // big enough to read but small enough not to crowd
                        // tiny takes near the start of a session.
                        if (liveRect.getWidth() >= 26)
                        {
                            const auto pill = liveRect.withWidth (24)
                                                  .withTrimmedTop (2)
                                                  .withTrimmedBottom (2)
                                                  .toFloat();
                            g.setColour (juce::Colour (0xffd03030));
                            g.fillRoundedRectangle (pill, 2.0f);
                            g.setColour (juce::Colours::white);
                            g.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
                            g.drawText ("REC", pill.toNearestInt(),
                                          juce::Justification::centred, false);
                        }
                    }
                }
            }
        }
    }

    }   // end row-clip scope — ruler-band chrome below stays unclipped

    // ── Vertical separator between labels and tracks ──
    g.setColour (juce::Colour (0xff2a2a32));
    g.drawVerticalLine (col.getX() - 1, 0.0f, (float) getHeight());

    // ── Loop / punch brackets ──
    // Translucent fill across the track area + a solid bar at the top so
    // the region is unambiguous even when the underlying tracks are dense.
    // Optional `pillLabel` draws a small filled label at both endpoints,
    // matching the in/out marker style of pro DAWs.
    auto drawRange = [&] (juce::int64 start, juce::int64 end,
                           juce::Colour colour, bool enabled,
                           const juce::String& pillLabel)
    {
        if (end < start) return;
        const int x0Raw = xForSample (start);
        const int x1Raw = xForSample (end);

        // Zero-width = a single in/out point set, partner not yet placed (e.g.
        // right after Shift+[ before Shift+] lands). Draw a single bracket
        // marker so the user gets immediate feedback. Guard start>0 so an unset
        // (0,0) range stays invisible.
        if (end == start)
        {
            if (start > 0 && x0Raw >= col.getX() && x0Raw <= col.getRight())
            {
                constexpr int kBarH = 4;
                g.setColour (colour.withAlpha (enabled ? 1.0f : 0.7f));
                g.fillRect (x0Raw, kRulerH, 1, getHeight() - kRulerH);        // thin stem
                g.fillRect (x0Raw - 1, ruler.getBottom() - kBarH, 3, kBarH);  // bracket foot
                if (pillLabel.isNotEmpty())
                {
                    g.setFont (juce::Font (juce::FontOptions (10.5f, juce::Font::bold)));
                    const int textW = juce::jmax (40,
                        g.getCurrentFont().getStringWidth (pillLabel) + 10);
                    const int pillH = kRulerPillBandH - 2;
                    const int pillY = ruler.getY() + kRulerTickBandH;
                    const int pillX = juce::jlimit (col.getX(), col.getRight() - textW,
                                                    x0Raw - textW / 2);
                    juce::Rectangle<int> r (pillX, pillY, textW, pillH);
                    g.setColour (colour.withAlpha (enabled ? 1.0f : 0.7f));
                    g.fillRoundedRectangle (r.toFloat(), 3.0f);
                    g.setColour (juce::Colours::white);
                    g.drawText (pillLabel, r, juce::Justification::centred, false);
                }
            }
            return;
        }

        const int x0 = juce::jmax (col.getX(),     x0Raw);
        const int x1 = juce::jmin (col.getRight(), x1Raw);
        if (x1 <= x0) return;

        // Translucent fill across the track area so the range reads as
        // "this stretch is the loop/punch zone" without competing with
        // recorded regions. Brighter when the toggle's on.
        g.setColour (colour.withAlpha (enabled ? 0.18f : 0.08f));
        g.fillRect (x0, kRulerH, x1 - x0, getHeight() - kRulerH);

        // Solid bracket bar across the bottom of the ruler. Full opacity
        // when enabled; half-opacity when the bounds are set but the
        // toggle is off, so the user can still see where the range will
        // jump to when they re-enable.
        constexpr int kBarH = 4;
        g.setColour (colour.withAlpha (enabled ? 1.0f : 0.55f));
        g.fillRect (x0, ruler.getBottom() - kBarH, x1 - x0, kBarH);

        // Endpoint pills - sit in the pill band of the ruler, above the
        // bracket bar, with rounded "tail" pointing down into the bar so
        // the pill+bar reads as a single bracket shape.
        if (pillLabel.isNotEmpty())
        {
            g.setFont (juce::Font (juce::FontOptions (10.5f, juce::Font::bold)));
            const int textW = juce::jmax (40,
                g.getCurrentFont().getStringWidth (pillLabel) + 10);
            const int pillH = kRulerPillBandH - 2;     // small gap above the bar
            const int pillY = ruler.getY() + kRulerTickBandH;

            auto drawPill = [&] (int xCentre)
            {
                int pillX = xCentre - textW / 2;
                pillX = juce::jlimit (col.getX(), col.getRight() - textW, pillX);
                juce::Rectangle<int> r (pillX, pillY, textW, pillH);
                g.setColour (colour.withAlpha (enabled ? 1.0f : 0.7f));
                g.fillRoundedRectangle (r.toFloat(), 3.0f);
                g.setColour (juce::Colours::white);
                g.drawText (pillLabel, r, juce::Justification::centred, false);
            };

            if (x0Raw >= col.getX() && x0Raw <= col.getRight()) drawPill (x0Raw);
            if (x1Raw >= col.getX() && x1Raw <= col.getRight()
                && std::abs (x1Raw - x0Raw) > textW + 8)
            {
                drawPill (x1Raw);
            }
        }
    };

    // ── In-flight ruler selection ──
    // Painted under loop/punch so a freshly-finished drag's highlight doesn't
    // overpaint the result the user just chose. Neutral grey so it doesn't
    // look like a committed loop or punch range.
    if (rulerSelection.active)
    {
        const auto sa = juce::jmin (rulerSelection.originSample,
                                      rulerSelection.currentSample);
        const auto sb = juce::jmax (rulerSelection.originSample,
                                      rulerSelection.currentSample);
        const int x0 = juce::jmax (col.getX(),     xForSample (sa));
        const int x1 = juce::jmin (col.getRight(), xForSample (sb));
        if (x1 > x0)
        {
            g.setColour (juce::Colour (0xffd0d0d8).withAlpha (0.18f));
            g.fillRect (x0, kRulerH, x1 - x0, getHeight() - kRulerH);
            g.setColour (juce::Colour (0xffd0d0d8).withAlpha (0.85f));
            g.fillRect (x0, ruler.getBottom() - 4, x1 - x0, 4);
        }
    }

    auto& transport = engine.getTransport();
    // Loop in green (visually distinct from punch's red), punch in red -
    // matches the colour language the user expects from pro DAWs.
    drawRange (transport.getLoopStart(),  transport.getLoopEnd(),
                juce::Colour (0xff3aa860), transport.isLoopEnabled(), "Loop");
    drawRange (transport.getPunchIn(),    transport.getPunchOut(),
                juce::Colour (0xffd05a5a), transport.isPunchEnabled(), "Punch");

    // ── Markers ──
    // Flag in the ruler's pill band + vertical guideline through tracks.
    // Drawn after loop/punch so a marker that lands exactly on a loop/punch
    // boundary is still readable on top.
    {
        g.setFont (juce::Font (juce::FontOptions (10.5f, juce::Font::bold)));
        for (const auto& marker : session.getMarkers())
        {
            const int x = xForSample (marker.timelineSamples);
            if (x < col.getX() - 80 || x > col.getRight()) continue;

            // Guideline through the track area.
            g.setColour (marker.colour.withAlpha (0.35f));
            g.drawVerticalLine (x, (float) col.getY(), (float) getHeight());

            // Flag positioned in the pill band of the ruler so it sits at
            // the same y as loop/punch pills - one consistent "ruler
            // overlay" zone.
            const int textW  = g.getCurrentFont().getStringWidth (marker.name) + 10;
            const int flagW  = juce::jlimit (32, 160, textW);
            const int flagH  = kRulerPillBandH - 2;
            const int flagX  = juce::jmin (x, getWidth() - flagW - 2);
            const int flagY  = ruler.getY() + kRulerTickBandH;
            juce::Rectangle<int> flag (flagX, flagY, flagW, flagH);

            g.setColour (marker.colour);
            g.fillRoundedRectangle (flag.toFloat(), 2.0f);
            g.setColour (juce::Colour (0xff181820));
            g.drawText (marker.name, flag.reduced (4, 0),
                         juce::Justification::centredLeft, true);
        }
    }

    // ── Playhead line ──
    const auto playhead = engine.getTransport().getPlayhead();
    const int phX = xForSample (playhead);
    if (phX >= col.getX() && phX <= col.getRight())
    {
        g.setColour (juce::Colour (0xffe04040));
        g.drawVerticalLine (phX, 0.0f, (float) getHeight());
    }

    // ── Fade alignment guide (Reaper-style) ──
    // Cyan vertical at the fade boundary while the user drags a fade
    // handle; gives the user a precise visual reference against the
    // waveform/grid behind the fade. Cleared on mouseUp.
    if (fadeGuideX >= col.getX() && fadeGuideX <= col.getRight())
    {
        g.setColour (juce::Colour (0xff5ad6e0).withAlpha (0.85f));
        g.drawVerticalLine (fadeGuideX, (float) kRulerH, (float) getHeight());
    }

    // ── Rubber-band overlay (Shift/Cmd + drag box-select) ──
    // Drawn last so it sits on top of every region / playhead / marker.
    if (rubberBandActive && ! rubberBand.isEmpty())
    {
        const auto highlight = juce::Colour (0xff70b0e0);   // Dusk Studio accent blue
        g.setColour (highlight.withAlpha (0.15f));
        g.fillRect (rubberBand);
        g.setColour (highlight.withAlpha (0.85f));
        g.drawRect (rubberBand, 1);
    }

    // File-drop visual feedback. Painted last so it overlays everything.
    if (dropAccepted)
    {
        const auto accent = juce::Colour (0xff70b0e0);
        if (dropHoverTrack >= 0)
        {
            const auto row = rowBounds (dropHoverTrack);
            g.setColour (accent.withAlpha (0.18f));
            g.fillRect (row);
        }
        if (dropHoverX >= 0)
        {
            g.setColour (accent.withAlpha (0.85f));
            g.drawVerticalLine (dropHoverX, (float) kRulerH, (float) getHeight());
        }
    }

    // Tempo points sit on top of the ruler + markers.
    paintTempoPoints (g);
}

TapeStrip::BracketHit TapeStrip::hitTestBracket (int x, int y) const noexcept
{
    auto ruler = rulerBounds();
    if (! ruler.contains (x, y)) return BracketHit::None;

    // Pill band sits below the tick band; bar sits in the bottom 4px of
    // the ruler. Outside both → no hit.
    const int pillTop  = ruler.getY() + kRulerTickBandH;
    const int barTop   = ruler.getBottom() - 4;
    const bool inPills = (y >= pillTop && y < barTop);
    const bool inBar   = (y >= barTop  && y < ruler.getBottom());
    if (! inPills && ! inBar) return BracketHit::None;

    auto& transport = engine.getTransport();

    auto pillBounds = [this] (juce::int64 sample, const juce::String& label)
    {
        // Same width math the painter uses; exposes a forgiving 6px hit
        // gutter on each side of the pill so users don't have to land
        // pixel-perfect on the label.
        auto col = tracksColumnBounds();
        const int xMid = xForSample (sample);
        const int textW = juce::jmax (40, label.length() * 7 + 14);
        int pillX = juce::jlimit (col.getX(), col.getRight() - textW,
                                    xMid - textW / 2);
        return juce::Rectangle<int> (pillX - 6, 0, textW + 12, 0);
    };

    auto inXRange = [] (int xx, juce::Rectangle<int> r)
    { return xx >= r.getX() && xx <= r.getRight(); };

    // Loop pills + bar (checked first so "loop" wins on overlap with
    // punch when both happen to share an endpoint).
    if (transport.getLoopEnd() > transport.getLoopStart())
    {
        if (inPills)
        {
            if (inXRange (x, pillBounds (transport.getLoopStart(), "Loop")))
                return BracketHit::LoopIn;
            if (inXRange (x, pillBounds (transport.getLoopEnd(),   "Loop")))
                return BracketHit::LoopOut;
        }
        if (inBar)
        {
            const int x0 = xForSample (transport.getLoopStart());
            const int x1 = xForSample (transport.getLoopEnd());
            if (x >= x0 && x <= x1) return BracketHit::LoopBar;
        }
    }

    if (transport.getPunchOut() > transport.getPunchIn())
    {
        if (inPills)
        {
            if (inXRange (x, pillBounds (transport.getPunchIn(),  "Punch")))
                return BracketHit::PunchIn;
            if (inXRange (x, pillBounds (transport.getPunchOut(), "Punch")))
                return BracketHit::PunchOut;
        }
        if (inBar)
        {
            const int x0 = xForSample (transport.getPunchIn());
            const int x1 = xForSample (transport.getPunchOut());
            if (x >= x0 && x <= x1) return BracketHit::PunchBar;
        }
    }
    return BracketHit::None;
}

bool TapeStrip::overMidiRegionBody (int x, int y) const noexcept
{
    if (! tracksColumnBounds().contains (x, y)) return false;
    for (int t = 0; t < Session::kNumTracks; ++t)
    {
        const auto row = rowBounds (t);
        if (! row.contains (x, y)) continue;
        const auto& mr = session.track (t).midiRegions.current();
        for (int i = (int) mr.size() - 1; i >= 0; --i)
        {
            const auto& r  = mr[(size_t) i];
            const int   x0 = xForSample (r.timelineStart);
            const int   x1 = xForSample (r.timelineStart + r.lengthInSamples);
            if (x >= x0 && x <= x1) return true;
        }
        break;
    }
    return false;
}

int TapeStrip::hitTestMarker (int x, int y) const noexcept
{
    auto ruler = rulerBounds();
    if (! ruler.contains (x, y)) return -1;
    // Flags sit in the pill band of the ruler (below the tick band).
    if (y < ruler.getY() + kRulerTickBandH) return -1;

    const auto& markers = session.getMarkers();
    // Iterate back-to-front so a later (rightmost) marker wins on overlap,
    // matching the painter's left-to-right draw order.
    for (int i = (int) markers.size() - 1; i >= 0; --i)
    {
        const int mx = xForSample (markers[(size_t) i].timelineSamples);
        // Same width math the painter uses, just without a Graphics
        // context. Slightly looser bound (8 px/char) so hit-testing is
        // forgiving on the right edge of the flag.
        const int approxFlagW = juce::jlimit (28, 160,
            (int) markers[(size_t) i].name.length() * 8 + 12);
        const int flagX = juce::jmin (mx, getWidth() - approxFlagW - 2);
        if (x >= flagX && x <= flagX + approxFlagW) return i;
    }
    return -1;
}

int TapeStrip::hitTestTempoPoint (int x, int y) const noexcept
{
    auto ruler = rulerBounds();
    if (! ruler.contains (x, y)) return -1;
    // The glyph + label live in the top tick band, but accept a right-click
    // anywhere in the ruler column under them — the thin 14px band is too
    // easy to miss, and markers (lower band) use their own hit test, so a
    // generous vertical zone here doesn't steal their menu.

    // Hit zone spans the triangle (a few px left) plus the BPM label drawn to
    // its right, so right-clicking the number — not just the tiny glyph —
    // still lands on the marker.
    const auto onHandle = [this, x] (juce::int64 sample)
    {
        const int px = xForSample (sample);
        return x >= px - 6 && x <= px + 28;
    };
    const auto& pts = session.tempoMap.points();
    // No map yet: the only handle is the bar-1 base (the starting tempo).
    if (pts.empty())
        return onHandle (0) ? kTempoBaseHandle : -1;
    // Back-to-front so a later (rightmost) point wins on overlap.
    for (int i = (int) pts.size() - 1; i >= 0; --i)
        if (onHandle (pts[(size_t) i].timelineSamples))
            return i;
    return -1;
}

void TapeStrip::commitMidiRegionToggle (int trackIdx, int regionIdx,
                                         const juce::String& name,
                                         std::function<void (MidiRegion&)> mutate)
{
    const auto& live = session.track (trackIdx).midiRegions.current();
    if (regionIdx < 0 || regionIdx >= (int) live.size()) return;
    MidiRegion before = live[(size_t) regionIdx];
    MidiRegion after  = before;
    mutate (after);
    auto& um = engine.getUndoManager();
    um.beginNewTransaction (name);
    um.perform (new MidiRegionEditAction (session, engine, trackIdx, regionIdx, before, after));
    repaint();
}

void TapeStrip::commitTempoPoints (std::vector<duskstudio::TempoPoint> after,
                                    const juce::String& name)
{
    auto& um = engine.getUndoManager();
    um.beginNewTransaction (name);
    um.perform (new SetTempoMapAction (engine, session.tempoMap.points(), std::move (after)));
    repaint();
}

void TapeStrip::promptAddTempoPoint (juce::int64 sample)
{
    // Prompt for the BPM first and add the point only on Accept, so Cancel
    // leaves no stray marker on the grid.
    auto* host = getTopLevelComponent();
    if (host == nullptr) host = this;
    showEmbeddedTextInput (*host, "Tempo", "BPM (30-300):",
        juce::String ((int) std::round (session.bpmAt (sample))), "Add",
        [safeThis = juce::Component::SafePointer<TapeStrip> (this), sample] (juce::String s)
        {
            if (safeThis == nullptr) return;
            // Validate BEFORE clamping: getFloatValue() returns 0 for junk, and
            // jlimit would then silently turn that into a 30 BPM entry.
            const auto t = s.trim();
            float parsed = 0.0f;
            if (! parseFullFloat (t, parsed)) return;
            const float b = juce::jlimit (30.0f, 300.0f, parsed);
            auto vec = safeThis->session.tempoMap.points();
            // First point ever: seed a base anchor at the origin so the span
            // before this change keeps the starting tempo.
            if (vec.empty())
                vec.push_back ({ 0, safeThis->session.tempoBpm.load (std::memory_order_relaxed) });
            bool existed = false;
            for (auto& p : vec)
                if (p.timelineSamples == sample) { p.bpm = b; existed = true; break; }
            if (! existed) vec.push_back ({ sample, b });
            safeThis->commitTempoPoints (std::move (vec), "Add tempo");
        });
}

void TapeStrip::editTempoPointBpm (juce::int64 atSample)
{
    const auto& pts = session.tempoMap.points();
    float currentBpm = session.bpmAt (atSample);
    for (const auto& p : pts)
        if (p.timelineSamples == atSample) { currentBpm = p.bpm; break; }

    auto* host = getTopLevelComponent();
    if (host == nullptr) host = this;
    showEmbeddedTextInput (*host, "Tempo", "BPM (30-300):",
        juce::String ((int) std::round (currentBpm)), "Set",
        [safeThis = juce::Component::SafePointer<TapeStrip> (this), atSample]
        (juce::String s)
        {
            if (safeThis == nullptr) return;
            const auto t = s.trim();
            float parsed = 0.0f;
            if (! parseFullFloat (t, parsed)) return;
            const float b = juce::jlimit (30.0f, 300.0f, parsed);
            auto vec = safeThis->session.tempoMap.points();
            for (auto& p : vec)
                if (p.timelineSamples == atSample) p.bpm = b;
            safeThis->commitTempoPoints (std::move (vec), "Set tempo");
        });
}

void TapeStrip::editBaseTempo()
{
    // Starting tempo when no map exists yet. Seeds a single bar-0 point, which
    // reads as a constant tempo and keeps the edit on the (undoable) tempo-map
    // path like every other tempo edit.
    auto* host = getTopLevelComponent();
    if (host == nullptr) host = this;
    showEmbeddedTextInput (*host, "Tempo", "Starting BPM (30-300):",
        juce::String ((int) std::round (session.tempoBpm.load (std::memory_order_relaxed))), "Set",
        [safeThis = juce::Component::SafePointer<TapeStrip> (this)] (juce::String s)
        {
            if (safeThis == nullptr) return;
            const auto t = s.trim();
            float parsed = 0.0f;
            if (! parseFullFloat (t, parsed)) return;
            const float b = juce::jlimit (30.0f, 300.0f, parsed);
            safeThis->commitTempoPoints ({ { 0, b } }, "Set starting tempo");
        });
}

void TapeStrip::deleteTempoPoint (juce::int64 atSample)
{
    auto vec = session.tempoMap.points();
    // The base anchor at sample 0 is the song's starting tempo. Keep it while
    // any later tempo change still exists — deleting it would slide the next
    // point's tempo back to bar 1. Deleting it when it's the only point is fine:
    // the map clears back to a constant tempo.
    if (atSample == 0 && vec.size() > 1) return;
    vec.erase (std::remove_if (vec.begin(), vec.end(),
        [atSample] (const TempoPoint& p) { return p.timelineSamples == atSample; }),
        vec.end());
    commitTempoPoints (std::move (vec), "Delete tempo");
}

void TapeStrip::paintTempoPoints (juce::Graphics& g)
{
    auto ruler = rulerBounds();
    auto col   = tracksColumnBounds();
    const int yTop = ruler.getY();
    const auto amber = juce::Colour (0xffd0a050);
    g.setFont (juce::Font (juce::FontOptions (9.0f, juce::Font::bold)));

    const auto drawHandle = [&] (juce::int64 sample, float bpm, bool dim)
    {
        const int x = xForSample (sample);
        if (x < col.getX() - 8 || x > col.getRight()) return;
        juce::Path tri;
        tri.addTriangle ((float) x - 3.5f, (float) yTop,
                          (float) x + 3.5f, (float) yTop,
                          (float) x,        (float) yTop + 7.0f);
        g.setColour (dim ? amber.withAlpha (0.55f) : amber);
        g.fillPath (tri);

        const juce::String label ((int) std::round (bpm));
        const int labelW = g.getCurrentFont().getStringWidth (label) + 2;
        if (x + 5 + labelW <= col.getRight())
            g.drawText (label, x + 5, yTop, labelW, kRulerTickBandH - 1,
                         juce::Justification::centredLeft, false);
    };

    if (session.tempoMap.empty())
    {
        // No changes yet: show the bar-1 base handle (dimmed) so the starting
        // tempo stays editable here — the transport BPM field is read-only.
        drawHandle (0, session.tempoBpm.load (std::memory_order_relaxed), /*dim*/ true);
        return;
    }
    for (const auto& p : session.tempoMap.points())
        drawHandle (p.timelineSamples, p.bpm, /*dim*/ false);
}

bool TapeStrip::copySelectedRegion()
{
    if (selectedTrack < 0 || selectedRegion < 0) return false;
    const auto& regs = session.track (selectedTrack).regions;
    if (selectedRegion >= (int) regs.size()) return false;

    auto& clip = engine.getRegionClipboard();
    clip.region      = regs[(size_t) selectedRegion];
    clip.sourceTrack = selectedTrack;
    clip.hasContent  = true;
    return true;
}

bool TapeStrip::cutSelectedRegion()
{
    // Copy first (non-undoable side effect on the clipboard), then push a
    // DeleteRegionAction so Cmd+Z brings the region back. The clipboard
    // keeps the cut content even after undo - same as Logic / Pro Tools.
    if (! copySelectedRegion()) return false;
    return deleteSelectedRegion();
}

bool TapeStrip::pasteAtPlayhead()
{
    auto& clip = engine.getRegionClipboard();
    if (! clip.hasContent) return false;

    const int targetTrack = (clip.sourceTrack >= 0 && clip.sourceTrack < Session::kNumTracks)
                              ? clip.sourceTrack : 0;

    AudioRegion pasted = clip.region;
    pasted.timelineStart = engine.getTransport().getPlayhead();

    auto& um = engine.getUndoManager();
    um.beginNewTransaction ("Paste region");
    um.perform (new PasteRegionAction (session, engine, targetTrack, pasted));

    // Select the freshly-pasted region (now at the back of the target's
    // region list) so a follow-up Delete or another paste targets it.
    selectedTrack  = targetTrack;
    selectedRegion = (int) session.track (targetTrack).regions.size() - 1;
    repaint();
    return true;
}

bool TapeStrip::deleteSelectedRegion()
{
    auto selection = allSelectedRegions();
    if (selection.empty()) return false;

    // Drop locked regions from the target list. Locked = reject
    // destructive operations. If every selected region is locked
    // the call is a no-op.
    selection.erase (std::remove_if (selection.begin(), selection.end(),
        [this] (const RegionId& id)
        {
            const auto& regs = session.track (id.track).regions;
            return id.regionIdx < 0 || id.regionIdx >= (int) regs.size()
                || regs[(size_t) id.regionIdx].locked;
        }), selection.end());
    if (selection.empty()) return false;

    // Erase in descending order PER TRACK so earlier indices on the
    // same track stay valid through the loop. Sort by (track ASC,
    // regionIdx DESC) and walk linearly.
    std::sort (selection.begin(), selection.end(),
        [] (const RegionId& a, const RegionId& b)
        {
            return a.track != b.track ? a.track < b.track
                                       : a.regionIdx > b.regionIdx;
        });

    auto& um = engine.getUndoManager();
    um.beginNewTransaction (selection.size() == 1 ? "Delete region"
                                                    : "Delete regions");
    for (const auto& id : selection)
    {
        const auto& regs = session.track (id.track).regions;
        if (id.regionIdx < 0 || id.regionIdx >= (int) regs.size()) continue;
        um.perform (new DeleteRegionAction (session, engine,
                                              id.track, id.regionIdx));
    }
    clearAllSelections();
    repaint();
    return true;
}

bool TapeStrip::duplicateSelectedRegion()
{
    const auto selection = allSelectedRegions();
    if (selection.empty()) return false;

    auto& um = engine.getUndoManager();
    um.beginNewTransaction (selection.size() == 1 ? "Duplicate region"
                                                    : "Duplicate regions");
    for (const auto& id : selection)
    {
        const auto& regs = session.track (id.track).regions;
        if (id.regionIdx < 0 || id.regionIdx >= (int) regs.size()) continue;
        AudioRegion clone = regs[(size_t) id.regionIdx];
        // Drop take history on the duplicate - it's a fresh region as
        // far as the user is concerned; cycling alternate takes on the
        // clone would be confusing. The original keeps its history.
        clone.previousTakes.clear();
        clone.timelineStart = regs[(size_t) id.regionIdx].timelineStart
                             + regs[(size_t) id.regionIdx].lengthInSamples;
        um.perform (new PasteRegionAction (session, engine, id.track, clone));
    }
    repaint();
    return true;
}

bool TapeStrip::nudgeSelectedRegion (juce::int64 deltaSamples)
{
    auto selection = allSelectedRegions();
    if (selection.empty()) return false;
    if (deltaSamples == 0) return false;
    // Locked regions reject nudge. Drop them from the target list.
    selection.erase (std::remove_if (selection.begin(), selection.end(),
        [this] (const RegionId& id)
        {
            const auto& regs = session.track (id.track).regions;
            return id.regionIdx < 0 || id.regionIdx >= (int) regs.size()
                || regs[(size_t) id.regionIdx].locked;
        }), selection.end());
    if (selection.empty()) return false;

    // Group-clamp: don't let any selected region's timelineStart go
    // negative. So minSelectedStart sets the floor on -delta.
    juce::int64 minStart = std::numeric_limits<juce::int64>::max();
    for (const auto& id : selection)
    {
        const auto& regs = session.track (id.track).regions;
        if (id.regionIdx < 0 || id.regionIdx >= (int) regs.size()) continue;
        minStart = juce::jmin (minStart, regs[(size_t) id.regionIdx].timelineStart);
    }
    deltaSamples = juce::jmax (deltaSamples, -minStart);
    if (deltaSamples == 0) return false;

    auto& um = engine.getUndoManager();
    um.beginNewTransaction (deltaSamples > 0 ? "Nudge regions right"
                                              : "Nudge regions left");
    for (const auto& id : selection)
    {
        const auto& regs = session.track (id.track).regions;
        if (id.regionIdx < 0 || id.regionIdx >= (int) regs.size()) continue;
        const auto& current = regs[(size_t) id.regionIdx];
        AudioRegion afterState  = current;
        AudioRegion beforeState = current;
        afterState.timelineStart = current.timelineStart + deltaSamples;
        um.perform (new RegionEditAction (session, engine,
                                            id.track, id.regionIdx,
                                            beforeState, afterState));
    }
    repaint();
    return true;
}

bool TapeStrip::cycleSelectedTakeForward()
{
    // Audio take cycle: rotate the take stack so the next previous take
    // becomes live and the displaced live drops to the back of the
    // stack. Walks every selected region and applies the rotation.
    // No-op when no region in the selection has take history.
    // MIDI fallback: when the selection is a MIDI region (no audio
    // RegionId), rotate the MIDI take stack instead.
    auto selection = allSelectedRegions();
    bool didAny = false;
    auto& um = engine.getUndoManager();
    if (! selection.empty())
    {
        for (const auto& id : selection)
        {
            const auto& regs = session.track (id.track).regions;
            if (id.regionIdx < 0 || id.regionIdx >= (int) regs.size()) continue;
            const auto& cur = regs[(size_t) id.regionIdx];
            if (cur.previousTakes.empty()) continue;

            AudioRegion before = cur;
            AudioRegion after  = cur;
            TakeRef oldLive { after.file, after.sourceOffset, after.lengthInSamples };
            const TakeRef chosen = after.previousTakes.front();
            after.previousTakes.erase (after.previousTakes.begin());
            after.file            = chosen.file;
            after.sourceOffset    = chosen.sourceOffset;
            after.lengthInSamples = chosen.lengthInSamples;
            after.previousTakes.push_back (oldLive);

            if (! didAny) um.beginNewTransaction ("Cycle take");
            um.perform (new RegionEditAction (session, engine,
                                                id.track, id.regionIdx,
                                                before, after));
            didAny = true;
        }
    }
    if (! didAny
        && selectedMidiTrack >= 0 && selectedMidiTrack < Session::kNumTracks
        && selectedMidiRegion >= 0)
    {
        const auto& curList = session.track (selectedMidiTrack).midiRegions.current();
        if (selectedMidiRegion < (int) curList.size())
        {
            const auto& cur = curList[(size_t) selectedMidiRegion];
            if (! cur.previousTakes.empty())
            {
                MidiRegion before = cur;
                MidiRegion after  = cur;
                MidiTakeRef oldLive { after.lengthInTicks,
                                       std::move (after.notes),
                                       std::move (after.ccs) };
                after.lengthInTicks = after.previousTakes.front().lengthInTicks;
                after.notes         = std::move (after.previousTakes.front().notes);
                after.ccs           = std::move (after.previousTakes.front().ccs);
                after.previousTakes.erase (after.previousTakes.begin());
                after.previousTakes.push_back (std::move (oldLive));

                const double sr = engine.getCurrentSampleRate();
                const float bpm = session.tempoBpm.load (std::memory_order_relaxed);
                if (sr > 0.0 && bpm > 0.0f)
                {
                    const double samplesPerTick =
                        (sr * 60.0) / ((double) bpm * (double) kMidiTicksPerQuarter);
                    after.lengthInSamples = (juce::int64) std::llround (
                        (double) after.lengthInTicks * samplesPerTick);
                }

                um.beginNewTransaction ("Cycle take");
                um.perform (new MidiRegionEditAction (session, engine,
                                                        selectedMidiTrack, selectedMidiRegion,
                                                        before, after));
                didAny = true;
            }
        }
    }
    if (didAny) repaint();
    return didAny;
}

bool TapeStrip::cycleSelectedTakeBackward()
{
    // Reverse-cycle: the LAST previous take becomes live; the displaced
    // live drops to the FRONT of the stack. Symmetric to forward cycle
    // so the user can step in either direction through the history.
    auto selection = allSelectedRegions();
    bool didAny = false;
    auto& um = engine.getUndoManager();
    if (! selection.empty())
    {
        for (const auto& id : selection)
        {
            const auto& regs = session.track (id.track).regions;
            if (id.regionIdx < 0 || id.regionIdx >= (int) regs.size()) continue;
            const auto& cur = regs[(size_t) id.regionIdx];
            if (cur.previousTakes.empty()) continue;

            AudioRegion before = cur;
            AudioRegion after  = cur;
            TakeRef oldLive { after.file, after.sourceOffset, after.lengthInSamples };
            const TakeRef chosen = after.previousTakes.back();
            after.previousTakes.pop_back();
            after.file            = chosen.file;
            after.sourceOffset    = chosen.sourceOffset;
            after.lengthInSamples = chosen.lengthInSamples;
            after.previousTakes.insert (after.previousTakes.begin(), oldLive);

            if (! didAny) um.beginNewTransaction ("Cycle take");
            um.perform (new RegionEditAction (session, engine,
                                                id.track, id.regionIdx,
                                                before, after));
            didAny = true;
        }
    }
    if (! didAny
        && selectedMidiTrack >= 0 && selectedMidiTrack < Session::kNumTracks
        && selectedMidiRegion >= 0)
    {
        const auto& curList = session.track (selectedMidiTrack).midiRegions.current();
        if (selectedMidiRegion < (int) curList.size())
        {
            const auto& cur = curList[(size_t) selectedMidiRegion];
            if (! cur.previousTakes.empty())
            {
                MidiRegion before = cur;
                MidiRegion after  = cur;
                MidiTakeRef oldLive { after.lengthInTicks,
                                       std::move (after.notes),
                                       std::move (after.ccs) };
                after.lengthInTicks = after.previousTakes.back().lengthInTicks;
                after.notes         = std::move (after.previousTakes.back().notes);
                after.ccs           = std::move (after.previousTakes.back().ccs);
                after.previousTakes.pop_back();
                after.previousTakes.insert (after.previousTakes.begin(),
                                              std::move (oldLive));

                const double sr = engine.getCurrentSampleRate();
                const float bpm = session.tempoBpm.load (std::memory_order_relaxed);
                if (sr > 0.0 && bpm > 0.0f)
                {
                    const double samplesPerTick =
                        (sr * 60.0) / ((double) bpm * (double) kMidiTicksPerQuarter);
                    after.lengthInSamples = (juce::int64) std::llround (
                        (double) after.lengthInTicks * samplesPerTick);
                }

                um.beginNewTransaction ("Cycle take");
                um.perform (new MidiRegionEditAction (session, engine,
                                                        selectedMidiTrack, selectedMidiRegion,
                                                        before, after));
                didAny = true;
            }
        }
    }
    if (didAny) repaint();
    return didAny;
}

bool TapeStrip::splitSelectedAtPlayhead()
{
    auto selection = allSelectedRegions();
    if (selection.empty()) return false;

    const auto playhead = engine.getTransport().getPlayhead();

    // Filter down to regions whose range strictly contains the
    // playhead AND that aren't locked. SplitRegionAction tolerates
    // edge cases internally but a click without movement shouldn't
    // change anything; the strict-inside gate matches the right-
    // click menu's behaviour. Locked regions reject split.
    auto eligible = [this, playhead] (const RegionId& id)
    {
        const auto& regs = session.track (id.track).regions;
        if (id.regionIdx < 0 || id.regionIdx >= (int) regs.size()) return false;
        const auto& r = regs[(size_t) id.regionIdx];
        if (r.locked) return false;
        return playhead > r.timelineStart
            && playhead < r.timelineStart + r.lengthInSamples;
    };
    selection.erase (std::remove_if (selection.begin(), selection.end(),
                                       [&] (const RegionId& id)
                                       { return ! eligible (id); }),
                       selection.end());
    if (selection.empty()) return false;

    // SplitRegionAction inserts the new piece at idx+1, shifting any
    // higher indices on the same track up by one. Process splits in
    // (track ASC, regionIdx DESC) so each split's index stays valid
    // through the loop. Same idiom as deleteSelectedRegion.
    std::sort (selection.begin(), selection.end(),
        [] (const RegionId& a, const RegionId& b)
        {
            return a.track != b.track ? a.track < b.track
                                       : a.regionIdx > b.regionIdx;
        });

    auto& um = engine.getUndoManager();
    um.beginNewTransaction (selection.size() == 1 ? "Split region"
                                                    : "Split regions");
    for (const auto& id : selection)
    {
        um.perform (new SplitRegionAction (session, engine,
                                             id.track, id.regionIdx,
                                             playhead));
    }
    repaint();
    return true;
}

bool TapeStrip::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (const auto& path : files)
    {
        const auto ext = juce::File (path).getFileExtension().toLowerCase();
        if (ext == ".wav" || ext == ".aiff" || ext == ".aif"
            || ext == ".flac" || ext == ".mid" || ext == ".midi")
            return true;
    }
    return false;
}

void TapeStrip::fileDragEnter (const juce::StringArray& files, int x, int y)
{
    dropAccepted = isInterestedInFileDrag (files);
    fileDragMove (files, x, y);
}

void TapeStrip::fileDragMove (const juce::StringArray&, int x, int y)
{
    if (! dropAccepted) return;
    int hoveredTrack = -1;
    for (int t = 0; t < Session::kNumTracks; ++t)
        if (rowBounds (t).contains (x, y)) { hoveredTrack = t; break; }
    if (hoveredTrack != dropHoverTrack || x != dropHoverX)
    {
        dropHoverTrack = hoveredTrack;
        dropHoverX     = x;
        repaint();
    }
}

void TapeStrip::fileDragExit (const juce::StringArray&)
{
    dropAccepted   = false;
    dropHoverTrack = -1;
    dropHoverX     = -1;
    repaint();
}

void TapeStrip::filesDropped (const juce::StringArray& files, int x, int y)
{
    dropAccepted   = false;
    dropHoverTrack = -1;
    dropHoverX     = -1;
    repaint();

    if (files.isEmpty() || ! onFilesDropped) return;

    int trackHint = -1;
    for (int t = 0; t < Session::kNumTracks; ++t)
        if (rowBounds (t).contains (x, y)) { trackHint = t; break; }

    const auto col = tracksColumnBounds();
    const int clampedX = juce::jlimit (col.getX(), col.getRight(), x);
    const auto timelineStart = sampleAtX (clampedX);

    juce::Array<juce::File> compatible;
    for (const auto& path : files)
    {
        const juce::File f (path);
        const auto ext = f.getFileExtension().toLowerCase();
        if (ext == ".wav" || ext == ".aiff" || ext == ".aif"
            || ext == ".flac" || ext == ".mid" || ext == ".midi")
            compatible.add (f);
    }
    if (! compatible.isEmpty())
        onFilesDropped (compatible, timelineStart, trackHint);
}
} // namespace duskstudio
