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
// .cpp when DUSKSTUDIO_HAS_DUSK_DSP is set. The pointer parameter stays valid
// either way (passed as nullptr when the donor isn't available).
class TapeMachineAudioProcessor;

namespace duskstudio
{
class AudioEngine;
class MasterStripComponent final : public juce::Component,
                                     private juce::Timer,
                                     public juce::AudioProcessorListener
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

    // AudioProcessorListener — registered on the donor TapeMachine so
    // any parameter change in the editor auto-arms tapeEnabled (same
    // UX as EQ / COMP arm-on-touch). Fires from any thread; impl
    // defers to the message thread before touching UI.
    void audioProcessorParameterChanged (juce::AudioProcessor*, int, float) override;
    void audioProcessorChanged          (juce::AudioProcessor*,
                                           const juce::AudioProcessorListener::ChangeDetails&) override;

    // Shrinks the VU meter's vertical footprint when the tape TIMELINE
    // is expanded. Toggled by ConsoleView alongside setStripsCompactMode.
    void setCompactVu (bool compact);

    // Collapses the EQ + COMP sections into compact placeholder buttons
    // (same grammar as BusComponent + ChannelStripComponent) when the
    // tape TIMELINE consumes vertical room. Toggled by ConsoleView.
    void setCompactMode (bool compact);

private:
    bool compactVu = false;
    bool compactMode = false;
    // Gate the compact-pill repaint on actual state change (mirrors
    // ChannelStripComponent / BusComponent) so the 30 Hz timer does not
    // burn a repaint per tick.
    int lastCompactEqOn   = -1;
    int lastCompactCompOn = -1;
    void timerCallback() override;

    MasterBusParams& params;
    class Session& session;
    AudioEngine& engine;

    juce::Label nameLabel;

    // Pultec-style Tube EQ. Inline surface mirrors the popup editor so
    // a user dialling on the strip and a user opening the modal see the
    // same controls (LF + HF gain shelves, atten shelves, and discrete
    // Pultec-position freq pickers via DuskComboBox).
    //
    // Header pill uses the shared CompHeaderButton (green LED on left,
    // bold white label) so the master EQ header reads the same as the
    // channel + bus EQ headers. Left-click toggles eqEnabled; no
    // right-click pickFn (no mode picker — single Pultec topology).
    std::unique_ptr<CompHeaderButton> eqHeaderBtn;
    juce::Slider     eqLfBoost   { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Slider     eqLfAtten   { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Slider     eqHfBoost   { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Slider     eqHfAtten   { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Label      eqLfBoostLabel, eqLfAttenLabel;
    juce::Label      eqHfBoostLabel, eqHfAttenLabel;
    // OUT trim removed — atom (eqOutputGainDb) stays at default 0 dB
    // and continues to feed the donor TubeEQProcessor's outputGain, but
    // there's no UI knob: the master fader is the canonical output-
    // level control and the extra knob was wasting vertical room that
    // the fader needs more.
    // Freq pickers inline — stepped rotary knobs (dented) that snap to
    // the Pultec discrete positions. Range is the index 0..N-1; the
    // textFromValueFunction renders the corresponding Hz / kHz label
    // in the textbox below the knob. HF Bandwidth lives in the popup
    // editor only — set-once setup control, doesn't earn permanent
    // strip real estate.
    juce::Slider     eqLfFreqKnob       { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Slider     eqHfBoostFreqKnob  { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Slider     eqHfAttenFreqKnob  { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Label      eqLfFreqLabel;
    juce::Label      eqHfBoostFreqLabel, eqHfAttenFreqLabel;

    // Master bus compressor. Same shell as channel + bus strips:
    // CompHeaderButton on top, CompMeterStrip (with triangle-handle
    // threshold) on the LEFT, knob grid on the RIGHT. No mode picker —
    // the master comp is a fixed SSL-style glue topology.
    std::unique_ptr<CompHeaderButton> compHeaderBtn;
    std::unique_ptr<CompMeterStrip>   compMeter;
    juce::Slider     compRatio     { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Slider     compAttack    { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Slider     compRelease   { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Slider     compMakeup    { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Label      compRatLabel, compAtkLabel, compRelLabel, compMakLabel;

    // TAPE pill — plain juce::TextButton styled to match the compact
    // EQ / COMP pills (tinted background, centred bold label, lit
    // when tape is enabled). Left-click toggles tapeEnabled; right-
    // click opens the full TapeMachine editor via the strip's
    // mouseDown handler (addMouseListener route).
    juce::TextButton tapeButton { "TAPE" };
    // Regular (expanded) mode: TAPE header uses the shared
    // CompHeaderButton (green LED + bold label) to read with the same
    // grammar as the EQ + COMP headers. Left-click toggles enable;
    // right-click opens the full TapeMachine editor via pickFn. The
    // compact-mode tapeButton above stays for the compact strip pill.
    std::unique_ptr<CompHeaderButton> tapeHeaderBtn;
    ::TapeMachineAudioProcessor* tapeProcessorPtr = nullptr;
    void openTapeMachineModal();
    std::unique_ptr<class DimOverlay> tapeMachineDim;
    juce::Component::SafePointer<juce::Component> tapeMachineModal;

    juce::Slider faderSlider { juce::Slider::LinearVertical, juce::Slider::TextBoxBelow };
    // Standalone fader-value readout below the slider — mirrors the
    // channel + bus strip's fader-side grammar.
    juce::Label  faderValueLabel;
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
    // Compact-mode placeholder buttons. Hidden when compactMode=false;
    // visible (sections hidden) when true. Decorative — tooltip points
    // at TIMELINE toggle as the expand/collapse owner.
    juce::TextButton eqCompactButton  { "EQ"   };
    juce::TextButton compCompactButton { "COMP" };
    EmbeddedModal eqEditorModal;
    EmbeddedModal compEditorModal;
    void openEqEditorPopup();
    void openCompEditorPopup();
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
