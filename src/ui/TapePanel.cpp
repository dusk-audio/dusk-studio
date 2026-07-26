#include "TapePanel.h"
#include "../engine/AudioEngine.h"
#include <algorithm>
#include <cmath>

#if DUSKSTUDIO_HAS_DUSK_DSP
  #include <GUI/TapeReelComponent.h>
#endif

namespace duskstudio
{
namespace
{
const auto kChassis = juce::Colour (0xff261d12);
const auto kAccent  = juce::Colour (0xffd0a060);
const auto kStencil = juce::Colour (0xffe8d8b8);
const auto kKnob    = juce::Colour (0xff1a1a1c);

constexpr int kOuterPad   = 12;
constexpr int kHeaderH    = 24;
constexpr int kHeaderGap  = 8;
constexpr int kDeckH      = 132;
constexpr int kLabelH     = 14;
constexpr int kComboH     = 24;
constexpr int kKnobLabelH = 16;
constexpr int kKnobBlockH = 82;
constexpr int kToggleRowH = 22;

void styleKnob (juce::Slider& k, double mn, double mx, double defaultVal,
                 const juce::String& suffix, int decimals, double skew = 1.0)
{
    k.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    k.setColour (juce::Slider::rotarySliderFillColourId, kKnob);
    k.setColour (juce::Slider::rotarySliderOutlineColourId, juce::Colour (0xff4a3c28));
    k.setColour (juce::Slider::thumbColourId, kAccent.brighter (0.3f));
    k.setRange (mn, mx, mx - mn > 200.0 ? 1.0 : 0.1);
    k.setSkewFactor (skew);   // 1.0 = linear
    k.setDoubleClickReturnValue (true, defaultVal);
    k.setTextBoxStyle (juce::Slider::TextBoxBelow, true, 80, 18);
    k.setColour (juce::Slider::textBoxTextColourId,       juce::Colour (0xffe0d8c8));
    k.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colour (0));
    k.setColour (juce::Slider::textBoxOutlineColourId,    juce::Colour (0));
    k.setNumDecimalPlacesToDisplay (decimals);
    k.setTextValueSuffix (suffix);
    k.setVelocityBasedMode (false);
    k.setVelocityModeParameters (1.0, 1, 0.0, /*userCanPressKeyToSwap*/ true);
}

void styleCaption (juce::Label& l, const juce::String& text)
{
    l.setText (text, juce::dontSendNotification);
    l.setJustificationType (juce::Justification::centred);
    l.setColour (juce::Label::textColourId, kStencil.withAlpha (0.85f));
    l.setFont (juce::Font (juce::FontOptions (10.5f, juce::Font::bold)));
    l.setMinimumHorizontalScale (0.6f);
}

void styleCombo (DuskComboBox& c)
{
    c.setColour (juce::ComboBox::backgroundColourId, juce::Colour (0xff171009));
    c.setColour (juce::ComboBox::textColourId,       kStencil);
    c.setColour (juce::ComboBox::outlineColourId,    juce::Colour (0xff4a3c28));
    c.setColour (juce::ComboBox::arrowColourId,      kAccent);
}

void styleToggle (juce::ToggleButton& t)
{
    t.setMouseClickGrabsKeyboardFocus (false);
    t.setColour (juce::ToggleButton::textColourId,   kStencil);
    t.setColour (juce::ToggleButton::tickColourId,   kAccent);
    t.setColour (juce::ToggleButton::tickDisabledColourId, juce::Colour (0xff4a3c28));
}

int clampId (const DuskComboBox& c, int stored) noexcept
{
    return std::clamp (stored, 0, std::max (0, c.getNumItems() - 1)) + 1;
}

void syncKnob (juce::Slider& k, float value, float epsilon)
{
    if (k.isMouseButtonDown() || k.hasKeyboardFocus (true)) return;
    if (std::abs ((float) k.getValue() - value) > epsilon)
        k.setValue (value, juce::dontSendNotification);
}
} // namespace

TapePanel::TapePanel (MasterBusParams& p, AudioEngine& e)
    : params (p), engine (e)
{
    setOpaque (true);

    enableBtn.setClickingTogglesState (true);
    enableBtn.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff202024));
    enableBtn.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffc08850));
    enableBtn.setColour (juce::TextButton::textColourOffId,  juce::Colour (0xffd0c0a0));
    enableBtn.setColour (juce::TextButton::textColourOnId,   juce::Colour (0xff121214));
    enableBtn.setButtonText ("TAPE");
    enableBtn.setTooltip ("Engage / bypass the tape machine.");
    enableBtn.setToggleState (params.tapeEnabled.load (std::memory_order_relaxed),
                               juce::dontSendNotification);
    enableBtn.onClick = [this]
    {
        params.tapeEnabled.store (enableBtn.getToggleState(), std::memory_order_relaxed);
    };
    addAndMakeVisible (enableBtn);

    auto setupCombo = [this] (DuskComboBox& combo, juce::Label& label,
                                const juce::String& caption,
                                std::initializer_list<const char*> items,
                                std::atomic<int>& atom)
    {
        styleCaption (label, caption);
        addAndMakeVisible (label);

        styleCombo (combo);
        int id = 1;
        for (auto* item : items) combo.addItem (item, id++);
        combo.setSelectedId (clampId (combo, atom.load (std::memory_order_relaxed)),
                              juce::dontSendNotification);
        combo.onChange = [this, &combo, &atom]
        {
            atom.store (std::clamp (combo.getSelectedId() - 1, 0, combo.getNumItems() - 1),
                         std::memory_order_relaxed);
            arm();
        };
        addAndMakeVisible (combo);
    };

    setupCombo (machineCombo, machineLabel, "MACHINE",
                 { "Swiss 800", "Classic 102" }, params.tape.machine);
    setupCombo (speedCombo, speedLabel, "SPEED",
                 { "7.5 IPS", "15 IPS", "30 IPS" }, params.tape.speed);
    setupCombo (typeCombo, typeLabel, "TAPE TYPE",
                 { "456", "GP9", "911", "250" }, params.tape.type);
    setupCombo (pathCombo, pathLabel, "SIGNAL PATH",
                 { "Repro", "Sync", "Input", "Thru" }, params.tape.signalPath);
    setupCombo (eqStdCombo, eqStdLabel, "EQ STD",
                 { "NAB", "CCIR", "AES" }, params.tape.eqStandard);
    setupCombo (calCombo, calLabel, "CAL",
                 { "0 dB", "+3 dB", "+6 dB", "+9 dB" }, params.tape.calibration);

    auto setupKnob = [this] (juce::Slider& knob, juce::Label& label,
                               const juce::String& caption,
                               double mn, double mx, double defaultVal,
                               const juce::String& suffix, int decimals,
                               std::atomic<float>& atom, double skew = 1.0)
    {
        styleCaption (label, caption);
        addAndMakeVisible (label);

        styleKnob (knob, mn, mx, defaultVal, suffix, decimals, skew);
        knob.setValue (atom.load (std::memory_order_relaxed), juce::dontSendNotification);
        knob.onValueChange = [this, &knob, &atom]
        {
            atom.store ((float) knob.getValue(), std::memory_order_relaxed);
            arm();
        };
        addAndMakeVisible (knob);
    };

    setupKnob (inputKnob, inputLabel, "INPUT", -12.0, 12.0, 0.0, " dB", 1,
                params.tape.inputGainDb);
    setupKnob (biasKnob, biasLabel, "BIAS", 0.0, 100.0, 50.0, " %", 0,
                params.tape.bias);
    setupKnob (hpfKnob, hpfLabel, "HPF", 20.0, 500.0, 20.0, " Hz", 0,
                params.tape.highpassHz, 0.5);
    setupKnob (lpfKnob, lpfLabel, "LPF", 3000.0, 20000.0, 20000.0, " Hz", 0,
                params.tape.lowpassHz, 0.5);
    setupKnob (wowKnob, wowLabel, "WOW", 0.0, 100.0, 7.0, " %", 0,
                params.tape.wow);
    setupKnob (flutterKnob, flutterLabel, "FLUTTER", 0.0, 100.0, 3.0, " %", 0,
                params.tape.flutter);
    setupKnob (noiseKnob, noiseLabel, "NOISE", 0.0, 100.0, 0.0, " %", 0,
                params.tape.noiseAmount);
    setupKnob (outputKnob, outputLabel, "OUTPUT", -12.0, 12.0, 0.0, " dB", 1,
                params.tape.outputGainDb);

    inputKnob  .setTooltip ("Level into the tape - drives saturation.");
    biasKnob   .setTooltip ("Record bias. Under-bias adds edge, over-bias dulls the top. Auto cal overrides it.");
    noiseKnob  .setTooltip ("Tape hiss level.");
    outputKnob .setTooltip ("Level out of the tape. Auto comp overrides it.");

    styleToggle (autoCalToggle);
    autoCalToggle.setTooltip ("Calibrate bias for the selected tape type and speed.");
    autoCalToggle.setToggleState (params.tape.autoCal.load (std::memory_order_relaxed),
                                   juce::dontSendNotification);
    autoCalToggle.onClick = [this]
    {
        const bool on = autoCalToggle.getToggleState();
        params.tape.autoCal.store (on, std::memory_order_relaxed);
        biasKnob.setEnabled (! on);
        arm();
    };
    addAndMakeVisible (autoCalToggle);

    styleToggle (autoCompToggle);
    autoCompToggle.setTooltip ("Match output level to input so drive changes don't change loudness.");
    autoCompToggle.setToggleState (params.tape.autoComp.load (std::memory_order_relaxed),
                                    juce::dontSendNotification);
    autoCompToggle.onClick = [this]
    {
        params.tape.autoComp.store (autoCompToggle.getToggleState(), std::memory_order_relaxed);
        arm();
    };
    addAndMakeVisible (autoCompToggle);

    biasKnob.setEnabled (! params.tape.autoCal.load (std::memory_order_relaxed));

    vuMeter = std::make_unique<AnalogVuMeter> (&vuLeft, &vuRight);
    vuMeter->setRichStyle (true);
    addAndMakeVisible (*vuMeter);

#if DUSKSTUDIO_HAS_DUSK_DSP
    supplyReel = std::make_unique<TapeReelComponent>();
    supplyReel->setIsSupplyReel (true);
    supplyReel->setTapeAmount (0.75f);
    addAndMakeVisible (*supplyReel);

    takeupReel = std::make_unique<TapeReelComponent>();
    takeupReel->setIsSupplyReel (false);
    takeupReel->setTapeAmount (0.35f);
    takeupReel->setReelType (TapeReelComponent::ReelType::Cine);
    addAndMakeVisible (*takeupReel);
#endif

    setSize (740, 500);
    startTimerHz (25);
}

TapePanel::~TapePanel() { stopTimer(); }

void TapePanel::arm()
{
    params.tapeEnabled.store (true, std::memory_order_relaxed);
    if (! enableBtn.getToggleState())
        enableBtn.setToggleState (true, juce::dontSendNotification);
}

void TapePanel::syncControls()
{
    const auto& t = params.tape;

    const bool on = params.tapeEnabled.load (std::memory_order_relaxed);
    if (enableBtn.getToggleState() != on)
        enableBtn.setToggleState (on, juce::dontSendNotification);

    auto syncCombo = [] (DuskComboBox& c, const std::atomic<int>& atom)
    {
        const int id = clampId (c, atom.load (std::memory_order_relaxed));
        if (c.getSelectedId() != id) c.setSelectedId (id, juce::dontSendNotification);
    };
    syncCombo (machineCombo, t.machine);
    syncCombo (speedCombo,   t.speed);
    syncCombo (typeCombo,    t.type);
    syncCombo (pathCombo,    t.signalPath);
    syncCombo (eqStdCombo,   t.eqStandard);
    syncCombo (calCombo,     t.calibration);

    syncKnob (inputKnob,   t.inputGainDb .load (std::memory_order_relaxed), 0.05f);
    syncKnob (biasKnob,    t.bias        .load (std::memory_order_relaxed), 0.05f);
    syncKnob (hpfKnob,     t.highpassHz  .load (std::memory_order_relaxed), 0.5f);
    syncKnob (lpfKnob,     t.lowpassHz   .load (std::memory_order_relaxed), 0.5f);
    syncKnob (wowKnob,     t.wow         .load (std::memory_order_relaxed), 0.05f);
    syncKnob (flutterKnob, t.flutter     .load (std::memory_order_relaxed), 0.05f);
    syncKnob (noiseKnob,   t.noiseAmount .load (std::memory_order_relaxed), 0.05f);
    syncKnob (outputKnob,  t.outputGainDb.load (std::memory_order_relaxed), 0.05f);

    const bool autoCal = t.autoCal.load (std::memory_order_relaxed);
    if (autoCalToggle.getToggleState() != autoCal)
    {
        autoCalToggle.setToggleState (autoCal, juce::dontSendNotification);
        biasKnob.setEnabled (! autoCal);
    }

    const bool autoComp = t.autoComp.load (std::memory_order_relaxed);
    if (autoCompToggle.getToggleState() != autoComp)
        autoCompToggle.setToggleState (autoComp, juce::dontSendNotification);
}

void TapePanel::timerCallback()
{
    syncControls();

#if DUSKSTUDIO_HAS_DUSK_DSP
    // MasterBus skips the tape stage while it is off, freezing the core's peak
    // followers at their last value - decay the feed here so the needles fall
    // to rest instead of hanging.
    if (params.tapeEnabled.load (std::memory_order_relaxed))
    {
        const auto vu = engine.getMasterBus().getTapeVu();
        vuLeft .store (vu.outL, std::memory_order_relaxed);
        vuRight.store (vu.outR, std::memory_order_relaxed);
    }
    else
    {
        vuLeft .store (vuLeft .load (std::memory_order_relaxed) * 0.85f, std::memory_order_relaxed);
        vuRight.store (vuRight.load (std::memory_order_relaxed) * 0.85f, std::memory_order_relaxed);
    }

    // Reel speed: 7.5 / 15 / 30 IPS map to the donor component's 1.0 / 1.5 /
    // 2.0 multipliers, wobbled by the wow amount so a heavily-worn machine
    // visibly drifts. Zero stops the reels (the component stops its own timer).
    const bool rolling = ! engine.getTransport().isStopped();
    const int speedIdx = std::clamp (params.tape.speed.load (std::memory_order_relaxed), 0, 2);
    const float base   = speedIdx == 0 ? 1.0f : (speedIdx == 1 ? 1.5f : 2.0f);
    const float wow    = params.tape.wow.load (std::memory_order_relaxed) * 0.01f;

    wowPhase += 0.16f;
    if (wowPhase > juce::MathConstants<float>::twoPi)
        wowPhase -= juce::MathConstants<float>::twoPi;

    const float multiplier = rolling ? base * (1.0f + wow * 0.08f * std::sin (wowPhase))
                                     : 0.0f;
    if (supplyReel != nullptr) supplyReel->setSpeed (multiplier);
    if (takeupReel != nullptr) takeupReel->setSpeed (multiplier);
#endif
}

void TapePanel::paint (juce::Graphics& g)
{
    g.fillAll (kChassis);
    g.setColour (kAccent.withAlpha (0.45f));
    g.drawRect (getLocalBounds(), 1);

    g.setColour (kStencil);
    g.setFont (juce::Font (juce::FontOptions (14.0f, juce::Font::bold)));
    g.drawText ("TAPE MACHINE",
                 juce::Rectangle<int> (kOuterPad, kOuterPad, getWidth() / 2, kHeaderH),
                 juce::Justification::centredLeft, false);

    if (! deckArea.isEmpty())
    {
        g.setColour (juce::Colour (0xff15100a));
        g.fillRoundedRectangle (deckArea.toFloat(), 4.0f);
        g.setColour (kAccent.withAlpha (0.30f));
        g.drawRoundedRectangle (deckArea.toFloat().reduced (0.5f), 4.0f, 0.8f);
    }

    for (auto band : { comboArea, knobArea })
    {
        if (band.isEmpty()) continue;
        g.setColour (juce::Colour (0xff1d160e));
        g.fillRoundedRectangle (band.toFloat(), 3.0f);
        g.setColour (kAccent.withAlpha (0.18f));
        g.drawRoundedRectangle (band.toFloat().reduced (0.5f), 3.0f, 0.8f);
    }
}

void TapePanel::resized()
{
    auto area = getLocalBounds().reduced (kOuterPad);

    auto header = area.removeFromTop (kHeaderH);
    enableBtn.setBounds (header.removeFromRight (60));
    area.removeFromTop (kHeaderGap);

    deckArea = area.removeFromTop (kDeckH);
    {
        auto deck = deckArea.reduced (8);
        const int reelSize = std::min (deck.getHeight(), 116);
        auto left  = deck.removeFromLeft (reelSize);
        auto right = deck.removeFromRight (reelSize);
       #if DUSKSTUDIO_HAS_DUSK_DSP
        if (supplyReel != nullptr) supplyReel->setBounds (left .withSizeKeepingCentre (reelSize, reelSize));
        if (takeupReel != nullptr) takeupReel->setBounds (right.withSizeKeepingCentre (reelSize, reelSize));
       #else
        juce::ignoreUnused (left, right);
       #endif
        if (vuMeter != nullptr) vuMeter->setBounds (deck.reduced (10, 0));
    }
    area.removeFromTop (10);

    comboArea = area.removeFromTop (kLabelH + kComboH + 12);
    {
        auto row = comboArea.reduced (6);
        DuskComboBox* combos[] { &machineCombo, &speedCombo, &typeCombo,
                                 &pathCombo, &eqStdCombo, &calCombo };
        juce::Label* labels[] { &machineLabel, &speedLabel, &typeLabel,
                                &pathLabel, &eqStdLabel, &calLabel };
        constexpr int kNumCombos = 6;
        const int colW = row.getWidth() / kNumCombos;
        for (int i = 0; i < kNumCombos; ++i)
        {
            auto cell = (i == kNumCombos - 1) ? row : row.removeFromLeft (colW);
            labels[i]->setBounds (cell.removeFromTop (kLabelH));
            combos[i]->setBounds (cell.reduced (4, 0).withHeight (kComboH));
        }
    }
    area.removeFromTop (10);

    auto toggles = area.removeFromBottom (kToggleRowH);
    {
        auto row = toggles.reduced (8, 0);
        autoCalToggle .setBounds (row.removeFromLeft (110));
        row.removeFromLeft (16);
        autoCompToggle.setBounds (row.removeFromLeft (120));
    }
    area.removeFromBottom (8);

    knobArea = area;
    {
        auto rows = knobArea.reduced (6);
        juce::Slider* knobs[] { &inputKnob, &biasKnob, &hpfKnob, &lpfKnob,
                                &wowKnob, &flutterKnob, &noiseKnob, &outputKnob };
        juce::Label* labels[] { &inputLabel, &biasLabel, &hpfLabel, &lpfLabel,
                                &wowLabel, &flutterLabel, &noiseLabel, &outputLabel };
        constexpr int kPerRow = 4;
        const int rowH = std::min (kKnobLabelH + kKnobBlockH, rows.getHeight() / 2);
        for (int r = 0; r < 2; ++r)
        {
            auto rowArea = rows.removeFromTop (rowH);
            const int colW = rowArea.getWidth() / kPerRow;
            for (int c = 0; c < kPerRow; ++c)
            {
                const int i = r * kPerRow + c;
                auto cell = (c == kPerRow - 1) ? rowArea : rowArea.removeFromLeft (colW);
                labels[i]->setBounds (cell.removeFromTop (kKnobLabelH));
                knobs[i]->setBounds (cell.reduced (6, 0));
            }
        }
    }
}
} // namespace duskstudio
