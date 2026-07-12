#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../session/Session.h"

namespace duskstudio
{
class ChannelCompEditor final : public juce::Component, private juce::Timer
{
public:
    explicit ChannelCompEditor (Track& trackRef);
    ~ChannelCompEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    void mouseDown      (const juce::MouseEvent&) override;
    void mouseDrag      (const juce::MouseEvent&) override;
    void mouseUp        (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

private:
    void timerCallback() override;
    void setMode (int modeIndex);
    void refreshModeButtons();

    // Sets each knob's label to the mode-appropriate name and dims any
    // knobs that are no-ops in the active mode (Opto's attack/release are
    // fixed by the optical model). Called whenever the mode changes.
    void refreshLabelsForMode();

    // Unified-knob -> per-mode parameter routing. Each writeXxxToMode reads
    // the unified knob and stores into whatever per-mode atomic the engine
    // actually reads for the currently-selected comp mode. syncKnobsFromMode
    // does the inverse - called after a mode change so the displayed knob
    // values reflect the active mode's stored state.
    void writeThresholdToMode();
    void writeRatioToMode();
    void writeAttackToMode();
    void writeReleaseToMode();
    void writeMakeupToMode();
    void syncKnobsFromMode();

    Track& track;

    juce::Label titleLabel;
    juce::TextButton onButton  { "ON"   };
    juce::TextButton modeOpto  { "Opto" };
    juce::TextButton modeFet   { "FET"  };
    juce::TextButton modeVca   { "VCA"  };
    // VCA-only knee selector: hard knee (default) vs soft (parabolic) knee.
    // Visible / enabled only in VCA mode - see refreshLabelsForMode().
    juce::TextButton vcaOverEasyBtn { "Soft Knee" };
    // VCA detector mode: ADAPT (level-dependent) vs CLASSIC (fixed 10 ms TC).
    // Visible / enabled only in VCA mode - see refreshLabelsForMode().
    juce::TextButton vcaDetectorBtn { "Classic" };

    juce::Label  threshLabel,  ratioLabel,  attackLabel,  releaseLabel,  makeupLabel;
    juce::Slider threshKnob, ratioKnob, attackKnob, releaseKnob, makeupKnob;

    // Vertical meters on the right side of the popup. Input is the
    // pre-comp signal level (taken from track.meterInputDb), GR is the
    // gain reduction (negative dB). threshHandleArea hosts the draggable
    // threshold triangle to the LEFT of the input meter, mirroring the
    // CompMeterStrip layout on the inline strip.
    juce::Rectangle<int> inputMeterArea;
    juce::Rectangle<int> grMeterArea;
    juce::Rectangle<int> meterScaleArea;
    juce::Rectangle<int> threshHandleArea;
    float displayedInputDb = -100.0f;
    float displayedGrDb    = 0.0f;
    bool  draggingThreshold = false;
};
} // namespace duskstudio
