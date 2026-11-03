#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include "DspReadout.h"
#include "../engine/AudioEngine.h"
#include "../foundation/MessageThread.h"

class ChordAnalyzer;

namespace duskstudio
{
// Compact monospace readout that lives in the upper-right of the window:
//   Audio: 48 kHz 5.3 ms   DSP: 12% (3)
// Updated 4 Hz. CPU comes from AudioDeviceManager; xrun count from AudioEngine.
class SystemStatusBar final : public juce::Component, private dusk::Timer
{
public:
    explicit SystemStatusBar (AudioEngine& engineRef);
    ~SystemStatusBar() override;

    void paint (juce::Graphics&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

    // The DSP segment's bounds - shared by paint() and the double-click
    // hit test so the reset zone always matches what's drawn.
    juce::Rectangle<int> dspSegmentBounds() const noexcept
    {
        // Wide enough for the worst case incl. the oversampling badge and
        // the realtime count: "DSP: 100% (99/99) @4x RT 9".
        return getLocalBounds().reduced (8, 0).removeFromRight (195);
    }

    // The DSP segment exactly as the last tick built it.
    std::string dspReadoutForScenario() const { return dspInfo.toStdString(); }

    // Stands in for the engine's readings until cleared with nullopt, and
    // rebuilds the readout from them at once.
    void pinReadingsForScenario (std::optional<DspReadings> readings);

    // The DSP segment's text pixel furthest from the background, as ARGB.
    std::uint32_t dspPaintForScenario();

private:
    void timerCallback() override;

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
    DspReadings lastReadings;
    DspSeverity lastDspSeverity = DspSeverity::normal;
    bool        lastAudioWarn   = false;
    std::optional<DspReadings> pinnedReadings;

    // Last-painted snapshot. The tick fires at 10 Hz but the readout is
    // static most of the time; only repaint when something the paint()
    // actually renders has changed, so an idle session stops burning paint
    // cycles + Font work every 100 ms.
    juce::String paintedAudioInfo;
    juce::String paintedDspInfo;
    juce::String paintedChordInfo;
    bool         paintedAudioWarn   = false;
    DspSeverity  paintedDspSeverity = DspSeverity::normal;
};
} // namespace duskstudio
