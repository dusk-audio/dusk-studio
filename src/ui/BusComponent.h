#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <memory>
#include "../session/Session.h"
#include "AnalogVuMeter.h"

namespace duskstudio
{
class BusComponent final : public juce::Component, private juce::Timer
{
public:
    BusComponent (Bus& busRef, class Session& sessionRef, int busIndex);
    ~BusComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent& e) override;

    // Shrinks the VU meter's vertical footprint when the tape SUMMARY
    // is expanded - frees pixels for the fader. Toggled by ConsoleView
    // alongside setStripsCompactMode.
    void setCompactVu (bool compact);

private:
    bool compactVu = false;
    void timerCallback() override;
    void showColourMenu();
    void applyBusColour (juce::Colour c);

    Bus& bus;
    Session& sessionRef;
    int busIndex;
    juce::Label nameLabel;
    juce::Rectangle<int> fxArea;     // unused for now; reserved for future inline DSP UI

    // 3-band EQ controls (LF / MID / HF gains, fixed musical frequencies).
    juce::TextButton eqButton  { "EQ" };
    juce::Slider     eqLfGain  { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Slider     eqMidGain { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Slider     eqHfGain  { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Label      eqLfLbl, eqMidLbl, eqHfLbl;

    // Bus compressor controls.
    juce::TextButton compButton { "COMP" };
    juce::Slider     compThresh  { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Slider     compRatio   { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Slider     compAttack  { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Slider     compRelease { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Slider     compMakeup  { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Label      compThrLbl, compRatLbl, compAtkLbl, compRelLbl, compMakLbl;

    // Pan knob.
    juce::Slider panKnob { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Label  panLbl;

    juce::Slider     faderSlider { juce::Slider::LinearVertical, juce::Slider::TextBoxBelow };
    juce::TextButton muteButton { "M" };
    juce::TextButton soloButton { "S" };

    // Analog VU meter at the top of the strip. Reads the bus's post-DSP
    // peak atoms; its internal Timer applies VU ballistics and repaints
    // independently of the strip's own timer.
    std::unique_ptr<AnalogVuMeter> vuMeter;

    // Stereo output meter (L | R) on the right side of the fader, matching
    // the master strip's layout. Smoothed and peak-hold values per channel.
    // Plus a slim vertical GR bar (top-down fill, gold→red) so the user
    // sees compressor activity at a glance, not just as a numeric readout.
    juce::Rectangle<int> meterArea;
    juce::Rectangle<int> grMeterArea;
    juce::Rectangle<int> faderScaleArea;
    // Painted background bands for the EQ + COMP sections - same framed
    // look as the channel strip's eqArea / compArea so all strip types
    // share one visual grammar.
    juce::Rectangle<int> eqArea;
    juce::Rectangle<int> compArea;
    juce::Label outputPeakLabel;
    juce::Label grPeakLabel;
    float displayedOutputLDb = -100.0f;
    float displayedOutputRDb = -100.0f;
    float displayedGrDb      = 0.0f;
    float outputPeakHoldLDb  = -100.0f;
    float outputPeakHoldRDb  = -100.0f;
    int   outputPeakHoldFramesL = 0;
    int   outputPeakHoldFramesR = 0;
};
} // namespace duskstudio
