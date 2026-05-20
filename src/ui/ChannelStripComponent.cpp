#include "ChannelStripComponent.h"
#include "DuskStudioLookAndFeel.h"
#include "ChannelEqEditor.h"
#include "ChannelCompEditor.h"
#include "DimOverlay.h"
#include "HardwareInsertEditor.h"
#include "PluginPickerHelpers.h"
#include "../engine/AudioEngine.h"
#include "../engine/PluginSlot.h"
#include "../engine/PluginManager.h"
#include "PlatformWindowing.h"
#include "../session/RegionEditActions.h"

namespace duskstudio
{
namespace
{
struct BandSpec
{
    const char* rowName;
    juce::Colour accent;          // 4K-palette band color
    float freqMin, freqMax, freqDefault;
    std::atomic<float>* (*gainPtr) (ChannelStripParams&);
    std::atomic<float>* (*freqPtr) (ChannelStripParams&);
    // qPtr is non-null for bell-only mid bands (HM, LM). Shelf bands (HF, LF)
    // leave it null and get a 2-knob row instead of 3.
    std::atomic<float>* (*qPtr)    (ChannelStripParams&) = nullptr;
};

// Top-to-bottom order: HF (treble) on top, LF (bass) on the bottom - matches
// the SSL/console convention and the user's spatial expectation. HF and LF
// are shelf-only here (no Q), HM and LM are bell with Q exposed - three
// knobs per row, mirroring the SSL E-EQ layout.
static const std::array<BandSpec, 4>& bandSpecs()
{
    static const std::array<BandSpec, 4> specs {{
        { "HF", juce::Colour (sslEqColors::kHfRed),    ChannelStripParams::kHfFreqMin, ChannelStripParams::kHfFreqMax, 8000.0f,
            [] (ChannelStripParams& s) -> std::atomic<float>* { return &s.hfGainDb; },
            [] (ChannelStripParams& s) -> std::atomic<float>* { return &s.hfFreq; } },
        { "HM", juce::Colour (sslEqColors::kHmGreen),  ChannelStripParams::kHmFreqMin, ChannelStripParams::kHmFreqMax, 2000.0f,
            [] (ChannelStripParams& s) -> std::atomic<float>* { return &s.hmGainDb; },
            [] (ChannelStripParams& s) -> std::atomic<float>* { return &s.hmFreq; },
            [] (ChannelStripParams& s) -> std::atomic<float>* { return &s.hmQ; } },
        { "LM", juce::Colour (sslEqColors::kLmBlue),   ChannelStripParams::kLmFreqMin, ChannelStripParams::kLmFreqMax, 600.0f,
            [] (ChannelStripParams& s) -> std::atomic<float>* { return &s.lmGainDb; },
            [] (ChannelStripParams& s) -> std::atomic<float>* { return &s.lmFreq; },
            [] (ChannelStripParams& s) -> std::atomic<float>* { return &s.lmQ; } },
        { "LF", juce::Colour (sslEqColors::kLfBlack),  ChannelStripParams::kLfFreqMin, ChannelStripParams::kLfFreqMax, 100.0f,
            [] (ChannelStripParams& s) -> std::atomic<float>* { return &s.lfGainDb; },
            [] (ChannelStripParams& s) -> std::atomic<float>* { return &s.lfFreq; } },
    }};
    return specs;
}

void styleCompactKnob (juce::Slider& k, juce::Colour fill)
{
    k.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    k.setColour (juce::Slider::rotarySliderFillColourId, fill);
    k.setColour (juce::Slider::rotarySliderOutlineColourId, juce::Colour (0xff404048));
}

void enableValueLabel (juce::Slider& k, const juce::String& suffix, int decimals,
                       int width = 56, int height = 14)
{
    // readOnly=false so single-click on the value text opens an inline
    // editor and the user can type a precise value. Background + outline
    // stay transparent so the value reads as a plain number against the
    // strip's tinted section bands rather than a boxed-in textbox.
    k.setTextBoxStyle (juce::Slider::TextBoxBelow, false, width, height);
    k.setTextValueSuffix (suffix);
    k.setNumDecimalPlacesToDisplay (decimals);
    k.setColour (juce::Slider::textBoxTextColourId,       juce::Colour (0xffd8d8d8));
    k.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    k.setColour (juce::Slider::textBoxOutlineColourId,    juce::Colours::transparentBlack);
    k.setColour (juce::Slider::textBoxHighlightColourId,  juce::Colour (0xff404048));
}

// Format a frequency in Hz, switching to "1.2k" notation above 1 kHz and
// dropping a trailing ".0" so integer kHz values stay short ("2k", "8k")
// instead of "2.0k" / "8.0k". Tight-strip-friendly.
inline juce::String formatFrequency (double hz)
{
    if (hz >= 1000.0)
    {
        const double khz = hz / 1000.0;
        if (std::abs (khz - std::round (khz)) < 0.05)
            return juce::String ((int) std::round (khz)) + "k";
        return juce::String (khz, 1) + "k";
    }
    return juce::String ((int) std::round (hz));
}

// Format an EQ band gain in dB, dropping ".0" on integer values so "0" / "-2"
// / "+12" fit in narrow textboxes instead of "0.0" / "-2.0" / "+12.0".
inline juce::String formatBandGain (double db)
{
    const double rounded = std::round (db);
    if (std::abs (db - rounded) < 0.05)
    {
        const int idb = (int) rounded;
        if (idb > 0) return "+" + juce::String (idb);
        return juce::String (idb);
    }
    return juce::String (db, 1);
}
} // namespace

// Style helper for the EQ / COMP compact-mode placeholder buttons. They sit
// in the slots the inline EQ + COMP sections occupy in normal mode and
// open the corresponding editor as a popup when clicked. Hidden by default.
static void styleCompactSectionButton (juce::TextButton& b, juce::Colour accent)
{
    b.setClickingTogglesState (false);
    b.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff222226));
    b.setColour (juce::TextButton::textColourOffId,  accent.brighter (0.3f));
    b.setColour (juce::TextButton::textColourOnId,   accent.brighter (0.3f));
    b.setVisible (false);
}

ChannelStripComponent::ChannelStripComponent (int idx, Track& t, Session& s,
                                                PluginSlot& slot, AudioEngine& eng)
    : trackIndex (idx), track (t), session (s), pluginSlot (slot), engine (eng)
{
    // Accessibility floor: screen readers announce the strip as
    // "Track N" instead of the default "Component". Description
    // surfaces in extended-info readers.
    setTitle ("Track " + juce::String (idx + 1));
    setDescription ("Channel strip for track " + juce::String (idx + 1));

    // Listen for engine-side MIDI device-list rebuilds (USB hot-plug
    // refresh) so the dropdown stays in sync with the live device list.
    engine.addChangeListener (this);

    nameLabel.setText (track.name, juce::dontSendNotification);
    nameLabel.setJustificationType (juce::Justification::centred);
    nameLabel.setColour (juce::Label::textColourId, juce::Colours::white);
    nameLabel.setFont (juce::Font (juce::FontOptions (13.0f, juce::Font::bold)));
    nameLabel.setEditable (false, true, false);  // single-click no, double-click YES, no submit-on-empty
    nameLabel.setTooltip ("Double-click to rename, right-click for colour");
    nameLabel.onTextChange = [this]
    {
        auto txt = nameLabel.getText().trim();
        if (txt.isEmpty()) txt = juce::String (trackIndex + 1);
        track.name = txt;
        nameLabel.setText (txt, juce::dontSendNotification);
    };
    addAndMakeVisible (nameLabel);

    // ── HPF row label (now the first row inside the EQ block, styled like
    // the band row labels: centred, 12 pt bold, accent colour) ──
    hpfLabel.setText ("HPF", juce::dontSendNotification);
    hpfLabel.setJustificationType (juce::Justification::centred);
    hpfLabel.setColour (juce::Label::textColourId, juce::Colour (sslEqColors::kHpfBlue).brighter (0.2f));
    hpfLabel.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
    addAndMakeVisible (hpfLabel);

    hpfKnob.setRange (ChannelStripParams::kHpfMinHz, ChannelStripParams::kHpfMaxHz, 1.0);
    hpfKnob.setSkewFactorFromMidPoint (80.0);
    hpfKnob.setDoubleClickReturnValue (true, ChannelStripParams::kHpfOffHz);
    hpfKnob.setTooltip ("HPF cutoff (turn fully down to bypass)");
    styleCompactKnob (hpfKnob, juce::Colour (sslEqColors::kHpfBlue));
    enableValueLabel (hpfKnob, "", 0);
    hpfKnob.textFromValueFunction = [] (double v) -> juce::String
    {
        if (v <= ChannelStripParams::kHpfOffHz + 0.5) return "OFF";
        return formatFrequency (v);
    };
    hpfKnob.setValue (track.strip.hpfFreq.load (std::memory_order_relaxed), juce::dontSendNotification);
    hpfKnob.onValueChange = [this] { onHpfKnobChanged(); };
    hpfKnob.addMouseListener (this, false);
    addAndMakeVisible (hpfKnob);

    // ── EQ region ──
    eqHeaderLabel.setText ("EQ", juce::dontSendNotification);
    eqHeaderLabel.setJustificationType (juce::Justification::centredLeft);
    eqHeaderLabel.setColour (juce::Label::textColourId, juce::Colour (0xffd0d0d0));
    eqHeaderLabel.setFont (juce::Font (juce::FontOptions (10.5f, juce::Font::bold)));
    addAndMakeVisible (eqHeaderLabel);

    eqTypeButton.setClickingTogglesState (true);
    eqTypeButton.setToggleState (track.strip.eqBlackMode.load (std::memory_order_relaxed),
                                  juce::dontSendNotification);
    eqTypeButton.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff5a3a20));   // brown
    eqTypeButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff202020));   // black
    eqTypeButton.setColour (juce::TextButton::textColourOffId,  juce::Colours::white);
    eqTypeButton.setColour (juce::TextButton::textColourOnId,   juce::Colours::white);
    eqTypeButton.setTooltip ("Brown (E-series) / Black (G-series)");
    eqTypeButton.onClick = [this]
    {
        const bool isBlack = eqTypeButton.getToggleState();
        track.strip.eqBlackMode.store (isBlack, std::memory_order_relaxed);
        refreshEqTypeButton();
    };
    refreshEqTypeButton();
    addAndMakeVisible (eqTypeButton);

    // ── COMP region ──
    // Header acts as both the mode picker AND the on/off indicator. Text
    // shows the active mode (OPTO/FET/VCA). Click opens a popup menu with
    // Off / Opto / FET / VCA. Background fills gold when the comp is
    // engaged, sits dark when bypassed. Touching any knob auto-engages.
    compModeButton.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff202024));
    compModeButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffd09060));
    compModeButton.setColour (juce::TextButton::textColourOffId,  juce::Colour (0xffd09060));
    compModeButton.setColour (juce::TextButton::textColourOnId,   juce::Colour (0xff121214));
    compModeButton.setTooltip ("Compressor mode (click to pick: Off / Opto / FET / VCA). "
                                "Lit gold when the comp is engaged.");
    compModeButton.onClick = [this] { showCompModeMenu(); };
    addAndMakeVisible (compModeButton);
    refreshCompModeButtonState();
    refreshCompKnobVisibility();

    auto setupCompKnob = [this] (juce::Slider& k, juce::Label& label, const juce::String& labelText,
                              double rangeMin, double rangeMax, double defaultVal,
                              double skewMid, const juce::String& tooltip,
                              std::atomic<float>& target,
                              const juce::String& valueSuffix, int decimals)
    {
        k.setRange (rangeMin, rangeMax, 0.01);
        if (skewMid > 0) k.setSkewFactorFromMidPoint (skewMid);
        k.setValue (target.load (std::memory_order_relaxed), juce::dontSendNotification);
        k.setDoubleClickReturnValue (true, defaultVal);
        k.setColour (juce::Slider::rotarySliderFillColourId, juce::Colour (fourKColors::kCompGold));
        k.setColour (juce::Slider::rotarySliderOutlineColourId, juce::Colour (0xff404048));
        k.setTooltip (tooltip);
        enableValueLabel (k, valueSuffix, decimals);
        k.onValueChange = [this, &k, &target]
        {
            target.store ((float) k.getValue(), std::memory_order_relaxed);
            armCompOnUserEdit();
        };

        label.setText (labelText, juce::dontSendNotification);
        label.setJustificationType (juce::Justification::centred);
        label.setColour (juce::Label::textColourId, juce::Colour (0xffb07050));
        label.setFont (juce::Font (juce::FontOptions (9.0f, juce::Font::bold)));
    };

    // ── Opto knobs (LA-2A: peak reduction + gain + LIMIT toggle) ──
    setupCompKnob (optoPeakRedKnob, optoPeakRedLabel, "PEAK",
                   0.0, 100.0, 30.0, 0.0, "Peak Reduction (%)",
                   track.strip.compOptoPeakRed, "", 0);
    setupCompKnob (optoGainKnob, optoGainLabel, "GAIN",
                   0.0, 100.0, 50.0, 0.0, "Output gain (% - 50 = unity)",
                   track.strip.compOptoGain, "", 0);
    // Comp threshold + makeup right-click hooks. Each per-mode "threshold-
    // ish" / "makeup-ish" knob routes to the same logical MIDI Learn
    // target so one binding tracks the user's mode switches.
    optoPeakRedKnob.addMouseListener (this, false);
    optoGainKnob   .addMouseListener (this, false);
    addAndMakeVisible (optoPeakRedKnob);  addAndMakeVisible (optoPeakRedLabel);
    addAndMakeVisible (optoGainKnob);     addAndMakeVisible (optoGainLabel);

    optoLimitButton.setClickingTogglesState (true);
    optoLimitButton.setToggleState (track.strip.compOptoLimit.load (std::memory_order_relaxed),
                                     juce::dontSendNotification);
    optoLimitButton.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff181820));
    optoLimitButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffd09060));
    optoLimitButton.setColour (juce::TextButton::textColourOffId,  juce::Colour (0xffd09060));
    optoLimitButton.setColour (juce::TextButton::textColourOnId,   juce::Colour (0xff121214));
    optoLimitButton.setTooltip ("Limit mode (more aggressive than Compress)");
    optoLimitButton.onClick = [this]
    {
        const bool nowOn = optoLimitButton.getToggleState();
        track.strip.compOptoLimit.store (nowOn, std::memory_order_relaxed);
        // Only arm the comp when the user is ENGAGING limit, not when
        // they're disengaging it. Toggling a sub-feature off shouldn't
        // imply "user wants the comp engaged".
        if (nowOn) armCompOnUserEdit();
    };
    addAndMakeVisible (optoLimitButton);

    // ── FET knobs (1176: in/out/atk/rel + ratio button row) ──
    setupCompKnob (fetInputKnob,   fetInputLabel,   "IN",
                   -20.0, 40.0, 0.0, 0.0, "Input drive (dB)",
                   track.strip.compFetInput, "", 1);
    setupCompKnob (fetOutputKnob,  fetOutputLabel,  "OUT",
                   -20.0, 20.0, 0.0, 0.0, "Output gain (dB)",
                   track.strip.compFetOutput, "", 1);
    setupCompKnob (fetAttackKnob,  fetAttackLabel,  "ATK",
                   0.02, 80.0, 0.2, 0.5, "Attack (ms)",
                   track.strip.compFetAttack, "", 2);
    setupCompKnob (fetReleaseKnob, fetReleaseLabel, "REL",
                   50.0, 1100.0, 400.0, 300.0, "Release (ms)",
                   track.strip.compFetRelease, "", 0);
    fetInputKnob .addMouseListener (this, false);
    fetOutputKnob.addMouseListener (this, false);
    addAndMakeVisible (fetInputKnob);   addAndMakeVisible (fetInputLabel);
    addAndMakeVisible (fetOutputKnob);  addAndMakeVisible (fetOutputLabel);
    addAndMakeVisible (fetAttackKnob);  addAndMakeVisible (fetAttackLabel);
    addAndMakeVisible (fetReleaseKnob); addAndMakeVisible (fetReleaseLabel);

    // FET ratio: 5-step knob (4:1 / 8:1 / 12:1 / 20:1 / All) - replaces the
    // earlier button row which couldn't fit the "All" label in narrow strips.
    fetRatioKnob.setRange (0.0, 4.0, 1.0);
    fetRatioKnob.setValue ((double) track.strip.compFetRatio.load (std::memory_order_relaxed),
                            juce::dontSendNotification);
    fetRatioKnob.setDoubleClickReturnValue (true, 0.0);
    fetRatioKnob.setColour (juce::Slider::rotarySliderFillColourId, juce::Colour (fourKColors::kCompGold));
    fetRatioKnob.setColour (juce::Slider::rotarySliderOutlineColourId, juce::Colour (0xff404048));
    fetRatioKnob.setTooltip ("FET Ratio (4 / 8 / 12 / 20 / All-buttons)");
    enableValueLabel (fetRatioKnob, "", 0);
    fetRatioKnob.textFromValueFunction = [] (double v) -> juce::String
    {
        const int idx = juce::jlimit (0, 4, (int) std::round (v));
        const char* names[] = { "4:1", "8:1", "12:1", "20:1", "All" };
        return names[idx];
    };
    fetRatioKnob.onValueChange = [this]
    {
        track.strip.compFetRatio.store (
            juce::jlimit (0, 4, (int) std::round (fetRatioKnob.getValue())),
            std::memory_order_relaxed);
        armCompOnUserEdit();
    };
    fetRatioKnob.updateText();
    addAndMakeVisible (fetRatioKnob);

    fetRatioLabel.setText ("RATIO", juce::dontSendNotification);
    fetRatioLabel.setJustificationType (juce::Justification::centred);
    fetRatioLabel.setColour (juce::Label::textColourId, juce::Colour (0xffb07050));
    fetRatioLabel.setFont (juce::Font (juce::FontOptions (9.0f, juce::Font::bold)));
    addAndMakeVisible (fetRatioLabel);

    // ── VCA knobs (ratio/attack/release/output - threshold lives on the
    //   meter-strip drag handle so the engineer pulls it directly against
    //   the input level, the way a real VCA panel works) ──
    setupCompKnob (vcaRatioKnob,   vcaRatioLabel,   "RAT",
                   1.0, 120.0, 4.0, 4.0, "Ratio (n:1)",
                   track.strip.compVcaRatio, ":1", 1);
    setupCompKnob (vcaAttackKnob,  vcaAttackLabel,  "ATK",
                   0.1, 50.0, 1.0, 5.0, "Attack (ms)",
                   track.strip.compVcaAttack, "", 1);
    setupCompKnob (vcaReleaseKnob, vcaReleaseLabel, "REL",
                   10.0, 5000.0, 100.0, 200.0, "Release (ms)",
                   track.strip.compVcaRelease, "", 0);
    setupCompKnob (vcaOutputKnob,  vcaOutputLabel,  "OUT",
                   -20.0, 20.0, 0.0, 0.0, "Output gain (dB)",
                   track.strip.compVcaOutput, "", 1);
    vcaOutputKnob.addMouseListener (this, false);
    addAndMakeVisible (vcaRatioKnob);    addAndMakeVisible (vcaRatioLabel);
    addAndMakeVisible (vcaAttackKnob);   addAndMakeVisible (vcaAttackLabel);
    addAndMakeVisible (vcaReleaseKnob);  addAndMakeVisible (vcaReleaseLabel);
    addAndMakeVisible (vcaOutputKnob);   addAndMakeVisible (vcaOutputLabel);

    // Vertical comp metering with threshold drag-handle (Mixbus-style).
    compMeter = std::make_unique<CompMeterStrip> (track);
    addAndMakeVisible (compMeter.get());

    startTimerHz (30);  // input + GR meter refresh on the main strip

    for (size_t i = 0; i < bandSpecs().size(); ++i)
    {
        const auto& spec = bandSpecs()[i];
        auto& row = eqRows[i];

        row.rowLabel.setText (spec.rowName, juce::dontSendNotification);
        row.rowLabel.setJustificationType (juce::Justification::centred);
        row.rowLabel.setColour (juce::Label::textColourId, spec.accent.brighter (0.2f));
        row.rowLabel.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
        addAndMakeVisible (row.rowLabel);

        row.gain = std::make_unique<juce::Slider>();
        styleCompactKnob (*row.gain, spec.accent);
        row.gain->setRange (ChannelStripParams::kBandGainMin, ChannelStripParams::kBandGainMax, 0.1);
        row.gain->setDoubleClickReturnValue (true, 0.0);
        row.gain->setTooltip (juce::String (spec.rowName) + " Gain (dB)");
        enableValueLabel (*row.gain, "", 1);
        row.gain->textFromValueFunction = [] (double v) { return formatBandGain (v); };
        row.gain->setValue (spec.gainPtr (track.strip)->load (std::memory_order_relaxed),
                            juce::dontSendNotification);
        row.gain->updateText();  // setValue skips updateText() when value didn't change (default 0 → loaded 0)
        {
            auto* atomicPtr = spec.gainPtr (track.strip);
            auto* knob = row.gain.get();
            knob->onValueChange = [knob, atomicPtr]
            {
                atomicPtr->store ((float) knob->getValue(), std::memory_order_relaxed);
            };
        }
        // Mouse listener so the strip's mouseDown handler can route a
        // right-click on this band's gain slider to MIDI Learn.
        row.gain->addMouseListener (this, false);
        addAndMakeVisible (row.gain.get());

        row.freq = std::make_unique<juce::Slider>();
        styleCompactKnob (*row.freq, spec.accent);
        row.freq->setRange (spec.freqMin, spec.freqMax, 1.0);
        row.freq->setSkewFactorFromMidPoint ((double) spec.freqDefault);
        row.freq->setDoubleClickReturnValue (true, spec.freqDefault);
        row.freq->setTooltip (juce::String (spec.rowName) + " Frequency (Hz)");
        enableValueLabel (*row.freq, "", 0);
        // textFromValueFunction must be set BEFORE setValue, otherwise the
        // initial text is rendered with the default formatter ("2000") and
        // doesn't get our "2.0k" notation until the user moves the knob.
        row.freq->textFromValueFunction = [] (double v) { return formatFrequency (v); };
        row.freq->setValue (spec.freqPtr (track.strip)->load (std::memory_order_relaxed),
                            juce::dontSendNotification);

        // Q knob (mid-bands only - bell-only HM/LM). Same size and styling
        // as gain and freq, with a "Q 0.7"-style value label below so the
        // user can read the bandwidth without hovering.
        if (spec.qPtr != nullptr)
        {
            row.q = std::make_unique<juce::Slider>();
            styleCompactKnob (*row.q, spec.accent);
            row.q->setRange (ChannelStripParams::kBandQMin, ChannelStripParams::kBandQMax, 0.01);
            row.q->setSkewFactorFromMidPoint (1.0);  // Q midpoint at 1.0
            row.q->setDoubleClickReturnValue (true, 0.7);
            row.q->setTooltip (juce::String (spec.rowName) + " Q (bandwidth)");
            enableValueLabel (*row.q, "", 1);
            row.q->setValue (spec.qPtr (track.strip)->load (std::memory_order_relaxed),
                              juce::dontSendNotification);
            {
                auto* atomicPtr = spec.qPtr (track.strip);
                auto* knob = row.q.get();
                knob->onValueChange = [knob, atomicPtr]
                {
                    atomicPtr->store ((float) knob->getValue(), std::memory_order_relaxed);
                };
            }
            addAndMakeVisible (row.q.get());

            row.qLabel.setText ("Q", juce::dontSendNotification);
            row.qLabel.setJustificationType (juce::Justification::centred);
            row.qLabel.setColour (juce::Label::textColourId, spec.accent.brighter (0.2f));
            row.qLabel.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
            addAndMakeVisible (row.qLabel);
        }
        {
            auto* atomicPtr = spec.freqPtr (track.strip);
            auto* knob = row.freq.get();
            knob->onValueChange = [knob, atomicPtr]
            {
                atomicPtr->store ((float) knob->getValue(), std::memory_order_relaxed);
            };
        }
        addAndMakeVisible (row.freq.get());
    }

    // ── Bus assigns - on/off toggles laid out vertically alongside the fader.
    // Each button takes the colour of its corresponding aux bus header (the
    // same HSV ramp Session::Session() uses to colour the four aux strips),
    // so the engineer reads "BUS 1 = teal, BUS 2 = blue, …" without consulting
    // the strip headers. ──
    for (int i = 0; i < ChannelStripParams::kNumBuses; ++i)
    {
        const auto busColour = session.bus (i).colour;
        lastBusColours[(size_t) i] = busColour.getARGB();
        auto btn = std::make_unique<juce::TextButton> (juce::String (i + 1));
        btn->setClickingTogglesState (true);
        btn->setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff202024));
        btn->setColour (juce::TextButton::buttonOnColourId, busColour);
        btn->setColour (juce::TextButton::textColourOffId,  busColour.brighter (0.15f));
        btn->setColour (juce::TextButton::textColourOnId,   juce::Colour (0xff121214));
        btn->setToggleState (track.strip.busAssign[(size_t) i].load (std::memory_order_relaxed),
                              juce::dontSendNotification);
        btn->setTooltip ("Route post-fader signal to BUS " + juce::String (i + 1));
        btn->onClick = [this, i]
        {
            track.strip.busAssign[(size_t) i].store (busButtons[(size_t) i]->getToggleState(),
                                                      std::memory_order_relaxed);
        };
        addAndMakeVisible (btn.get());
        busButtons[(size_t) i] = std::move (btn);
    }

    // ── Pan / Fader / M / S ──
    panLabel.setText ("PAN", juce::dontSendNotification);
    panLabel.setJustificationType (juce::Justification::centred);
    panLabel.setColour (juce::Label::textColourId, juce::Colour (0xffe07070));
    panLabel.setFont (juce::Font (juce::FontOptions (10.5f, juce::Font::bold)));
    addAndMakeVisible (panLabel);

    panKnob.setRange (-1.0, 1.0, 0.001);
    panKnob.setDoubleClickReturnValue (true, 0.0);
    panKnob.setColour (juce::Slider::rotarySliderFillColourId, juce::Colour (0xffc04040));  // red pan
    enableValueLabel (panKnob, "", 0);
    panKnob.textFromValueFunction = [] (double v) -> juce::String
    {
        if (std::abs (v) < 0.01) return "C";
        const int pct = (int) std::round (std::abs (v) * 100.0);
        return (v < 0 ? "L" : "R") + juce::String (pct);
    };
    panKnob.setValue (track.strip.pan.load (std::memory_order_relaxed), juce::dontSendNotification);
    panKnob.onValueChange = [this]
    {
        track.strip.pan.store ((float) panKnob.getValue(), std::memory_order_relaxed);
    };
    panKnob.onDragStart = [this]
    {
        track.strip.panTouched.store (true, std::memory_order_release);
    };
    panKnob.onDragEnd = [this]
    {
        track.strip.panTouched.store (false, std::memory_order_release);
    };
    addAndMakeVisible (panKnob);

    faderSlider.setRange (ChannelStripParams::kFaderMinDb, ChannelStripParams::kFaderMaxDb, 0.1);
    faderSlider.setSkewFactorFromMidPoint (-12.0);
    faderSlider.setValue (track.strip.faderDb.load (std::memory_order_relaxed), juce::dontSendNotification);
    faderSlider.setDoubleClickReturnValue (true, 0.0);
    // No "dB" suffix - strip is narrow enough that "0.0 dB" truncates.
    // The dB scale column to the right of the meter makes the unit obvious.
    faderSlider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 50, 16);
    faderSlider.setTooltip ("Channel fader (dB) - double-click to reset to 0 dB");
    faderSlider.onValueChange = [this]
    {
        const auto newDb = (float) faderSlider.getValue();
        track.strip.faderDb.store (newDb, std::memory_order_relaxed);

        // Fader-group propagation. While dragging, write the same
        // relative-dB delta to every peer in this strip's group.
        // peerActive[] was built at onDragStart so the inner walk is
        // a single pass; we don't re-query session.tracks every value
        // change. Peer ChannelStrips' own 30 Hz timers pull faderDb back
        // to their slider widgets so the visual sync is automatic.
        const int gid = track.strip.faderGroupId.load (std::memory_order_relaxed);
        if (gid != 0)
        {
            const float delta = newDb - faderDragAnchorDb;
            for (int t = 0; t < Session::kNumTracks; ++t)
            {
                if (t == trackIndex || ! peerActive[(size_t) t]) continue;
                const float target = juce::jlimit (
                    ChannelStripParams::kFaderMinDb, ChannelStripParams::kFaderMaxDb,
                    peerAnchorsDb[(size_t) t] + delta);
                session.track (t).strip.faderDb.store (target,
                                                          std::memory_order_relaxed);
            }
        }
    };
    // Touch-mode hooks: while the user has the fader grabbed, set the
    // strip's faderTouched flag so the audio engine routes the manual
    // setpoint instead of the lane (and the timerCallback captures into
    // the lane while touched). On release, the existing fader smoother's
    // 20 ms ramp blends from manual back to lane - a fast but smooth
    // glide. The configurable 100 ms / 500 ms / 1 s glide-back from the
    // spec is a refinement that lands later if 20 ms feels jarring in
    // practice.
    faderSlider.onDragStart = [this]
    {
        track.strip.faderTouched.store (true, std::memory_order_release);

        // Snapshot anchors for fader-group propagation. Capture this
        // strip's current dB AND every peer's so the relative offset
        // between members is preserved across the drag (no "smash to
        // unison" if the user grabs a fader at +3 in a group where
        // others sit at -10).
        faderDragAnchorDb = (float) faderSlider.getValue();
        peerActive.fill (false);
        const int gid = track.strip.faderGroupId.load (std::memory_order_relaxed);
        if (gid != 0)
        {
            for (int t = 0; t < Session::kNumTracks; ++t)
            {
                if (t == trackIndex) continue;
                if (session.track (t).strip.faderGroupId.load (std::memory_order_relaxed) == gid)
                {
                    peerActive[(size_t) t]    = true;
                    peerAnchorsDb[(size_t) t] = session.track (t).strip.faderDb.load (
                        std::memory_order_relaxed);
                }
            }
        }
    };
    faderSlider.onDragEnd = [this]
    {
        track.strip.faderTouched.store (false, std::memory_order_release);
        peerActive.fill (false);
    };
    faderSlider.addMouseListener (this, false);
    addAndMakeVisible (faderSlider);

    muteButton.setClickingTogglesState (true);
    muteButton.setColour (juce::TextButton::buttonOnColourId, juce::Colours::orangered);
    muteButton.setToggleState (track.strip.mute.load (std::memory_order_relaxed), juce::dontSendNotification);
    muteButton.setTooltip ("Mute (M) - silences this channel at the master");
    muteButton.onClick = [this]
    {
        const bool newState = muteButton.getToggleState();
        track.strip.mute.store (newState, std::memory_order_relaxed);

        // Discrete-param automation capture: a mute click in Write or
        // Touch mode (with transport playing) writes a transition point
        // into the lane at the current playhead. Discrete = no
        // interpolation, no tick-based capture - the click IS the only
        // event worth recording. In Touch mode the audio thread reads
        // the lane, so this click is heard immediately on the next
        // block; in Write the audio reads manual which we just stored.
        const int amode = track.automationMode.load (std::memory_order_relaxed);
        const bool capturing = engine.getTransport().isPlaying()
            && (amode == (int) AutomationMode::Write
                || amode == (int) AutomationMode::Touch);
        if (capturing)
            captureWritePoint (AutomationParam::Mute, newState ? 1.0f : 0.0f);
    };
    muteButton.addMouseListener (this, false);
    addAndMakeVisible (muteButton);

    soloButton.setClickingTogglesState (true);
    soloButton.setColour (juce::TextButton::buttonOnColourId, juce::Colours::yellow.darker (0.2f));
    soloButton.setToggleState (track.strip.solo.load (std::memory_order_relaxed), juce::dontSendNotification);
    soloButton.setTooltip ("Solo (S) - when any channel is soloed, only soloed channels are heard");
    soloButton.onClick = [this]
    {
        // Route through the session-level setter so the counter-backed
        // path stays current. Note that anyTrackSoloed() now scans
        // liveSolo so it works with automation-overridden solos too,
        // but the counter is still updated for consistency.
        session.setTrackSoloed (trackIndex, soloButton.getToggleState());

        // Discrete-param automation capture - same pattern as mute.
        const int amode = track.automationMode.load (std::memory_order_relaxed);
        const bool capturing = engine.getTransport().isPlaying()
            && (amode == (int) AutomationMode::Write
                || amode == (int) AutomationMode::Touch);
        if (capturing)
            captureWritePoint (AutomationParam::Solo, soloButton.getToggleState() ? 1.0f : 0.0f);
    };
    soloButton.addMouseListener (this, false);
    addAndMakeVisible (soloButton);

    phaseButton.setClickingTogglesState (true);
    phaseButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff70c0d0));
    phaseButton.setToggleState (track.strip.phaseInvert.load (std::memory_order_relaxed),
                                 juce::dontSendNotification);
    phaseButton.setTooltip ("Polarity invert - flips the input signal's polarity");
    phaseButton.onClick = [this]
    {
        track.strip.phaseInvert.store (phaseButton.getToggleState(), std::memory_order_relaxed);
    };
    addAndMakeVisible (phaseButton);

    autoModeButton.setTooltip ("Automation mode (click to pick: Off / Read / Write / Touch). "
                                "READ replays the recorded ride; WRITE captures "
                                "fader+pan moves; TOUCH replays until you grab a "
                                "control, then captures while held.");
    autoModeButton.onClick = [this] { showAutoModeMenu(); };
    addAndMakeVisible (autoModeButton);
    refreshAutoModeButton();
    displayedLiveFaderDb = track.strip.liveFaderDb.load (std::memory_order_relaxed);
    displayedLivePan     = track.strip.livePan    .load (std::memory_order_relaxed);

    // ── Peak input level readout ──
    inputPeakLabel.setJustificationType (juce::Justification::centred);
    inputPeakLabel.setColour (juce::Label::textColourId, juce::Colour (0xffd0d0d0));
    inputPeakLabel.setColour (juce::Label::backgroundColourId, juce::Colour (0xff0a0a0c));
    // No outline - the label sat next to the fader's value textbox and the
    // 1 px border drew on top of the textbox edge, looking like overlap.
    inputPeakLabel.setColour (juce::Label::outlineColourId,    juce::Colours::transparentBlack);
    inputPeakLabel.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                                                            10.0f, juce::Font::bold)));
    inputPeakLabel.setMinimumHorizontalScale (1.0f);   // never truncate to "..."
    inputPeakLabel.setText ("-inf", juce::dontSendNotification);
    addAndMakeVisible (inputPeakLabel);

    // GR readout - sits to the right of the input peak. Negative dB when
    // the comp is pulling the signal down. Uses gold-on-black to read as a
    // comp-section indicator distinct from the input level.
    grPeakLabel.setJustificationType (juce::Justification::centred);
    grPeakLabel.setColour (juce::Label::textColourId,        juce::Colour (0xffe0c050));
    grPeakLabel.setColour (juce::Label::backgroundColourId,  juce::Colour (0xff0a0a0c));
    grPeakLabel.setColour (juce::Label::outlineColourId,     juce::Colours::transparentBlack);
    grPeakLabel.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                                                          12.0f, juce::Font::bold)));
    grPeakLabel.setText ("0.0", juce::dontSendNotification);
    grPeakLabel.setTooltip ("Gain reduction in dB (negative = comp pulling down). "
                             "Goes inert when the comp is bypassed.");
    addAndMakeVisible (grPeakLabel);

    // "THR" label sits above CompMeterStrip's drag handle so the engineer
    // sees what the triangle pointer controls. Same gold tone as the COMP
    // section labels so it reads as part of the comp UI.
    threshMeterLabel.setText ("THR", juce::dontSendNotification);
    threshMeterLabel.setJustificationType (juce::Justification::centred);
    threshMeterLabel.setColour (juce::Label::textColourId, juce::Colour (0xffb07050));
    threshMeterLabel.setFont (juce::Font (juce::FontOptions (8.5f, juce::Font::bold)));
    addAndMakeVisible (threshMeterLabel);

    // ── Input monitor toggle (IN) ──
    monitorButton.setClickingTogglesState (true);
    monitorButton.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff202024));
    monitorButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (fourKColors::kPanCyan));
    monitorButton.setColour (juce::TextButton::textColourOffId,  juce::Colour (0xff708090));
    monitorButton.setColour (juce::TextButton::textColourOnId,   juce::Colour (0xff121214));
    monitorButton.setToggleState (track.inputMonitor.load (std::memory_order_relaxed),
                                   juce::dontSendNotification);
    monitorButton.setTooltip ("Input monitor - when on, live input is heard at the master. "
                              "When off, the track still records and meters but is silent.");
    monitorButton.onClick = [this]
    {
        track.inputMonitor.store (monitorButton.getToggleState(), std::memory_order_relaxed);
    };
    addAndMakeVisible (monitorButton);

    // ── Record arm ──
    armButton.setClickingTogglesState (true);
    armButton.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff202024));
    armButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffd03030));
    armButton.setColour (juce::TextButton::textColourOffId,  juce::Colour (0xffd06060));
    armButton.setColour (juce::TextButton::textColourOnId,   juce::Colours::white);
    armButton.setToggleState (track.recordArmed.load (std::memory_order_relaxed),
                               juce::dontSendNotification);
    armButton.setTooltip ("Record arm - press REC on the transport to capture this input");
    armButton.onClick = [this]
    {
        session.setTrackArmed (trackIndex, armButton.getToggleState());
    };
    armButton.addMouseListener (this, false);
    addAndMakeVisible (armButton);

    // PRINT toggle - when on, recording captures the post-EQ/post-comp signal
    // so effects are committed to the WAV. Off (default) = clean input on
    // disk so the engineer can re-EQ / re-comp at mix time.
    printButton.setClickingTogglesState (true);
    printButton.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff202024));
    printButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffd09060));
    printButton.setColour (juce::TextButton::textColourOffId,  juce::Colour (0xff8a7060));
    printButton.setColour (juce::TextButton::textColourOnId,   juce::Colour (0xff121214));
    printButton.setToggleState (track.printEffects.load (std::memory_order_relaxed),
                                 juce::dontSendNotification);
    printButton.setTooltip ("PRINT - when on, the recorded WAV captures the post-EQ/post-comp signal "
                             "(effects baked in). Off = clean input recorded; effects only at playback.");
    printButton.onClick = [this]
    {
        track.printEffects.store (printButton.getToggleState(), std::memory_order_relaxed);
    };
    addAndMakeVisible (printButton);

    // ── Input selector ──
    // -2 = follow track index; -1 = none; 0..N = explicit input. We populate
    // a small set of options here; the device may have fewer inputs at runtime
    // and we'll just route silence in that case.
    inputSelector.addItem ("In " + juce::String (trackIndex + 1), 1);   // ID 1 = follow (-2)
    inputSelector.addItem ("None",                                  2); // ID 2 = -1
    for (int i = 0; i < 16; ++i)
        inputSelector.addItem ("In " + juce::String (i + 1) + " (fixed)", 100 + i);  // ID 100+i = explicit

    const int currentSrc = track.inputSource.load (std::memory_order_relaxed);
    if      (currentSrc == -2) inputSelector.setSelectedId (1, juce::dontSendNotification);
    else if (currentSrc == -1) inputSelector.setSelectedId (2, juce::dontSendNotification);
    else                       inputSelector.setSelectedId (100 + currentSrc, juce::dontSendNotification);

    auto styleCombo = [] (juce::ComboBox& c)
    {
        c.setColour (juce::ComboBox::backgroundColourId, juce::Colour (0xff202024));
        c.setColour (juce::ComboBox::textColourId,       juce::Colour (0xffd0d0d0));
        c.setColour (juce::ComboBox::outlineColourId,    juce::Colour (0xff404048));
    };
    styleCombo (inputSelector);
    inputSelector.onChange = [this] { onInputSelectorChanged(); };
    addChildComponent (inputSelector);   // hidden inline; lives in the I/O popup

    // ── Mode selector (Mono / Stereo / MIDI) ──
    modeSelector.addItem ("Mono",   1);   // ID 1 = Mode::Mono
    modeSelector.addItem ("Stereo", 2);   // ID 2 = Mode::Stereo
    modeSelector.addItem ("MIDI",   3);   // ID 3 = Mode::Midi
    modeSelector.setSelectedId (track.mode.load (std::memory_order_relaxed) + 1,
                                  juce::dontSendNotification);
    modeSelector.setTooltip ("Track signal mode. Mono = 1 audio input. "
                              "Stereo = 2 audio inputs (L + R) recorded as a "
                              "stereo WAV. MIDI = capture from a MIDI port "
                              "and feed the strip's hosted plugin.");
    styleCombo (modeSelector);
    modeSelector.onChange = [this] { onTrackModeChanged(); };
    addChildComponent (modeSelector);   // hidden inline; lives in the I/O popup

    // ── Stereo R-channel input (mirrors the L selector's options) ──
    inputSelectorR.addItem ("In " + juce::String (trackIndex + 2), 1);   // ID 1 = follow (-2 -> L+1)
    inputSelectorR.addItem ("None", 2);                                   // ID 2 = -1
    for (int i = 0; i < 16; ++i)
        inputSelectorR.addItem ("In " + juce::String (i + 1) + " (fixed)", 100 + i);
    {
        const int rSrc = track.inputSourceR.load (std::memory_order_relaxed);
        if      (rSrc == -2) inputSelectorR.setSelectedId (1, juce::dontSendNotification);
        else if (rSrc == -1) inputSelectorR.setSelectedId (2, juce::dontSendNotification);
        else                 inputSelectorR.setSelectedId (100 + rSrc, juce::dontSendNotification);
    }
    styleCombo (inputSelectorR);
    inputSelectorR.onChange = [this]
    {
        const int id = inputSelectorR.getSelectedId();
        int v = -2;
        if      (id == 1)              v = -2;
        else if (id == 2)              v = -1;
        else if (id >= 100 && id < 200) v = id - 100;
        track.inputSourceR.store (v, std::memory_order_relaxed);
        refreshIoConfigButton();
    };
    addChildComponent (inputSelectorR);   // visibility toggled by refreshInputSelectorVisibility

    // ── MIDI input selector — populated from the engine's current MIDI
    // input bank. Re-populated whenever AudioEngine signals a refresh
    // (USB hot-plug etc.) via the ChangeListener wiring in the dtor /
    // changeListenerCallback below. Item ID 1 = "(none)" (maps to
    // track.midiInputIndex = -1). Subsequent IDs are 2 + deviceIndex.
    rebuildMidiInputDropdown();
    midiInputSelector.onChange = [this]
    {
        const int id = midiInputSelector.getSelectedId();
        const int idx = id <= 1 ? -1 : (id - 2);
        track.midiInputIndex.store (idx, std::memory_order_relaxed);
        refreshIoConfigButton();
        // Capture the stable identifier alongside the index so a later
        // session save records WHICH device this was, not just where it
        // happened to sit in the list. Re-querying inside the change
        // handler is fine - it's a message-thread-only operation that
        // runs at most once per user click.
        if (idx >= 0)
        {
            const auto& inputs = engine.getMidiInputDevices();
            track.midiInputIdentifier = (idx < inputs.size())
                                          ? inputs[idx].identifier
                                          : juce::String();
        }
        else
        {
            track.midiInputIdentifier = juce::String();
        }
    };
    styleCombo (midiInputSelector);
    addChildComponent (midiInputSelector);

    // ── MIDI channel filter (Omni / 1..16) ──
    // ID 1 = Omni (atom = 0), IDs 2..17 = channels 1..16 (atom = 1..16).
    midiChannelSelector.addItem ("Omni", 1);
    for (int ch = 1; ch <= 16; ++ch)
        midiChannelSelector.addItem ("Ch " + juce::String (ch), 1 + ch);
    {
        const int chSaved = track.midiChannel.load (std::memory_order_relaxed);
        midiChannelSelector.setSelectedId (chSaved == 0 ? 1 : (1 + chSaved),
                                            juce::dontSendNotification);
    }
    midiChannelSelector.onChange = [this]
    {
        const int id = midiChannelSelector.getSelectedId();
        track.midiChannel.store (id <= 1 ? 0 : (id - 1), std::memory_order_relaxed);
        refreshIoConfigButton();
    };
    styleCombo (midiChannelSelector);
    addChildComponent (midiChannelSelector);

    // ── MIDI OUTPUT selector. Routes this track's per-block MIDI buffer
    // to a hardware synth port in addition to (or instead of) the
    // loaded instrument plugin. Visible in MIDI mode only.
    rebuildMidiOutputDropdown();
    midiOutputSelector.onChange = [this]
    {
        const int id = midiOutputSelector.getSelectedId();
        const int idx = id <= 1 ? -1 : (id - 2);
        track.midiOutputIndex.store (idx, std::memory_order_relaxed);
        if (idx >= 0)
        {
            const auto& outs = engine.getMidiOutputDevices();
            track.midiOutputIdentifier = (idx < outs.size())
                                           ? outs[idx].identifier
                                           : juce::String();
            // Eagerly open the port so the first audio-thread emission
            // doesn't race a synchronous ALSA snd_seq_connect.
            engine.ensureMidiOutputOpen (idx);
        }
        else
        {
            track.midiOutputIdentifier = juce::String();
        }
    };
    styleCombo (midiOutputSelector);
    addChildComponent (midiOutputSelector);

    // MIDI activity LED. Tiny custom paint inside a juce::Component child;
    // intercepts no clicks. Blink state is owned by ChannelStripComponent —
    // timerCallback flips midiActivityLit from track.midiActivity (clear-
    // on-read) and triggers a repaint.
    midiActivityLed.setInterceptsMouseClicks (false, false);
    midiActivityLed.setOpaque (false);
    midiActivityLed.setPaintingIsUnclipped (true);
    addChildComponent (midiActivityLed);

    refreshInputSelectorVisibility();

    // I/O config button. Replaces the inline mode/input/midi rows; opens a
    // CallOutBox populated with those widgets when clicked. Saves ~60 px
    // vertical per strip without losing access to any setting.
    ioConfigButton.setColour (juce::TextButton::buttonColourId,  juce::Colour (0xff222226));
    ioConfigButton.setColour (juce::TextButton::textColourOffId, juce::Colour (0xffa0a8b8));
    ioConfigButton.setTooltip ("Click to configure track type, audio inputs, MIDI port and channel.");
    ioConfigButton.onClick = [this] { openIoConfigPopup(); };
    addAndMakeVisible (ioConfigButton);
    refreshIoConfigButton();

    // ── Aux send knobs (Mixing stage only) ──
    // Phase A: knobs + atomics only - audio routing through the aux buses
    // happens in Phase B. The four send levels feed AUX 1..4's plugin chain
    // (reverb / delay / etc.). Default -inf (no send).
    static const juce::Colour kAuxColours[ChannelStripParams::kNumAuxSends] = {
        juce::Colour (0xff5a8ad0),    // AUX 1 - blue
        juce::Colour (0xff9080c0),    // AUX 2 - violet
        juce::Colour (0xffe0c050),    // AUX 3 - amber
        juce::Colour (0xff60c060),    // AUX 4 - green
    };

    for (int i = 0; i < ChannelStripParams::kNumAuxSends; ++i)
    {
        auto& lbl = auxIndexLabels[(size_t) i];
        lbl.setText (juce::String ("AUX") + juce::String (i + 1), juce::dontSendNotification);
        lbl.setJustificationType (juce::Justification::centred);
        lbl.setColour (juce::Label::textColourId, kAuxColours[i].brighter (0.2f));
        lbl.setFont (juce::Font (juce::FontOptions (8.5f, juce::Font::bold)));
        addChildComponent (lbl);
    }

    auto formatAuxSend = [] (float dB)
    {
        if (dB <= ChannelStripParams::kAuxSendMinDb + 0.01f)
            return juce::String (juce::CharPointer_UTF8 ("\xe2\x88\x92"));   // "−" (U+2212)
        // Tight format: integer when |v| >= 10 ("-12"), else 1 decimal ("0.0").
        // Drops the "dB" suffix so the label fits the narrow column.
        if (std::abs (dB) >= 10.0f) return juce::String ((int) std::round (dB));
        return juce::String (dB, 1);
    };

    for (int i = 0; i < ChannelStripParams::kNumAuxSends; ++i)
    {
        auto knob = std::make_unique<juce::Slider> (
            juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox);
        knob->setRange (ChannelStripParams::kAuxSendMinDb, ChannelStripParams::kAuxSendMaxDb, 0.1);
        knob->setSkewFactorFromMidPoint (-12.0);   // detail near unity
        knob->setColour (juce::Slider::rotarySliderFillColourId,    kAuxColours[i]);
        knob->setColour (juce::Slider::rotarySliderOutlineColourId, juce::Colour (0xff404048));
        knob->setDoubleClickReturnValue (true, ChannelStripParams::kAuxSendOffDb);
        knob->setTooltip ("AUX " + juce::String (i + 1) + " send level. "
                          "Defaults post-fader; pre/post toggle coming in Phase D.");

        // Map "fully CCW" of the slider onto the kAuxSendOffDb sentinel so a
        // knob at minimum stops the audio path entirely (Phase B will read
        // this). Without the sentinel, even a "minimum-but-not-quite" send
        // would still tap audio, making muting via the knob impossible.
        const float initial = track.strip.auxSendDb[i].load (std::memory_order_relaxed);
        knob->setValue (initial <= ChannelStripParams::kAuxSendMinDb + 0.01f
                          ? ChannelStripParams::kAuxSendOffDb
                          : initial,
                          juce::dontSendNotification);

        auto* knobPtr = knob.get();
        const int idx = i;
        knob->onValueChange = [this, knobPtr, idx, formatAuxSend]
        {
            const float v = (float) knobPtr->getValue();
            const float stored = (v <= ChannelStripParams::kAuxSendMinDb + 0.01f)
                                    ? ChannelStripParams::kAuxSendOffDb : v;
            track.strip.auxSendDb[idx].store (stored, std::memory_order_relaxed);
            auxKnobLabels[(size_t) idx].setText (formatAuxSend (stored), juce::dontSendNotification);
        };
        knob->onDragStart = [this, idx]
        {
            track.strip.auxSendTouched[(size_t) idx].store (true, std::memory_order_release);
        };
        knob->onDragEnd = [this, idx]
        {
            track.strip.auxSendTouched[(size_t) idx].store (false, std::memory_order_release);
        };
        // Route right-clicks on the aux knob through the strip's mouseDown
        // so MIDI Learn picks up `e.eventComponent == auxKnobs[i].get()`.
        knob->addMouseListener (this, false);
        addChildComponent (knob.get());
        auxKnobs[(size_t) i] = std::move (knob);

        auxKnobLabels[(size_t) i].setJustificationType (juce::Justification::centred);
        auxKnobLabels[(size_t) i].setFont (juce::Font (juce::FontOptions (9.0f, juce::Font::bold)));
        auxKnobLabels[(size_t) i].setColour (juce::Label::textColourId, kAuxColours[i].brighter (0.2f));
        auxKnobLabels[(size_t) i].setText (formatAuxSend (initial), juce::dontSendNotification);
        addChildComponent (auxKnobLabels[(size_t) i]);
    }

    // Insert slot button. Empty state shows "Insert"; plugin loaded
    // shows the plugin name; hardware insert shows "HW: out N-M / in N-M"
    // (or "HW (unrouted)"). refreshPluginSlotButton drives the label.
    pluginSlotButton.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff222226));
    pluginSlotButton.setColour (juce::TextButton::textColourOffId,  juce::Colour (0xff9080c0));
    pluginSlotButton.setColour (juce::TextButton::textColourOnId,   juce::Colour (0xffd0c0e0));
    pluginSlotButton.setTooltip (juce::CharPointer_UTF8 (
        "Empty: click to pick a plugin (VST3 / LV2) or an External "
        "Hardware Insert. Loaded plugin: click to toggle the editor; "
        "right-click for Replace / Remove. Hardware insert: click to "
        "open the routing editor."));
    pluginSlotButton.onClick = [this]
    {
        if (! pluginSlot.isLoaded())
            openPluginPicker();
        else
            togglePluginEditor();
    };
    pluginSlotButton.onRightClick = [this] (const juce::MouseEvent&)
    {
        showPluginSlotMenu();
    };
    addAndMakeVisible (pluginSlotButton);

    // Compact-mode placeholder buttons. Hidden by default; setCompactMode(true)
    // makes them visible and hides the full inline EQ + COMP controls.
    styleCompactSectionButton (eqCompactButton,   juce::Colour (fourKColors::kLfGreen));
    styleCompactSectionButton (compCompactButton, juce::Colour (fourKColors::kCompGold));
    eqCompactButton  .setButtonText ("EQ");
    compCompactButton.setButtonText ("COMP");
    eqCompactButton  .setTooltip ("Click to open the EQ editor (click again to close). "
                                    "Button is illuminated when the section is enabled.");
    compCompactButton.setTooltip ("Click to open the compressor editor (click again to close). "
                                    "Button is illuminated when the section is enabled.");
    eqCompactButton  .onClick = [this] { openEqEditorPopup(); };
    compCompactButton.onClick = [this] { openCompEditorPopup(); };
    addChildComponent (eqCompactButton);    // hidden until compact mode flips on
    addChildComponent (compCompactButton);

    // Accessibility floor (deeper a11y pass — names every user-driven
    // control on the strip so screen readers announce intent, not just
    // role+index). Track-relative names because each strip's controls
    // are otherwise indistinguishable to the AT.
    const auto tn = juce::String (trackIndex + 1);
    faderSlider .setTitle ("Track " + tn + " fader");
    panKnob     .setTitle ("Track " + tn + " pan");
    muteButton  .setTitle ("Track " + tn + " mute");
    soloButton  .setTitle ("Track " + tn + " solo");
    armButton   .setTitle ("Track " + tn + " record arm");
    monitorButton.setTitle ("Track " + tn + " input monitor");
    printButton .setTitle ("Track " + tn + " print effects on record");
    hpfKnob     .setTitle ("Track " + tn + " high-pass filter frequency");
    eqTypeButton.setTitle ("Track " + tn + " EQ type");
    pluginSlotButton.setTitle ("Track " + tn + " insert slot");
    for (size_t i = 0; i < auxKnobs.size(); ++i)
        if (auxKnobs[i] != nullptr)
            auxKnobs[i]->setTitle ("Track " + tn + " aux " + juce::String ((int) i + 1) + " send");
}

ChannelStripComponent::~ChannelStripComponent()
{
    engine.removeChangeListener (this);

    // If a popup editor is still on screen when the strip dies (e.g. the
    // window is closing), force-delete it so its content (which references
    // `track`) doesn't outlive us. Same SafePointer pattern as the audio
    // settings dialog in MainComponent.
    // CallOutBoxes auto-clean themselves when their content is deleted, but
    // we dismiss any in-flight box so it disappears immediately rather than
    // lingering through the next message-loop tick.
    if (auto* eq   = activeEqBox.getComponent())   eq->dismiss();
    if (auto* cmp  = activeCompBox.getComponent()) cmp->dismiss();
    // Drop the cached editor BEFORE the strip's PluginSlot destructs,
    // since the editor's destructor calls editorBeingDeleted on its
    // owning AudioProcessor. dropPluginEditor() also closes the modal.
    dropPluginEditor();
}

void ChannelStripComponent::changeListenerCallback (juce::ChangeBroadcaster*)
{
    // Engine signalled a MIDI device-list refresh (post-rebuild). The
    // device order may have changed; re-resolve our track's index from
    // its saved identifier and rebuild both dropdowns (input + output).
    rebuildMidiInputDropdown();
    rebuildMidiOutputDropdown();
}

void ChannelStripComponent::rebuildMidiInputDropdown()
{
    midiInputSelector.clear (juce::dontSendNotification);
    midiInputSelector.addItem ("(none)", 1);
    const auto& inputs = engine.getMidiInputDevices();
    for (int i = 0; i < inputs.size(); ++i)
        midiInputSelector.addItem (inputs[i].name, 2 + i);

    // Prefer identifier-based selection when we have one - it survives
    // device-list reordering. Fall back to the raw index for older
    // sessions / never-touched tracks.
    int idx = -1;
    if (track.midiInputIdentifier.isNotEmpty())
    {
        for (int i = 0; i < inputs.size(); ++i)
        {
            if (inputs[i].identifier == track.midiInputIdentifier)
            {
                idx = i; break;
            }
        }
    }
    else
    {
        idx = track.midiInputIndex.load (std::memory_order_relaxed);
        if (idx >= inputs.size()) idx = -1;
    }
    track.midiInputIndex.store (idx, std::memory_order_relaxed);
    midiInputSelector.setSelectedId (idx >= 0 ? (2 + idx) : 1,
                                      juce::dontSendNotification);
}

void ChannelStripComponent::rebuildMidiOutputDropdown()
{
    // Mirror of rebuildMidiInputDropdown for the output side. ID 1 =
    // "(none)" (= idx -1, no external output); 2..N = device index.
    midiOutputSelector.clear (juce::dontSendNotification);
    midiOutputSelector.addItem ("(none)", 1);
    const auto& outs = engine.getMidiOutputDevices();
    for (int i = 0; i < outs.size(); ++i)
        midiOutputSelector.addItem (outs[i].name, 2 + i);

    int idx = -1;
    if (track.midiOutputIdentifier.isNotEmpty())
    {
        for (int i = 0; i < outs.size(); ++i)
        {
            if (outs[i].identifier == track.midiOutputIdentifier)
            {
                idx = i; break;
            }
        }
    }
    else
    {
        idx = track.midiOutputIndex.load (std::memory_order_relaxed);
        if (idx >= outs.size()) idx = -1;
    }
    track.midiOutputIndex.store (idx, std::memory_order_relaxed);
    midiOutputSelector.setSelectedId (idx >= 0 ? (2 + idx) : 1,
                                        juce::dontSendNotification);
}

void ChannelStripComponent::setEqSectionVisible (bool visible)
{
    hpfKnob.setVisible (visible);
    hpfLabel.setVisible (visible);
    eqHeaderLabel.setVisible (visible);
    eqTypeButton.setVisible (visible);
    for (auto& row : eqRows)
    {
        if (row.gain) row.gain->setVisible (visible);
        if (row.freq) row.freq->setVisible (visible);
        if (row.q)    row.q   ->setVisible (visible);
        row.labelLeft .setVisible (visible);
        row.labelRight.setVisible (visible);
        row.rowLabel  .setVisible (visible);
        row.qLabel    .setVisible (visible);
    }
}

void ChannelStripComponent::setCompSectionVisible (bool visible)
{
    compModeButton.setVisible (visible);
    optoPeakRedKnob .setVisible (visible);  optoPeakRedLabel.setVisible (visible);
    optoGainKnob    .setVisible (visible);  optoGainLabel   .setVisible (visible);
    optoLimitButton .setVisible (visible);
    fetInputKnob    .setVisible (visible);  fetInputLabel   .setVisible (visible);
    fetOutputKnob   .setVisible (visible);  fetOutputLabel  .setVisible (visible);
    fetAttackKnob   .setVisible (visible);  fetAttackLabel  .setVisible (visible);
    fetReleaseKnob  .setVisible (visible);  fetReleaseLabel .setVisible (visible);
    fetRatioKnob    .setVisible (visible);  fetRatioLabel   .setVisible (visible);
    vcaRatioKnob    .setVisible (visible);  vcaRatioLabel   .setVisible (visible);
    vcaAttackKnob   .setVisible (visible);  vcaAttackLabel  .setVisible (visible);
    vcaReleaseKnob  .setVisible (visible);  vcaReleaseLabel .setVisible (visible);
    vcaOutputKnob   .setVisible (visible);  vcaOutputLabel  .setVisible (visible);
    // compMeter now lives INSIDE the COMP section (next to the per-mode
    // knobs) so it follows section visibility. In compact mode the popup
    // owns its own threshold drag so we don't lose access.
    if (compMeter != nullptr) compMeter->setVisible (visible);
    threshMeterLabel.setVisible (false);   // legacy "THR" header, no longer used
    grPeakLabel    .setVisible (false);    // numeric GR readout retired - the
                                            // meter bar already shows GR clearly

    // Re-apply the per-mode filter so only the active mode's knobs are
    // shown. Without this, flipping out of SUMMARY (or any path that
    // re-shows the section) leaves all 13 per-mode knobs visible at the
    // same physical positions and they overlap.
    if (visible)
    {
        refreshCompModeButtonState();
        refreshCompKnobVisibility();
    }
}

void ChannelStripComponent::setCompactMode (bool compact)
{
    if (compactMode == compact) return;
    compactMode = compact;

    setEqSectionVisible   (! compact);
    setCompSectionVisible (! compact);
    eqCompactButton  .setVisible (compact);
    compCompactButton.setVisible (compact);

    resized();
    repaint();
}

void ChannelStripComponent::setMixingMode (bool mixing)
{
    if (mixingMode == mixing) return;
    mixingMode = mixing;

    // The mode/input/IN/ARM/PRINT block is hidden in Mixing - those controls
    // are tracking-stage only. The aux send knobs take that real estate.
    modeSelector       .setVisible (! mixing);
    inputSelector      .setVisible (! mixing);
    inputSelectorR     .setVisible (! mixing);
    midiInputSelector  .setVisible (! mixing);
    midiChannelSelector.setVisible (! mixing);
    midiActivityLed    .setVisible (! mixing);
    midiOutputSelector .setVisible (! mixing);
    monitorButton      .setVisible (! mixing);
    armButton          .setVisible (! mixing);
    printButton        .setVisible (! mixing);

    for (auto& l : auxIndexLabels)  l.setVisible (mixing);
    for (auto& k : auxKnobs)        if (k != nullptr) k->setVisible (mixing);
    for (auto& l : auxKnobLabels)   l.setVisible (mixing);

    if (mixing)
        refreshInputSelectorVisibility();   // not needed since selectors are hidden,
                                            // but re-syncs when we leave Mixing later

    resized();
    repaint();
}

// Click semantics for the compact-mode placeholder buttons:
//   • Click while no popup is open → open this section's popup
//   • Click while THIS popup is already open → close it (toggle off)
//   • Click while the OTHER popup is open → close the other, open this one
// The mutual-exclusion path lets the user move quickly between EQ and COMP
// without first having to dismiss the open dialog manually.
void ChannelStripComponent::openPluginPicker()
{
    // MIDI tracks need an instrument plugin (synth / sampler) - the strip
    // routes per-track MIDI events into the slot. Mono / Stereo tracks
    // route audio through the slot, which only makes sense for effect
    // plugins; instrument plugins ignore the audio input entirely and
    // (in the case of some VSTGUI-based instruments) fail to render
    // their UI when loaded as an audio insert.
    const auto kind = (track.mode.load (std::memory_order_relaxed) == (int) Track::Mode::Midi)
                        ? pluginpicker::PluginKind::Instruments
                        : pluginpicker::PluginKind::Effects;

    juce::Component::SafePointer<ChannelStripComponent> safe (this);
    auto openHwEditor = [safe]
    {
        if (auto* self = safe.getComponent())
            self->openHardwareInsertEditor();
    };

    pluginpicker::openPickerMenu (pluginSlot,
                                    pluginSlotButton,
                                    activePluginChooser,
                                    [safe]
                                    {
                                        auto* self = safe.getComponent();
                                        if (self == nullptr) return;
                                        // Picking a plugin flips the strip back to Plugin mode
                                        // (overriding any prior Hardware selection on this slot).
                                        self->engine.getChannelStrip (self->trackIndex)
                                                  .insertMode.store (ChannelStrip::kInsertPlugin,
                                                                       std::memory_order_release);

                                        // Close the modal synchronously so the cached editor
                                        // (still bound to the just-replaced processor) gets
                                        // detached from the parent component before the next
                                        // paint cycle. The cached editor unique_ptr is dropped
                                        // by refreshPluginSlotButton when it sees the slot's
                                        // processor pointer change.
                                        //
                                        // Defer that drop + the slot-button refresh by one
                                        // message-loop tick so it doesn't run inside the
                                        // picker's own dismissal stack. Plugin destructors on
                                        // Linux frequently do X11 / OpenGL cleanup that
                                        // confuses Mutter when fired during another widget's
                                        // teardown - this single-tick gap is what stops the
                                        // Replace plugin... action from crashing the DAW (or
                                        // the entire compositor).
                                        //
                                        // Auto-open the new plugin's editor so a pick / replace
                                        // immediately surfaces the GUI. Sequence each step on its
                                        // own message-loop tick: close (now) → refresh (next
                                        // tick) → open (the tick after that). The two-tick gap
                                        // separates the old editor's destruction from the new
                                        // editor's construction so plugins with fragile lifecycle
                                        // teardown (Diva, MininnDrum) don't see destroy+create
                                        // collapse into a single frame.
                                        self->closePluginEditor();
                                        juce::Component::SafePointer<ChannelStripComponent> deferred (self);
                                        juce::MessageManager::callAsync ([deferred]
                                        {
                                            auto* s = deferred.getComponent();
                                            if (s == nullptr) return;
                                            s->refreshPluginSlotButton();
                                            juce::Component::SafePointer<ChannelStripComponent> openLater (s);
                                            juce::MessageManager::callAsync ([openLater]
                                            {
                                                auto* ss = openLater.getComponent();
                                                if (ss == nullptr) return;
                                                if (ss->pluginSlot.isLoaded() && ! ss->isPluginEditorOpen())
                                                    ss->openPluginEditor();
                                            });
                                        });
                                    },
                                    kind,
                                    juce::Point<int> { -1, -1 },
                                    std::move (openHwEditor));
}

void ChannelStripComponent::openHardwareInsertEditor()
{
    // Flip the strip to Hardware mode FIRST so the audio thread's
    // crossfade gate (Phase 3) starts ramping in even before the user
    // touches a control. The editor itself mutates `track.hardwareInsert`
    // via AtomicSnapshot::publish + scalar atomic stores.
    engine.getChannelStrip (trackIndex)
        .insertMode.store (ChannelStrip::kInsertHardware,
                            std::memory_order_release);
    refreshPluginSlotButton();

    juce::Component::SafePointer<ChannelStripComponent> safe (this);
    auto editor = std::make_unique<HardwareInsertEditor> (
        track.hardwareInsert,
        engine.getDeviceManager(),
        [safe]
        {
            if (auto* self = safe.getComponent())
            {
                self->hardwareInsertModal.close();
                self->refreshPluginSlotButton();
            }
        });

    auto* parent = findParentComponentOfClass<juce::Component>();
    if (parent == nullptr) parent = this;
    hardwareInsertModal.show (*parent, std::move (editor));
}

void ChannelStripComponent::unloadPluginSlot()
{
    // Close any open editor BEFORE the plugin is destroyed - the editor
    // holds a reference back to the AudioProcessor and would crash on
    // teardown otherwise.
    closePluginEditor();
    pluginSlot.unload();
    refreshPluginSlotButton();
}

void ChannelStripComponent::showPluginSlotMenu()
{
    juce::PopupMenu menu;
    if (pluginSlot.isLoaded())
    {
        // Editor toggle headline so right-click ALSO becomes a way to open
        // the plugin GUI (some users find right-click more discoverable).
        const bool editorOpen = isPluginEditorOpen();
        menu.addItem (2001, editorOpen ? "Close editor" : "Open editor");
        menu.addSeparator();
        menu.addItem (2002, "Replace plugin...");
        menu.addItem (2003, "Remove plugin");
        if (pluginSlot.wasCrashed())
            menu.addItem (2004, "Re-enable plugin (crashed)");
        else if (pluginSlot.wasAutoBypassed())
            menu.addItem (2004, "Re-enable plugin (auto-bypassed)");
        // MIDI Learn for the last parameter the user touched via the
        // plugin's own UI. Disabled when no parameter has been touched
        // since the slot loaded (no last-touched stamp to bind to).
        menu.addSeparator();
        const int lastParam = pluginSlot.getLastTouchedParamIndex();
        menu.addItem (2005,
                       "MIDI Learn last-touched parameter",
                       lastParam >= 0);
    }
    else
    {
        menu.addItem (2010, "Pick plugin...");
    }

    juce::Component::SafePointer<ChannelStripComponent> safe (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&pluginSlotButton),
        [safe] (int result)
        {
            auto* self = safe.getComponent();
            if (self == nullptr || result <= 0) return;
            switch (result)
            {
                case 2001: self->togglePluginEditor();             break;
                case 2002: self->openPluginPicker();               break;
                case 2003: self->unloadPluginSlot();               break;
                case 2004: self->pluginSlot.clearAutoBypass();     break;
                case 2005:
                    // Fire the MIDI Learn workflow targeting THIS
                    // track's plugin slot. The pending state stores
                    // only the track; the resolve site (TransportBar's
                    // timer) reads the slot's last-touched param at
                    // the moment a MIDI source arrives so the param
                    // index reflects what the user moved between
                    // clicking Learn and triggering the controller.
                    midilearn::showLearnMenu (
                        self->pluginSlotButton, self->session,
                        MidiBindingTarget::TrackPluginParam,
                        self->trackIndex);
                    break;
                case 2010: self->openPluginPicker();               break;
                default: break;
            }
        });
}

void ChannelStripComponent::refreshPluginSlotButton()
{
    const int mode = engine.getChannelStrip (trackIndex)
                       .insertMode.load (std::memory_order_relaxed);
    juce::String label;
    if (mode == ChannelStrip::kInsertHardware)
    {
        // Loaded hardware: show the routed channel pair so the user knows
        // at a glance where the strip is patched. Each side (out / in) is
        // formatted independently - mono when only L is assigned, stereo
        // when both are - and the label falls back to "HW (unrouted)" when
        // neither side has audio routing.
        const auto routing = track.hardwareInsert.routing.current();
        auto formatPair = [] (int l, int r) -> juce::String
        {
            if (l < 0 && r < 0) return {};
            if (r < 0)          return juce::String (l + 1);                       // mono
            if (l < 0)          return juce::String (r + 1);                       // mono on R only
            if (l == r)         return juce::String (l + 1);                       // same channel both
            return juce::String (l + 1) + "-" + juce::String (r + 1);              // stereo pair
        };
        const auto out = formatPair (routing.outputChL, routing.outputChR);
        const auto in  = formatPair (routing.inputChL,  routing.inputChR);
        if (out.isEmpty() && in.isEmpty())
            label = "HW (unrouted)";
        else
            label = juce::String ("HW: out ")
                  + (out.isNotEmpty() ? out : juce::String ("-"))
                  + " / in "
                  + (in .isNotEmpty() ? in  : juce::String ("-"));
    }
    else
    {
        const auto name = pluginSlot.getLoadedName();
        if (name.isNotEmpty())
            label = juce::String (juce::CharPointer_UTF8 ("\xe2\x96\xbe ")) + name;
        else if (pluginSlot.isOffline())
        {
            // Slot held a plugin in the saved session but couldn't be
            // re-instantiated (plugin missing / moved / format unsupported).
            // Show the saved name with a warning marker so the user knows
            // the slot's saved state is preserved on disk and that
            // reinstalling the plugin will restore it.
            const auto offline = pluginSlot.getOfflineName();
            label = juce::String (juce::CharPointer_UTF8 ("\xe2\x9a\xa0 "))
                  + (offline.isNotEmpty() ? offline : juce::String ("offline"))
                  + " (offline)";
        }
        else
            label = "Insert";
    }

    if (label == lastSlotName) return;
    lastSlotName = label;
    pluginSlotButton.setButtonText (label);

    // Re-fetch the plugin name for the dropPluginEditor / cached-editor
    // bookkeeping below (kept on the same atomic field the old code used).
    const auto name = pluginSlot.getLoadedName();

    // If the plugin was unloaded out from under an open editor (e.g. via
    // the right-click menu's Remove), the cached editor references a
    // now-being-destructed AudioProcessor. Drop it before that processor
    // disappears so we don't leave a dangling pointer in pluginEditor.
    if (name.isEmpty())
        dropPluginEditor();
    else if (pluginEditor != nullptr
             && pluginEditorOwner != pluginSlot.getInstance())
    {
        // Plugin was Replaced - the cached editor belongs to the prior
        // instance. Drop it so the next Open Editor builds a fresh one
        // for the new instance.
        dropPluginEditor();
    }
}

void ChannelStripComponent::togglePluginEditor()
{
    // Toggle: if the editor is up, close it; otherwise open. Same shape as
    // the EQ / COMP popup buttons in compact mode.
    if (isPluginEditorOpen())
        closePluginEditor();
    else
        openPluginEditor();
}

// Top-level window that wraps a plugin's editor with its own native
// peer (X11 Window / Wayland surface) so the plugin can render via its
// own renderer (GL / Cairo / VSTGUI). Owned by ChannelStripComponent;
// closeButtonPressed forwards back so the strip can drop our unique_ptr
// when the user clicks X.
class ChannelStripComponent::PluginEditorWindow final : public juce::DocumentWindow,
                                                          private juce::ComponentListener
{
public:
    // Body is taken as a juce::Component& rather than the more specific
    // AudioProcessorEditor& so this same wrapper can host either:
    //  - in-process: the plugin's own AudioProcessorEditor (which IS a
    //    Component);
    //  - OOP (Linux+DUSKSTUDIO_HAS_OOP_PLUGINS): a juce::XEmbedComponent
    //    wrapping the X11 Window the dusk-studio-plugin-host child reported.
    // The implementation only relies on Component-level API
    // (getWidth/getHeight/setSize/setContentNonOwned).
    PluginEditorWindow (const juce::String& title,
                        juce::Component& editor,
                        std::function<void()> onCloseButton,
                        AudioEngine* engineForTransport = nullptr)
        : juce::DocumentWindow (([&]
                                  {
                                      // Plugin editors are X11 children
                                      // (VST3 X11EmbedWindowID, LV2
                                      // LV2_UI__X11UI, JUCE-plugin
                                      // X11-windowed renderer). XEmbed
                                      // can't reparent into a wl_surface,
                                      // so this wrapper must be an X11
                                      // toplevel even when the rest of
                                      // Dusk Studio is on Wayland. The flag
                                      // is consumed by the very next
                                      // createNewPeer call (triggered
                                      // by addToDesktop=true below).
                                      duskstudio::platform::preferX11ForNextNativeWindow();
                                      return title;
                                  })(),
                                  juce::Colour (0xff202024),
                                  juce::DocumentWindow::closeButton,
                                  /*addToDesktop*/ true),
          onClose (std::move (onCloseButton)),
          enginePtr (engineForTransport),
          trackedEditor (&editor)
    {
        setUsingNativeTitleBar (true);
        editor.addComponentListener (this);

        // Force a size on the editor BEFORE we touch the window so the
        // window can be sized correctly even before content-set. Some
        // plugin editors construct at 0×0 and only set their final size
        // in the next resized() pass triggered by parent attachment.
        const int ew = juce::jmax (200, editor.getWidth());
        const int eh = juce::jmax (200, editor.getHeight());
        editor.setSize (ew, eh);

        // isResizable() lives on AudioProcessorEditor, not Component.
        // The OOP path passes an XEmbedComponent (Component, no
        // isResizable) so query through a dynamic_cast — diagnostic
        // prints a -1 for the OOP/non-APE case.
        const int isResz = [&]() -> int
        {
            if (auto* ape = dynamic_cast<juce::AudioProcessorEditor*> (&editor))
                return ape->isResizable() ? 1 : 0;
            return -1;
        }();
        std::fprintf (stderr,
                      "[Dusk Studio/PluginEditor] Opening \"%s\" editor: editor=%dx%d resizable=%d\n",
                      title.toRawUTF8(), ew, eh, isResz);

        // Size the WINDOW first (so its X11 peer is allocated at the
        // right geometry), then map it. We deliberately do NOT call
        // setContentNonOwned yet - on Linux, JUCE's VST3PluginWindow
        // owns an XEmbedComponent whose host X11 window is parented to
        // root at construction. The reparent into our peer happens in
        // VST3PluginWindow::componentVisibilityChanged ->
        // attachPluginWindow. If we attach the editor BEFORE our
        // DocumentWindow's peer is fully realized, the reparent fires
        // against a not-yet-mapped window and the plugin's child X11
        // window stays at root - the visible result is a blank/white
        // editor interior. Defer the content-set to after setVisible
        // has propagated through one message loop tick.
        setSize (ew, eh);
        setResizable (false, false);
        centreAroundComponent (nullptr, ew, eh);
        setVisible (true);

        juce::Component::SafePointer<PluginEditorWindow> attachThis (this);
        juce::Component::SafePointer<juce::Component> attachEditor (&editor);
        juce::MessageManager::callAsync ([attachThis, attachEditor]
        {
            auto* self = attachThis.getComponent();
            auto* ed   = attachEditor.getComponent();
            if (self == nullptr || ed == nullptr) return;
            // editor is a juce::Component but JUCE wants AudioProcessorEditor*
            // for setContentNonOwned. Both signatures accept a Component*
            // since AudioProcessorEditor IS a Component.
            self->setContentNonOwned (ed, /*resizeToFitContent*/ true);
        });

        // Staged re-fits. LV2 plugin UIs (Antress 4K_EQ being the canonical
        // offender) finalize their preferred geometry after the first few
        // X11 idle pumps - the bounds reported at createEditorIfNeeded()
        // time are stale. ComponentListener catches resizes that go through
        // Editor::setSize but plugins that grow their internal X11 widget
        // without re-issuing ui_resize don't trigger that path. Re-pull
        // the editor's current size and inflate the window to match at
        // 100 / 350 / 800 ms; cheap, no Timer churn, covers slow plugins.
        for (int delayMs : { 100, 350, 800 })
        {
            juce::Component::SafePointer<PluginEditorWindow> refit (this);
            juce::Component::SafePointer<juce::Component> rEditor (&editor);
            juce::Timer::callAfterDelay (delayMs, [refit, rEditor]
            {
                auto* self = refit.getComponent();
                auto* ed   = rEditor.getComponent();
                if (self == nullptr || ed == nullptr) return;
                const int ew = ed->getWidth();
                const int eh = ed->getHeight();
                if (ew <= 0 || eh <= 0) return;
                auto* content = self->getContentComponent();
                if (content == nullptr) return;
                if (content->getWidth() == ew && content->getHeight() == eh) return;
                self->setContentComponentSize (ew, eh);
            });
        }

        // Same foreground-promotion as DuskStudioApp's MainWindow: without
        // it Mutter on Linux/XWayland may open this editor iconified,
        // leaving the user to Alt+Tab to find it. No-op on Mac/Win.
        juce::Component::SafePointer<PluginEditorWindow> safeThis (this);
        juce::MessageManager::callAsync ([safeThis]
        {
            if (auto* self = safeThis.getComponent())
                if (auto* peer = self->getPeer())
                    duskstudio::platform::bringWindowToFront (*peer);
        });

        // Release the latched X11 preference so subsequent unrelated
        // top-level windows (e.g. another plugin editor opened later
        // for a different strip) start from the platform default.
        duskstudio::platform::clearPreferX11ForNativeWindow();
    }

    ~PluginEditorWindow() override
    {
        if (trackedEditor != nullptr)
            trackedEditor->removeComponentListener (this);
    }

    void closeButtonPressed() override
    {
        // Detach the borrowed editor before we go away so this window's
        // destructor doesn't touch it, then ask the host to drop us.
        if (trackedEditor != nullptr)
        {
            trackedEditor->removeComponentListener (this);
            trackedEditor = nullptr;
        }
        setContentNonOwned (nullptr, false);
        // EWMH-activate a sibling top-level so mutter's focus_window is
        // off this peer before the deferred destroy lands - else
        // meta_window_unmanage asserts on a focused xdg_toplevel.
        duskstudio::platform::prepareForTopLevelDestruction (*this);
        if (onClose) onClose();
    }

    // Transport keys still work when a plugin editor has window focus.
    // Without this the user has to click back into the main window
    // before Space starts/stops playback. We only handle the most
    // common transport bindings here (Space = play/stop, R = record,
    // C = metronome) and route everything else through the default so
    // the plugin's own UI keeps its hotkeys.
    bool keyPressed (const juce::KeyPress& k) override
    {
        if (enginePtr == nullptr) return false;
        const bool noMods = ! k.getModifiers().isAnyModifierKeyDown();
        if (noMods && k == juce::KeyPress::spaceKey)
        {
            auto& transport = enginePtr->getTransport();
            if (transport.isStopped()) enginePtr->play();
            else                       enginePtr->stop();
            return true;
        }
        return false;
    }

    // Plugin editors that finalize their size after construction (LV2 X11
    // UIs whose actual widget size is reported only once the LV2 UI has
    // realized — Antress 4K_EQ and similar) self-resize on a later tick.
    // Without this listener, the wrapper window stays at the initial
    // construction-time size and the user has to drag-resize. Re-inflate
    // the window to fit whenever the borrowed editor reports a size
    // change.
    void componentMovedOrResized (juce::Component& c, bool /*wasMoved*/, bool wasResized) override
    {
        if (! wasResized) return;
        if (&c != trackedEditor) return;
        const int ew = c.getWidth();
        const int eh = c.getHeight();
        if (ew <= 0 || eh <= 0) return;
        setContentComponentSize (ew, eh);
    }

private:
    std::function<void()> onClose;
    AudioEngine* enginePtr = nullptr;
    juce::Component* trackedEditor = nullptr;
};

bool ChannelStripComponent::isPluginEditorOpen() const noexcept
{
    return pluginEditorWindow != nullptr || pluginGenericEditorModal.isOpen();
}

void ChannelStripComponent::openPluginEditor()
{
    if (isPluginEditorOpen()) return;

   #if JUCE_LINUX && DUSKSTUDIO_HAS_OOP_PLUGINS
    if (pluginSlot.isRemote())
    {
        // OOP path: ask the child to show the editor + report its X11
        // Window ID; embed via JUCE's XEmbedComponent. The child owns
        // the editor's lifecycle; we just host its native window.
        std::uint64_t windowId = 0;
        int w = 0, h = 0;
        if (! pluginSlot.showRemoteEditor (windowId, w, h)) return;
        if (windowId == 0) return;

        if (remoteEditorEmbed == nullptr)
        {
            // XEmbedComponent's allowEmbedding ctor: takes a Window
            // (unsigned long) and adopts it as the foreign client.
            // wantsKeyboardFocus = true so plugin GUIs receive key
            // events; allowForeignWidgetToResizeComponent = false so
            // the parent (PluginEditorWindow) controls sizing.
            remoteEditorEmbed = std::make_unique<juce::XEmbedComponent> (
                (unsigned long) windowId,
                /*wantsKeyboardFocus*/ true,
                /*allowForeignWidgetToResizeComponent*/ false);
            remoteEditorEmbed->setSize (juce::jmax (200, w),
                                          juce::jmax (200, h));
        }

        juce::Component::SafePointer<ChannelStripComponent> safe (this);
        pluginEditorWindow = std::make_unique<PluginEditorWindow> (
            pluginSlot.getLoadedName(),
            *remoteEditorEmbed,
            [safe]
            {
                juce::MessageManager::callAsync ([safe]
                {
                    if (auto* self = safe.getComponent())
                        self->closePluginEditor();
                });
            },
            &engine);
        return;
    }
   #endif

    auto* instance = pluginSlot.getInstance();
    if (instance == nullptr) return;

    if (pluginEditor != nullptr && pluginEditorOwner != instance)
        dropPluginEditor();

    if (pluginEditor == nullptr)
    {
        // LV2 plugin editors eager-create an embedded X11 sub-window
        // inside their Editor constructor (juce_LV2PluginFormat.cpp
        // Inner::Inner → addToDesktop(0)). On the JUCE-wayland fork
        // that addToDesktop call lands on a Wayland peer by default,
        // which then can't host an X11 child window. The X11 latch
        // routes the nested peer creation to LinuxComponentPeer (X11)
        // so the LV2's reparent target is a valid X11 host. No-op for
        // non-LV2 paths. Skipped entirely when there's no native UI
        // (we fall back to GenericAudioProcessorEditor below).
        std::unique_ptr<juce::AudioProcessorEditor> fresh;
        if (instance->hasEditor())
        {
            duskstudio::platform::preferX11ForNextNativeWindow();
            fresh.reset (instance->createEditorIfNeeded());
            duskstudio::platform::clearPreferX11ForNativeWindow();
        }
        if (fresh == nullptr)
            fresh = std::make_unique<juce::GenericAudioProcessorEditor> (*instance);
        pluginEditor      = std::move (fresh);
        pluginEditorOwner = instance;
    }

    juce::Component::SafePointer<ChannelStripComponent> safe (this);

    // Generic fallback editor: host inside the main window as an
    // EmbeddedModal so combobox / popup peers share the main wl_surface.
    // Wrapping a JUCE-native editor in an X11 PluginEditorWindow makes
    // PopupMenu peer creation fall back to Wayland, which the JUCE-
    // wayland fork can't create from an X11 parent — combo dropdowns
    // never open. Native plugin editors still need the X11 wrapper.
    if (dynamic_cast<juce::GenericAudioProcessorEditor*> (pluginEditor.get()) != nullptr)
    {
        auto* parent = findParentComponentOfClass<juce::Component>();
        if (parent == nullptr) parent = this;
        pluginGenericEditorModal.showBorrowed (*parent, *pluginEditor,
            [safe]
            {
                if (auto* self = safe.getComponent())
                    self->closePluginEditor();
            });
        return;
    }

    pluginEditorWindow = std::make_unique<PluginEditorWindow> (
        pluginSlot.getLoadedName(),
        *pluginEditor,
        [safe]
        {
            // Defer the unique_ptr reset to the next message-loop tick
            // so we don't destruct the window from inside its own
            // closeButtonPressed callback (which is a JUCE no-no).
            juce::MessageManager::callAsync ([safe]
            {
                if (auto* self = safe.getComponent())
                    self->pluginEditorWindow.reset();
            });
        },
        &engine);
}

void ChannelStripComponent::closePluginEditor()
{
    if (pluginGenericEditorModal.isOpen())
    {
        pluginGenericEditorModal.close();
        return;
    }
    if (pluginEditorWindow == nullptr) return;
    pluginEditorWindow->setContentNonOwned (nullptr, false);
    duskstudio::platform::prepareForTopLevelDestruction (*pluginEditorWindow);

    // Defer the wrapper's destruction one message-loop tick so mutter
    // gets a chance to retarget focus_window from the EWMH activate
    // above. Synchronous reset() races mutter's compositor loop and
    // trips meta_window_unmanage.
    juce::Component::SafePointer<ChannelStripComponent> safe (this);
    juce::MessageManager::callAsync ([safe]
    {
        if (auto* self = safe.getComponent())
            self->pluginEditorWindow.reset();
    });

   #if JUCE_LINUX && DUSKSTUDIO_HAS_OOP_PLUGINS
    if (pluginSlot.isRemote())
    {
        // Tell the child to drop its toplevel so the X11 client window
        // is fully unmapped before our XEmbedComponent's destructor
        // runs (when the deferred reset above fires). Removed on the
        // child side, the XEmbed reparent dance becomes a no-op and
        // we avoid a stale Window reference in the foreign-client
        // bookkeeping.
        pluginSlot.hideRemoteEditor();
        // XEmbedComponent stays cached so reopen reuses it; only drop
        // it on full unload.
    }
   #endif
}

void ChannelStripComponent::dropPluginEditor()
{
    closePluginEditor();
    // ~AudioProcessorEditor tears down the plugin's internal X11
    // children synchronously (colour pickers, preset browsers,
    // transient popups). On a Wayland session, any of those could
    // theoretically be mutter's focus_window. A yield via
    // requestFocusOnMainWaylandSurface lets the compositor process
    // any pending unmaps before the synchronous destruction lands.
    //
    // Why not defer pluginEditor.reset() into a callAsync the same way
    // closePluginEditor defers the wrapper window: the pluginEditor
    // destructor calls editorBeingDeleted on its AudioProcessor. The
    // AudioProcessor is only kept alive across ONE swap by PluginSlot's
    // previousInstance keep-alive; two quick "Replace plugin" actions
    // within the deferred-reset window would destroy the processor
    // before the deferred reset, turning a rare focus race into a
    // certain use-after-free.
    duskstudio::platform::requestFocusOnMainWaylandSurface();
    pluginEditor.reset();
    pluginEditorOwner = nullptr;

   #if JUCE_LINUX && DUSKSTUDIO_HAS_OOP_PLUGINS
    // Drop the OOP-side embed too. The XEmbedComponent's destructor
    // tells the foreign X11 client to detach; the child has already
    // been told to hide the editor by closePluginEditor above (or by
    // unloadPluginSlot which calls dropPluginEditor before
    // pluginSlot.unload). Either way the X client is safe to release.
    remoteEditorEmbed.reset();
   #endif
}

namespace
{
// Popup body for the I/O config button. Re-parents the 6 inline ComboBoxes
// (+ MIDI activity LED) into a single column. When the popup closes the
// widgets become orphans; ChannelStripComponent retains ownership and
// state lives on Track atoms, so reopening the popup just re-parents.
class IoConfigPopup : public juce::Component
{
public:
    IoConfigPopup (juce::ComboBox& mode, juce::ComboBox& in, juce::ComboBox& inR,
                    juce::ComboBox& mIn, juce::ComboBox& mCh, juce::ComboBox& mOut,
                    juce::Component& led)
        : modeSelector (mode), inputSelector (in), inputSelectorR (inR),
          midiInputSelector (mIn), midiChannelSelector (mCh),
          midiOutputSelector (mOut), activityLed (led)
    {
        addAndMakeVisible (modeSelector);
        addAndMakeVisible (inputSelector);
        addAndMakeVisible (inputSelectorR);
        addAndMakeVisible (midiInputSelector);
        addAndMakeVisible (midiChannelSelector);
        addAndMakeVisible (midiOutputSelector);
        addAndMakeVisible (activityLed);
        setSize (240, 160);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (8);
        constexpr int kRowH = 24, kGap = 6;

        modeSelector.setBounds (area.removeFromTop (kRowH));
        area.removeFromTop (kGap);

        const int mode = juce::jlimit (0, 2, modeSelector.getSelectedId() - 1);
        inputSelector.setVisible (mode == 0 || mode == 1);
        inputSelectorR.setVisible (mode == 1);
        midiInputSelector  .setVisible (mode == 2);
        midiChannelSelector.setVisible (mode == 2);
        midiOutputSelector .setVisible (mode == 2);
        activityLed       .setVisible (mode == 2);

        if (mode == 0)
        {
            inputSelector.setBounds (area.removeFromTop (kRowH));
        }
        else if (mode == 1)
        {
            auto row = area.removeFromTop (kRowH);
            const int half = (row.getWidth() - 4) / 2;
            inputSelector .setBounds (row.removeFromLeft (half));
            row.removeFromLeft (4);
            inputSelectorR.setBounds (row);
        }
        else
        {
            constexpr int kLedW = 14;
            auto inRow = area.removeFromTop (kRowH);
            activityLed.setBounds (inRow.removeFromRight (kLedW).reduced (1));
            inRow.removeFromRight (4);
            midiInputSelector.setBounds (inRow);
            area.removeFromTop (kGap);
            midiChannelSelector.setBounds (area.removeFromTop (kRowH));
            area.removeFromTop (kGap);
            midiOutputSelector.setBounds (area.removeFromTop (kRowH));
        }
    }

private:
    juce::ComboBox& modeSelector;
    juce::ComboBox& inputSelector;
    juce::ComboBox& inputSelectorR;
    juce::ComboBox& midiInputSelector;
    juce::ComboBox& midiChannelSelector;
    juce::ComboBox& midiOutputSelector;
    juce::Component& activityLed;
};
} // namespace

void ChannelStripComponent::openIoConfigPopup()
{
    if (auto* existing = activeIoBox.getComponent()) { existing->dismiss(); return; }

    auto panel = std::make_unique<IoConfigPopup> (modeSelector, inputSelector, inputSelectorR,
                                                   midiInputSelector, midiChannelSelector,
                                                   midiOutputSelector, midiActivityLed);
    const int mode = juce::jlimit (0, 2, modeSelector.getSelectedId() - 1);
    const int rows = mode == 2 ? 4 : 2;
    panel->setSize (240, 8 + rows * 24 + (rows - 1) * 6 + 8);

    auto* topLevel = getTopLevelComponent();
    if (topLevel == nullptr) topLevel = this;
    const auto anchor = topLevel->getLocalArea (this, ioConfigButton.getBounds());

    auto& box = juce::CallOutBox::launchAsynchronously (std::move (panel), anchor, topLevel);
    box.setDismissalMouseClicksAreAlwaysConsumed (false);
    activeIoBox = &box;
}

void ChannelStripComponent::refreshIoConfigButton()
{
    const int mode = juce::jlimit (0, 2, track.mode.load (std::memory_order_relaxed));
    juce::String text;
    if (mode == 0)        // Mono
    {
        text = "Mono ";
        const auto in = inputSelector.getText();
        text += in.isEmpty() ? "(none)" : in;
    }
    else if (mode == 1)   // Stereo
    {
        const auto inL = inputSelector .getText();
        const auto inR = inputSelectorR.getText();
        text = "Stereo " + (inL.isEmpty() ? juce::String ("?") : inL)
                + " / " + (inR.isEmpty() ? juce::String ("?") : inR);
    }
    else                  // MIDI
    {
        const auto port = midiInputSelector.getText();
        const auto ch   = midiChannelSelector.getText();
        // U+00B7 middle dot via CharPointer_UTF8 - juce::String's char*
        // ctor uses the system locale which mangles UTF-8 on Linux.
        const juce::String midDot (juce::CharPointer_UTF8 ("\xc2\xb7"));
        text = "MIDI " + (port.isEmpty() ? juce::String ("(none)") : port)
                + " " + midDot + " " + (ch.isEmpty() ? juce::String ("Omni") : ch);
    }
    if (ioConfigButton.getButtonText() != text)
        ioConfigButton.setButtonText (text);
}

void ChannelStripComponent::openEqEditorPopup()
{
    // Toggle: clicking EQ while EQ is up dismisses it. With CallOutBox the
    // click that lands on the button while the box is open is consumed by
    // the box's dismissal handler (setDismissalMouseClicksAreAlwaysConsumed
    // below), so we never actually re-enter this function on the second
    // click - the box dismisses itself and the button never sees the click.
    // This branch handles programmatic toggle calls (e.g. keyboard shortcut).
    if (auto* existing = activeEqBox.getComponent())
    {
        existing->dismiss();
        return;
    }

    // Mutual exclusion: close the COMP popup if it's open.
    if (auto* otherBox = activeCompBox.getComponent())
        otherBox->dismiss();

    auto panel = std::make_unique<ChannelEqEditor> (track);
    panel->setSize (380, 628);

    auto* topLevel = getTopLevelComponent();
    if (topLevel == nullptr) topLevel = this;
    const auto anchor = topLevel->getLocalArea (this, eqCompactButton.getBounds());

    attachDimOverlay();
    auto& box = juce::CallOutBox::launchAsynchronously (
        std::move (panel), anchor, topLevel);
    box.setDismissalMouseClicksAreAlwaysConsumed (true);
    // Force the popup to fire from the RIGHT side of the button by
    // restricting the available fit-area to the strip's right edge onward.
    // Without this, JUCE's CallOutBox picks whichever side has the most
    // space, which can drop below the button when the strip is centred.
    {
        const auto rightFit = topLevel->getLocalBounds()
                                  .withTrimmedLeft (anchor.getRight());
        box.updatePosition (anchor, rightFit);
    }
    activeEqBox = &box;
}

void ChannelStripComponent::openCompEditorPopup()
{
    if (auto* existing = activeCompBox.getComponent())
    {
        existing->dismiss();
        return;
    }
    if (auto* otherBox = activeEqBox.getComponent())
        otherBox->dismiss();

    auto panel = std::make_unique<ChannelCompEditor> (track);
    // Editor sets its own size based on the active comp mode (Opto is
    // shorter than FET/VCA). The CallOutBox follows via childBoundsChanged.

    auto* topLevel = getTopLevelComponent();
    if (topLevel == nullptr) topLevel = this;
    const auto anchor = topLevel->getLocalArea (this, compCompactButton.getBounds());

    attachDimOverlay();
    auto& box = juce::CallOutBox::launchAsynchronously (
        std::move (panel), anchor, topLevel);
    box.setDismissalMouseClicksAreAlwaysConsumed (true);
    // Force right-side placement (same logic as the EQ popup).
    {
        const auto rightFit = topLevel->getLocalBounds()
                                  .withTrimmedLeft (anchor.getRight());
        box.updatePosition (anchor, rightFit);
    }
    activeCompBox = &box;
}

void ChannelStripComponent::attachDimOverlay()
{
    if (activeDimOverlay != nullptr) return;

    auto* topLevel = getTopLevelComponent();
    if (topLevel == nullptr) return;

    activeDimOverlay = std::make_unique<DimOverlay>();
    activeDimOverlay->setBounds (topLevel->getLocalBounds());
    activeDimOverlay->onClick = [this]
    {
        if (auto* eq  = activeEqBox.getComponent())   eq->dismiss();
        if (auto* cmp = activeCompBox.getComponent()) cmp->dismiss();
    };
    topLevel->addAndMakeVisible (activeDimOverlay.get());
    // CallOutBox::launchAsynchronously adds the box to the desktop / top
    // level after this call returns; since it's added later, it ends up
    // above the dim in z-order — exactly what we want.
}

void ChannelStripComponent::detachDimOverlay()
{
    activeDimOverlay.reset();
}

void ChannelStripComponent::timerCallback()
{
    // Plugin-slot button reflects the slot's current load state. Cheap -
    // just an atomic-pointer read + string compare against the cached name.
    refreshPluginSlotButton();

    // Sync nameLabel with track.name when something external rewrites it
    // (e.g. an audio import auto-renames the track to the file's basename).
    // Skip while the user is mid-edit so their typed text isn't stomped.
    if (! nameLabel.isBeingEdited()
        && nameLabel.getText (false) != track.name)
        nameLabel.setText (track.name, juce::dontSendNotification);

    // Tear down the dim overlay once both popups have closed. Polling at
    // 30 Hz is cheaper than wiring a ComponentListener on each CallOutBox.
    if (activeDimOverlay != nullptr
        && activeEqBox.getComponent() == nullptr
        && activeCompBox.getComponent() == nullptr)
    {
        detachDimOverlay();
    }

    // MIDI activity LED. Read-and-clear the engine's flag each tick — a
    // continuous stream sets it back to true on the next block, so the LED
    // stays lit while traffic flows and turns off ~33 ms after it stops.
    {
        const bool fired = track.midiActivity.exchange (false, std::memory_order_relaxed);
        if (midiActivityLed.lit != fired)
        {
            midiActivityLed.lit = fired;
            midiActivityLed.repaint();
        }
    }

    // Sync the inline COMP mode button with the underlying atoms so it
    // reflects writes from other surfaces (meter-strip threshold drag,
    // popup editor's ON toggle, MIDI binding, etc.). State-only refresh
    // every tick - knob visibility only changes on explicit mode swaps,
    // so we skip that work here.
    refreshCompModeButtonState();

    // Sync the inline COMP knobs with their underlying atoms so writes
    // from the meter-strip threshold drag or the popout editor reflect
    // here without the user having to reload. Skip a knob the user is
    // actively dragging — otherwise their drag would snap back to the
    // stored value mid-motion.
    {
        auto syncKnob = [] (juce::Slider& k, float target)
        {
            if (k.isMouseButtonDown()) return;
            if (std::abs ((float) k.getValue() - target) < 1.0e-4f) return;
            k.setValue (target, juce::dontSendNotification);
        };
        const auto& sp = track.strip;
        syncKnob (optoPeakRedKnob, sp.compOptoPeakRed.load (std::memory_order_relaxed));
        syncKnob (optoGainKnob,    sp.compOptoGain.load    (std::memory_order_relaxed));
        syncKnob (fetInputKnob,    sp.compFetInput.load    (std::memory_order_relaxed));
        syncKnob (fetOutputKnob,   sp.compFetOutput.load   (std::memory_order_relaxed));
        syncKnob (fetAttackKnob,   sp.compFetAttack.load   (std::memory_order_relaxed));
        syncKnob (fetReleaseKnob,  sp.compFetRelease.load  (std::memory_order_relaxed));
        syncKnob (fetRatioKnob,    (float) sp.compFetRatio.load (std::memory_order_relaxed));
        syncKnob (vcaRatioKnob,    sp.compVcaRatio.load    (std::memory_order_relaxed));
        syncKnob (vcaAttackKnob,   sp.compVcaAttack.load   (std::memory_order_relaxed));
        syncKnob (vcaReleaseKnob,  sp.compVcaRelease.load  (std::memory_order_relaxed));
        syncKnob (vcaOutputKnob,   sp.compVcaOutput.load   (std::memory_order_relaxed));
    }

    // SUMMARY-mode compact buttons get an on-air indicator so the user
    // knows whether EQ / COMP is engaged without having to open the
    // popup. Bullet character + brighter background when on.
    if (compactMode)
    {
        // Channel-strip EQ has no master on/off - it's "active" when the
        // HPF is engaged or any of the four bands has a non-trivial gain.
        // Comp has a real on/off atom.
        const auto& sp = track.strip;
        const bool anyBandActive =
               std::abs (sp.lfGainDb.load (std::memory_order_relaxed)) > 0.1f
            || std::abs (sp.lmGainDb.load (std::memory_order_relaxed)) > 0.1f
            || std::abs (sp.hmGainDb.load (std::memory_order_relaxed)) > 0.1f
            || std::abs (sp.hfGainDb.load (std::memory_order_relaxed)) > 0.1f;
        const bool eqOn   = sp.hpfEnabled.load (std::memory_order_relaxed) || anyBandActive;
        const bool compOn = sp.compEnabled.load (std::memory_order_relaxed);

        const auto eqAccent   = juce::Colour (fourKColors::kLfGreen);
        const auto compAccent = juce::Colour (fourKColors::kCompGold);

        // Text stays as the section name; the illuminated background
        // (set below) is the on-state indicator. No leading bullet/dot.
        if (eqCompactButton.getButtonText() != "EQ")
            eqCompactButton.setButtonText ("EQ");
        if (compCompactButton.getButtonText() != "COMP")
            compCompactButton.setButtonText ("COMP");

        eqCompactButton  .setColour (juce::TextButton::buttonColourId,
                                       eqOn   ? eqAccent.darker (0.55f)   : juce::Colour (0xff222226));
        compCompactButton.setColour (juce::TextButton::buttonColourId,
                                       compOn ? compAccent.darker (0.55f) : juce::Colour (0xff222226));
    }

    // Detect bus-colour edits made via the aux strip's right-click menu and
    // re-skin the bus assignment buttons accordingly. Polling at 30 Hz is
    // negligible (16 strips × 4 buses).
    for (int i = 0; i < ChannelStripParams::kNumBuses; ++i)
    {
        const auto current = session.bus (i).colour;
        if (current.getARGB() != lastBusColours[(size_t) i])
        {
            lastBusColours[(size_t) i] = current.getARGB();
            if (auto& btn = busButtons[(size_t) i])
            {
                btn->setColour (juce::TextButton::buttonOnColourId, current);
                btn->setColour (juce::TextButton::textColourOffId,  current.brighter (0.15f));
                btn->repaint();
            }
        }
    }

    const float gr = track.meterGrDb.load (std::memory_order_relaxed);
    if (gr < displayedGrDb)
        displayedGrDb = gr;
    else
        displayedGrDb += (gr - displayedGrDb) * 0.18f;

    // Input level meter - fast attack on rise, slow decay; with a peak-hold
    // marker that lingers for ~600 ms before falling. Stereo mode also
    // smooths the R channel so the second LED bar can be drawn alongside.
    auto smoothMeter = [] (float incoming, float& displayed,
                            float& peakHold, int& peakHoldFrames)
    {
        if (incoming > displayed) displayed = incoming;
        else                       displayed += (incoming - displayed) * 0.15f;

        if (incoming >= peakHold)
        {
            peakHold       = incoming;
            peakHoldFrames = 18;  // ~600 ms at 30 Hz
        }
        else if (peakHoldFrames > 0) --peakHoldFrames;
        else peakHold = juce::jmax (-100.0f, peakHold - 1.5f);
    };

    const float inputDb = track.meterInputDb.load (std::memory_order_relaxed);
    smoothMeter (inputDb, displayedInputDb, inputPeakHoldDb, inputPeakHoldFrames);

    const bool stereoMode = (track.mode.load (std::memory_order_relaxed)
                              == (int) Track::Mode::Stereo);
    if (stereoMode)
    {
        const float inputRDb = track.meterInputRDb.load (std::memory_order_relaxed);
        smoothMeter (inputRDb, displayedInputRDb, inputPeakHoldRDb, inputPeakHoldRFrames);
    }
    else
    {
        // Bleed the R channel back to silence when the user flips out of
        // stereo so the dual-bar doesn't ghost-hold its last value.
        displayedInputRDb = -100.0f;
        inputPeakHoldRDb  = -100.0f;
    }

    // GR repaint is handled inside CompMeterStrip's own Timer.
    if (! inputMeterArea.isEmpty())  repaint (inputMeterArea);

    // Update the numeric readout below the meter.
    // Integer dB readout — narrow column doesn't fit "-60.0" at a
    // readable font size; one decimal of precision wasn't actionable
    // anyway since the meter ballistics smear sub-dB changes.
    if (inputPeakHoldDb <= -60.0f)
        inputPeakLabel.setText ("-inf", juce::dontSendNotification);
    else
        inputPeakLabel.setText (juce::String ((int) std::round (inputPeakHoldDb)),
                                  juce::dontSendNotification);

    // Tint the readout when peaks get hot.
    inputPeakLabel.setColour (juce::Label::textColourId,
        inputPeakHoldDb >= -3.0f  ? juce::Colour (0xffff5050) :
        inputPeakHoldDb >= -12.0f ? juce::Colour (0xffe0c050) :
                                     juce::Colour (0xffd0d0d0));

    // GR readout: show "-X.X" when the comp is reducing, dim "0.0" otherwise.
    // displayedGrDb is already smoothed above (asymmetric: fast attack on
    // rise, slow release on fall), matching the visual GR meter.
    if (displayedGrDb <= -0.05f)
    {
        grPeakLabel.setText (juce::String (displayedGrDb, 1), juce::dontSendNotification);
        grPeakLabel.setColour (juce::Label::textColourId, juce::Colour (0xffe0c050));
    }
    else
    {
        grPeakLabel.setText ("0.0", juce::dontSendNotification);
        grPeakLabel.setColour (juce::Label::textColourId, juce::Colour (0xff606064));
    }

    // Motor-fader / motor-pan animation: when the audio engine is feeding
    // a live atom from the lane (Read, or Touch when not grabbed), mirror
    // it into the slider/knob visually. Gate on a small delta to avoid
    // setValue churn every tick when manual mode just mirrors the user's
    // setpoint. While the user is grabbing in Touch mode, their gesture
    // IS the value source, so we don't fight it.
    {
        const int amode = track.automationMode.load (std::memory_order_relaxed);
        const bool isRead  = amode == (int) AutomationMode::Read;
        const bool isWrite = amode == (int) AutomationMode::Write;
        const bool isTouch = amode == (int) AutomationMode::Touch;
        const bool playing = engine.getTransport().isPlaying();

        // Fader animate / capture.
        {
            const float live    = track.strip.liveFaderDb.load (std::memory_order_relaxed);
            const bool  touched = track.strip.faderTouched.load (std::memory_order_relaxed);
            const bool  animating = isRead || (isTouch && ! touched);
            if (animating && std::abs (live - displayedLiveFaderDb) > 0.05f)
            {
                displayedLiveFaderDb = live;
                faderSlider.setValue (live, juce::dontSendNotification);
            }
            else if (! animating)
            {
                displayedLiveFaderDb = live;
            }

            const bool capturing = playing && (isWrite || (isTouch && touched));
            if (capturing)
                captureWritePoint (AutomationParam::FaderDb,
                                    track.strip.faderDb.load (std::memory_order_relaxed));
        }

        // Pan animate / capture. Same shape as fader; threshold is in
        // pan units (-1..+1), so 0.005 = 0.25 % of the knob's travel,
        // well below visible.
        {
            const float live    = track.strip.livePan.load (std::memory_order_relaxed);
            const bool  touched = track.strip.panTouched.load (std::memory_order_relaxed);
            const bool  animating = isRead || (isTouch && ! touched);
            if (animating && std::abs (live - displayedLivePan) > 0.005f)
            {
                displayedLivePan = live;
                panKnob.setValue (live, juce::dontSendNotification);
            }
            else if (! animating)
            {
                displayedLivePan = live;
            }

            const bool capturing = playing && (isWrite || (isTouch && touched));
            if (capturing)
                captureWritePoint (AutomationParam::Pan,
                                    track.strip.pan.load (std::memory_order_relaxed));
        }

        // Mute / Solo - discrete params. Sync the button visuals to
        // liveMute / liveSolo when the audio engine is driving them
        // (Read or Touch). In Off / Write the button state already
        // matches the user's clicks and the live atoms mirror them,
        // so syncing is harmless idempotent in all modes.
        {
            const bool live = track.strip.liveMute.load (std::memory_order_relaxed);
            if (muteButton.getToggleState() != live)
                muteButton.setToggleState (live, juce::dontSendNotification);
        }
        {
            const bool live = track.strip.liveSolo.load (std::memory_order_relaxed);
            if (soloButton.getToggleState() != live)
                soloButton.setToggleState (live, juce::dontSendNotification);
        }

        // Aux sends - animate + capture in lockstep with fader / pan.
        // Threshold 0.1 dB on a -60..+6 dB knob (~0.15 % of travel).
        // Knob is null when the strip isn't in mixing mode (visible).
        for (int i = 0; i < ChannelStripParams::kNumAuxSends; ++i)
        {
            const float live    = track.strip.liveAuxSendDb[(size_t) i].load (std::memory_order_relaxed);
            const bool  touched = track.strip.auxSendTouched[(size_t) i].load (std::memory_order_relaxed);
            const bool  animating = isRead || (isTouch && ! touched);
            if (animating && std::abs (live - displayedLiveAuxSendDb[(size_t) i]) > 0.1f)
            {
                displayedLiveAuxSendDb[(size_t) i] = live;
                if (auto* knob = auxKnobs[(size_t) i].get())
                    knob->setValue (live, juce::dontSendNotification);
            }
            else if (! animating)
            {
                displayedLiveAuxSendDb[(size_t) i] = live;
            }

            const bool capturing = playing && (isWrite || (isTouch && touched));
            if (capturing)
            {
                const auto param = (AutomationParam) ((int) AutomationParam::AuxSend1 + i);
                captureWritePoint (param,
                                    track.strip.auxSendDb[(size_t) i].load (std::memory_order_relaxed));
            }
        }
    }
}

void ChannelStripComponent::captureWritePoint (AutomationParam param, float denormValue)
{
    // Convert denormalized value back to lane storage (0..1). Mirrors
    // Session.cpp's denormalizeAutomation - kept here as a small switch
    // because there are only two callers (this and a future Touch hook)
    // and a free function isn't worth the noise.
    auto normalize = [] (AutomationParam p, float v) -> float
    {
        switch (p)
        {
            case AutomationParam::FaderDb:
            {
                const float lo = ChannelStripParams::kFaderMinDb;
                const float hi = ChannelStripParams::kFaderMaxDb;
                return juce::jlimit (0.0f, 1.0f, (v - lo) / (hi - lo));
            }
            case AutomationParam::Pan:
                return juce::jlimit (0.0f, 1.0f, (v + 1.0f) * 0.5f);
            case AutomationParam::Mute:
            case AutomationParam::Solo:
                return v >= 0.5f ? 1.0f : 0.0f;
            case AutomationParam::AuxSend1:
            case AutomationParam::AuxSend2:
            case AutomationParam::AuxSend3:
            case AutomationParam::AuxSend4:
            {
                if (v <= ChannelStripParams::kAuxSendOffDb) return 0.0f;
                const float lo = ChannelStripParams::kAuxSendMinDb;
                const float hi = ChannelStripParams::kAuxSendMaxDb;
                return juce::jlimit (0.0f, 1.0f, (v - lo) / (hi - lo));
            }
            case AutomationParam::kCount: break;
        }
        return 0.0f;
    };

    auto& lane = track.automationLanes[(size_t) param];
    AutomationPoint pt;
    pt.timeSamples   = engine.getTransport().getPlayhead();
    pt.value         = normalize (param, denormValue);
    pt.recordedAtBPM = session.tempoBpm.load (std::memory_order_relaxed);

    // Coalesce: if the most recent point is at the same timeline sample
    // (or earlier), replace its value (don't append). This handles two
    // cases: (a) Timer fires faster than transport advances (paused?
    // shouldn't happen since we gated on isPlaying), (b) Time-travel
    // backward via loop wraparound mid-Write -- subsequent appends
    // belong AFTER the most recent timeline position, not before it.
    // Strict ascending invariant is required by evaluateLane's binary
    // search.
    if (! lane.points.empty() && lane.points.back().timeSamples >= pt.timeSamples)
    {
        // Loop wraparound case: drop the rest of the lane that's now in
        // the future relative to playhead, so the binary-search invariant
        // (sorted ascending) holds and the upcoming Write captures land
        // in their natural order. Discrete params (mute / solo) keep
        // the same rule.
        if (lane.points.back().timeSamples > pt.timeSamples)
        {
            auto cutoff = std::lower_bound (lane.points.begin(), lane.points.end(),
                pt.timeSamples,
                [] (const AutomationPoint& a, juce::int64 t) { return a.timeSamples < t; });
            lane.points.erase (cutoff, lane.points.end());
        }
        // Same-sample replace: keep the latest value at this exact sample.
        if (! lane.points.empty() && lane.points.back().timeSamples == pt.timeSamples)
        {
            lane.points.back() = pt;
            return;
        }
    }
    lane.points.push_back (pt);
}

void ChannelStripComponent::showAutoModeMenu()
{
    const int cur = track.automationMode.load (std::memory_order_relaxed);
    juce::PopupMenu menu;
    menu.addItem (1, "Off",   true, cur == (int) AutomationMode::Off);
    menu.addItem (2, "Read",  true, cur == (int) AutomationMode::Read);
    menu.addItem (3, "Write", true, cur == (int) AutomationMode::Write);
    menu.addItem (4, "Touch", true, cur == (int) AutomationMode::Touch);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&autoModeButton),
        [safeThis = juce::Component::SafePointer<ChannelStripComponent> (this)]
        (int chosen)
        {
            auto* self = safeThis.getComponent();
            if (self == nullptr || chosen < 1 || chosen > 4) return;
            const AutomationMode picked[] = {
                AutomationMode::Off, AutomationMode::Read,
                AutomationMode::Write, AutomationMode::Touch
            };
            self->setAutoMode (picked[chosen - 1]);
        });
}

void ChannelStripComponent::setAutoMode (AutomationMode mode)
{
    // When transitioning OUT of Write or Touch, the points just appended
    // to the lane need to be visible to the audio thread BEFORE it starts
    // reading from the lane. The release-store on mode synchronizes those
    // writes - any prior append to lane.points happens-before the audio
    // thread's acquire-load of the new mode.
    track.automationMode.store ((int) mode, std::memory_order_release);

    // Read mode disables every automated control (spec: "User cannot
    // override"). Off / Write / Touch leave them interactive so the
    // user can either ride them (Write) or grab to override (Touch).
    const bool interactive = mode != AutomationMode::Read;
    faderSlider.setEnabled (interactive);
    panKnob    .setEnabled (interactive);
    muteButton .setEnabled (interactive);
    soloButton .setEnabled (interactive);
    for (auto& knob : auxKnobs)
        if (knob != nullptr) knob->setEnabled (interactive);

    refreshAutoModeButton();
}

void ChannelStripComponent::refreshAutoModeButton()
{
    const int m = track.automationMode.load (std::memory_order_relaxed);
    const char* label = "OFF";
    juce::Colour bg (0xff222226);
    juce::Colour fg (0xff909094);
    switch (m)
    {
        case (int) AutomationMode::Read:
            label = "READ";
            bg = juce::Colour (0xff20603a);   // muted green
            fg = juce::Colour (0xffd0e8d0);
            break;
        case (int) AutomationMode::Write:
            label = "WRITE";
            bg = juce::Colour (0xff803020);   // muted red - 3c-ii
            fg = juce::Colour (0xfff0d0c8);
            break;
        case (int) AutomationMode::Touch:
            label = "TOUCH";
            bg = juce::Colour (0xff806020);   // muted amber - 3c-ii
            fg = juce::Colour (0xfff0e0c0);
            break;
        case (int) AutomationMode::Off:
        default: break;
    }
    autoModeButton.setButtonText (label);
    autoModeButton.setColour (juce::TextButton::buttonColourId, bg);
    autoModeButton.setColour (juce::TextButton::textColourOffId, fg);
}

void ChannelStripComponent::mouseDown (const juce::MouseEvent& e)
{
    // Any click on the strip (background pixels - children consume their
    // own mouse first) puts the focus on this track so keyboard shortcuts
    // (A / S / X) target it. Fires on left AND right clicks because the
    // user reasonably expects the right-click colour-menu to ALSO have
    // selected the track.
    if (onTrackFocusRequested) onTrackFocusRequested (trackIndex);

    // Right-click on a specific child surface routes to the MIDI Learn
    // menu for that target. The route is gated on eventComponent so a
    // hit on the strip background still falls through to the colour menu.
    if (e.mods.isPopupMenu())
    {
        if (e.eventComponent == &faderSlider)
        {
            midilearn::showLearnMenu (faderSlider, session,
                                        MidiBindingTarget::TrackFader, trackIndex);
            return;
        }
        if (e.eventComponent == &muteButton)
        {
            midilearn::showLearnMenu (muteButton, session,
                                        MidiBindingTarget::TrackMute, trackIndex);
            return;
        }
        if (e.eventComponent == &soloButton)
        {
            midilearn::showLearnMenu (soloButton, session,
                                        MidiBindingTarget::TrackSolo, trackIndex);
            return;
        }
        if (e.eventComponent == &armButton)
        {
            midilearn::showLearnMenu (armButton, session,
                                        MidiBindingTarget::TrackArm, trackIndex);
            return;
        }
        // Right-click on any aux knob -> MIDI Learn for that (track, aux)
        // pair. The packed index encodes track * kNumAuxSends + aux so
        // one binding can address any of the 16 x 4 = 64 send positions.
        for (int i = 0; i < (int) auxKnobs.size(); ++i)
        {
            if (auxKnobs[(size_t) i] != nullptr
                && e.eventComponent == auxKnobs[(size_t) i].get())
            {
                midilearn::showLearnMenu (*auxKnobs[(size_t) i], session,
                                            MidiBindingTarget::TrackAuxSend,
                                            packTrackAux (trackIndex, i));
                return;
            }
        }
        // HPF + EQ band gains. Same eventComponent-match shape as the
        // other strip controls. Freq + Q knobs aren't bindable in v1 -
        // gain is the most-automated EQ knob in practice.
        if (e.eventComponent == &hpfKnob)
        {
            midilearn::showLearnMenu (hpfKnob, session,
                                        MidiBindingTarget::TrackHpfFreq, trackIndex);
            return;
        }
        for (int i = 0; i < (int) eqRows.size(); ++i)
        {
            if (eqRows[(size_t) i].gain != nullptr
                && e.eventComponent == eqRows[(size_t) i].gain.get())
            {
                midilearn::showLearnMenu (*eqRows[(size_t) i].gain, session,
                                            MidiBindingTarget::TrackEqGain,
                                            packTrackEqBand (trackIndex, i));
                return;
            }
        }
        // Comp threshold/makeup: each per-mode knob routes to the LOGICAL
        // target so the binding survives mode swaps (audio thread reads
        // compMode and writes to the matching atom). VCA threshold has
        // no dedicated knob in the UI (it lives on the GR-strip drag
        // handle); right-clicking the Opto/FET threshold knob binds it
        // for all modes.
        if (e.eventComponent == &optoPeakRedKnob
            || e.eventComponent == &fetInputKnob)
        {
            auto& src = (e.eventComponent == &optoPeakRedKnob)
                            ? optoPeakRedKnob : fetInputKnob;
            midilearn::showLearnMenu (src, session,
                                        MidiBindingTarget::TrackCompThresh, trackIndex);
            return;
        }
        if (e.eventComponent == &optoGainKnob
            || e.eventComponent == &fetOutputKnob
            || e.eventComponent == &vcaOutputKnob)
        {
            auto& src = (e.eventComponent == &optoGainKnob)
                            ? optoGainKnob
                            : (e.eventComponent == &fetOutputKnob ? fetOutputKnob
                                                                    : vcaOutputKnob);
            midilearn::showLearnMenu (src, session,
                                        MidiBindingTarget::TrackCompMakeup, trackIndex);
            return;
        }
    }

    // Right-click anywhere on the strip body opens the colour menu. Children
    // (sliders, buttons, labels) consume their own mouse events first, so this
    // only fires on background pixels - exactly the affordance we want.
    if (e.mods.isPopupMenu())
        showColourMenu();
    else
        Component::mouseDown (e);
}

void ChannelStripComponent::applyTrackColour (juce::Colour c)
{
    track.colour = c;
    repaint();
}

void ChannelStripComponent::showColourMenu()
{
    // Eight 4K-palette presets for fast picking, plus a "Custom…" entry that
    // pops a JUCE ColourSelector for fine-grained choice.
    const std::pair<const char*, juce::uint32> presets[] = {
        { "Red",        fourKColors::kHfRed     },
        { "Orange",     fourKColors::kHmOrange  },
        { "Amber",      fourKColors::kLmAmber   },
        { "Green",      fourKColors::kLfGreen   },
        { "Cyan",       fourKColors::kPanCyan   },
        { "Blue",       fourKColors::kHpfBlue   },
        { "Purple",     fourKColors::kSendPurple},
        { "Tan",        fourKColors::kMasterTan },
    };

    juce::PopupMenu menu;
    menu.addSectionHeader ("Track colour");
    for (size_t i = 0; i < std::size (presets); ++i)
    {
        juce::PopupMenu::Item item;
        item.itemID = (int) (i + 1);
        item.text = presets[i].first;
        item.colour = juce::Colour (presets[i].second);
        menu.addItem (item);
    }
    menu.addSeparator();
    menu.addItem (1001, "Rename track...");

    // Plugin slot menu items, only meaningful when a plugin is loaded.
    // Replace/Remove live here (rather than on the slot button itself) so
    // the slot button's primary click stays as a toggle for the editor.
    if (pluginSlot.isLoaded())
    {
        menu.addSeparator();
        menu.addItem (1010, "Replace plugin...");
        menu.addItem (1011, "Remove plugin");
        if (pluginSlot.wasCrashed())
            menu.addItem (1012, "Re-enable plugin (crashed)");
        else if (pluginSlot.wasAutoBypassed())
            menu.addItem (1012, "Re-enable plugin (auto-bypassed)");
    }

    // Clone-to submenu. IDs 2000 + dest-index target the destination
    // track. The action class CAPTURES the dest's prior state at first
    // perform() so the user gets clean undo even when overwriting a
    // populated track.
    menu.addSeparator();
    juce::PopupMenu cloneMenu;
    for (int t = 0; t < Session::kNumTracks; ++t)
    {
        if (t == trackIndex) continue;
        const auto& destTrack = session.track (t);
        const auto label = juce::String (t + 1) + ": "
            + (destTrack.name.isNotEmpty() ? destTrack.name : juce::String());
        cloneMenu.addItem (2000 + t, label);
    }
    menu.addSubMenu ("Clone to track...", cloneMenu);

    // Fader group submenu. 8 group slots is plenty for a 16-channel
    // console (max parallel groups in practice is 4-5: drums, guitars,
    // synths, vocals, BG). IDs 2100 + group-id; 2100 = ungrouped.
    juce::PopupMenu groupMenu;
    const int currentGroup = track.strip.faderGroupId.load (std::memory_order_relaxed);
    groupMenu.addItem (2100, "None", true, currentGroup == 0);
    for (int g = 1; g <= 8; ++g)
        groupMenu.addItem (2100 + g, "Group " + juce::String (g),
                            true, currentGroup == g);
    menu.addSubMenu ("Fader group...", groupMenu);

    juce::Component::SafePointer<ChannelStripComponent> safe (this);
    // Copy the presets into a std::vector so the async callback owns its own
    // copy (the local C-array on the stack is gone by the time the menu fires).
    std::vector<std::pair<juce::String, juce::uint32>> presetCopy;
    presetCopy.reserve (std::size (presets));
    for (auto& p : presets) presetCopy.emplace_back (juce::String (p.first), p.second);

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this),
        [safe, presetCopy] (int result)
        {
            if (result <= 0) return;
            auto* self = safe.getComponent();
            if (self == nullptr) return;
            if (result == 1001)
            {
                self->nameLabel.showEditor();
                return;
            }
            if (result == 1010) { self->openPluginPicker();      return; }
            if (result == 1011) { self->unloadPluginSlot();      return; }
            if (result == 1012) { self->pluginSlot.clearAutoBypass(); return; }
            if (result >= 2000 && result < 2000 + Session::kNumTracks)
            {
                const int dest = result - 2000;
                auto& um = self->engine.getUndoManager();
                um.beginNewTransaction ("Clone track");
                um.perform (new CloneTrackAction (self->session, self->engine,
                                                    self->trackIndex, dest));
                return;
            }
            if (result >= 2100 && result <= 2108)
            {
                const int gid = result - 2100;   // 0 = ungrouped, 1..8
                self->track.strip.faderGroupId.store (gid, std::memory_order_relaxed);
                return;
            }
            const int idx = result - 1;
            if (idx >= 0 && idx < (int) presetCopy.size())
                self->applyTrackColour (juce::Colour (presetCopy[(size_t) idx].second));
        });
}

void ChannelStripComponent::onInputSelectorChanged()
{
    const int id = inputSelector.getSelectedId();
    int src = -2;  // follow track index
    if      (id == 1)            src = -2;
    else if (id == 2)            src = -1;
    else if (id >= 100)          src = id - 100;
    track.inputSource.store (src, std::memory_order_relaxed);
    refreshIoConfigButton();
}

void ChannelStripComponent::onTrackModeChanged()
{
    // ComboBox IDs: 1 = Mono, 2 = Stereo, 3 = MIDI. Stored as int on the
    // Track so the audio thread can read it lock-free.
    const int id = modeSelector.getSelectedId();
    const int mode = juce::jlimit (0, 2, id - 1);  // 0..2 = Track::Mode

    // Auto-unload a mode-mismatched plugin: the picker filter prevents
    // loading the wrong type in the first place, but flipping a track's
    // mode after-the-fact bypasses that gate and would leave the strip
    // with a plugin that's silent (effect on a MIDI strip ignores MIDI
    // and processes silence; instrument on an audio strip ignores audio
    // input). Unloading here keeps the rule consistent and avoids the
    // confusing-silence trap. Editor goes with the slot via
    // unloadPluginSlot, which closes the modal first.
    if (pluginSlot.isLoaded())
    {
        const bool willBeMidi   = (mode == (int) Track::Mode::Midi);
        const bool isInstrument = pluginSlot.isLoadedPluginInstrument();
        if (willBeMidi != isInstrument)
            unloadPluginSlot();
    }

    track.mode.store (mode, std::memory_order_relaxed);
    refreshInputSelectorVisibility();
    refreshPluginSlotButton();
    refreshIoConfigButton();
    // If the I/O popup is open it needs to re-lay out (rows differ per
    // mode: 2 for audio, 4 for MIDI).
    if (auto* box = activeIoBox.getComponent())
    {
        if (auto* body = box->findChildWithID ({}); body == nullptr)
        {
            // CallOutBox holds the content as its sole non-internal child.
            // Walk children and call resized on the first IoConfigPopup-ish
            // component.
            for (int i = 0; i < box->getNumChildComponents(); ++i)
                if (auto* c = box->getChildComponent (i)) c->resized();
        }
    }
    // Resize so the layout reflects the new mode (extra dropdown for stereo,
    // hidden audio dropdown for MIDI).
    resized();
    repaint();
}

void ChannelStripComponent::MidiActivityLed::paint (juce::Graphics& g)
{
    auto area = getLocalBounds().toFloat().reduced (1.5f);
    if (area.isEmpty()) return;
    // Rim + body. Lit = bright green; dim = dark green so the LED is always
    // visible (the user knows the LED exists even when no MIDI is flowing).
    const juce::Colour rim  (0xff202024);
    const juce::Colour off  (0xff2a4a30);
    const juce::Colour onG  (0xff7afb8a);
    g.setColour (rim);
    g.drawEllipse (area, 1.0f);
    g.setColour (lit ? onG : off);
    g.fillEllipse (area.reduced (1.5f));
}

void ChannelStripComponent::refreshInputSelectorVisibility()
{
    const int mode = juce::jlimit (0, 2, track.mode.load (std::memory_order_relaxed));
    const bool isMono   = (mode == 0);
    const bool isStereo = (mode == 1);
    const bool isMidi   = (mode == 2);

    // Tracking-stage selectors hide entirely when the strip is in
    // Mixing mode — without this gate, post-setMixingMode calls (track
    // mode change, plugin load triggering a re-layout) flip them back
    // on and leave stale rows above the EQ on MIDI / stereo strips.
    const bool trackingVisible = ! mixingMode;
    inputSelector      .setVisible (trackingVisible && (isMono || isStereo));
    inputSelectorR     .setVisible (trackingVisible && isStereo);
    midiInputSelector  .setVisible (trackingVisible && isMidi);
    midiChannelSelector.setVisible (trackingVisible && isMidi);
    midiActivityLed    .setVisible (trackingVisible && isMidi);
    midiOutputSelector .setVisible (trackingVisible && isMidi);
}

void ChannelStripComponent::onHpfKnobChanged()
{
    const float freq = (float) hpfKnob.getValue();
    track.strip.hpfFreq.store (freq, std::memory_order_relaxed);
    // Bypass the HPF DSP entirely when the knob is at the floor - saves
    // 16 channels worth of biquad cost when nobody's using HPF.
    track.strip.hpfEnabled.store (freq > ChannelStripParams::kHpfOffHz + 0.5f,
                                   std::memory_order_relaxed);
}

void ChannelStripComponent::refreshEqTypeButton()
{
    eqTypeButton.setButtonText (eqTypeButton.getToggleState() ? "G" : "E");
}

void ChannelStripComponent::setCompMode (int modeIndex)
{
    track.strip.compMode.store (modeIndex, std::memory_order_relaxed);
    refreshCompModeButtonState();
    refreshCompKnobVisibility();
    resized();  // mode swap reshapes the comp body
}

void ChannelStripComponent::refreshCompModeButtonState()
{
    const int m = juce::jlimit (0, 2, track.strip.compMode.load (std::memory_order_relaxed));
    const char* names[] = { "OPTO", "FET", "VCA" };
    if (compModeButton.getButtonText() != names[m])
        compModeButton.setButtonText (names[m]);

    // Illuminate the button when the comp is engaged so the user gets a
    // single-glance "is it on?" cue without a separate LED widget.
    const bool on = track.strip.compEnabled.load (std::memory_order_relaxed);
    if (compModeButton.getToggleState() != on)
        compModeButton.setToggleState (on, juce::dontSendNotification);
}

void ChannelStripComponent::refreshCompKnobVisibility()
{
    // In compact mode the entire COMP section is hidden (replaced by the
    // compCompactButton). Bail before touching per-mode knob visibility.
    if (compactMode) return;

    const int m = juce::jlimit (0, 2, track.strip.compMode.load (std::memory_order_relaxed));
    const bool isOpto = (m == 0), isFet = (m == 1), isVca = (m == 2);
    optoPeakRedKnob.setVisible (isOpto);  optoPeakRedLabel.setVisible (isOpto);
    optoGainKnob   .setVisible (isOpto);  optoGainLabel   .setVisible (isOpto);
    optoLimitButton.setVisible (isOpto);

    fetInputKnob   .setVisible (isFet);   fetInputLabel   .setVisible (isFet);
    fetOutputKnob  .setVisible (isFet);   fetOutputLabel  .setVisible (isFet);
    fetAttackKnob  .setVisible (isFet);   fetAttackLabel  .setVisible (isFet);
    fetReleaseKnob .setVisible (isFet);   fetReleaseLabel .setVisible (isFet);
    fetRatioKnob   .setVisible (isFet);   fetRatioLabel   .setVisible (isFet);

    vcaRatioKnob   .setVisible (isVca);   vcaRatioLabel   .setVisible (isVca);
    vcaAttackKnob  .setVisible (isVca);   vcaAttackLabel  .setVisible (isVca);
    vcaReleaseKnob .setVisible (isVca);   vcaReleaseLabel .setVisible (isVca);
    vcaOutputKnob  .setVisible (isVca);   vcaOutputLabel  .setVisible (isVca);
}

void ChannelStripComponent::showCompModeMenu()
{
    // Bypass + mode are orthogonal: the toggle at the top flips compEnabled
    // without changing compMode (so the user can A/B mode timbres while
    // bypassed). The mode items below set compMode only - they don't
    // implicitly enable the comp.
    const int  m  = juce::jlimit (0, 2, track.strip.compMode.load (std::memory_order_relaxed));
    const bool on = track.strip.compEnabled.load (std::memory_order_relaxed);
    juce::PopupMenu menu;
    menu.addItem (1, on ? "Bypass" : "Engage", true, false);
    menu.addSeparator();
    menu.addItem (2, "OPTO - program-dependent, smooth", true, m == 0);
    menu.addItem (3, "FET - fast attack, gritty under load", true, m == 1);
    menu.addItem (4, "VCA - clean, predictable", true, m == 2);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&compModeButton),
        [safeThis = juce::Component::SafePointer<ChannelStripComponent> (this), on]
        (int chosen)
        {
            auto* self = safeThis.getComponent();
            if (self == nullptr) return;
            if (chosen == 1)
                self->setCompEnabled (! on);
            else if (chosen >= 2 && chosen <= 4)
                self->setCompMode (chosen - 2);
        });
}

void ChannelStripComponent::setCompEnabled (bool enabled)
{
    track.strip.compEnabled.store (enabled, std::memory_order_relaxed);
    refreshCompModeButtonState();   // only the lit state changed; knobs unchanged
}

void ChannelStripComponent::armCompOnUserEdit()
{
    if (! track.strip.compEnabled.load (std::memory_order_relaxed))
    {
        track.strip.compEnabled.store (true, std::memory_order_relaxed);
        refreshCompModeButtonState();
    }
}

static void drawSectionPlaceholder (juce::Graphics& g, juce::Rectangle<int> r,
                                    const juce::String& label, juce::Colour accent)
{
    if (r.isEmpty()) return;
    g.setColour (juce::Colour (0xff222226));
    g.fillRoundedRectangle (r.toFloat(), 3.0f);
    g.setColour (accent.withAlpha (0.45f));
    g.drawRoundedRectangle (r.toFloat().reduced (0.5f), 3.0f, 0.8f);
    g.setColour (accent.withAlpha (0.85f));
    g.setFont (juce::Font (juce::FontOptions (9.5f, juce::Font::bold)));
    g.drawText (label, r.reduced (4, 2), juce::Justification::centredTop, false);
}

void ChannelStripComponent::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (1.5f);
    g.setColour (juce::Colour (0xff1a1a1c));
    g.fillRoundedRectangle (r, 4.0f);
    g.setColour (track.colour.withAlpha (0.85f));
    g.fillRoundedRectangle (r.removeFromTop (4.0f), 2.0f);
    g.setColour (juce::Colour (0xff2a2a2e));
    g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (1.5f), 4.0f, 1.0f);

    // EQ region: full background (header + HPF row + 4 band rows)
    if (! eqArea.isEmpty())
    {
        g.setColour (juce::Colour (0xff1f231e));
        g.fillRoundedRectangle (eqArea.toFloat(), 3.0f);
        g.setColour (juce::Colour (0xff80c090).withAlpha (0.40f));
        g.drawRoundedRectangle (eqArea.toFloat().reduced (0.5f), 3.0f, 0.8f);
    }

    if (! compArea.isEmpty())
    {
        g.setColour (juce::Colour (0xff241f1c));
        g.fillRoundedRectangle (compArea.toFloat(), 3.0f);
        g.setColour (juce::Colour (fourKColors::kCompGold).withAlpha (0.40f));
        g.drawRoundedRectangle (compArea.toFloat().reduced (0.5f), 3.0f, 0.8f);
    }

    // SEND box (Mixing stage only) - same framed-block shape as EQ/COMP
    // with the AUX purple accent so the row reads as a coherent section
    // instead of floating loose knobs above PAN.
    if (! auxRowArea.isEmpty())
    {
        g.setColour (juce::Colour (0xff1f1d24));
        g.fillRoundedRectangle (auxRowArea.toFloat(), 3.0f);
        g.setColour (juce::Colour (0xff9080c0).withAlpha (0.40f));
        g.drawRoundedRectangle (auxRowArea.toFloat().reduced (0.5f), 3.0f, 0.8f);
    }

    // ── Channel input LED (next to the fader). Shows the pre-fader signal
    //    level so the engineer always sees what's hitting the strip.
    //    Threshold + GR meters live INSIDE the COMP section now, so this is
    //    just a clean input bar with a peak-hold tick.
    constexpr float kFloorDb   = -60.0f;
    constexpr float kCeilingDb =  +6.0f;   // match kFaderTicks's top label
    auto dbToFrac = [&] (float db) {
        return juce::jlimit (0.0f, 1.0f, (db - kFloorDb) / (kCeilingDb - kFloorDb));
    };

    if (! inputMeterArea.isEmpty())
    {
        const bool stereoMode = (track.mode.load (std::memory_order_relaxed)
                                  == (int) Track::Mode::Stereo);

        // Single bar (mono / midi) or split L|R bars (stereo) in the same
        // total meter footprint. Stereo halves the bar width so both fit
        // without expanding the meter column.
        auto drawBar = [&] (juce::Rectangle<float> bar, float dispDb, float peakDb)
        {
            g.setColour (juce::Colour (0xff060608));
            g.fillRoundedRectangle (bar, 1.5f);
            g.setColour (juce::Colour (0xff2a2a30));
            g.drawRoundedRectangle (bar, 1.5f, 0.5f);

            // LED-style colour zones — hard transitions at -5 dB (green ->
            // yellow) and +5 dB (yellow -> red). Bright saturated values
            // matching the reference image's hardware-LED look instead of
            // the prior soft gradient.
            const juce::Colour kLedGreen  (0xff20d040);
            const juce::Colour kLedYellow (0xfff0e020);
            const juce::Colour kLedRed    (0xffff2020);
            auto colourForDb = [&] (float db) -> juce::Colour
            {
                if (db >=  5.0f) return kLedRed;
                if (db >= -5.0f) return kLedYellow;
                return kLedGreen;
            };

            const float frac = dbToFrac (dispDb);
            if (frac > 0.001f)
            {
                const float fillH = (bar.getHeight() - 2.0f) * frac;
                const float x = bar.getX() + 1.0f;
                const float w = bar.getWidth() - 2.0f;
                const float y = bar.getBottom() - 1.0f - fillH;
                const auto fillRect = juce::Rectangle<float> (x, y, w, fillH);

                // Soft outer glow under the fill — tip-colour driven so the
                // glow shifts with the meter peak.
                const auto tipCol = colourForDb (dispDb);
                g.setColour (tipCol.withAlpha (0.20f));
                g.fillRect (fillRect.expanded (1.5f));
                g.setColour (tipCol.withAlpha (0.10f));
                g.fillRect (fillRect.expanded (3.0f));

                // Three hard zones stacked from the top of the fill down.
                // Each zone's pixel range = clip(filled vs zone boundary).
                const float yRedTop    = bar.getBottom() - 1.0f - dbToFrac ( 5.0f) * (bar.getHeight() - 2.0f);
                const float yYellowTop = bar.getBottom() - 1.0f - dbToFrac (-5.0f) * (bar.getHeight() - 2.0f);
                const float yFillTop   = y;
                const float yFillBot   = bar.getBottom() - 1.0f;

                auto fillBand = [&] (float top, float bottom, juce::Colour col)
                {
                    if (bottom <= top) return;
                    g.setColour (col);
                    g.fillRect (juce::Rectangle<float> (x, top, w, bottom - top));
                };
                // Red band: fill top -> min(yRedTop, fillBot)
                fillBand (juce::jmax (yFillTop, bar.getY()),
                            juce::jmin (yRedTop, yFillBot),
                            kLedRed);
                // Yellow band: max(fillTop, yRedTop) -> min(yYellowTop, fillBot)
                fillBand (juce::jmax (yFillTop, yRedTop),
                            juce::jmin (yYellowTop, yFillBot),
                            kLedYellow);
                // Green band: max(fillTop, yYellowTop) -> fillBot
                fillBand (juce::jmax (yFillTop, yYellowTop),
                            yFillBot,
                            kLedGreen);
            }

            const float peakFrac = dbToFrac (peakDb);
            if (peakFrac > 0.001f)
            {
                const float y = bar.getBottom() - 1.0f - peakFrac * (bar.getHeight() - 2.0f);
                g.setColour (peakDb >= 5.0f ? juce::Colour (0xffff8080)
                                              : juce::Colour (0xfff0f0f0));
                g.fillRect (juce::Rectangle<float> (bar.getX() + 1.0f, y - 0.5f,
                                                      bar.getWidth() - 2.0f, 1.4f));
            }

            const int segments = juce::jlimit (8, 30, (int) (bar.getHeight() / 3.5f));
            const float segStep = bar.getHeight() / (float) segments;
            g.setColour (juce::Colour (0xff020203));
            for (int i = 1; i < segments; ++i)
            {
                const float yy = bar.getY() + i * segStep;
                g.fillRect (juce::Rectangle<float> (bar.getX() + 1.0f, yy - 0.4f,
                                                      bar.getWidth() - 2.0f, 0.8f));
            }
        };

        if (stereoMode)
        {
            const auto full = inputMeterArea.toFloat();
            const float halfW = (full.getWidth() - 1.0f) * 0.5f;   // 1 px gap between bars
            const auto barL = juce::Rectangle<float> (full.getX(),               full.getY(),
                                                        halfW, full.getHeight());
            const auto barR = juce::Rectangle<float> (full.getX() + halfW + 1.0f, full.getY(),
                                                        halfW, full.getHeight());
            drawBar (barL, displayedInputDb,  inputPeakHoldDb);
            drawBar (barR, displayedInputRDb, inputPeakHoldRDb);

            // Tiny "L" / "R" caption above each bar so the user immediately
            // sees the meter is now stereo.
            g.setColour (juce::Colour (0xffa0a0a8));
            g.setFont (juce::Font (juce::FontOptions (7.5f, juce::Font::bold)));
            g.drawText ("L", juce::Rectangle<float> (barL.getX(), barL.getY() - 9.0f,
                                                       barL.getWidth(), 8.0f),
                          juce::Justification::centred, false);
            g.drawText ("R", juce::Rectangle<float> (barR.getX(), barR.getY() - 9.0f,
                                                       barR.getWidth(), 8.0f),
                          juce::Justification::centred, false);
        }
        else
        {
            drawBar (inputMeterArea.toFloat(), displayedInputDb, inputPeakHoldDb);
        }
    }

    // Fader dB scale - labels in the scale column aligned with the tick
    // marks the LookAndFeel paints across the fader's track. Same set of
    // values as kFaderTicks; format matches the screenshot's absolute-
    // value style ("6", "3", "0", "3", "6", "12", "24", "40", "90").
    if (! meterScaleArea.isEmpty())
    {
        const auto scale = meterScaleArea;
        const auto& range = faderSlider.getNormalisableRange();
        for (const auto& t : kFaderTicks)
        {
            if (t.db < range.start - 0.01f || t.db > range.end + 0.01f) continue;
            const float y = faderYForDb (faderSlider, t.db);
            const bool isZero = (std::abs (t.db) < 0.01f);
            g.setColour (isZero ? juce::Colour (0xffffffff) : juce::Colour (0xffc0c0c8));
            g.setFont (juce::Font (juce::FontOptions (isZero ? 11.5f : 10.5f,
                                                        isZero ? juce::Font::bold
                                                                : juce::Font::plain)));
            const auto rect = juce::Rectangle<float> ((float) scale.getX(), y - 7.0f,
                                                        (float) scale.getWidth(), 14.0f);
            g.drawText (t.label, rect, juce::Justification::centredRight, false);
        }
    }
}

void ChannelStripComponent::resized()
{
    auto area = getLocalBounds().reduced (4);
    area.removeFromTop (6);

    nameLabel.setBounds (area.removeFromTop (20));
    area.removeFromTop (3);

    // Mixing stage swaps the tracking-only block (mode + input + IN/ARM/PRINT)
    // for a row of 4 AUX send knobs. Recording/Mastering keep the original
    // block. setMixingMode() drives this; here we just lay out whatever's
    // currently visible.
    // Tracking-only block (mode selector + input + IN/ARM/PRINT). Hidden in
    // Mixing stage - the AUX send knobs land below the COMP section instead
    // (signal-flow placement: EQ → COMP → SENDS → PAN → fader).
    if (! mixingMode)
    {
        // I/O region: one summary button (opens the I/O config popup with
        // mode + input/output dropdowns) followed by the IN/ARM/PRINT row.
        // Same fixed height for every track type so EQ / COMP / fader line
        // up across audio + MIDI strips.
        constexpr int kIoButtonH = 18;
        constexpr int kIoRowH    = 18;
        constexpr int kIoGap     = 3;
        constexpr int kIoRegionH = kIoButtonH + kIoGap + kIoRowH + 2;
        auto ioRegion = area.removeFromTop (kIoRegionH);

        ioConfigButton.setBounds (ioRegion.removeFromTop (kIoButtonH));
        ioRegion.removeFromTop (kIoGap);

        auto buttonRow = ioRegion.removeFromTop (kIoRowH);
        const int colW = buttonRow.getWidth() / 3;
        monitorButton.setBounds (buttonRow.removeFromLeft (colW).reduced (1));
        armButton    .setBounds (buttonRow.removeFromLeft (colW).reduced (1));
        printButton  .setBounds (buttonRow.reduced (1));
    }

    // Plugin-slot button right below the IN/ARM/PRINT row. Always visible -
    // available in both compact and full modes since it's independent of the
    // EQ/COMP collapse.
    pluginSlotButton.setBounds (area.removeFromTop (20).reduced (2, 0));
    area.removeFromTop (3);

    // In compact mode the EQ + COMP sections collapse to two narrow buttons
    // so the fader / bus assigns / meter / M-S-Ø stay visible when the
    // SUMMARY view is consuming half the window. Click either button to
    // open the full editor as a popup.
    if (compactMode)
    {
        eqCompactButton  .setBounds (area.removeFromTop (24).reduced (2, 0));
        area.removeFromTop (4);
        compCompactButton.setBounds (area.removeFromTop (24).reduced (2, 0));
        area.removeFromTop (8);
        eqArea   = juce::Rectangle<int>();
        compArea = juce::Rectangle<int>();
    }
    else
    {

    // ── EQ block ─ SSL 9000J / E-EQ inspired layout.
    //   • Each band is a 2-column pair: GAIN on the left, FREQ on the right
    //     (same Y, prominent same-size knobs).
    //   • Bell bands (HM, LM) add a smaller Q knob STACKED BELOW the gain in
    //     the same left column - no third column competing for horizontal
    //     space, freq stays at full size on the right.
    //   • HPF lives at the top of the block as a single centred knob.
    //   • Shelf rows are short; bell rows are taller (gain block + Q block).
    // All primary knobs share the pan-knob diameter (26 px) - frees vertical
    // space for the fader and keeps the strip readable at 16-track widths.
    // Q stays smaller (20 px) since it's a subordinate control on bell bands.
    constexpr int kKnobSize    = 26;
    constexpr int kValueLabelH = 14;
    constexpr int kKnobBlockH  = kKnobSize + kValueLabelH + 2;        // 42
    constexpr int kQKnobSize   = 26;                                  // Q matches gain/freq per user directive
    constexpr int kQBlockH     = kQKnobSize + kValueLabelH;           // 40
    constexpr int kFreqYStagger = 2;                                  // tighter SSL nudge - was 4
    constexpr int kEqHeaderH   = 12;  // tighter - pulls HPF closer to the EQ label
    constexpr int kEqHpfRowH   = kKnobBlockH;                         // 42 - no extra padding
    constexpr int kEqShelfRowH = kKnobBlockH + kFreqYStagger + 2;     // 46 - tighter than before
    constexpr int kEqBellRowH  = kKnobBlockH + kQBlockH;              // 82

    // 1 HPF row + 2 shelf rows (HF, LF) + 2 bell rows (HM, LM). No vertical
    // reduce - every pixel of EQ height is used so the knobs hug the header.
    eqArea = area.removeFromTop (kEqHeaderH + kEqHpfRowH + 2 * kEqShelfRowH + 2 * kEqBellRowH);
    {
        auto s = eqArea.reduced (3, 0);
        auto header = s.removeFromTop (kEqHeaderH);
        eqHeaderLabel.setBounds (header.removeFromLeft (header.getWidth() - 26));
        eqTypeButton.setBounds  (header.reduced (1, 1));

        constexpr int kRowLabelW = 28;

        // HPF row - single centred knob, hugging the top of the EQ block.
        {
            auto row = s.removeFromTop (kEqHpfRowH);
            hpfLabel.setBounds (row.removeFromLeft (kRowLabelW));
            const int knobBoundsW = juce::jmin (60, row.getWidth());
            const int hpfX = row.getX() + (row.getWidth() - knobBoundsW) / 2;
            hpfKnob.setBounds (hpfX, row.getY(), knobBoundsW, kKnobBlockH);
        }

        // Band rows. Knobs are top-aligned within each row (no leading
        // padding) - keeps everything pulled up toward the EQ header.
        for (size_t i = 0; i < eqRows.size(); ++i)
        {
            const bool hasQ = eqRows[i].q != nullptr;
            const int rowH = hasQ ? kEqBellRowH : kEqShelfRowH;
            auto row = s.removeFromTop (rowH);
            // Row-label area, vertically aligned to the rotary's centre so
            // "HM" / "LM" / etc sit next to the actual knob, not centred
            // within the row's full height (which puts them next to the
            // value text instead). The label box is sized to the rotary
            // area only (kKnobSize) - JUCE's centred Label vertical-centres
            // the text within those bounds.
            auto labelArea = row.removeFromLeft (kRowLabelW);
            const int rotaryH = kKnobSize;  // height of the actual rotary
            eqRows[i].rowLabel.setBounds (labelArea.getX(), row.getY(),
                                           labelArea.getWidth(), rotaryH);
            if (hasQ)
            {
                const int qRotaryH = kQKnobSize;
                const int qY       = row.getY() + kKnobBlockH;
                eqRows[i].qLabel.setBounds (labelArea.getX(), qY,
                                             labelArea.getWidth(), qRotaryH);
            }

            const int colW = row.getWidth() / 2;
            const int leftX  = row.getX();
            const int rightX = row.getX() + colW;
            const int gainY = row.getY();
            // Bell rows have a Q knob stacked under the gain on the left;
            // the freq sits in its own column on the right and is now
            // vertically centred inside the FULL bell row (between the
            // gain and Q rows). Shelf rows keep the small SSL nudge.
            const int freqY = hasQ
                ? row.getY() + (rowH - kKnobBlockH) / 2
                : gainY + kFreqYStagger;

            eqRows[i].gain->setBounds (leftX,  gainY, colW, kKnobBlockH);
            eqRows[i].freq->setBounds (rightX, freqY, colW, kKnobBlockH);

            if (hasQ)
            {
                const int qW    = juce::jmin (colW, 44);
                const int qX    = leftX + (colW - qW) / 2;
                const int qY    = gainY + kKnobBlockH;
                eqRows[i].q->setBounds (qX, qY, qW, kQBlockH);
            }
        }
    }
    area.removeFromTop (3);

    // COMP region:
    //   Header  : ON button
    //   Mode    : O / F / V
    //   Body    : per-mode knob set on the LEFT, threshold/IN/GR meter on
    //             the RIGHT. Putting the meter inside the comp section
    //             (rather than next to the fader) keeps all comp UI grouped
    //             and frees up the fader column for a taller fader.
    constexpr int kCompKnobSize     = 26;
    constexpr int kCompKnobBlockH   = kCompKnobSize + kValueLabelH + 2;
    constexpr int kCompKnobLabelH   = 10;
    constexpr int kCompKnobRowH     = kCompKnobLabelH + kCompKnobBlockH;  // ~52
    constexpr int kCompKnobGap      = 4;
    constexpr int kCompMeterW       = 26;   // CompMeterStrip handle + 2 bars
    constexpr int kCompMeterGap     = 4;

    // Every mode now fits in 2 rows - Opto pairs PEAK+GAIN with LIMIT shrunk
    // to a small toggle on row 2; FET squeezes IN/OUT/RATIO across row 1 with
    // ATK/REL on row 2; VCA stays at RAT/OUT + ATK/REL. Body height is fixed
    // so the strip layout doesn't shift when the user switches modes.
    constexpr int kCompBodyH = 2 * kCompKnobRowH + kCompKnobGap;
    compArea = area.removeFromTop (16 + 2 + kCompBodyH + 4);
    {
        auto s = compArea.reduced (3, 2);

        compModeButton.setBounds (s.removeFromTop (16));
        s.removeFromTop (2);

        // Body: split into knob area (left) + meter strip (right). Meter
        // shows pre-comp input level, GR, and the threshold drag handle.
        auto body = s.removeFromTop (kCompBodyH);
        auto meterRect = body.removeFromRight (kCompMeterW);
        body.removeFromRight (kCompMeterGap);
        if (compMeter != nullptr)
            compMeter->setBounds (meterRect);

        auto layoutKnobCell = [&] (juce::Rectangle<int> cell,
                                    juce::Slider& knob, juce::Label& label)
        {
            label.setBounds (cell.removeFromTop (kCompKnobLabelH));
            knob.setBounds  (cell.getX(), cell.getY(), cell.getWidth(), kCompKnobBlockH);
        };

        const int currentMode = juce::jlimit (0, 2,
            track.strip.compMode.load (std::memory_order_relaxed));

        if (currentMode == 0)  // Opto: PEAK + GAIN on row 1, LIMIT toggle on row 2
        {
            auto row1 = body.removeFromTop (kCompKnobRowH);
            body.removeFromTop (kCompKnobGap);
            auto row2 = body.removeFromTop (kCompKnobRowH);
            const int colW = row1.getWidth() / 2;
            layoutKnobCell (row1.removeFromLeft (colW), optoPeakRedKnob, optoPeakRedLabel);
            layoutKnobCell (row1,                       optoGainKnob,    optoGainLabel);
            const int limW = juce::jmin (54, row2.getWidth());
            optoLimitButton.setBounds (row2.getX() + (row2.getWidth() - limW) / 2,
                                        row2.getY() + (row2.getHeight() - 18) / 2,
                                        limW, 18);
        }
        else if (currentMode == 1)  // FET: IN/OUT/RATIO row 1, ATK/REL row 2
        {
            auto row1 = body.removeFromTop (kCompKnobRowH);
            body.removeFromTop (kCompKnobGap);
            auto row2 = body.removeFromTop (kCompKnobRowH);
            const int col3W = row1.getWidth() / 3;
            layoutKnobCell (row1.removeFromLeft (col3W), fetInputKnob,   fetInputLabel);
            layoutKnobCell (row1.removeFromLeft (col3W), fetRatioKnob,   fetRatioLabel);
            layoutKnobCell (row1,                        fetOutputKnob,  fetOutputLabel);
            const int col2W = row2.getWidth() / 2;
            layoutKnobCell (row2.removeFromLeft (col2W), fetAttackKnob,  fetAttackLabel);
            layoutKnobCell (row2,                        fetReleaseKnob, fetReleaseLabel);
        }
        else  // VCA: 4 knobs in 2×2 (threshold lives on the meter-strip handle)
        {
            auto row1 = body.removeFromTop (kCompKnobRowH);
            body.removeFromTop (kCompKnobGap);
            auto row2 = body.removeFromTop (kCompKnobRowH);
            const int colW = row1.getWidth() / 2;
            layoutKnobCell (row1.removeFromLeft (colW), vcaRatioKnob,   vcaRatioLabel);
            layoutKnobCell (row1,                       vcaOutputKnob,  vcaOutputLabel);
            layoutKnobCell (row2.removeFromLeft (colW), vcaAttackKnob,  vcaAttackLabel);
            layoutKnobCell (row2,                       vcaReleaseKnob, vcaReleaseLabel);
        }
    }
    area.removeFromTop (3);
    }   // end of else (! compactMode)

    // ── AUX sends (Mixing stage only). Single row of 4 knobs with a slight
    //    vertical zig-zag - even-index knobs sit higher, odd-index sit lower.
    //    Staggering keeps each knob full-size while they share a narrow strip
    //    width that wouldn't allow 4 knobs at the same Y without crowding the
    //    value labels. Sits between COMP and PAN to match signal-flow order:
    //    EQ → COMP → SENDS → PAN → fader.
    if (mixingMode)
    {
        constexpr int kAuxKnobSize  = 24;
        constexpr int kAuxStaggerY  = 10;     // odd knobs offset down by this much
        constexpr int kAuxValueH    = 10;
        constexpr int kAuxBlockH    = kAuxStaggerY + kAuxKnobSize + 2 + kAuxValueH;
        constexpr int kAuxLabelH    = 11;
        constexpr int kAuxLabelGap  = 1;
        constexpr int kAuxRowTotalH = kAuxLabelH + kAuxLabelGap + kAuxBlockH;

        // Capture the framed-box bounds for paint() (drawn in the same
        // style as eqArea / compArea, with the SEND purple accent).
        auxRowArea = area.removeFromTop (kAuxRowTotalH);

        // Lay out the label + knob block inside the box, with a small
        // horizontal padding so the SEND knobs don't kiss the frame.
        auto inner = auxRowArea.reduced (3, 0);
        auto headerRow = inner.removeFromTop (kAuxLabelH);
        inner.removeFromTop (kAuxLabelGap);

        auto block = inner.removeFromTop (kAuxBlockH);
        const int colW = block.getWidth() / ChannelStripParams::kNumAuxSends;

        for (int i = 0; i < ChannelStripParams::kNumAuxSends; ++i)
        {
            auto col = juce::Rectangle<int> (block.getX() + i * colW, block.getY(),
                                                colW, block.getHeight());

            // Index numeral sits centred above its knob column, painted
            // in the matching kAuxColours tint. Same X-grid as the knob
            // below, so it scans as a header for that knob.
            auxIndexLabels[(size_t) i].setBounds (col.getX(), headerRow.getY(),
                                                    col.getWidth(), kAuxLabelH);

            const int knobX = col.getX() + (col.getWidth() - kAuxKnobSize) / 2;
            const int knobY = col.getY() + ((i % 2 == 0) ? 0 : kAuxStaggerY);
            if (auxKnobs[(size_t) i] != nullptr)
                auxKnobs[(size_t) i]->setBounds (knobX, knobY,
                                                  kAuxKnobSize, kAuxKnobSize);

            const int labelY = col.getBottom() - kAuxValueH;
            auxKnobLabels[(size_t) i].setBounds (col.getX(), labelY,
                                                   col.getWidth(), kAuxValueH);
        }

        area.removeFromTop (3);
    }
    else
    {
        // Non-mixing stages don't show the SEND row, so the framed box
        // disappears with it.
        auxRowArea = juce::Rectangle<int>();
    }

    // The horizontal BUSES region used to live here. Bus toggles now sit in
    // a vertical column to the left of the fader (laid out below).

    {
        // Pan section: small "PAN" header above a compact red knob.
        constexpr int kPanKnobSize = 26;
        constexpr int kPanBlockH = kPanKnobSize + 12 + 2;
        constexpr int kPanLabelH = 11;
        auto panArea = area.removeFromTop (kPanLabelH + kPanBlockH + 2);
        panLabel.setBounds (panArea.removeFromTop (kPanLabelH));
        panKnob.setBounds (panArea.getX(), panArea.getY(), panArea.getWidth(), kPanBlockH);
    }
    area.removeFromTop (2);

    auto buttons = area.removeFromBottom (24);
    const int btnW = buttons.getWidth() / 3;
    muteButton .setBounds (buttons.removeFromLeft (btnW).reduced (1));
    soloButton .setBounds (buttons.removeFromLeft (btnW).reduced (1));
    phaseButton.setBounds (buttons.reduced (1));
    area.removeFromBottom (2);

    // Automation mode button - sits as a thin full-width row directly above
    // M/S/Ø. Single label that mirrors the current mode; click cycles.
    auto autoRow = area.removeFromBottom (16);
    autoModeButton.setBounds (autoRow.reduced (1, 0));
    area.removeFromBottom (4);

    // Fader + input meter pinned to the bottom of the strip. To the right of
    // the fader: a small dB scale column (0/-12/-24/-60) and a vertical LED
    // input meter. GR + threshold drag moved into the COMP section, so this
    // column is now slim and the fader gets the reclaimed width.
    constexpr int kMaxFaderHeight  = 360;  // 280 -> 360: bank row left ConsoleView, freed height goes to faders
    constexpr int kPeakLabelH      = 18;
    // Meter column width is mode-aware: in stereo we draw two bars side by
    // side, so we need extra room. Mono / Midi use a narrower column.
    const bool stereoMode = (track.mode.load (std::memory_order_relaxed)
                              == (int) Track::Mode::Stereo);
    const int kMeterWidth = stereoMode ? 18 : 12;
    constexpr int kMeterScaleWidth = 16;
    constexpr int kMeterGap        = 3;
    const int     kPeakColumnW     = kMeterScaleWidth + kMeterGap + kMeterWidth;

    auto faderArea = area;
    if (faderArea.getHeight() > kMaxFaderHeight + kPeakLabelH + 2)
        faderArea = faderArea.removeFromBottom (kMaxFaderHeight + kPeakLabelH + 2);

    // Vertical bus-assign column on the LEFT of the fader, mirroring the
    // meter column on the right. Tascam Model 2400 layout - the bus toggles
    // sit alongside the fader where the engineer's fingers naturally rest.
    constexpr int kBusColumnW   = 18;  // narrower - saves horizontal space for the fader
    constexpr int kBusColumnGap = 3;
    constexpr int kBusButtonH   = 20;  // shorter - sits alongside the fader without stealing height
    constexpr int kBusButtonGap = 2;
    auto busColumn = faderArea.removeFromLeft (kBusColumnW);
    faderArea.removeFromLeft (kBusColumnGap);

    auto meterColumn = faderArea.removeFromRight (kMeterWidth);
    faderArea.removeFromRight (kMeterGap);
    auto scaleColumn = faderArea.removeFromRight (kMeterScaleWidth);
    faderArea.removeFromRight (kMeterGap);

    // Position the 4 bus buttons vertically centred within busColumn.
    {
        const int totalButtonsH = ChannelStripParams::kNumBuses * kBusButtonH
                                + (ChannelStripParams::kNumBuses - 1) * kBusButtonGap;
        int y = busColumn.getY() + (busColumn.getHeight() - totalButtonsH) / 2;
        for (int i = 0; i < ChannelStripParams::kNumBuses; ++i)
        {
            busButtons[(size_t) i]->setBounds (busColumn.getX(), y, kBusColumnW, kBusButtonH);
            y += kBusButtonH + kBusButtonGap;
        }
    }

    // Peak readout beneath the meter column. GR readout retired - the GR bar
    // inside the COMP section is the canonical readout now.
    const int peakRight = meterColumn.getRight();
    const int peakLeft  = peakRight - kPeakColumnW;
    auto peakArea = juce::Rectangle<int> (peakLeft, meterColumn.getBottom() - kPeakLabelH,
                                           kPeakColumnW, kPeakLabelH);
    inputPeakLabel.setBounds (peakArea);
    grPeakLabel  .setVisible (false);

    meterColumn = meterColumn.withTrimmedBottom (kPeakLabelH + 2);
    scaleColumn = scaleColumn.withTrimmedBottom (kPeakLabelH + 2);

    // Hide the legacy "THR" header - threshold drag now lives in the COMP
    // section's meter strip, so the header next to the fader is misleading.
    threshMeterLabel.setVisible (false);

    inputMeterArea = meterColumn;
    meterScaleArea = scaleColumn;
    faderSlider.setBounds (faderArea);
}
} // namespace duskstudio
