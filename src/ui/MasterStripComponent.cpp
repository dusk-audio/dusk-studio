#include "MasterStripComponent.h"
#include "../engine/AudioEngine.h"
#include "DimOverlay.h"
#include "DuskStudioLookAndFeel.h"
#include "TapeMachineModalEditor.h"

#if DUSKSTUDIO_HAS_DUSK_DSP
  #include "PluginProcessor.h"   // TapeMachineAudioProcessor + createEditor
#endif

namespace duskstudio
{
namespace
{
void styleSmallKnob (juce::Slider& s, double minV, double maxV, double midPt,
                      double initialV, juce::Colour col, const juce::String& suffix,
                      int decimals)
{
    s.setRange (minV, maxV, 0.01);
    if (midPt > minV && midPt < maxV)
        s.setSkewFactorFromMidPoint (midPt);
    s.setValue (initialV, juce::dontSendNotification);
    s.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 48, 14);
    s.setColour (juce::Slider::rotarySliderFillColourId, col);
    s.setColour (juce::Slider::rotarySliderOutlineColourId, juce::Colour (0xff2a2a2e));
    s.setColour (juce::Slider::thumbColourId, col.brighter (0.3f));
    s.setColour (juce::Slider::textBoxTextColourId, juce::Colour (0xffd0d0d0));
    s.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    s.setColour (juce::Slider::textBoxOutlineColourId,    juce::Colours::transparentBlack);
    s.setNumDecimalPlacesToDisplay (decimals);
    s.setTextValueSuffix (suffix);
}

void styleSmallLabel (juce::Label& lbl, const juce::String& text, juce::Colour col)
{
    lbl.setText (text, juce::dontSendNotification);
    lbl.setJustificationType (juce::Justification::centred);
    lbl.setColour (juce::Label::textColourId, col);
    lbl.setFont (juce::Font (juce::FontOptions (8.5f, juce::Font::bold)));
}
} // namespace

MasterStripComponent::MasterStripComponent (MasterBusParams& p,
                                              Session& s,
                                              AudioEngine& e,
                                              ::TapeMachineAudioProcessor* tapeProc)
    : params (p), session (s), engine (e), tapeProcessorPtr (tapeProc)
{
    nameLabel.setText ("MASTER", juce::dontSendNotification);
    nameLabel.setJustificationType (juce::Justification::centred);
    nameLabel.setColour (juce::Label::textColourId, juce::Colours::white);
    nameLabel.setFont (juce::Font (juce::FontOptions (14.0f, juce::Font::bold)));
    addAndMakeVisible (nameLabel);

    // Analog VU meter, fed by the post-master RMS atoms so it tracks the
    // exact stereo signal that hits the audio device with VU-style 300 ms
    // ballistics applied to the linear RMS amplitudes. Two needles (L + R).
    vuMeter = std::make_unique<AnalogVuMeter> (
        &params.meterPostMasterRmsL, &params.meterPostMasterRmsR);
    addAndMakeVisible (*vuMeter);

    auto styleToggle = [] (juce::TextButton& b, juce::Colour onColour)
    {
        b.setClickingTogglesState (true);
        b.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff202024));
        b.setColour (juce::TextButton::buttonOnColourId, onColour);
        b.setColour (juce::TextButton::textColourOffId,  juce::Colour (0xffb0a080));
        b.setColour (juce::TextButton::textColourOnId,   juce::Colour (0xff121214));
    };

    // EQ + comp section toggles + their parameter knobs.
    const auto pultecGold = juce::Colour (0xffd09050);
    const auto compGold   = juce::Colour (0xffe0c050);

    styleToggle (eqButton, pultecGold);
    eqButton.setToggleState (params.eqEnabled.load (std::memory_order_relaxed),
                              juce::dontSendNotification);
    eqButton.setTooltip ("Pultec-style Tube EQ on/off");
    eqButton.onClick = [this]
    {
        params.eqEnabled.store (eqButton.getToggleState(), std::memory_order_relaxed);
    };
    addAndMakeVisible (eqButton);

    // Suffixes intentionally empty - section labels (LF / HF+ / DRIVE / OUT)
    // already convey the unit, and a 6-char " dB"/" ms" suffix won't fit the
    // 38-px text box without truncating to "...".
    styleSmallKnob (eqLfBoost,   0.0,  10.0,   3.0, params.eqLfBoost.load(),       pultecGold, "", 1);
    styleSmallKnob (eqHfBoost,   0.0,  10.0,   3.0, params.eqHfBoost.load(),       pultecGold, "", 1);
    styleSmallKnob (eqHfAtten,   0.0,  10.0,   3.0, params.eqHfAtten.load(),       pultecGold, "", 1);
    styleSmallKnob (eqTubeDrive, 0.0,   1.0,   0.3, params.eqTubeDrive.load(),     pultecGold, "", 2);
    styleSmallKnob (eqOutputGain,-12.0, 12.0,  0.0, params.eqOutputGainDb.load(),  pultecGold, "", 1);

    eqLfBoost   .onValueChange = [this] { params.eqLfBoost     .store ((float) eqLfBoost   .getValue(), std::memory_order_relaxed); };
    eqHfBoost   .onValueChange = [this] { params.eqHfBoost     .store ((float) eqHfBoost   .getValue(), std::memory_order_relaxed); };
    eqHfAtten   .onValueChange = [this] { params.eqHfAtten     .store ((float) eqHfAtten   .getValue(), std::memory_order_relaxed); };
    eqTubeDrive .onValueChange = [this] { params.eqTubeDrive   .store ((float) eqTubeDrive .getValue(), std::memory_order_relaxed); };
    eqOutputGain.onValueChange = [this] { params.eqOutputGainDb.store ((float) eqOutputGain.getValue(), std::memory_order_relaxed); };

    addAndMakeVisible (eqLfBoost);  addAndMakeVisible (eqHfBoost);
    addAndMakeVisible (eqHfAtten); addAndMakeVisible (eqTubeDrive);
    addAndMakeVisible (eqOutputGain);

    styleSmallLabel (eqLfLabel,      "LF",     pultecGold);
    styleSmallLabel (eqHfBoostLabel, "HF+",    pultecGold);
    // U+2212 minus. Construct via CharPointer_UTF8 - juce::String's const char*
    // ctor uses the system locale, which mangles UTF-8 into garbage on Linux
    // ("HFâ\x88\x92" instead of "HF−").
    styleSmallLabel (eqHfAttenLabel,
                      juce::String (juce::CharPointer_UTF8 ("HF\xe2\x88\x92")),
                      pultecGold);
    styleSmallLabel (eqDriveLabel,   "DRIVE",  pultecGold);
    styleSmallLabel (eqOutLabel,     "OUT",    pultecGold);
    addAndMakeVisible (eqLfLabel);   addAndMakeVisible (eqHfBoostLabel);
    addAndMakeVisible (eqHfAttenLabel); addAndMakeVisible (eqDriveLabel);
    addAndMakeVisible (eqOutLabel);

    styleToggle (compButton, compGold);
    compButton.setToggleState (params.compEnabled.load (std::memory_order_relaxed),
                                juce::dontSendNotification);
    compButton.setTooltip ("Bus compressor on/off (UniversalCompressor in Bus mode)");
    compButton.onClick = [this]
    {
        params.compEnabled.store (compButton.getToggleState(), std::memory_order_relaxed);
    };
    addAndMakeVisible (compButton);

    styleSmallKnob (compThreshold, -30.0,  0.0,  -10.0, params.compThreshDb.load(),   compGold, "",   1);
    styleSmallKnob (compRatio,       1.0, 10.0,    4.0, params.compRatio.load(),       compGold, ":1", 1);
    styleSmallKnob (compAttack,      0.1, 50.0,    5.0, params.compAttackMs.load(),    compGold, "",   1);
    styleSmallKnob (compRelease,    50.0,1000.0, 200.0, params.compReleaseMs.load(),   compGold, "",   0);
    styleSmallKnob (compMakeup,    -10.0, 20.0,    0.0, params.compMakeupDb.load(),    compGold, "",   1);

    compThreshold.onValueChange = [this] { params.compThreshDb .store ((float) compThreshold.getValue(), std::memory_order_relaxed); };
    compRatio    .onValueChange = [this] { params.compRatio    .store ((float) compRatio    .getValue(), std::memory_order_relaxed); };
    compAttack   .onValueChange = [this] { params.compAttackMs .store ((float) compAttack   .getValue(), std::memory_order_relaxed); };
    compMakeup   .onValueChange = [this] { params.compMakeupDb .store ((float) compMakeup   .getValue(), std::memory_order_relaxed); };

    // SSL-style release: the top of the knob's travel = AUTO. Below that
    // the user dials a continuous release in ms; the display reads "AUTO"
    // only at the very top.
    compRelease.textFromValueFunction = [] (double v) -> juce::String
    {
        return v >= 999.5 ? juce::String ("AUTO") : juce::String ((int) std::round (v));
    };
    compRelease.valueFromTextFunction = [] (const juce::String& s) -> double
    {
        return s.trim().equalsIgnoreCase ("auto") ? 1000.0 : (double) s.getDoubleValue();
    };
    compRelease.onValueChange = [this]
    {
        const double v = compRelease.getValue();
        const bool autoOn = v >= 999.5;
        params.compReleaseAuto.store (autoOn, std::memory_order_relaxed);
        if (! autoOn)
            params.compReleaseMs.store ((float) v, std::memory_order_relaxed);
    };
    // Sync initial slider position with the auto flag so the knob lands
    // at the AUTO detent when sessions saved in Auto mode are restored.
    if (params.compReleaseAuto.load (std::memory_order_relaxed))
        compRelease.setValue (1000.0, juce::dontSendNotification);
    compRelease.updateText();

    addAndMakeVisible (compThreshold); addAndMakeVisible (compRatio);
    addAndMakeVisible (compAttack);    addAndMakeVisible (compRelease);
    addAndMakeVisible (compMakeup);

    styleSmallLabel (compThrLabel, "THR", compGold);
    styleSmallLabel (compRatLabel, "RAT", compGold);
    styleSmallLabel (compAtkLabel, "ATK", compGold);
    styleSmallLabel (compRelLabel, "REL", compGold);
    styleSmallLabel (compMakLabel, "MAK", compGold);
    addAndMakeVisible (compThrLabel); addAndMakeVisible (compRatLabel);
    addAndMakeVisible (compAtkLabel); addAndMakeVisible (compRelLabel);
    addAndMakeVisible (compMakLabel);

    styleToggle (tapeButton, juce::Colour (0xffd0a060));

    tapeButton.setToggleState (params.tapeEnabled.load (std::memory_order_relaxed),
                                juce::dontSendNotification);
    tapeButton.setTooltip ("Master tape saturation on/off - harmonic colour at the bus output");
    tapeButton.onClick = [this]
    {
        params.tapeEnabled.store (tapeButton.getToggleState(), std::memory_order_relaxed);
    };
    addAndMakeVisible (tapeButton);

    // Gear next to TAPE opens the full TapeMachine editor centered + dimmed.
    // Not a toggle - momentary trigger.
    tapeGearButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff2a2a2e));
    tapeGearButton.setColour (juce::TextButton::textColourOffId, juce::Colour (0xffd0a060));
    tapeGearButton.setTooltip ("Open the full tape machine editor");
    tapeGearButton.onClick = [this] { openTapeMachineModal(); };
    addAndMakeVisible (tapeGearButton);

    faderSlider.setRange (ChannelStripParams::kFaderMinDb, ChannelStripParams::kFaderMaxDb, 0.1);
    faderSlider.setSkewFactorFromMidPoint (-12.0);
    faderSlider.setValue (params.faderDb.load (std::memory_order_relaxed), juce::dontSendNotification);
    faderSlider.setDoubleClickReturnValue (true, 0.0);
    faderSlider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 64, 18);
    faderSlider.setTextValueSuffix (" dB");
    faderSlider.onValueChange = [this]
    {
        params.faderDb.store ((float) faderSlider.getValue(), std::memory_order_relaxed);
    };
    faderSlider.onDragStart = [this]
    {
        params.faderTouched.store (true, std::memory_order_release);
    };
    faderSlider.onDragEnd = [this]
    {
        params.faderTouched.store (false, std::memory_order_release);
    };
    faderSlider.addMouseListener (this, false);
    addAndMakeVisible (faderSlider);

    // Automation mode button — cycles Off / R / W / T via popup.
    autoModeButton.setTooltip ("Master automation: Off / Read / Write / Touch");
    autoModeButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff222226));
    autoModeButton.onClick = [this] { showAutoModeMenu(); };
    {
        const int amode = params.automationMode.load (std::memory_order_relaxed);
        autoModeButton.setButtonText (amode == (int) AutomationMode::Off   ? "Off"
                                       : amode == (int) AutomationMode::Read  ? "R"
                                       : amode == (int) AutomationMode::Write ? "W"
                                                                              : "T");
    }
    addAndMakeVisible (autoModeButton);

    // Output meter readouts (peak dBFS + GR dB).
    auto styleReadout = [] (juce::Label& lbl, juce::Colour col)
    {
        lbl.setJustificationType (juce::Justification::centred);
        lbl.setColour (juce::Label::textColourId,        col);
        lbl.setColour (juce::Label::backgroundColourId,  juce::Colour (0xff0a0a0c));
        // No outline - sits next to the fader textbox; the 1 px border looked
        // like the two boxes were overlapping.
        lbl.setColour (juce::Label::outlineColourId,     juce::Colours::transparentBlack);
        lbl.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                                                      11.0f, juce::Font::bold)));
    };
    outputPeakLabel.setText ("-inf", juce::dontSendNotification);
    styleReadout (outputPeakLabel, juce::Colour (0xffd0d0d0));
    addAndMakeVisible (outputPeakLabel);
    grPeakLabel.setText ("0.0", juce::dontSendNotification);
    styleReadout (grPeakLabel, juce::Colour (0xff606064));
    addAndMakeVisible (grPeakLabel);

    startTimerHz (30);
}

MasterStripComponent::~MasterStripComponent()
{
    // See BusComponent::~BusComponent - explicit stopTimer() runs
    // before any modal / member teardown so the timer thread can't
    // fire on objects we're about to clean up.
    stopTimer();
    if (tapeMachineDim != nullptr)
        tapeMachineDim->onClick = nullptr;
    if (auto* m = tapeMachineModal.getComponent())
    {
        if (auto* p = m->getParentComponent())
            p->removeChildComponent (m);
        delete m;
    }
    tapeMachineDim.reset();
}

void MasterStripComponent::timerCallback()
{
    // Output peak per channel - fast attack, slow release; with peak-hold.
    auto smoothChannel = [] (float& displayed, float& peakHold, int& peakFrames, float src)
    {
        if (src > displayed) displayed = src;
        else                  displayed += (src - displayed) * 0.15f;

        if (src >= peakHold) { peakHold = src; peakFrames = 18; }
        else if (peakFrames > 0) --peakFrames;
        else peakHold = juce::jmax (-100.0f, peakHold - 1.5f);
    };
    const float outL = params.meterPostMasterLDb.load (std::memory_order_relaxed);
    const float outR = params.meterPostMasterRDb.load (std::memory_order_relaxed);
    smoothChannel (displayedOutputLDb, outputPeakHoldLDb, outputPeakHoldFramesL, outL);
    smoothChannel (displayedOutputRDb, outputPeakHoldRDb, outputPeakHoldFramesR, outR);

    // Numeric readout shows the louder of the two channels (typical mixer
    // convention - we don't have room for two separate values).
    const float maxHold = juce::jmax (outputPeakHoldLDb, outputPeakHoldRDb);
    if (maxHold <= -60.0f)
        outputPeakLabel.setText ("-inf", juce::dontSendNotification);
    else
        outputPeakLabel.setText (juce::String (maxHold, 1), juce::dontSendNotification);
    outputPeakLabel.setColour (juce::Label::textColourId,
        maxHold >= -3.0f  ? juce::Colour (0xffff5050) :
        maxHold >= -12.0f ? juce::Colour (0xffe0c050) :
                             juce::Colour (0xffd0d0d0));

    // Bus-comp GR.
    const float gr = params.meterGrDb.load (std::memory_order_relaxed);
    if (gr < displayedGrDb) displayedGrDb = gr;
    else                    displayedGrDb += (gr - displayedGrDb) * 0.18f;

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

    if (! meterArea.isEmpty())
        repaint (meterArea);
    if (! grMeterArea.isEmpty())
        repaint (grMeterArea.expanded (2, 10));   // include "GR" caption

    // Motor-fader animate + Write/Touch capture — mirrors the per-channel
    // and per-aux pattern. Master only has one automatable param.
    {
        const int amode = params.automationMode.load (std::memory_order_relaxed);
        const bool isRead  = amode == (int) AutomationMode::Read;
        const bool isWrite = amode == (int) AutomationMode::Write;
        const bool isTouch = amode == (int) AutomationMode::Touch;
        const bool playing = engine.getTransport().isPlaying();

        const float live    = params.liveFaderDb.load (std::memory_order_relaxed);
        const bool  touched = params.faderTouched.load (std::memory_order_relaxed);
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
            captureFaderWritePoint (params.faderDb.load (std::memory_order_relaxed));
    }
}

void MasterStripComponent::showAutoModeMenu()
{
    juce::PopupMenu m;
    const int cur = params.automationMode.load (std::memory_order_relaxed);
    auto add = [&] (int v, const char* label)
    {
        m.addItem (v + 1, label, true, cur == v);
    };
    add ((int) AutomationMode::Off,   "Off");
    add ((int) AutomationMode::Read,  "Read");
    add ((int) AutomationMode::Write, "Write");
    add ((int) AutomationMode::Touch, "Touch");
    m.showMenuAsync (juce::PopupMenu::Options{}.withTargetComponent (&autoModeButton),
                       [this] (int picked)
    {
        if (picked <= 0) return;
        setAutoMode ((AutomationMode) (picked - 1));
    });
}

void MasterStripComponent::setAutoMode (AutomationMode m)
{
    // Release-store on the mode word so any pending captureFaderWritePoint
    // appends from a Write/Touch pass are visible before the audio thread's
    // next acquire-load. Thinning is intentionally NOT triggered here —
    // handleWritePassComplete rewrites lane.points wholesale and would race
    // the audio thread; the safe entry point is File ▸ Optimize automation,
    // which gates on transport-stopped + all modes Off.
    params.automationMode.store ((int) m, std::memory_order_release);
    autoModeButton.setButtonText (m == AutomationMode::Off   ? "Off"
                                   : m == AutomationMode::Read  ? "R"
                                   : m == AutomationMode::Write ? "W"
                                                                : "T");
}

void MasterStripComponent::captureFaderWritePoint (float denormDb)
{
    const float lo = ChannelStripParams::kFaderMinDb;
    const float hi = ChannelStripParams::kFaderMaxDb;
    const float v  = juce::jlimit (0.0f, 1.0f, (denormDb - lo) / (hi - lo));

    auto& lane = params.automationLanes[(size_t) AutomationParam::FaderDb];
    AutomationPoint pt;
    pt.timeSamples   = engine.getTransport().getPlayhead();
    pt.value         = v;
    pt.recordedAtBPM = session.tempoBpm.load (std::memory_order_relaxed);

    // Pre-filter: delta + max-span. Master fader is the most-rideable
    // single control on a session, so the savings here matter most.
    if (! lane.points.empty())
    {
        constexpr float kDeltaEps = 0.001f;
        constexpr juce::int64 kMaxSpanSamples = 22050;
        const auto& last = lane.points.back();
        if (std::abs (pt.value - last.value) < kDeltaEps
            && (pt.timeSamples - last.timeSamples) < kMaxSpanSamples)
            return;
    }

    if (! lane.points.empty() && lane.points.back().timeSamples >= pt.timeSamples)
    {
        if (lane.points.back().timeSamples > pt.timeSamples)
        {
            auto cutoff = std::lower_bound (lane.points.begin(), lane.points.end(),
                pt.timeSamples,
                [] (const AutomationPoint& a, juce::int64 t) { return a.timeSamples < t; });
            lane.points.erase (cutoff, lane.points.end());
        }
        if (! lane.points.empty() && lane.points.back().timeSamples == pt.timeSamples)
        {
            lane.points.back() = pt;
            return;
        }
    }
    lane.points.push_back (pt);
}

void MasterStripComponent::paint (juce::Graphics& g)
{
    // Visible gap + gold-tinted outline so the master reads as its own
    // group, separate from the bus row to its left.
    auto r = getLocalBounds().toFloat().reduced (3.0f);
    g.setColour (juce::Colour (0xff202024));
    g.fillRoundedRectangle (r, 5.0f);
    g.setColour (juce::Colour (0xffd0a060));
    g.fillRoundedRectangle (r.removeFromTop (4.0f), 2.0f);
    g.setColour (juce::Colour (0xffd0a060).withAlpha (0.55f));
    g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (3.0f), 5.0f, 2.0f);

    // EQ / COMP framed bands - same grammar as channel + bus strips.
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
        g.setColour (juce::Colour (0xffd09060).withAlpha (0.40f));
        g.drawRoundedRectangle (compArea.toFloat().reduced (0.5f), 3.0f, 0.8f);
    }

    // Stereo LED meter - two vertical bars (L | R) inside meterArea.
    if (! meterArea.isEmpty())
    {
        constexpr float kMinDb = -60.0f, kMaxDb = 6.0f;
        constexpr float kBarGap = 1.0f;

        auto drawColumn = [&] (juce::Rectangle<float> bar, float displayedDb)
        {
            g.setColour (juce::Colour (0xff0c0c0e));
            g.fillRoundedRectangle (bar, 1.5f);
            g.setColour (juce::Colour (0xff2a2a2e));
            g.drawRoundedRectangle (bar, 1.5f, 0.6f);

            // LED-style hard zones — match the channel-strip + bus meters.
            const juce::Colour kLedGreen  (0xff20d040);
            const juce::Colour kLedYellow (0xfff0e020);
            const juce::Colour kLedRed    (0xffff2020);
            auto fracForDb = [&] (float db)
            {
                return juce::jlimit (0.0f, 1.0f, (db - kMinDb) / (kMaxDb - kMinDb));
            };
            auto colourForDb = [&] (float db) -> juce::Colour
            {
                if (db >=  5.0f) return kLedRed;
                if (db >= -5.0f) return kLedYellow;
                return kLedGreen;
            };

            const float frac = fracForDb (displayedDb);
            const float fillH = (bar.getHeight() - 4.0f) * frac;
            if (fillH > 0.5f)
            {
                const float x = bar.getX() + 1.5f;
                const float w = bar.getWidth() - 3.0f;
                const float yFillTop = bar.getBottom() - 2.0f - fillH;
                const float yFillBot = bar.getBottom() - 2.0f;
                const auto fillRect = juce::Rectangle<float> (x, yFillTop, w, fillH);

                const auto tipCol = colourForDb (displayedDb);
                g.setColour (tipCol.withAlpha (0.20f));
                g.fillRoundedRectangle (fillRect.expanded (1.5f), 2.0f);
                g.setColour (tipCol.withAlpha (0.10f));
                g.fillRoundedRectangle (fillRect.expanded (3.0f), 3.0f);

                const float yRedTop    = bar.getBottom() - 2.0f - fracForDb ( 5.0f) * (bar.getHeight() - 4.0f);
                const float yYellowTop = bar.getBottom() - 2.0f - fracForDb (-5.0f) * (bar.getHeight() - 4.0f);

                auto fillBand = [&] (float top, float bottom, juce::Colour col)
                {
                    if (bottom <= top) return;
                    g.setColour (col);
                    g.fillRect (juce::Rectangle<float> (x, top, w, bottom - top));
                };
                fillBand (juce::jmax (yFillTop, bar.getY()),
                            juce::jmin (yRedTop, yFillBot),
                            kLedRed);
                fillBand (juce::jmax (yFillTop, yRedTop),
                            juce::jmin (yYellowTop, yFillBot),
                            kLedYellow);
                fillBand (juce::jmax (yFillTop, yYellowTop),
                            yFillBot,
                            kLedGreen);
            }
        };

        const auto fullBar = meterArea.toFloat();
        const float colW   = (fullBar.getWidth() - kBarGap) * 0.5f;
        drawColumn (juce::Rectangle<float> (fullBar.getX(), fullBar.getY(),
                                              colW, fullBar.getHeight()),
                     displayedOutputLDb);
        drawColumn (juce::Rectangle<float> (fullBar.getX() + colW + kBarGap, fullBar.getY(),
                                              colW, fullBar.getHeight()),
                     displayedOutputRDb);
    }

    // Master-bus comp GR meter - fills DOWN from the top as compression
    // bites. Same gold→red colour story as the channel and aux strips.
    if (! grMeterArea.isEmpty())
    {
        const auto bar = grMeterArea.toFloat();
        g.setColour (juce::Colour (0xff0c0c0e));
        g.fillRoundedRectangle (bar, 1.5f);
        g.setColour (juce::Colour (0xff2a2a2e));
        g.drawRoundedRectangle (bar, 1.5f, 0.5f);

        constexpr float kGrFloorDb = 20.0f;
        const float grAbs = juce::jlimit (0.0f, kGrFloorDb, std::abs (displayedGrDb));
        if (grAbs > 0.05f)
        {
            const float frac = grAbs / kGrFloorDb;
            const float fillH = (bar.getHeight() - 4.0f) * frac;
            auto fillRect = juce::Rectangle<float> (bar.getX() + 1.5f,
                                                      bar.getY() + 2.0f,
                                                      bar.getWidth() - 3.0f, fillH);
            juce::ColourGradient grad (juce::Colour (0xffe0c050).brighter (0.2f),
                                         bar.getX(), bar.getY(),
                                         juce::Colour (0xffe05050).brighter (0.1f),
                                         bar.getX(), bar.getBottom(), false);
            g.setGradientFill (grad);
            g.fillRoundedRectangle (fillRect, 1.0f);
        }

        // "GR" caption above the bar.
        g.setColour (juce::Colour (0xff909094));
        g.setFont (juce::Font (juce::FontOptions (7.0f, juce::Font::bold)));
        g.drawText ("GR",
                     juce::Rectangle<float> (bar.getX() - 2.0f, bar.getY() - 9.0f,
                                              bar.getWidth() + 4.0f, 8.0f),
                     juce::Justification::centred, false);
    }

    // Fader dB scale labels - aligned with the LookAndFeel-drawn ticks on
    // the fader track. Same kFaderTicks set as the channel + aux strips.
    if (! faderScaleArea.isEmpty())
    {
        const auto& range = faderSlider.getNormalisableRange();
        for (const auto& t : kFaderTicks)
        {
            if (t.db < range.start - 0.01f || t.db > range.end + 0.01f) continue;
            const float y = faderYForDb (faderSlider, t.db);
            const bool isZero = (std::abs (t.db) < 0.01f);
            g.setColour (isZero ? juce::Colour (0xffe0e0e0) : juce::Colour (0xff909094));
            g.setFont (juce::Font (juce::FontOptions (isZero ? 9.5f : 8.5f,
                                                        isZero ? juce::Font::bold
                                                                : juce::Font::plain)));
            const auto rect = juce::Rectangle<float> ((float) faderScaleArea.getX(), y - 5.0f,
                                                        (float) faderScaleArea.getWidth(), 10.0f);
            g.drawText (t.label, rect, juce::Justification::centredRight, false);
        }
    }
}

void MasterStripComponent::setCompactVu (bool compact)
{
    if (compactVu == compact) return;
    compactVu = compact;
    resized();
}

void MasterStripComponent::resized()
{
    auto area = getLocalBounds().reduced (4);
    area.removeFromTop (6);
    {
        auto headerRow = area.removeFromTop (22);
        // 40 px slot at top-right for the automation mode button.
        autoModeButton.setBounds (headerRow.removeFromRight (40));
        headerRow.removeFromRight (4);
        nameLabel.setBounds (headerRow);
    }
    area.removeFromTop (6);

    // Analog VU meter at the top - same proportions as the bus strips so
    // the meter row reads consistently across the console.
    if (vuMeter != nullptr)
    {
        const int ratioNum = compactVu ? 5 : 7;
        const int minH     = compactVu ? 32 : 40;
        const int vuH = juce::jmax (minH, area.getWidth() * ratioNum / 12);
        vuMeter->setBounds (area.removeFromTop (vuH));
        area.removeFromTop (4);
    }

    // 26 px rotary diameter (matches channel strip) + 14 px textbox below.
    // Block width is 40 - 28 px was too narrow and clipped both the bottom
    // value readout (e.g. "4.0:1" became "4...." ) and the top label.
    constexpr int kKnobDia    = 26;
    constexpr int kTextBoxH   = 14;
    constexpr int kKnobBlockH = kKnobDia + kTextBoxH + 2;   // 42
    constexpr int kKnobBlockW = 40;                          // textbox fits "4.0:1", "1100"

    auto layKnobRow = [&] (juce::Rectangle<int>& parent, int n)
                       -> std::pair<juce::Rectangle<int>, juce::Rectangle<int>>
    {
        auto labelRow = parent.removeFromTop (10);
        auto knobRow  = parent.removeFromTop (kKnobBlockH);
        const int totalW = n * kKnobBlockW;
        const int leftPad = juce::jmax (0, (labelRow.getWidth() - totalW) / 2);
        labelRow.removeFromLeft (leftPad);
        knobRow .removeFromLeft (leftPad);
        return { labelRow, knobRow };
    };

    // EQ section: bypass toggle, then 5 fixed-size knob blocks centred in row.
    // Wrap header + knob row in a framed band (eqArea) so the master reads
    // with the same green-tinted EQ / amber-tinted COMP grammar as channels.
    {
        constexpr int kEqSectionH = 20 + 2 + 10 + kKnobBlockH;
        eqArea = area.removeFromTop (kEqSectionH);
        auto s = eqArea;
        eqButton.setBounds (s.removeFromTop (20).reduced (4, 0));
        s.removeFromTop (2);
        auto rows = layKnobRow (s, 5);
        auto& lblRow  = rows.first;
        auto& knobRow = rows.second;
        eqLfLabel     .setBounds (lblRow .removeFromLeft (kKnobBlockW));
        eqHfBoostLabel.setBounds (lblRow .removeFromLeft (kKnobBlockW));
        eqHfAttenLabel.setBounds (lblRow .removeFromLeft (kKnobBlockW));
        eqDriveLabel  .setBounds (lblRow .removeFromLeft (kKnobBlockW));
        eqOutLabel    .setBounds (lblRow .removeFromLeft (kKnobBlockW));
        eqLfBoost     .setBounds (knobRow.removeFromLeft (kKnobBlockW));
        eqHfBoost     .setBounds (knobRow.removeFromLeft (kKnobBlockW));
        eqHfAtten     .setBounds (knobRow.removeFromLeft (kKnobBlockW));
        eqTubeDrive   .setBounds (knobRow.removeFromLeft (kKnobBlockW));
        eqOutputGain  .setBounds (knobRow.removeFromLeft (kKnobBlockW));
    }
    area.removeFromTop (4);

    // Comp section: same shape.
    {
        constexpr int kCompSectionH = 20 + 2 + 10 + kKnobBlockH;
        compArea = area.removeFromTop (kCompSectionH);
        auto s = compArea;
        compButton.setBounds (s.removeFromTop (20).reduced (4, 0));
        s.removeFromTop (2);
        auto rows = layKnobRow (s, 5);
        auto& lblRow  = rows.first;
        auto& knobRow = rows.second;
        compThrLabel .setBounds (lblRow .removeFromLeft (kKnobBlockW));
        compRatLabel .setBounds (lblRow .removeFromLeft (kKnobBlockW));
        compAtkLabel .setBounds (lblRow .removeFromLeft (kKnobBlockW));
        compRelLabel .setBounds (lblRow .removeFromLeft (kKnobBlockW));
        compMakLabel .setBounds (lblRow .removeFromLeft (kKnobBlockW));
        compThreshold.setBounds (knobRow.removeFromLeft (kKnobBlockW));
        compRatio    .setBounds (knobRow.removeFromLeft (kKnobBlockW));
        compAttack   .setBounds (knobRow.removeFromLeft (kKnobBlockW));
        compRelease  .setBounds (knobRow.removeFromLeft (kKnobBlockW));
        compMakeup   .setBounds (knobRow.removeFromLeft (kKnobBlockW));
    }
    area.removeFromTop (6);

    // TAPE + gear row. Gear (22 px) opens the full TapeMachine editor; TAPE
    // takes the remaining width on the row. The standalone HQ button retired
    // when the global Audio Settings oversampling became the single source
    // of truth for tape oversampling.
    auto buttonRow = area.removeFromTop (24).reduced (2, 0);
    constexpr int kGearW = 22;
    tapeGearButton.setBounds (buttonRow.removeFromRight (kGearW));
    buttonRow.removeFromRight (2);
    tapeButton    .setBounds (buttonRow);

    area.removeFromTop (4);

    // Peak readouts above the meter.
    auto peakRow = area.removeFromBottom (16);
    const int prW = peakRow.getWidth() / 2;
    outputPeakLabel.setBounds (peakRow.removeFromLeft (prW));
    grPeakLabel    .setBounds (peakRow);
    area.removeFromBottom (2);

    // Stereo LED meter on the RIGHT side of the fader (matches channel-strip
    // visual hierarchy - fader on the left, meter on the right edge of the
    // strip). 16 px = 2 × 7 px columns + 2 px gap; same proportions as the
    // channel strip's meter column. A slim GR bar sits to the meter's left
    // so the bus comp's gain reduction is visible alongside output level.
    constexpr int kMeterW       = 16;
    constexpr int kGrMeterW     = 9;
    constexpr int kGrGap        = 2;
    constexpr int kFaderScaleW  = 16;
    constexpr int kFaderScaleGap = 3;
    meterArea   = area.removeFromRight (kMeterW);
    area.removeFromRight (kGrGap);
    grMeterArea = area.removeFromRight (kGrMeterW);
    area.removeFromRight (kFaderScaleGap);
    faderScaleArea = area.removeFromRight (kFaderScaleW);
    area.removeFromRight (4);
    faderSlider.setBounds (area);
}

void MasterStripComponent::openTapeMachineModal()
{
    // Toggle: pressing the gear while modal is open dismisses it.
    if (tapeMachineModal != nullptr)
    {
        if (auto* m = tapeMachineModal.getComponent())
        {
            if (auto* parent = m->getParentComponent()) parent->removeChildComponent (m);
            delete m;
        }
        tapeMachineDim.reset();
        return;
    }

    auto* topLevel = getTopLevelComponent();
    if (topLevel == nullptr) return;

    juce::Component* body = nullptr;

#if DUSKSTUDIO_HAS_DUSK_DSP
    // Spawn the TapeMachine plugin's native editor and wrap it so the
    // donor's plugin-style header (painted "TapeMachine" + "Vintage Tape
    // Emulation") is masked and the HQ oversampling combo (now driven
    // globally from Audio Settings) is hidden. The wrapper takes
    // ownership of the editor; deleting the wrapper deletes the editor,
    // which calls editorBeingDeleted on the processor.
    if (tapeProcessorPtr != nullptr)
        if (auto* editor = tapeProcessorPtr->createEditor())
            body = new TapeMachineModalEditor (editor);
#endif

    if (body == nullptr)
    {
        // Defensive fallback if the donor DSP is disabled or the editor
        // failed to construct.
        body = new juce::Component();
        body->setSize (520, 320);
    }

    tapeMachineDim = std::make_unique<DimOverlay>();
    tapeMachineDim->setBounds (topLevel->getLocalBounds());
    tapeMachineDim->onClick = [this] { openTapeMachineModal(); };
    topLevel->addAndMakeVisible (tapeMachineDim.get());

    body->setBounds (topLevel->getLocalBounds()
                        .withSizeKeepingCentre (body->getWidth(), body->getHeight()));
    topLevel->addAndMakeVisible (body);
    tapeMachineModal = body;
}

void MasterStripComponent::mouseDown (const juce::MouseEvent& e)
{
    // Right-click on the master fader opens the MIDI Learn menu.
    if (e.eventComponent == &faderSlider && e.mods.isPopupMenu())
    {
        midilearn::showLearnMenu (faderSlider, session,
                                    MidiBindingTarget::MasterFader);
        return;
    }
    juce::Component::mouseDown (e);
}
} // namespace duskstudio
