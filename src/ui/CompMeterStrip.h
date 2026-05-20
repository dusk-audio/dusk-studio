#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../session/Session.h"

namespace duskstudio
{
// Mixbus-style compressor metering: two vertical LED bars side by side
// (input level on the left, gain reduction on the right) plus a draggable
// threshold marker on the input meter. Replaces the THR knob - drag the
// triangle handle up/down on the input meter to set the threshold relative
// to the live input signal.
class CompMeterStrip final : public juce::Component, private juce::Timer
{
public:
    explicit CompMeterStrip (Track& trackRef);
    ~CompMeterStrip() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp   (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

    static constexpr float kFloorDb   = -60.0f;
    static constexpr float kCeilingDb =   0.0f;
    static constexpr float kGrFloorDb = -20.0f;
    static constexpr float kHandleW   = 10.0f;
    static constexpr float kBarW      = 10.0f;

private:
    void timerCallback() override;

    float dbToFrac (float db) const noexcept;
    float fracToDb (float frac) const noexcept;
    float yForDb (float db, juce::Rectangle<float> area) const noexcept;
    float dbForY (int y, juce::Rectangle<float> area) const noexcept;

    Track& track;

    // Smoothed display values - updated from the Timer callback so the meter
    // breathes naturally with fast-attack / slow-decay envelopes.
    float displayedInputDb   = -100.0f;
    float inputPeakHoldDb    = -100.0f;
    int   inputPeakHoldFrames = 0;
    float displayedGrDb      = 0.0f;

    // Layout rectangles set in resized().
    juce::Rectangle<float> handleArea;
    juce::Rectangle<float> inputBarArea;
    juce::Rectangle<float> grBarArea;

    bool draggingThreshold = false;
};
} // namespace duskstudio
