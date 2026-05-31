#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <memory>
#include "../session/Session.h"
#include "AnalogVuMeter.h"
#include "CompMeterStrip.h"
#include "CompHeaderButton.h"
#include "DuskComboBox.h"
#include "EmbeddedModal.h"

// Forward decl unconditional; the definition is only #included from the
// .cpp when DUSKSTUDIO_HAS_DUSK_DSP is set. Pointer parameter stays
// valid either way (nullptr without donor).
class TapeMachineAudioProcessor;

namespace duskstudio
{
class AudioEngine;
class MasterStripComponent final : public juce::Component,
                                     private juce::Timer,
                                     public juce::AudioProcessorListener
{
public:
    // tapeProcessor is borrowed from AudioEngine; gear button spawns
    // its editor. Nullable when DUSKSTUDIO_HAS_DUSK_DSP=0.
    explicit MasterStripComponent (MasterBusParams& paramsRef,
                                   class Session& sessionRef,
                                   AudioEngine& engineRef,
                                   ::TapeMachineAudioProcessor* tapeProcessor = nullptr);
    ~MasterStripComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent& e) override;

    // Registered on the donor TapeMachine so any editor change auto-arms
    // tapeEnabled (matches EQ / COMP arm-on-touch). Fires from any
    // thread; defers to message before touching UI.
    void audioProcessorParameterChanged (juce::AudioProcessor*, int, float) override;
    void audioProcessorChanged          (juce::AudioProcessor*,
                                           const juce::AudioProcessorListener::ChangeDetails&) override;

    void setCompactVu (bool compact);
    void setCompactMode (bool compact);

private:
    bool compactVu = false;
    bool compactMode = false;
    // Gate the compact-pill repaint on actual state change so the
    // 30 Hz timer doesn't burn a repaint per tick.
    int lastCompactEqOn   = -1;
    int lastCompactCompOn = -1;
    void timerCallback() override;

    MasterBusParams& params;
    class Session& session;
    AudioEngine& engine;

    juce::Label nameLabel;

    // Pultec-style Tube EQ. Inline matches the popup editor so on-strip
    // dialling and modal-open dialling see the same controls.
    // Header pill = shared CompHeaderButton (green LED + bold label).
    // Single Pultec topology — no right-click mode picker.
    std::unique_ptr<CompHeaderButton> eqHeaderBtn;
    juce::Slider     eqLfBoost   { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Slider     eqLfAtten   { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Slider     eqHfBoost   { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Slider     eqHfAtten   { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Label      eqLfBoostLabel, eqLfAttenLabel;
    juce::Label      eqHfBoostLabel, eqHfAttenLabel;
    // OUT trim removed — eqOutputGainDb atom stays at 0 dB and still
    // feeds the donor TubeEQ's outputGain, but no UI knob. Master fader
    // is the canonical output-level control.
    // Stepped (dented) rotary knobs that snap to Pultec discrete
    // positions. Value is the index 0..N-1; textFromValueFunction
    // renders the Hz/kHz label. HF Bandwidth lives only in the popup
    // (set-once, doesn't earn strip space).
    juce::Slider     eqLfFreqKnob       { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Slider     eqHfBoostFreqKnob  { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Slider     eqHfAttenFreqKnob  { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Label      eqLfFreqLabel;
    juce::Label      eqHfBoostFreqLabel, eqHfAttenFreqLabel;

    // Same shell as channel + bus strips: CompHeaderButton top,
    // CompMeterStrip (triangle-handle threshold) left, knob grid right.
    // Fixed SSL-style glue topology — no mode picker.
    std::unique_ptr<CompHeaderButton> compHeaderBtn;
    std::unique_ptr<CompMeterStrip>   compMeter;
    juce::Slider     compRatio     { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Slider     compAttack    { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Slider     compRelease   { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Slider     compMakeup    { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Label      compRatLabel, compAtkLabel, compRelLabel, compMakLabel;

    // Compact-mode pill labelled "TAPE". Left-click toggles tapeEnabled
    // (so the timeline / compact view still has a way to bypass tape
    // without expanding the master strip), right-click opens the
    // TapeMachine editor modal — symmetric with the expanded-mode
    // CompHeaderButton's left/right behaviour. The lit state is driven
    // by the tapeEnabled atom (synced from the 30 Hz timer) so the
    // editor's auto-arm is still reflected here.
    struct TapePillButton final : public juce::TextButton
    {
        using juce::TextButton::TextButton;
        std::function<void(const juce::MouseEvent&)> onRightClick;
        void mouseDown (const juce::MouseEvent& e) override
        {
            if (e.mods.isPopupMenu() && onRightClick)
            {
                onRightClick (e);
                return;
            }
            juce::TextButton::mouseDown (e);
        }
    };
    TapePillButton tapeButton { "TAPE" };
    // Expanded-mode header: shared CompHeaderButton (matches EQ/COMP
    // grammar). Right-click opens TapeMachine editor via pickFn.
    std::unique_ptr<CompHeaderButton> tapeHeaderBtn;
    ::TapeMachineAudioProcessor* tapeProcessorPtr = nullptr;
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
    // Slim GR bar between fader and L/R output bars — comp's
    // contribution to the final signal at a glance.
    juce::Rectangle<int> grMeterArea;
    juce::Rectangle<int> faderScaleArea;
    juce::Rectangle<int> eqArea;
    juce::Rectangle<int> compArea;
    juce::TextButton eqCompactButton  { "EQ"   };
    juce::TextButton compCompactButton { "COMP" };
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
