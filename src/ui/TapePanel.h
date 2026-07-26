#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "AnalogVuMeter.h"
#include "DuskComboBox.h"
#include "../session/Session.h"
#include "../foundation/MessageThread.h"

#include <atomic>
#include <memory>

#if DUSKSTUDIO_HAS_DUSK_DSP
class TapeReelComponent;
#endif

namespace duskstudio
{
class AudioEngine;

// Native tape-machine panel, shown in the master strip's TAPE modal. Every
// control reads and writes the session's TapeParams atoms; MasterBus pushes
// them into the core per block. Touching any control arms tapeEnabled, so
// dialling tape in never needs a separate engage click.
class TapePanel final : public juce::Component, private dusk::Timer
{
public:
    TapePanel (MasterBusParams& paramsRef, AudioEngine& engineRef);
    ~TapePanel() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void arm();
    void syncControls();

    MasterBusParams& params;
    AudioEngine&     engine;

    juce::TextButton enableBtn;

    DuskComboBox machineCombo, speedCombo, typeCombo, pathCombo, eqStdCombo, calCombo;
    juce::Label  machineLabel, speedLabel, typeLabel, pathLabel, eqStdLabel, calLabel;

    juce::Slider inputKnob, biasKnob, hpfKnob, lpfKnob;
    juce::Slider wowKnob, flutterKnob, noiseKnob, outputKnob;
    juce::Label  inputLabel, biasLabel, hpfLabel, lpfLabel;
    juce::Label  wowLabel, flutterLabel, noiseLabel, outputLabel;

    juce::ToggleButton autoCalToggle  { "Auto cal"  };
    juce::ToggleButton autoCompToggle { "Auto comp" };

    // The meter reads these directly on its own refresh tick; the panel timer
    // refills them from the core's peak followers. Declared before vuMeter so
    // they outlive it.
    std::atomic<float> vuLeft  { 0.0f };
    std::atomic<float> vuRight { 0.0f };
    std::unique_ptr<AnalogVuMeter> vuMeter;

#if DUSKSTUDIO_HAS_DUSK_DSP
    std::unique_ptr<TapeReelComponent> supplyReel, takeupReel;
#endif
    float wowPhase = 0.0f;

    juce::Rectangle<int> deckArea;
    juce::Rectangle<int> comboArea;
    juce::Rectangle<int> knobArea;
};
} // namespace duskstudio
