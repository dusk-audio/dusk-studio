#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace duskstudio
{
class Session;
class AudioEngine;

// Thin "song map" shown when the tape strip is collapsed: song start, marker
// ticks, song end, and the playhead, drawn full-song-fit (no horizontal scroll).
// Click to seek; click a marker tick to jump to it. The active marker's name is
// drawn as a small flag next to its tick (the textual section readout lives here
// now, not in the transport bar); other names appear as hover tooltips.
class MiniTimelineStrip final : public juce::Component,
                                public juce::SettableTooltipClient,
                                private juce::Timer,
                                private juce::ChangeListener
{
public:
    MiniTimelineStrip (Session& sessionRef, AudioEngine& engineRef);
    ~MiniTimelineStrip() override;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;

private:
    void timerCallback() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    // Song extent: 0 .. max(rightmost region end, playhead, 60 s floor).
    juce::int64 songEndSamples() const noexcept;
    int         xForSample (juce::int64 s, juce::int64 end) const noexcept;
    juce::int64 sampleForX (int x, juce::int64 end) const noexcept;
    int         markerIndexAtX (int x, juce::int64 end) const noexcept;

    Session&     session;
    AudioEngine& engine;
    juce::int64  lastPlayhead = -1;
    size_t       lastContentSig = 0;   // region/marker counts + extent, for refresh polling

    static constexpr int kPadL = 6;
    static constexpr int kPadR = 6;
    static constexpr int kMarkerHitPx = 4;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MiniTimelineStrip)
};
} // namespace duskstudio
