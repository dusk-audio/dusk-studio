#include "MiniTimelineStrip.h"

#include "../session/Session.h"
#include "../engine/AudioEngine.h"

#include <cmath>

namespace duskstudio
{
namespace
{
const juce::Colour kBg        { 0xff15151a };
const juce::Colour kBorder    { 0xff32323a };
const juce::Colour kTrackLine { 0xff3a3a44 };
const juce::Colour kCap       { 0xff90a0b0 };
const juce::Colour kPlayhead  { 0xffe04040 };   // tape-ruler red
const juce::Colour kMarkerDim { 0xffe0a050 };   // default marker amber
} // namespace

MiniTimelineStrip::MiniTimelineStrip (Session& s, AudioEngine& e)
    : session (s), engine (e)
{
    setInterceptsMouseClicks (true, false);
    startTimerHz (30);
    engine.getUndoManager().addChangeListener (this);
}

MiniTimelineStrip::~MiniTimelineStrip()
{
    engine.getUndoManager().removeChangeListener (this);
}

juce::int64 MiniTimelineStrip::songEndSamples() const noexcept
{
    juce::int64 end = engine.getTransport().getPlayhead();
    for (int t = 0; t < Session::kNumTracks; ++t)
    {
        for (const auto& r : session.track (t).regions)
            end = juce::jmax (end, r.timelineStart + r.lengthInSamples);
        for (const auto& m : session.track (t).midiRegions.current())
            end = juce::jmax (end, m.timelineStart + m.lengthInSamples);
    }
    // Markers can sit past the last region (e.g. an "End" marker placed beyond
    // the audio). Extend the extent to cover them so they don't get clamped to
    // the right edge and mis-hit-test in the collapsed timeline.
    for (const auto& mk : session.getMarkers())
        end = juce::jmax (end, mk.timelineSamples);
    const double sr = engine.getCurrentSampleRate();
    const juce::int64 floorLen = (juce::int64) ((sr > 0.0 ? sr : 48000.0) * 60.0);  // 60 s
    return juce::jmax (end, floorLen);
}

int MiniTimelineStrip::xForSample (juce::int64 s, juce::int64 end) const noexcept
{
    const int usable = juce::jmax (1, getWidth() - kPadL - kPadR);
    const double frac = end > 0 ? juce::jlimit (0.0, 1.0, (double) s / (double) end) : 0.0;
    return kPadL + (int) std::lround (frac * (double) usable);
}

juce::int64 MiniTimelineStrip::sampleForX (int x, juce::int64 end) const noexcept
{
    const int usable = juce::jmax (1, getWidth() - kPadL - kPadR);
    const double frac = juce::jlimit (0.0, 1.0, (double) (x - kPadL) / (double) usable);
    return (juce::int64) (frac * (double) end);
}

int MiniTimelineStrip::markerIndexAtX (int x, juce::int64 end) const noexcept
{
    const auto& markers = session.getMarkers();
    for (int i = 0; i < (int) markers.size(); ++i)
        if (std::abs (xForSample (markers[(size_t) i].timelineSamples, end) - x) <= kMarkerHitPx)
            return i;
    return -1;
}

void MiniTimelineStrip::paint (juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat();
    g.setColour (kBg);
    g.fillRoundedRectangle (b, 3.0f);
    g.setColour (kBorder);
    g.drawRoundedRectangle (b.reduced (0.5f), 3.0f, 0.8f);

    const juce::int64 end = songEndSamples();
    const juce::int64 ph  = engine.getTransport().getPlayhead();
    const float midY = b.getCentreY();
    const int   x0 = xForSample (0,   end);
    const int   x1 = xForSample (end, end);

    // Centre track line between the start/end caps.
    g.setColour (kTrackLine);
    g.drawLine ((float) x0, midY, (float) x1, midY, 1.5f);

    // Start + end caps (short vertical brackets).
    g.setColour (kCap);
    const float capH = b.getHeight() * 0.5f;
    g.drawLine ((float) x0, midY - capH * 0.5f, (float) x0, midY + capH * 0.5f, 1.6f);
    g.drawLine ((float) x1, midY - capH * 0.5f, (float) x1, midY + capH * 0.5f, 1.6f);

    // Active marker = last one at or before the playhead (markers are sorted).
    const auto& markers = session.getMarkers();
    int activeIdx = -1;
    for (int i = 0; i < (int) markers.size(); ++i)
    {
        if (markers[(size_t) i].timelineSamples <= ph) activeIdx = i;
        else break;
    }

    for (int i = 0; i < (int) markers.size(); ++i)
    {
        const auto& mk = markers[(size_t) i];
        const int   mx = xForSample (mk.timelineSamples, end);
        const bool  active = (i == activeIdx);
        const auto  col = mk.colour.isTransparent() ? kMarkerDim : mk.colour;
        const float tickH = b.getHeight() * (active ? 0.80f : 0.52f);
        g.setColour (active ? col.brighter (0.30f) : col.withAlpha (0.85f));
        g.drawLine ((float) mx, midY - tickH * 0.5f, (float) mx, midY + tickH * 0.5f,
                    active ? 2.0f : 1.4f);
    }

    // Marker names: a small flag right of each tick, clipped to the gap before
    // the next marker so adjacent labels never overlap (ellipsised if cramped).
    // The active marker's flag is brightened. Rects are recorded so a click on
    // the visible label — not just the tick — hits the marker.
    markerFlags.clear();
    const juce::Font nameFont { juce::FontOptions (11.0f, juce::Font::bold) };
    g.setFont (nameFont);
    for (int i = 0; i < (int) markers.size(); ++i)
    {
        const auto& mk    = markers[(size_t) i];
        const int   mx    = xForSample (mk.timelineSamples, end);
        const int   nextX = (i + 1 < (int) markers.size())
                              ? xForSample (markers[(size_t) (i + 1)].timelineSamples, end)
                              : (int) b.getRight();
        const int   textX = mx + 5;
        const int   avail = juce::jmin ((int) b.getRight() - textX - 3, nextX - textX - 2);
        if (avail < 12) continue;

        const bool  active = (i == activeIdx);
        const auto  col = mk.colour.isTransparent() ? kMarkerDim : mk.colour;
        const int   tw  = juce::jmin (avail, nameFont.getStringWidth (mk.name) + 6);
        const juce::Rectangle<int> ri (textX, (int) b.getY() + 2, tw, (int) b.getHeight() - 4);
        g.setColour (kBg.withAlpha (active ? 0.9f : 0.72f));
        g.fillRoundedRectangle (ri.toFloat(), 2.0f);
        g.setColour (active ? col.brighter (0.35f) : col.withAlpha (0.9f));
        g.drawText (mk.name, ri.reduced (3, 0), juce::Justification::centredLeft, true);
        markerFlags.push_back ({ i, ri });
    }

    // Playhead: full-height line + small triangle head.
    const int phX = xForSample (ph, end);
    g.setColour (kPlayhead);
    g.drawLine ((float) phX, b.getY() + 1.0f, (float) phX, b.getBottom() - 1.0f, 1.4f);
    juce::Path tri;
    tri.addTriangle ((float) phX - 3.0f, b.getY() + 1.0f,
                     (float) phX + 3.0f, b.getY() + 1.0f,
                     (float) phX,        b.getY() + 4.0f);
    g.fillPath (tri);
}

int MiniTimelineStrip::markerAtX (int x) const noexcept
{
    const int n = (int) session.getMarkers().size();
    for (const auto& f : markerFlags)
        if (f.first < n && x >= f.second.getX() - 2 && x <= f.second.getRight() + 2)
            return f.first;
    return markerIndexAtX (x, songEndSamples());
}

void MiniTimelineStrip::mouseDown (const juce::MouseEvent& ev)
{
    const juce::int64 end = songEndSamples();
    const int mi = markerAtX (ev.x);
    if (mi >= 0)
        engine.getTransport().setPlayhead (session.getMarkers()[(size_t) mi].timelineSamples);
    else
        engine.getTransport().setPlayhead (sampleForX (ev.x, end));
    repaint();
}

void MiniTimelineStrip::mouseDoubleClick (const juce::MouseEvent& ev)
{
    const int mi = markerAtX (ev.x);
    if (mi >= 0 && onMarkerEdit)
        onMarkerEdit (mi);
}

void MiniTimelineStrip::mouseMove (const juce::MouseEvent& ev)
{
    const int mi = markerAtX (ev.x);
    const auto& mk = session.getMarkers();
    setTooltip (mi >= 0 && mi < (int) mk.size() ? mk[(size_t) mi].name : juce::String());
    setMouseCursor (mi >= 0 ? juce::MouseCursor::PointingHandCursor
                            : juce::MouseCursor::NormalCursor);
}

void MiniTimelineStrip::timerCallback()
{
    bool dirty = false;

    // Repaint on playhead motion.
    const auto ph = engine.getTransport().getPlayhead();
    if (ph != lastPlayhead) { lastPlayhead = ph; dirty = true; }

    // The UndoManager broadcast (changeListenerCallback) covers undoable edits,
    // but direct session mutations — import / load, programmatic region or
    // marker changes — don't go through it. Poll a cheap content signature
    // (region + marker counts and the song extent) so those still refresh.
    size_t sig = session.getMarkers().size();
    for (int t = 0; t < Session::kNumTracks; ++t)
    {
        sig = sig * 131 + session.track (t).regions.size();
        sig = sig * 131 + (size_t) session.track (t).midiRegions.current().size();
    }
    sig = sig * 131 + (size_t) songEndSamples();
    if (sig != lastContentSig) { lastContentSig = sig; dirty = true; }

    if (dirty) repaint();
}

void MiniTimelineStrip::changeListenerCallback (juce::ChangeBroadcaster*)
{
    repaint();
}
} // namespace duskstudio
