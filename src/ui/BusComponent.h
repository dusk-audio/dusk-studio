#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <memory>
#include "../session/Session.h"
#include "AnalogVuMeter.h"
#include "CompMeterStrip.h"
#include "CompHeaderButton.h"
#include "EmbeddedModal.h"

namespace duskstudio
{
class AudioEngine;

class BusComponent final : public juce::Component, private juce::Timer
{
public:
    BusComponent (Bus& busRef, class Session& sessionRef,
                  class AudioEngine& engineRef, int busIndex);
    ~BusComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent& e) override;

    // Shrinks the VU meter's vertical footprint when the tape TIMELINE
    // is expanded - frees pixels for the fader. Toggled by ConsoleView
    // alongside setStripsCompactMode.
    void setCompactVu (bool compact);

    // Collapses the EQ + COMP sections into small placeholder buttons
    // (matching ChannelStripComponent's compact-mode behaviour) so the
    // bus + master strips visually shrink alongside the channel strips
    // when the tape TIMELINE view consumes vertical room. Toggled by
    // ConsoleView::applyCompactState.
    void setCompactMode (bool compact);

private:
    bool compactVu = false;
    bool compactMode = false;
    // Cached state for the compact-mode pill colour gating so the 30 Hz
    // timer only repaints the pills when their on/off state actually
    // flipped. -1 forces the first tick to refresh.
    int lastCompactEqOn   = -1;
    int lastCompactCompOn = -1;
    juce::Colour lastBusColour;   // cached so the timer repaints on external colour change (undo)
    void timerCallback() override;
    void showColourMenu();
    void applyBusColour (juce::Colour c);

    Bus& bus;
    Session& sessionRef;
    AudioEngine& engine;
    int busIndex;
    juce::Label nameLabel;

    // 3-band EQ controls (LF / MID / HF gains, fixed musical frequencies).
    // Header is a CompHeaderButton (LED + label pill) so the EQ section
    // shares its visual grammar with the COMP header above and with the
    // channel-strip EQ header — single look across the desk.
    std::unique_ptr<CompHeaderButton> eqHeaderBtn;
    juce::Slider     eqLfGain  { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Slider     eqMidGain { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Slider     eqHfGain  { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Label      eqLfLbl, eqMidLbl, eqHfLbl;

    // Bus compressor controls. Shell mirrors the channel-strip COMP
    // section visually: a single CompHeaderButton on top, a CompMeterStrip
    // on the left, and the parameter knob grid on the right. The DSP
    // underneath is still a fixed SSL-style glue topology — no mode
    // picker, so the header button only toggles enable.
    std::unique_ptr<CompHeaderButton> compHeaderBtn;
    std::unique_ptr<CompMeterStrip>   compMeter;
    juce::Slider     compRatio   { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Slider     compAttack  { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Slider     compRelease { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Slider     compMakeup  { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Label      compRatLbl, compAtkLbl, compRelLbl, compMakLbl;

    // Pan knob.
    juce::Slider panKnob { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Label  panLbl;

    juce::Slider     faderSlider { juce::Slider::LinearVertical, juce::Slider::TextBoxBelow };
    // Standalone fader value readout below the slider — mirrors the channel
    // strip's fader-side grammar so the bus value sits in the same place /
    // font weight as a track's value.
    juce::Label      faderValueLabel;
    juce::TextButton muteButton { "M" };
    juce::TextButton soloButton { "S" };

    // Automation-mode button — thin full-width row above M/S, exactly like
    // the channel strips. Cycles Off/Read/Write/Touch via an in-window menu;
    // the engine's per-block bus automation routing honours the mode for the
    // fader, pan, and mute.
    juce::TextButton autoModeButton { "OFF" };
    void showAutoModeMenu();
    void setAutoMode (AutomationMode mode);
    void refreshAutoModeButton();
    void captureWritePoint (AutomationParam param, float denormValue);
    // Last-displayed live values so the motor-fader/pan timer only calls
    // setValue when the automated value actually moved.
    float displayedLiveFaderDb = 0.0f;
    float displayedLivePan      = 0.0f;

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
    // Compact-mode placeholder buttons. Hidden when compactMode=false;
    // visible (and the section knobs hidden) when true. Decorative —
    // click does nothing (tooltip explains the TIMELINE toggle owns the
    // expand/collapse). Mirrors ChannelStripComponent's eqCompactButton
    // / compCompactButton grammar.
    juce::TextButton eqCompactButton  { "EQ"   };
    juce::TextButton compCompactButton { "COMP" };
    // Compact-mode popups: clicking the placeholder button shows a
    // mini-editor mirroring the bus's EQ / COMP knobs so the user can
    // tweak without expanding the strip back to full mode.
    EmbeddedModal eqEditorModal;
    EmbeddedModal compEditorModal;
    void openEqEditorPopup();
    void openCompEditorPopup();
    juce::Label outputPeakLabel;
    float displayedOutputLDb = -100.0f;
    float displayedOutputRDb = -100.0f;
    float displayedGrDb      = 0.0f;
    float outputPeakHoldLDb  = -100.0f;
    float outputPeakHoldRDb  = -100.0f;
    int   outputPeakHoldFramesL = 0;
    int   outputPeakHoldFramesR = 0;
};
} // namespace duskstudio
