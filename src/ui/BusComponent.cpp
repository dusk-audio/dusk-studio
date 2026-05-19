#include "BusComponent.h"
#include "FocalLookAndFeel.h"  // fourKColors palette
#include "../session/MidiBindings.h"

namespace focal
{
namespace
{
// SSL-style release knob: top of travel = AUTO. File-scope so the slider
// callbacks (which outlive the constructor) reference them without
// having to capture function-local constexpr values - which MSVC
// rejects under its strict reading of the lambda-capture rules.
constexpr double kAutoThreshold = 1000.0;
constexpr double kAutoEps       = 1.0e-3;

void styleSmallKnob (juce::Slider& s, double minV, double maxV, double midPt,
                      double initialV, juce::Colour col, const juce::String& suffix,
                      int decimals)
{
    s.setRange (minV, maxV, 0.01);
    if (midPt > minV && midPt < maxV)
        s.setSkewFactorFromMidPoint (midPt);
    s.setValue (initialV, juce::dontSendNotification);
    s.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 38, 14);
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

void styleToggle (juce::TextButton& b, juce::Colour onColour)
{
    b.setClickingTogglesState (true);
    b.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff202024));
    b.setColour (juce::TextButton::buttonOnColourId, onColour);
    b.setColour (juce::TextButton::textColourOffId,  juce::Colour (0xffb0a080));
    b.setColour (juce::TextButton::textColourOnId,   juce::Colour (0xff121214));
}
} // namespace

BusComponent::BusComponent (Bus& b, Session& s, int idx)
    : bus (b), sessionRef (s), busIndex (idx)
{
    nameLabel.setText (bus.name, juce::dontSendNotification);
    nameLabel.setJustificationType (juce::Justification::centred);
    nameLabel.setColour (juce::Label::textColourId, juce::Colours::white);
    nameLabel.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
    nameLabel.setEditable (false, true, false);
    nameLabel.setColour (juce::Label::backgroundWhenEditingColourId, juce::Colour (0xff202024));
    nameLabel.setColour (juce::Label::textWhenEditingColourId,       juce::Colours::white);
    nameLabel.setTooltip ("Double-click to rename this bus.");
    nameLabel.onTextChange = [this]
    {
        const auto txt = nameLabel.getText().trim();
        if (txt.isEmpty()) nameLabel.setText (bus.name, juce::dontSendNotification);
        else bus.name = txt;
    };
    addAndMakeVisible (nameLabel);

    // Analog VU meter at the top of the strip - shows post-DSP bus level
    // with classic 300 ms ballistics so the user can read average level the
    // way they would on an analog console. Stereo: two needles overlaid on
    // one face (L = black, R = oxblood).
    vuMeter = std::make_unique<AnalogVuMeter> (
        &bus.strip.meterPostBusRmsL, &bus.strip.meterPostBusRmsR);
    // Bus VUs use the compact Sifam / Mixbus-style scale (no numerals, "-"
    // and "+" endpoint glyphs). Same white face as the master VU keeps the
    // visual family consistent; the compact scale just removes the noise
    // that crowds a small bus face.
    vuMeter->setCompactScale (true);
    addAndMakeVisible (*vuMeter);

    const auto eqGreen = juce::Colour (0xff80c090);
    const auto compGold = juce::Colour (0xffe0c050);
    // Pan red - matches the channel-strip pan colour (0xffc04040) so the
    // pan-control language is consistent across channels and buses.
    const auto panRed   = juce::Colour (0xffc04040);

    // EQ section.
    styleToggle (eqButton, eqGreen);
    eqButton.setToggleState (bus.strip.eqEnabled.load (std::memory_order_relaxed),
                              juce::dontSendNotification);
    eqButton.setTooltip ("3-band British-style EQ on/off");
    eqButton.onClick = [this]
    {
        bus.strip.eqEnabled.store (eqButton.getToggleState(), std::memory_order_release);
    };
    addAndMakeVisible (eqButton);

    // Suffixes empty - same rationale as master: 38-px textbox can't fit
    // " dB"/" ms" without truncating, and the L/M/H labels already imply dB.
    styleSmallKnob (eqLfGain,  -15.0, 15.0, 0.0, bus.strip.eqLfGainDb.load(),  eqGreen, "", 1);
    styleSmallKnob (eqMidGain, -15.0, 15.0, 0.0, bus.strip.eqMidGainDb.load(), eqGreen, "", 1);
    styleSmallKnob (eqHfGain,  -15.0, 15.0, 0.0, bus.strip.eqHfGainDb.load(),  eqGreen, "", 1);
    eqLfGain .onValueChange = [this] { bus.strip.eqLfGainDb .store ((float) eqLfGain .getValue(), std::memory_order_relaxed); };
    eqMidGain.onValueChange = [this] { bus.strip.eqMidGainDb.store ((float) eqMidGain.getValue(), std::memory_order_relaxed); };
    eqHfGain .onValueChange = [this] { bus.strip.eqHfGainDb .store ((float) eqHfGain .getValue(), std::memory_order_relaxed); };
    addAndMakeVisible (eqLfGain); addAndMakeVisible (eqMidGain); addAndMakeVisible (eqHfGain);
    // L = low shelf, M = bell, H = high shelf - standard 3-band EQ labelling.
    styleSmallLabel (eqLfLbl,  "L", eqGreen);
    styleSmallLabel (eqMidLbl, "M", eqGreen);
    styleSmallLabel (eqHfLbl,  "H", eqGreen);
    addAndMakeVisible (eqLfLbl); addAndMakeVisible (eqMidLbl); addAndMakeVisible (eqHfLbl);

    // Comp section.
    styleToggle (compButton, compGold);
    compButton.setToggleState (bus.strip.compEnabled.load (std::memory_order_relaxed),
                                juce::dontSendNotification);
    compButton.setTooltip ("Bus compressor on/off (UniversalCompressor in Bus mode)");
    compButton.onClick = [this]
    {
        bus.strip.compEnabled.store (compButton.getToggleState(), std::memory_order_release);
    };
    addAndMakeVisible (compButton);

    styleSmallKnob (compThresh,  -30.0,    0.0,  -10.0, bus.strip.compThreshDb.load(),  compGold, "",   1);
    styleSmallKnob (compRatio,     1.0,   10.0,    4.0, bus.strip.compRatio.load(),     compGold, ":1", 1);
    styleSmallKnob (compAttack,    0.1,   50.0,    5.0, bus.strip.compAttackMs.load(),  compGold, "",   1);
    styleSmallKnob (compRelease,  50.0, 1000.0,  200.0, bus.strip.compReleaseMs.load(), compGold, "",   0);
    styleSmallKnob (compMakeup,  -10.0,   20.0,    0.0, bus.strip.compMakeupDb.load(),  compGold, "",   1);
    compThresh .onValueChange = [this] { bus.strip.compThreshDb .store ((float) compThresh .getValue(), std::memory_order_relaxed); };
    compRatio  .onValueChange = [this] { bus.strip.compRatio    .store ((float) compRatio  .getValue(), std::memory_order_relaxed); };
    compAttack .onValueChange = [this] { bus.strip.compAttackMs .store ((float) compAttack .getValue(), std::memory_order_relaxed); };
    compMakeup .onValueChange = [this] { bus.strip.compMakeupDb .store ((float) compMakeup .getValue(), std::memory_order_relaxed); };

    // SSL-style release: top of the knob's travel = AUTO. Display reads
    // "AUTO" only at the top detent; everywhere else shows the ms value.
    // Threshold is the slider's max with a tiny eps so floating-point
    // rounding can't accidentally drop a typed 999.8 into AUTO.
    compRelease.textFromValueFunction = [] (double v) -> juce::String
    {
        return std::abs (v - kAutoThreshold) <= kAutoEps
                 ? juce::String ("AUTO")
                 : juce::String ((int) std::round (v));
    };
    compRelease.valueFromTextFunction = [] (const juce::String& s) -> double
    {
        if (s.trim().equalsIgnoreCase ("auto")) return kAutoThreshold;
        const double parsed = (double) s.getDoubleValue();
        return parsed >= kAutoThreshold - kAutoEps ? kAutoThreshold : parsed;
    };
    compRelease.onValueChange = [this]
    {
        const double v = compRelease.getValue();
        const bool autoOn = std::abs (v - kAutoThreshold) <= kAutoEps;
        bus.strip.compReleaseAuto.store (autoOn, std::memory_order_relaxed);
        if (! autoOn)
            bus.strip.compReleaseMs.store ((float) v, std::memory_order_relaxed);
    };
    if (bus.strip.compReleaseAuto.load (std::memory_order_relaxed))
        compRelease.setValue (kAutoThreshold, juce::dontSendNotification);
    compRelease.updateText();

    addAndMakeVisible (compThresh); addAndMakeVisible (compRatio);
    addAndMakeVisible (compAttack); addAndMakeVisible (compRelease);
    addAndMakeVisible (compMakeup);
    styleSmallLabel (compThrLbl, "THR", compGold);
    styleSmallLabel (compRatLbl, "RAT", compGold);
    styleSmallLabel (compAtkLbl, "ATK", compGold);
    styleSmallLabel (compRelLbl, "REL", compGold);
    styleSmallLabel (compMakLbl, "MAK", compGold);
    addAndMakeVisible (compThrLbl); addAndMakeVisible (compRatLbl);
    addAndMakeVisible (compAtkLbl); addAndMakeVisible (compRelLbl);
    addAndMakeVisible (compMakLbl);

    // Pan.
    styleSmallKnob (panKnob, -1.0, 1.0, 0.0, bus.strip.pan.load(), panRed, "", 2);
    panKnob.onValueChange = [this] { bus.strip.pan.store ((float) panKnob.getValue(), std::memory_order_relaxed); };
    addAndMakeVisible (panKnob);
    styleSmallLabel (panLbl, "PAN", panRed);
    addAndMakeVisible (panLbl);

    faderSlider.setRange (ChannelStripParams::kFaderMinDb, ChannelStripParams::kFaderMaxDb, 0.1);
    faderSlider.setSkewFactorFromMidPoint (-12.0);
    faderSlider.setValue (bus.strip.faderDb.load (std::memory_order_relaxed), juce::dontSendNotification);
    faderSlider.setDoubleClickReturnValue (true, 0.0);
    faderSlider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 56, 16);
    faderSlider.setTextValueSuffix (" dB");
    faderSlider.onValueChange = [this]
    {
        bus.strip.faderDb.store ((float) faderSlider.getValue(), std::memory_order_relaxed);
    };
    addAndMakeVisible (faderSlider);

    muteButton.setClickingTogglesState (true);
    muteButton.setColour (juce::TextButton::buttonOnColourId, juce::Colours::orangered);
    muteButton.setToggleState (bus.strip.mute.load (std::memory_order_relaxed), juce::dontSendNotification);
    muteButton.onClick = [this] { bus.strip.mute.store (muteButton.getToggleState(), std::memory_order_release); };
    addAndMakeVisible (muteButton);

    soloButton.setClickingTogglesState (true);
    soloButton.setColour (juce::TextButton::buttonOnColourId, juce::Colours::yellow.darker (0.2f));
    soloButton.setToggleState (bus.strip.solo.load (std::memory_order_relaxed), juce::dontSendNotification);
    soloButton.onClick = [this]
    {
        // Counter-aware so anyBusSoloed() stays O(1) on the audio thread.
        sessionRef.setBusSoloed (busIndex, soloButton.getToggleState());
    };
    addAndMakeVisible (soloButton);

    // Mouse listeners so the strip's mouseDown sees right-clicks on each
    // child (e.eventComponent identifies which control was hit). Matches
    // the ChannelStripComponent pattern - the strip body handles all
    // right-click routing in one place rather than each control owning
    // its own MIDI Learn menu.
    faderSlider.addMouseListener (this, false);
    muteButton .addMouseListener (this, false);
    soloButton .addMouseListener (this, false);
    panKnob    .addMouseListener (this, false);

    auto styleReadout = [] (juce::Label& lbl, juce::Colour col)
    {
        lbl.setJustificationType (juce::Justification::centred);
        lbl.setColour (juce::Label::textColourId,        col);
        lbl.setColour (juce::Label::backgroundColourId,  juce::Colour (0xff0a0a0c));
        // No outline - same rationale as the channel strip: the 1 px border
        // drew on top of the adjacent fader textbox edge and looked like overlap.
        lbl.setColour (juce::Label::outlineColourId,     juce::Colours::transparentBlack);
        lbl.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                                                      10.0f, juce::Font::bold)));
    };
    outputPeakLabel.setText ("-inf", juce::dontSendNotification);
    styleReadout (outputPeakLabel, juce::Colour (0xffd0d0d0));
    addAndMakeVisible (outputPeakLabel);
    grPeakLabel.setText ("0.0", juce::dontSendNotification);
    styleReadout (grPeakLabel, juce::Colour (0xff606064));
    addAndMakeVisible (grPeakLabel);

    startTimerHz (30);
}

BusComponent::~BusComponent()
{
    // stopTimer() must run before any member destructor so the timer
    // thread can't fire timerCallback on freed atomics. juce::Timer's
    // own destructor calls stopTimer too but that's the BASE-class
    // destructor - it runs AFTER derived-class members destruct.
    stopTimer();
}

void BusComponent::timerCallback()
{
    auto smoothChannel = [] (float& displayed, float& peakHold, int& peakFrames, float src)
    {
        if (src > displayed) displayed = src;
        else                  displayed += (src - displayed) * 0.15f;

        if (src >= peakHold) { peakHold = src; peakFrames = 18; }
        else if (peakFrames > 0) --peakFrames;
        else peakHold = juce::jmax (-100.0f, peakHold - 1.5f);
    };
    const float outL = bus.strip.meterPostBusLDb.load (std::memory_order_relaxed);
    const float outR = bus.strip.meterPostBusRDb.load (std::memory_order_relaxed);
    smoothChannel (displayedOutputLDb, outputPeakHoldLDb, outputPeakHoldFramesL, outL);
    smoothChannel (displayedOutputRDb, outputPeakHoldRDb, outputPeakHoldFramesR, outR);

    const float maxHold = juce::jmax (outputPeakHoldLDb, outputPeakHoldRDb);
    if (maxHold <= -60.0f)
        outputPeakLabel.setText ("-inf", juce::dontSendNotification);
    else
        outputPeakLabel.setText (juce::String (maxHold, 1), juce::dontSendNotification);
    outputPeakLabel.setColour (juce::Label::textColourId,
        maxHold >= -3.0f  ? juce::Colour (0xffff5050) :
        maxHold >= -12.0f ? juce::Colour (0xffe0c050) :
                             juce::Colour (0xffd0d0d0));

    const float gr = bus.strip.meterGrDb.load (std::memory_order_relaxed);
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

    if (! meterArea.isEmpty())   repaint (meterArea);
    if (! grMeterArea.isEmpty()) repaint (grMeterArea.expanded (2, 10));  // include "GR" caption
}

void BusComponent::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
    {
        // Per-child MIDI Learn routes. The strip is registered as a
        // mouse listener on each control above; eventComponent tells us
        // which one was right-clicked. Falls through to the colour menu
        // on background clicks (no eventComponent match).
        if (e.eventComponent == &faderSlider)
        {
            midilearn::showLearnMenu (faderSlider, sessionRef,
                                        MidiBindingTarget::BusFader, busIndex);
            return;
        }
        if (e.eventComponent == &muteButton)
        {
            midilearn::showLearnMenu (muteButton, sessionRef,
                                        MidiBindingTarget::BusMute, busIndex);
            return;
        }
        if (e.eventComponent == &soloButton)
        {
            midilearn::showLearnMenu (soloButton, sessionRef,
                                        MidiBindingTarget::BusSolo, busIndex);
            return;
        }
        if (e.eventComponent == &panKnob)
        {
            midilearn::showLearnMenu (panKnob, sessionRef,
                                        MidiBindingTarget::BusPan, busIndex);
            return;
        }
        showColourMenu();
    }
}

void BusComponent::applyBusColour (juce::Colour c) { bus.colour = c; repaint(); }

void BusComponent::showColourMenu()
{
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
    menu.addSectionHeader ("Bus colour");
    for (size_t i = 0; i < std::size (presets); ++i)
    {
        juce::PopupMenu::Item item;
        item.itemID = (int) (i + 1);
        item.text = presets[i].first;
        item.colour = juce::Colour (presets[i].second);
        menu.addItem (item);
    }
    juce::Component::SafePointer<BusComponent> safe (this);
    std::vector<std::pair<juce::String, juce::uint32>> presetCopy;
    presetCopy.reserve (std::size (presets));
    for (auto& p : presets) presetCopy.emplace_back (juce::String (p.first), p.second);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this),
        [safe, presetCopy] (int result)
        {
            if (result <= 0) return;
            auto* self = safe.getComponent();
            if (self == nullptr) return;
            const int idx = result - 1;
            if (idx >= 0 && idx < (int) presetCopy.size())
                self->applyBusColour (juce::Colour (presetCopy[(size_t) idx].second));
        });
}

void BusComponent::paint (juce::Graphics& g)
{
    // Inset the card more (3.0 vs 1.5) so adjacent bus strips have a
    // visible gap between their outlines. Tint the outline with the bus's
    // own colour so each bus reads as a self-contained group, not a slice
    // of an undifferentiated row.
    auto r = getLocalBounds().toFloat().reduced (3.0f);
    g.setColour (juce::Colour (0xff181820));
    g.fillRoundedRectangle (r, 5.0f);
    g.setColour (bus.colour.withAlpha (0.85f));
    g.fillRoundedRectangle (r.removeFromTop (4.0f), 2.0f);
    g.setColour (bus.colour.withAlpha (0.45f));
    g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (3.0f), 5.0f, 1.5f);

    // EQ region: green-tinted background + soft green outline, matching
    // the channel strip's eqArea grammar.
    if (! eqArea.isEmpty())
    {
        g.setColour (juce::Colour (0xff1f231e));
        g.fillRoundedRectangle (eqArea.toFloat(), 3.0f);
        g.setColour (juce::Colour (0xff80c090).withAlpha (0.40f));
        g.drawRoundedRectangle (eqArea.toFloat().reduced (0.5f), 3.0f, 0.8f);
    }

    // COMP region: amber-tinted background + gold outline.
    if (! compArea.isEmpty())
    {
        g.setColour (juce::Colour (0xff241f1c));
        g.fillRoundedRectangle (compArea.toFloat(), 3.0f);
        g.setColour (juce::Colour (0xffd09060).withAlpha (0.40f));
        g.drawRoundedRectangle (compArea.toFloat().reduced (0.5f), 3.0f, 0.8f);
    }

    // Stereo LED meter - two columns side by side inside meterArea.
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

            // LED-style hard zones — match the channel-strip meter look.
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

    // Bus-comp GR meter - fills DOWN from top as compression bites. Same
    // colour story as the channel strip's GR bar so the visual language is
    // consistent across the mixer.
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

        // Tiny "GR" caption above the bar so the user knows what it is.
        g.setColour (juce::Colour (0xff909094));
        g.setFont (juce::Font (juce::FontOptions (7.0f, juce::Font::bold)));
        g.drawText ("GR",
                     juce::Rectangle<float> (bar.getX() - 2.0f, bar.getY() - 9.0f,
                                              bar.getWidth() + 4.0f, 8.0f),
                     juce::Justification::centred, false);
    }

    // Fader dB scale labels - aligned with the tick marks the LookAndFeel
    // paints on the fader track. Same kFaderTicks set used by the channel
    // strips so all faders read identically.
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

void BusComponent::setCompactVu (bool compact)
{
    if (compactVu == compact) return;
    compactVu = compact;
    resized();
}

void BusComponent::resized()
{
    auto area = getLocalBounds().reduced (4);
    area.removeFromTop (6);
    nameLabel.setBounds (area.removeFromTop (20));
    area.removeFromTop (3);

    // Analog VU meter spans the full strip width at a ~12:7 (~1.71:1)
    // aspect ratio. Sits between the name label and the EQ block so the
    // user reads level first (the most common monitoring task). The
    // squatter 12:7 ratio (vs the 12:5 photo aspect) leaves more
    // vertical room for the EQ + comp section below at narrow strip
    // widths.
    if (vuMeter != nullptr)
    {
        // Squatter VU when the SUMMARY tape strip is expanded so the
        // bus fader keeps a workable height. Ratio drops from 7/12 to
        // 5/12 of the strip width (~70 px -> ~50 px on a 120 px strip).
        const int ratioNum = compactVu ? 5 : 7;
        const int minH     = compactVu ? 28 : 36;
        const int vuH = juce::jmax (minH, area.getWidth() * ratioNum / 12);
        vuMeter->setBounds (area.removeFromTop (vuH));
        area.removeFromTop (3);
    }

    // No plugin slots on buses - reserved space stays a thin gap so the
    // strip's vertical layout matches the channel strips' rhythm. Plugins
    // live on AUX return lanes (the AUX stage), not on bus subgroups.
    fxArea = juce::Rectangle<int>();
    area.removeFromTop (3);

    // 26 px rotary + 14 px textbox. Block width is 40 - wider than the
    // initial 28 to keep value readouts ("4.0:1", "1100", etc.) un-truncated
    // and label text above readable.
    constexpr int kKnobDia    = 26;
    constexpr int kTextBoxH   = 14;
    constexpr int kKnobBlockH = kKnobDia + kTextBoxH + 2;   // 42
    constexpr int kKnobBlockW = 40;

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

    // EQ section. Pre-compute area so paint() can draw the green-tinted
    // background band around the knob block.
    {
        constexpr int kEqSectionH = 16 + 1 + 10 + kKnobBlockH;
        eqArea = area.removeFromTop (kEqSectionH);
        auto s = eqArea;
        eqButton.setBounds (s.removeFromTop (16).reduced (4, 0));
        s.removeFromTop (1);
        auto rows = layKnobRow (s, 3);
        auto& lblRow  = rows.first;
        auto& knobRow = rows.second;
        eqLfLbl .setBounds (lblRow .removeFromLeft (kKnobBlockW));
        eqMidLbl.setBounds (lblRow .removeFromLeft (kKnobBlockW));
        eqHfLbl .setBounds (lblRow .removeFromLeft (kKnobBlockW));
        eqLfGain .setBounds (knobRow.removeFromLeft (kKnobBlockW));
        eqMidGain.setBounds (knobRow.removeFromLeft (kKnobBlockW));
        eqHfGain .setBounds (knobRow.removeFromLeft (kKnobBlockW));
    }
    area.removeFromTop (3);

    // Comp: 3 + 2 knobs across two rows. Frame the whole pair with a
    // gold-tinted background band so the section reads as one unit.
    {
        constexpr int kCompSectionH = 16 + 1 + 10 + kKnobBlockH + 1
                                       + 10 + kKnobBlockH;
        compArea = area.removeFromTop (kCompSectionH);
        auto s = compArea;
        compButton.setBounds (s.removeFromTop (16).reduced (4, 0));
        s.removeFromTop (1);
        {
            auto rows = layKnobRow (s, 3);
            auto& lblRow  = rows.first;
            auto& knobRow = rows.second;
            compThrLbl.setBounds (lblRow .removeFromLeft (kKnobBlockW));
            compRatLbl.setBounds (lblRow .removeFromLeft (kKnobBlockW));
            compAtkLbl.setBounds (lblRow .removeFromLeft (kKnobBlockW));
            compThresh.setBounds (knobRow.removeFromLeft (kKnobBlockW));
            compRatio .setBounds (knobRow.removeFromLeft (kKnobBlockW));
            compAttack.setBounds (knobRow.removeFromLeft (kKnobBlockW));
        }
        s.removeFromTop (1);
        {
            auto rows = layKnobRow (s, 2);
            auto& lblRow  = rows.first;
            auto& knobRow = rows.second;
            compRelLbl .setBounds (lblRow .removeFromLeft (kKnobBlockW));
            compMakLbl .setBounds (lblRow .removeFromLeft (kKnobBlockW));
            compRelease.setBounds (knobRow.removeFromLeft (kKnobBlockW));
            compMakeup .setBounds (knobRow.removeFromLeft (kKnobBlockW));
        }
    }
    area.removeFromTop (3);

    // Pan (single knob, centred).
    {
        auto rows = layKnobRow (area, 1);
        auto& lblRow  = rows.first;
        auto& knobRow = rows.second;
        panLbl .setBounds (lblRow .removeFromLeft (kKnobBlockW));
        panKnob.setBounds (knobRow.removeFromLeft (kKnobBlockW));
    }
    area.removeFromTop (4);

    // Mute / solo at the bottom.
    auto buttons = area.removeFromBottom (24);
    muteButton.setBounds (buttons.removeFromLeft (buttons.getWidth() / 2).reduced (2));
    soloButton.setBounds (buttons.reduced (2));
    area.removeFromBottom (2);

    // Peak/GR readouts above the meter+fader pair.
    auto peakRow = area.removeFromBottom (14);
    const int prW = peakRow.getWidth() / 2;
    outputPeakLabel.setBounds (peakRow.removeFromLeft (prW));
    grPeakLabel    .setBounds (peakRow);
    area.removeFromBottom (2);

    // Right-side stack: stereo LED meter | small GR meter | fader scale
    // labels | fader. The scale column shows fader dB labels aligned with
    // the LookAndFeel-drawn ticks across the fader's track.
    constexpr int kMeterW       = 14;   // 2 × ~6 px columns + 1 px gap
    constexpr int kGrMeterW     = 8;
    constexpr int kGrGap        = 2;
    constexpr int kFaderScaleW  = 14;
    constexpr int kFaderScaleGap = 2;
    meterArea   = area.removeFromRight (kMeterW);
    area.removeFromRight (kGrGap);
    grMeterArea = area.removeFromRight (kGrMeterW);
    area.removeFromRight (kFaderScaleGap);
    faderScaleArea = area.removeFromRight (kFaderScaleW);
    area.removeFromRight (3);
    faderSlider.setBounds (area);
}
} // namespace focal
