#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <memory>
#include "DuskComboBox.h"
#include "../session/Session.h"

namespace duskstudio
{
class BrickwallLimiter;

// Waves L4-style brickwall limiter editor for the mastering chain.
//
// Layout (left → right):
//   • Two big vertical meters: Threshold (drag the handle to adjust drive
//     into the limiter via params.limiterDriveDb) and Ceiling (drag to set
//     limiterCeilingDb). Each meter shows the live level next to its handle.
//   • Atten meter: live gain reduction (1..30 dB).
//   • LUFS readout box (Long-term integrated I-LUFS via params).
//   • Right column: Mode dropdown + Release / Stereo-link toggles.
class MasteringLimiterEditor final : public juce::Component, private juce::Timer
{
public:
    MasteringLimiterEditor (MasteringParams& params,
                              BrickwallLimiter& limiterRef);
    ~MasteringLimiterEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp   (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

private:
    void timerCallback() override;

    // Vertical drag mapping. A click+drag inside one of the meter columns
    // adjusts that column's parameter (Threshold = drive, Ceiling = ceiling).
    enum class DragMode { None, Threshold, Ceiling };
    DragMode currentDrag = DragMode::None;

    float yToDriveDb   (int y) const noexcept;   // threshold meter -> drive (-20..0)
    float yToCeilingDb (int y) const noexcept;   // ceiling meter   -> ceiling (-12..0)

    MasteringParams&  params;
    BrickwallLimiter& limiter;

    // Shared console chrome — LED-pill header (click toggles enable), same
    // component the strips use. See MasteringEqEditor.
    std::unique_ptr<class CompHeaderButton> headerBtn;

    juce::Label       modeCaption  { {}, "Mode"   };
    DuskComboBox    modeCombo;

    juce::Slider releaseKnob;
    juce::Label  releaseLabel { {}, "Release" };

    juce::Slider lookaheadKnob;
    juce::Label  lookaheadLabel { {}, "Lookahead" };

    juce::ToggleButton stereoLinkToggle { "Stereo link" };

    // Threshold and Ceiling meter columns. dB ranges:
    //   Threshold meter: -20..0 dB. The handle position represents the
    //     applied input drive; the live signal fills the column.
    //   Ceiling meter: -12..0 dB. The handle is the ceiling parameter; the
    //     live post-limiter peak fills it.
    juce::Rectangle<int> threshMeterArea, ceilingMeterArea, attenMeterArea;
    juce::Rectangle<int> lufsBoxArea;

    // Smoothed display values, updated from the timer at 30 Hz.
    float displayedInDb   = -100.0f;   // pre-limiter peak (uses params input meter)
    float displayedOutDb  = -100.0f;   // post-limiter peak
    float displayedGrDb   = 0.0f;
};
} // namespace duskstudio
