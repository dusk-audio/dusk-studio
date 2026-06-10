#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <memory>
#include "../engine/AudioEngine.h"

class ChordAnalyzer;

namespace duskstudio
{
// Compact monospace readout that lives in the upper-right of the window:
//   Audio: 48 kHz 5.3 ms   DSP: 12% (3)
// Updated 4 Hz. CPU comes from AudioDeviceManager; xrun count from AudioEngine.
class SystemStatusBar final : public juce::Component, private juce::Timer
{
public:
    explicit SystemStatusBar (AudioEngine& engineRef);
    ~SystemStatusBar() override;

    void paint (juce::Graphics&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

private:
    void timerCallback() override;

    // The DSP segment's bounds — shared by paint() and the double-click
    // hit test so the reset zone always matches what's drawn.
    juce::Rectangle<int> dspSegmentBounds() const noexcept
    {
        return getLocalBounds().reduced (8, 0).removeFromRight (140);
    }

    AudioEngine& engine;
    juce::String audioInfo  { "Audio: -" };
    juce::String dspInfo    { "DSP: -"   };
    juce::String chordInfo;   // empty when nothing is held

    // The ChordAnalyzer ctor allocates (it builds an internal pattern
    // table), so it lives behind a unique_ptr to keep this header
    // forward-declaration-clean. Lives on the message thread; analyze()
    // runs from the timer.
    std::unique_ptr<::ChordAnalyzer> chordAnalyzer;
    int lastHeldFingerprint = 0;   // simple hash of held-notes mask; skip re-analysis when unchanged

    // Sampled once per timer tick and reused by paint() so the displayed
    // text and warning colour always reflect the same snapshot of engine
    // state - querying the engine separately from paint() can race a fresh
    // tick mid-frame and show "12%" in green next to a 90% warn colour.
    double lastCpuUsage      = 0.0;
    int    lastEngineXruns   = 0;
    int    lastBackendXruns  = 0;
    bool   lastAudioWarn     = false;

    // Last-painted snapshot. The tick fires at 10 Hz but the readout is
    // static most of the time; only repaint when something the paint()
    // actually renders has changed, so an idle session stops burning paint
    // cycles + Font work every 100 ms.
    juce::String paintedAudioInfo;
    juce::String paintedDspInfo;
    juce::String paintedChordInfo;
    bool         paintedAudioWarn = false;
    bool         paintedDspWarn   = false;
};
} // namespace duskstudio
