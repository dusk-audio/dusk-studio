#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <memory>
#include "../session/Session.h"
#include "AnalogVuMeter.h"

// Forward decl unconditional; the definition is only #included from the
// .cpp when DUSKSTUDIO_HAS_DUSK_DSP is set. The pointer parameter stays valid
// either way (passed as nullptr when the donor isn't available).
class TapeMachineAudioProcessor;

namespace duskstudio
{
class AudioEngine;
class MasterStripComponent final : public juce::Component, private juce::Timer
{
public:
    // tapeProcessor is a borrowed pointer to the master-bus TapeMachine
    // instance owned by AudioEngine; the gear button uses it to spawn the
    // editor. Nullable when the donor DSP is disabled (DUSKSTUDIO_HAS_DUSK_DSP=0).
    // sessionRef is the live session, used by the right-click MIDI Learn
    // menu on the master fader. engineRef is needed so the automation
    // capture path can read the transport state (playhead + isPlaying).
    explicit MasterStripComponent (MasterBusParams& paramsRef,
                                   class Session& sessionRef,
                                   AudioEngine& engineRef,
                                   ::TapeMachineAudioProcessor* tapeProcessor = nullptr);
    ~MasterStripComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent& e) override;

    // Shrinks the VU meter's vertical footprint when the tape SUMMARY
    // is expanded. Toggled by ConsoleView alongside setStripsCompactMode.
    void setCompactVu (bool compact);

private:
    bool compactVu = false;
    void timerCallback() override;

    MasterBusParams& params;
    class Session& session;
    AudioEngine& engine;

    juce::Label nameLabel;

    // Pultec-style Tube EQ.
    juce::TextButton eqButton    { "EQ" };
    juce::Slider     eqLfBoost   { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Slider     eqHfBoost   { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Slider     eqHfAtten   { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Slider     eqTubeDrive { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Slider     eqOutputGain { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Label      eqLfLabel, eqHfBoostLabel, eqHfAttenLabel, eqDriveLabel, eqOutLabel;

    // Bus compressor (UniversalCompressor in Bus mode).
    juce::TextButton compButton    { "COMP" };
    juce::Slider     compThreshold { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Slider     compRatio     { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Slider     compAttack    { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Slider     compRelease   { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Slider     compMakeup    { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Label      compThrLabel, compRatLabel, compAtkLabel, compRelLabel, compMakLabel;

    juce::TextButton tapeButton    { "TAPE" };
    juce::TextButton tapeGearButton { juce::CharPointer_UTF8 ("\xe2\x9a\x99") };  // U+2699 GEAR
    ::TapeMachineAudioProcessor* tapeProcessorPtr = nullptr;
    void openTapeMachineModal();
    std::unique_ptr<class DimOverlay> tapeMachineDim;
    juce::Component::SafePointer<juce::Component> tapeMachineModal;

    juce::Slider faderSlider { juce::Slider::LinearVertical, juce::Slider::TextBoxBelow };
    juce::TextButton autoModeButton { "Off" };

    // Master mute + mono-sum buttons. Mute zeros the bus; mono sums
    // L+R*0.5 into both channels for mono-compat checks. The
    // monoStereoButton's runtime label flips between "STEREO" (default)
    // and "MONO" (mono-sum on); the constructor sets the initial text
    // from the params.monoSum atom, so the inline literal here only
    // matters until that runs.
    juce::TextButton muteButton       { "M" };
    juce::TextButton monoStereoButton { "STEREO" };

    // Throttled motor-fader display state.
    float displayedLiveFaderDb { 0.0f };

    void showAutoModeMenu();
    void setAutoMode (AutomationMode m);
    void captureFaderWritePoint (float denormDb);

    // Analog VU meter at the top of the strip - same look as the bus VUs
    // so the user reads master level the same way they read bus level.
    std::unique_ptr<AnalogVuMeter> vuMeter;

    // Output stereo meter (post-master peak in dB, L/R split) + GR readout.
    // The meter sits to the RIGHT of the fader to match the channel-strip
    // layout. Two columns (L | R) live inside meterArea; we cache smoothed
    // and peak-hold values per channel.
    juce::Rectangle<int> meterArea;
    // Slim vertical bar showing the master bus comp's gain reduction.
    // Sits between the fader and the L/R output bars so the user can see
    // the compressor's contribution to the final signal at a glance.
    juce::Rectangle<int> grMeterArea;
    juce::Rectangle<int> faderScaleArea;
    // Framed bands for the EQ + COMP sections - same grammar as the
    // channel + bus strips.
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
