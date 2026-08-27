#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <memory>
#include "../session/Session.h"
#include "AnalogVuMeter.h"
#include "CompMeterStrip.h"
#include "DuskComboBox.h"
#include "EmbeddedModal.h"
#include "SplitModuleButton.h"
#include "../foundation/MessageThread.h"

namespace duskstudio
{
class AudioEngine;
class MasterStripComponent final : public juce::Component,
                                     private dusk::Timer
{
public:
    explicit MasterStripComponent (MasterBusParams& paramsRef,
                                   class Session& sessionRef,
                                   AudioEngine& engineRef);
    ~MasterStripComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent& e) override;

    void setCompactVu (bool compact);
    void setCompactMode (bool compact);

private:
    bool compactVu = false;
    bool compactMode = false;
    // Gate split-button repaints on actual state changes so the 30 Hz timer
    // does not repaint static controls every tick.
    int lastCompactEqOn   = -1;
    int lastCompactCompOn = -1;
    int lastTapeOn        = -1;
    void timerCallback() override;

    MasterBusParams& params;
    class Session& session;
    AudioEngine& engine;

    juce::Label nameLabel;

    // Pultec-style Tube EQ. Inline matches the popup editor so on-strip
    // dialling and modal-open dialling see the same controls.
    // Split header shared with the channel and bus strips. Single Pultec
    // topology - no right-click mode picker.
    std::unique_ptr<SplitModuleButton> eqHeaderBtn;
    juce::Slider     eqLfBoost   { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Slider     eqLfAtten   { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Slider     eqHfBoost   { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Slider     eqHfAtten   { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Label      eqLfBoostLabel, eqLfAttenLabel;
    juce::Label      eqHfBoostLabel, eqHfAttenLabel;
    // No OUT trim knob: eqOutputGainDb atom stays at 0 dB and still feeds
    // the donor TubeEQ's outputGain. Master fader is the canonical
    // output-level control.
    // Stepped (dented) rotary knobs that snap to Pultec discrete
    // positions. Value is the index 0..N-1; textFromValueFunction
    // renders the Hz/kHz label. HF Bandwidth lives only in the popup
    // (set-once, doesn't earn strip space).
    juce::Slider     eqLfFreqKnob       { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Slider     eqHfBoostFreqKnob  { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Slider     eqHfAttenFreqKnob  { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Label      eqLfFreqLabel;
    juce::Label      eqHfBoostFreqLabel, eqHfAttenFreqLabel;

    // Same shell as channel + bus strips: split module button on top,
    // CompMeterStrip (triangle-handle threshold) left, knob grid right.
    // Fixed SSL-style glue topology - no mode picker.
    std::unique_ptr<SplitModuleButton> compHeaderBtn;
    std::unique_ptr<CompMeterStrip>   compMeter;
    juce::Slider     compRatio     { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Slider     compAttack    { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Slider     compRelease   { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Slider     compMakeup    { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Label      compRatLabel, compAtkLabel, compRelLabel, compMakLabel;

    // Tape uses the same split enable/editor grammar as EQ and COMP in both
    // layouts. The left status hitbox toggles tapeEnabled; the right label
    // opens the editor; right-clicking either side opens the section menu.
    SplitModuleButton tapeButton { "TAPE" };
    std::unique_ptr<SplitModuleButton> tapeHeaderBtn;
    void openTapeMachineModal();
    std::unique_ptr<class DimOverlay> tapeMachineDim;
    juce::Component::SafePointer<juce::Component> tapeMachineModal;

    juce::Slider faderSlider { juce::Slider::LinearVertical, juce::Slider::TextBoxBelow };
    juce::Label  faderValueLabel;
    juce::TextButton autoModeButton { "Off" };

    // Mute zeros the bus. monoStereoButton label flips STEREO (default)
    // / MONO (mono-sum on) at runtime; ctor sets initial text from
    // params.monoSum.
    juce::TextButton muteButton       { "M" };
    juce::TextButton monoStereoButton { "STEREO" };

    float displayedLiveFaderDb { 0.0f };

    void showAutoModeMenu();
    void setAutoMode (AutomationMode m);
    void captureFaderWritePoint (float denormDb);

    std::unique_ptr<AnalogVuMeter> vuMeter;

    juce::Rectangle<int> meterArea;
    // Slim GR bar between fader and L/R output bars - comp's
    // contribution to the final signal at a glance.
    juce::Rectangle<int> grMeterArea;
    juce::Rectangle<int> faderScaleArea;
    juce::Rectangle<int> eqArea;
    juce::Rectangle<int> compArea;
    juce::Rectangle<int> tapeArea;   // framed band behind the TAPE header (regular mode)
    SplitModuleButton eqCompactButton  { "EQ"   };
    SplitModuleButton compCompactButton { "COMP" };
    EmbeddedModal eqEditorModal;
    EmbeddedModal compEditorModal;
    void openEqEditorPopup();
    void openCompEditorPopup();
    // Unified section context menus (right-click on a header or compact pill):
    // whole-section reset (where a per-knob reset precedent exists) + open
    // editor. Master sections are fixed-topology, so no character items.
    void showEqSectionMenu();
    void showCompSectionMenu();
    void showTapeSectionMenu();
    void resetEqSection();
    void resetCompSection();
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
