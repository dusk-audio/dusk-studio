#include "ChannelCompEditor.h"
#include "DuskStudioLookAndFeel.h"

namespace duskstudio
{
namespace
{
void styleKnob (juce::Slider& k, juce::Colour fill,
                double mn, double mx, double defaultVal, double skewMid,
                const juce::String& suffix, int decimals)
{
    k.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    k.setColour (juce::Slider::rotarySliderFillColourId, fill);
    k.setColour (juce::Slider::rotarySliderOutlineColourId, juce::Colour (0xff404048));
    k.setRange (mn, mx, 0.01);
    if (skewMid > 0) k.setSkewFactorFromMidPoint (skewMid);
    k.setDoubleClickReturnValue (true, defaultVal);
    k.setTextBoxStyle (juce::Slider::TextBoxBelow, true, 80, 18);
    k.setColour (juce::Slider::textBoxTextColourId,       juce::Colour (0xffe0e0e0));
    k.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colour (0));
    k.setColour (juce::Slider::textBoxOutlineColourId,    juce::Colour (0));
    k.setNumDecimalPlacesToDisplay (decimals);
    k.setTextValueSuffix (suffix);
}

void styleLabel (juce::Label& l, const juce::String& text)
{
    l.setText (text, juce::dontSendNotification);
    l.setJustificationType (juce::Justification::centred);
    l.setColour (juce::Label::textColourId, juce::Colour (0xffb07050));
    l.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
}
} // namespace

ChannelCompEditor::ChannelCompEditor (Track& t) : track (t)
{
    // The window title bar shows "Comp - N" already; no inline duplicate.
    titleLabel.setVisible (false);

    onButton.setClickingTogglesState (true);
    onButton.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff202024));
    onButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (fourKColors::kCompGold));
    onButton.setColour (juce::TextButton::textColourOffId,  juce::Colour (0xffd09060));
    onButton.setColour (juce::TextButton::textColourOnId,   juce::Colour (0xff121214));
    onButton.setToggleState (track.strip.compEnabled.load (std::memory_order_relaxed),
                              juce::dontSendNotification);
    onButton.onClick = [this]
    {
        track.strip.compEnabled.store (onButton.getToggleState(), std::memory_order_relaxed);
    };
    addAndMakeVisible (onButton);

    auto styleModeBtn = [] (juce::TextButton& b)
    {
        b.setClickingTogglesState (true);
        b.setRadioGroupId (5, juce::dontSendNotification);
        b.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff181820));
        b.setColour (juce::TextButton::buttonOnColourId, juce::Colour (fourKColors::kCompGold));
        b.setColour (juce::TextButton::textColourOffId,  juce::Colour (0xffd09060));
        b.setColour (juce::TextButton::textColourOnId,   juce::Colour (0xff121214));
    };
    styleModeBtn (modeOpto);  styleModeBtn (modeFet);  styleModeBtn (modeVca);
    modeOpto.onClick = [this] { setMode (0); };
    modeFet.onClick  = [this] { setMode (1); };
    modeVca.onClick  = [this] { setMode (2); };
    addAndMakeVisible (modeOpto);
    addAndMakeVisible (modeFet);
    addAndMakeVisible (modeVca);
    refreshModeButtons();

    const auto gold = juce::Colour (fourKColors::kCompGold);
    styleKnob (threshKnob,  gold, ChannelStripParams::kCompThreshMin,  ChannelStripParams::kCompThreshMax,  -12.0,  -24.0, " dB", 1);
    styleKnob (ratioKnob,   gold, ChannelStripParams::kCompRatioMin,   ChannelStripParams::kCompRatioMax,    4.0,    4.0, ":1",  1);
    styleKnob (attackKnob,  gold, ChannelStripParams::kCompAttackMin,  ChannelStripParams::kCompAttackMax,  10.0,   10.0, " ms", 1);
    styleKnob (releaseKnob, gold, ChannelStripParams::kCompReleaseMin, ChannelStripParams::kCompReleaseMax,100.0,  200.0, " ms", 0);
    styleKnob (makeupKnob,  gold, ChannelStripParams::kCompMakeupMin,  ChannelStripParams::kCompMakeupMax,   0.0,    0.0, " dB", 1);

    // Initialise knob values from the per-mode params via the same mapping
    // used by writeKnobToCompParams() / readKnobFromCompParams(). Done after
    // setValue calls below.
    syncKnobsFromMode();

    threshKnob.onValueChange  = [this] { writeThresholdToMode(); };
    ratioKnob.onValueChange   = [this] { writeRatioToMode(); };
    attackKnob.onValueChange  = [this] { writeAttackToMode(); };
    releaseKnob.onValueChange = [this] { writeReleaseToMode(); };
    makeupKnob.onValueChange  = [this] { writeMakeupToMode(); };

    addAndMakeVisible (threshKnob);  addAndMakeVisible (ratioKnob);
    addAndMakeVisible (attackKnob);  addAndMakeVisible (releaseKnob);
    addAndMakeVisible (makeupKnob);

    // Labels are styled with placeholders here; refreshLabelsForMode() below
    // sets the actual text per active mode.
    styleLabel (threshLabel,  "");
    styleLabel (ratioLabel,   "");
    styleLabel (attackLabel,  "");
    styleLabel (releaseLabel, "");
    styleLabel (makeupLabel,  "");
    addAndMakeVisible (threshLabel);  addAndMakeVisible (ratioLabel);
    addAndMakeVisible (attackLabel);  addAndMakeVisible (releaseLabel);
    addAndMakeVisible (makeupLabel);

    refreshLabelsForMode();   // also calls setSize for the active mode

    startTimerHz (30);
}

ChannelCompEditor::~ChannelCompEditor()
{
    // See BusComponent::~BusComponent - explicit stopTimer() prevents
    // the timer callback from racing member destruction.
    stopTimer();
}

void ChannelCompEditor::setMode (int modeIndex)
{
    track.strip.compMode.store (modeIndex, std::memory_order_relaxed);
    refreshModeButtons();
    refreshLabelsForMode();
    syncKnobsFromMode();
}

void ChannelCompEditor::refreshLabelsForMode()
{
    const int m = juce::jlimit (0, 2, track.strip.compMode.load (std::memory_order_relaxed));

    // Mode-appropriate labels reflect what each knob actually drives in the
    // active mode. The underlying knob ranges + onValueChange routing stay
    // identical - see writeXxxToMode for the per-mode parameter mapping.
    const char* thresh  = "THRESHOLD";
    const char* ratio   = "RATIO";
    const char* attack  = "ATTACK";
    const char* release = "RELEASE";
    const char* makeup  = "MAKEUP";

    switch (m)
    {
        case 0:  // Opto: peak-reduction style. Attack/release are fixed by
                 // the optical model - knobs stay visible but inert.
            thresh  = "PEAK RED";
            ratio   = "(fixed)";
            attack  = "(fixed)";
            release = "(fixed)";
            makeup  = "GAIN";
            break;
        case 1:  // FET (1176): drive in, level out.
            thresh  = "INPUT";
            makeup  = "OUTPUT";
            break;
        case 2:  // VCA: textbook threshold/ratio/attack/release/makeup.
        default:
            break;
    }

    threshLabel .setText (thresh,  juce::dontSendNotification);
    ratioLabel  .setText (ratio,   juce::dontSendNotification);
    attackLabel .setText (attack,  juce::dontSendNotification);
    releaseLabel.setText (release, juce::dontSendNotification);
    makeupLabel .setText (makeup,  juce::dontSendNotification);

    // Per-mode threshold knob range. FET's "INPUT" knob is drive (0..40 dB)
    // into the DSP's fixed -10 dBFS detection threshold — the real 1176
    // has no threshold control. Opto / VCA use the generic -60..0 dB
    // threshold scale.
    if (m == 1)
        threshKnob.setRange (0.0,   40.0, 0.1);
    else
        threshKnob.setRange (-60.0, 0.0,  0.1);

    // HIDE knobs that don't drive anything in the active mode rather than
    // dimming them. Opto's threshold (peak-red) and makeup (gain) are the
    // only meaningful controls - the optical model's attack/release/ratio
    // are baked in. FET and VCA expose all 5 knobs.
    const bool optoMode = (m == 0);
    ratioKnob.setVisible   (! optoMode);  ratioLabel.setVisible   (! optoMode);
    attackKnob.setVisible  (! optoMode);  attackLabel.setVisible  (! optoMode);
    releaseKnob.setVisible (! optoMode);  releaseLabel.setVisible (! optoMode);

    // Per-mode height. Opto is just two centred knobs; FET/VCA need the
    // 2x2 + centred Makeup. Calling setSize here propagates a
    // childBoundsChanged event up to the parent CallOutBox, which
    // re-layouts itself around the new content rect.
    constexpr int kKnobBlockH = 56 + 18 + 4;
    constexpr int kRowGap     = 6;
    constexpr int kHeaderH    = 24 + 8 + 24 + 12;   // header + gap + mode tabs + gap
    constexpr int kFooterPad  = 16;
    const int contentH = optoMode
        ? (kHeaderH + kKnobBlockH + kRowGap + kKnobBlockH + kFooterPad)            // 2 single rows
        : (kHeaderH + kKnobBlockH + kRowGap + kKnobBlockH + kRowGap + kKnobBlockH  // pairs + makeup
                       + kRowGap + kFooterPad);
    constexpr int kBaseW = 380;
    setSize (kBaseW, contentH + 24);  // +24 for outer reduce(12)

    // Trigger a re-layout because the visible-knob set changed.
    resized();
}

// Per-mode parameter routing.
//
// The unified knobs (THRESHOLD/RATIO/ATTACK/RELEASE/MAKEUP) are visual
// controls that route to whichever per-mode params actually drive the
// embedded UniversalCompressor for the currently-selected mode. Without
// this mapping the knobs only updated the legacy `compThresholdDb` /
// `compRatio` / etc atomics, which the engine doesn't read - so adjusting
// them did nothing audible. The engine reads `compVcaThreshDb`,
// `compOptoPeakRed`, `compFetInput`/`compFetOutput`, etc.
//
// Threshold knob range is the unified -60..0 dB. Per mode:
//   VCA:  threshDb -> compVcaThreshDb  (clamped to that param's -38..12)
//   Opto: threshDb -> compOptoPeakRed  (lower threshold = more reduction;
//                                       linear remap of -60..0 to 100..0 %)
//   FET:  threshDb -> chain compFetInput up and compFetOutput down by the
//                     same amount so net level holds while compression
//                     increases (the user's stated intent).
//
// Similar logic for the other four knobs - Opto's release/attack/ratio are
// fixed by the optical model so those knobs are no-ops in Opto mode.

void ChannelCompEditor::writeThresholdToMode()
{
    // Touching threshold turns the comp on - mirrors the inline strip's
    // meter-drag behavior. Engineer rarely wants to set threshold WITHOUT
    // engaging the comp; making them flick the ON toggle separately is just
    // a round-trip for no gain.
    track.strip.compEnabled.store (true, std::memory_order_relaxed);
    onButton.setToggleState (true, juce::dontSendNotification);

    const float threshDb = (float) threshKnob.getValue();
    const int mode = juce::jlimit (0, 2, track.strip.compMode.load (std::memory_order_relaxed));
    switch (mode)
    {
        case 0: // Opto: -60 dB -> 100% peak reduction, 0 dB -> 0%
        {
            const float peakRed = juce::jlimit (0.0f, 100.0f, -threshDb * (100.0f / 60.0f));
            track.strip.compOptoPeakRed.store (peakRed, std::memory_order_relaxed);
            break;
        }
        case 1: // FET
        {
            // INPUT knob writes drive directly into the FET's fixed-threshold
            // detector — matches hardware 1176 (no threshold control). Output
            // is independent; no chain coupling so dragging INPUT doesn't
            // mysteriously change output level.
            const float drive = juce::jlimit (0.0f, 40.0f, threshDb);
            track.strip.compFetInput.store (drive, std::memory_order_relaxed);
            break;
        }
        case 2: // VCA: direct
        default:
            track.strip.compVcaThreshDb.store (juce::jlimit (-38.0f, 12.0f, threshDb),
                                                std::memory_order_relaxed);
            break;
    }
}

void ChannelCompEditor::writeRatioToMode()
{
    const float r = (float) ratioKnob.getValue();
    const int mode = juce::jlimit (0, 2, track.strip.compMode.load (std::memory_order_relaxed));
    switch (mode)
    {
        case 0: break;  // Opto: ratio fixed by optical curve
        case 1:         // FET: map 1..20+ to ratio enum 0=4:1, 1=8:1, 2=12:1, 3=20:1, 4=All
        {
            int idx = 0;
            if      (r >= 18.0f) idx = 4;
            else if (r >= 14.0f) idx = 3;
            else if (r >= 10.0f) idx = 2;
            else if (r >=  6.0f) idx = 1;
            track.strip.compFetRatio.store (idx, std::memory_order_relaxed);
            break;
        }
        case 2:
        default:
            track.strip.compVcaRatio.store (juce::jlimit (1.0f, 120.0f, r),
                                             std::memory_order_relaxed);
            break;
    }
}

void ChannelCompEditor::writeAttackToMode()
{
    const float a = (float) attackKnob.getValue();
    const int mode = juce::jlimit (0, 2, track.strip.compMode.load (std::memory_order_relaxed));
    switch (mode)
    {
        case 0: break;  // Opto attack is the optical lag - fixed
        case 1:
            track.strip.compFetAttack.store (juce::jlimit (0.02f, 80.0f, a),
                                              std::memory_order_relaxed);
            break;
        case 2:
        default:
            track.strip.compVcaAttack.store (juce::jlimit (0.1f, 50.0f, a),
                                              std::memory_order_relaxed);
            break;
    }
}

void ChannelCompEditor::writeReleaseToMode()
{
    const float r = (float) releaseKnob.getValue();
    const int mode = juce::jlimit (0, 2, track.strip.compMode.load (std::memory_order_relaxed));
    switch (mode)
    {
        case 0: break;  // Opto release is the optical decay - fixed
        case 1:
            track.strip.compFetRelease.store (juce::jlimit (50.0f, 1100.0f, r),
                                               std::memory_order_relaxed);
            break;
        case 2:
        default:
            track.strip.compVcaRelease.store (juce::jlimit (10.0f, 5000.0f, r),
                                               std::memory_order_relaxed);
            break;
    }
}

void ChannelCompEditor::writeMakeupToMode()
{
    const float dB = (float) makeupKnob.getValue();
    // Always store the user's intent in the unified `compMakeupDb` field.
    // FET reads this back when the threshold knob changes so the two
    // controls compose without overwriting each other (otherwise lowering
    // threshold would wipe whatever makeup the user had dialled in).
    track.strip.compMakeupDb.store (dB, std::memory_order_relaxed);

    const int mode = juce::jlimit (0, 2, track.strip.compMode.load (std::memory_order_relaxed));
    switch (mode)
    {
        case 0: // Opto: 0..100 % gain, 50 % = unity
        {
            const float pct = juce::jlimit (0.0f, 100.0f, 50.0f + dB * 2.5f);
            track.strip.compOptoGain.store (pct, std::memory_order_relaxed);
            break;
        }
        case 1: // FET: independent OUTPUT knob, no chain coupling.
        {
            track.strip.compFetOutput.store (juce::jlimit (-20.0f, 20.0f, dB),
                                              std::memory_order_relaxed);
            break;
        }
        case 2:
        default:
            track.strip.compVcaOutput.store (juce::jlimit (-20.0f, 20.0f, dB),
                                              std::memory_order_relaxed);
            break;
    }
}

// Read back per-mode params and update the knob displays (called when the
// mode changes so the user sees the calibration for the active mode).
void ChannelCompEditor::syncKnobsFromMode()
{
    const int mode = juce::jlimit (0, 2, track.strip.compMode.load (std::memory_order_relaxed));
    switch (mode)
    {
        case 0: // Opto
        {
            const float peakRed = track.strip.compOptoPeakRed.load (std::memory_order_relaxed);
            threshKnob.setValue (-peakRed * (60.0f / 100.0f), juce::dontSendNotification);
            const float gain = track.strip.compOptoGain.load (std::memory_order_relaxed);
            makeupKnob.setValue ((gain - 50.0f) / 2.5f, juce::dontSendNotification);
            // ratio/attack/release: leave at displayed values; they don't apply
            break;
        }
        case 1: // FET
        {
            // INPUT knob mirrors compFetInput drive directly (0..40 dB).
            // OUTPUT knob mirrors compFetOutput directly — both independent.
            threshKnob.setValue (track.strip.compFetInput.load  (std::memory_order_relaxed),
                                    juce::dontSendNotification);
            // Map the discrete FET ratio index back to the unified ratio knob's
            // continuous scale. Mirrors the inverse of writeRatioToMode():
            //   0=4:1, 1=8:1, 2=12:1, 3=20:1, 4=All (clamped to display max).
            static const float kFetRatioDisplay[] = { 4.0f, 8.0f, 12.0f, 20.0f, 20.0f };
            const int ratioIdx = juce::jlimit (0, 4,
                track.strip.compFetRatio.load (std::memory_order_relaxed));
            ratioKnob.setValue   (kFetRatioDisplay[ratioIdx], juce::dontSendNotification);
            attackKnob.setValue  (track.strip.compFetAttack.load  (std::memory_order_relaxed), juce::dontSendNotification);
            releaseKnob.setValue (track.strip.compFetRelease.load (std::memory_order_relaxed), juce::dontSendNotification);
            makeupKnob.setValue  (track.strip.compFetOutput.load  (std::memory_order_relaxed), juce::dontSendNotification);
            break;
        }
        case 2: // VCA
        default:
            threshKnob.setValue  (track.strip.compVcaThreshDb.load (std::memory_order_relaxed), juce::dontSendNotification);
            ratioKnob.setValue   (track.strip.compVcaRatio.load    (std::memory_order_relaxed), juce::dontSendNotification);
            attackKnob.setValue  (track.strip.compVcaAttack.load   (std::memory_order_relaxed), juce::dontSendNotification);
            releaseKnob.setValue (track.strip.compVcaRelease.load  (std::memory_order_relaxed), juce::dontSendNotification);
            makeupKnob.setValue  (track.strip.compVcaOutput.load   (std::memory_order_relaxed), juce::dontSendNotification);
            break;
    }
}

void ChannelCompEditor::refreshModeButtons()
{
    const int m = juce::jlimit (0, 2, track.strip.compMode.load (std::memory_order_relaxed));
    modeOpto.setToggleState (m == 0, juce::dontSendNotification);
    modeFet.setToggleState  (m == 1, juce::dontSendNotification);
    modeVca.setToggleState  (m == 2, juce::dontSendNotification);
}

void ChannelCompEditor::timerCallback()
{
    // GR — peak-style meter: fast-down (snap), slow release-up.
    const float gr = track.meterGrDb.load (std::memory_order_relaxed);
    if (gr < displayedGrDb)
        displayedGrDb = gr;
    else
        displayedGrDb += (gr - displayedGrDb) * 0.18f;

    // Input — peak-style on the way up, slower on the way down so the
    // engineer can read fast transients.
    const float in = track.meterInputDb.load (std::memory_order_relaxed);
    if (in > displayedInputDb)
        displayedInputDb = in;
    else
        displayedInputDb += (in - displayedInputDb) * 0.10f;

    if (! grMeterArea.isEmpty())    repaint (grMeterArea.expanded (0, 14));
    if (! inputMeterArea.isEmpty()) repaint (inputMeterArea.expanded (0, 14));
}

void ChannelCompEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff181820));
    g.setColour (juce::Colour (0xff2a2a30));
    g.drawRect (getLocalBounds(), 1);

    auto drawVerticalMeter = [&] (juce::Rectangle<int> area,
                                    float dB, float minDb, float maxDb,
                                    juce::Colour fillTop, juce::Colour fillBottom,
                                    const juce::String& caption,
                                    const juce::String& valueText)
    {
        if (area.isEmpty()) return;
        const auto bar = area.toFloat();
        g.setColour (juce::Colour (0xff0c0c0e));
        g.fillRoundedRectangle (bar, 2.0f);
        g.setColour (juce::Colour (0xff2a2a2e));
        g.drawRoundedRectangle (bar, 2.0f, 0.8f);

        const float clamped = juce::jlimit (minDb, maxDb, dB);
        const float frac = (clamped - minDb) / (maxDb - minDb);
        if (frac > 0.001f)
        {
            const float fillH = (bar.getHeight() - 4.0f) * frac;
            auto fillRect = juce::Rectangle<float> (bar.getX() + 2.0f,
                                                      bar.getBottom() - 2.0f - fillH,
                                                      bar.getWidth() - 4.0f, fillH);
            juce::ColourGradient grad (fillTop, fillRect.getX(), fillRect.getY(),
                                         fillBottom, fillRect.getX(), fillRect.getBottom(),
                                         false);
            g.setGradientFill (grad);
            g.fillRoundedRectangle (fillRect, 1.5f);
        }

        // Caption above and value below — both in small grey type so the
        // bar stays the dominant element.
        g.setColour (juce::Colour (0xffa0a0a0));
        g.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
        g.drawText (caption, area.withY (area.getY() - 14).withHeight (12),
                     juce::Justification::centred, false);
        g.setFont (juce::Font (juce::FontOptions (9.5f)));
        g.drawText (valueText, area.withY (area.getBottom()).withHeight (14),
                     juce::Justification::centred, false);
    };

    // Input meter (left of pair) — green at low, yellow at -6, red at -1.
    drawVerticalMeter (inputMeterArea, displayedInputDb, -60.0f, 0.0f,
                        juce::Colour (0xffd05a5a),  // top (loud)
                        juce::Colour (0xff60c060),  // bottom (quiet)
                        "IN",
                        displayedInputDb <= -99.0f ? juce::String ("-inf")
                                                   : juce::String::formatted ("%.1f", displayedInputDb));

    // GR meter (right of pair) — gradient from gold at low GR to red at deep GR.
    // GR is negative dB; map -20 .. 0 onto the bar with 0 = empty, -20 = full.
    drawVerticalMeter (grMeterArea, -displayedGrDb, 0.0f, 20.0f,
                        juce::Colour (fourKColors::kHfRed).brighter (0.1f),
                        juce::Colour (fourKColors::kCompGold).brighter (0.2f),
                        "GR",
                        juce::String::formatted ("%.1f", displayedGrDb));

    // ── Threshold marker on the IN bar. Read the per-mode atomic the engine
    //    uses, then convert back to a unified threshold-dB axis for display.
    //    Mirrors CompMeterStrip's drawing exactly so the inline strip and
    //    the popup look the same.
    if (! inputMeterArea.isEmpty() && ! threshHandleArea.isEmpty())
    {
        const int mode = juce::jlimit (0, 2,
            track.strip.compMode.load (std::memory_order_relaxed));
        float thresh = 0.0f;
        switch (mode)
        {
            case 0:
            {
                const float peakRed = track.strip.compOptoPeakRed.load (std::memory_order_relaxed);
                thresh = -peakRed * (60.0f / 100.0f);
                break;
            }
            case 1:
            {
                const float drive = track.strip.compFetInput.load (std::memory_order_relaxed);
                thresh = -drive;
                break;
            }
            case 2:
            default:
                thresh = track.strip.compVcaThreshDb.load (std::memory_order_relaxed);
                break;
        }
        const float clamped = juce::jlimit (-60.0f, 0.0f, thresh);
        const float frac = (clamped - (-60.0f)) / 60.0f;
        const auto inBar = inputMeterArea.toFloat();
        const float y = inBar.getBottom() - 2.0f - frac * (inBar.getHeight() - 4.0f);

        // Larger, brighter triangle so the marker pops out of the meter.
        const float halfH = 9.0f;          // popup has more room than the inline strip
        const float baseX = (float) threshHandleArea.getX();
        const float tipX  = (float) threshHandleArea.getRight() + 2.0f;
        juce::Path tri;
        tri.addTriangle (baseX, y - halfH, baseX, y + halfH, tipX, y);

        const bool engaged = track.strip.compEnabled.load (std::memory_order_relaxed);
        const auto fill = engaged ? juce::Colour (fourKColors::kCompGold).brighter (0.30f)
                                    : juce::Colour (0xff909098);

        g.setColour (juce::Colours::black.withAlpha (0.45f));
        juce::Path shadow;
        shadow.addTriangle (baseX, y - halfH + 1.0f,
                             baseX, y + halfH + 1.0f,
                             tipX + 1.0f, y + 1.0f);
        g.fillPath (shadow);

        g.setColour (fill);
        g.fillPath (tri);
        g.setColour (juce::Colour (0xff0a0a0a));
        g.strokePath (tri, juce::PathStrokeType (1.0f));

        g.setColour (juce::Colour (0xfff8c878).withAlpha (engaged ? 0.75f : 0.40f));
        g.drawLine (inBar.getX() - 1.0f, y, inBar.getRight() + 1.0f, y, 1.2f);
    }
}

// ── Threshold drag (mirrors CompMeterStrip's mouse handling). The triangle
//    in threshHandleArea / clicks on the IN bar set threshold for the
//    currently-active comp mode, using the same per-mode mapping the engine
//    reads (see writeThresholdToMode for routing).
namespace
{
float dbForYInBar (int y, juce::Rectangle<int> bar)
{
    const float relY = (float) (bar.getBottom() - 2 - y) / (float) (bar.getHeight() - 4);
    return juce::jlimit (-60.0f, 0.0f, -60.0f + juce::jlimit (0.0f, 1.0f, relY) * 60.0f);
}

void writeThresholdForMode (Track& t, float threshDb)
{
    const int mode = juce::jlimit (0, 2, t.strip.compMode.load (std::memory_order_relaxed));
    switch (mode)
    {
        case 0:
        {
            const float peakRed = juce::jlimit (0.0f, 100.0f, -threshDb * (100.0f / 60.0f));
            t.strip.compOptoPeakRed.store (peakRed, std::memory_order_relaxed);
            break;
        }
        case 1:
        {
            const float drive = juce::jlimit (0.0f, 40.0f, -threshDb);
            t.strip.compFetInput.store (drive, std::memory_order_relaxed);
            break;
        }
        case 2:
        default:
            t.strip.compVcaThreshDb.store (juce::jlimit (-38.0f, 12.0f, threshDb),
                                            std::memory_order_relaxed);
            break;
    }
}
}

void ChannelCompEditor::mouseDown (const juce::MouseEvent& e)
{
    // Drag region: handle column + IN bar. Anywhere else is a no-op so the
    // user can still click empty popup background to lose focus.
    const auto hitArea = threshHandleArea.getUnion (inputMeterArea).expanded (2);
    draggingThreshold = hitArea.contains (e.getPosition());
    if (draggingThreshold)
    {
        writeThresholdForMode (track, dbForYInBar (e.y, inputMeterArea));
        // Auto-enable the comp on threshold touch (both surfaces - meter-
        // strip drag, popup-editor drag, popup-editor knob - now share the
        // same "engineer touched threshold => comp ON" rule).
        track.strip.compEnabled.store (true, std::memory_order_relaxed);
        onButton.setToggleState (true, juce::dontSendNotification);
        // Sync the THRESHOLD knob in the popup so its rotary catches up too.
        // Without this, dragging the triangle moved audio but the displayed
        // knob lagged until the user touched it.
        syncKnobsFromMode();
        repaint();
    }
}

void ChannelCompEditor::mouseDrag (const juce::MouseEvent& e)
{
    if (! draggingThreshold) return;
    writeThresholdForMode (track, dbForYInBar (e.y, inputMeterArea));
    track.strip.compEnabled.store (true, std::memory_order_relaxed);
    onButton.setToggleState (true, juce::dontSendNotification);
    syncKnobsFromMode();
    repaint();
}

void ChannelCompEditor::mouseUp (const juce::MouseEvent&)
{
    draggingThreshold = false;
}

void ChannelCompEditor::mouseDoubleClick (const juce::MouseEvent& e)
{
    const auto hitArea = threshHandleArea.getUnion (inputMeterArea).expanded (2);
    if (! hitArea.contains (e.getPosition())) return;

    // Reset to "no compression" - mode-specific because 0 dB threshold
    // doesn't mean the same thing across modes (see CompMeterStrip's note).
    const int mode = juce::jlimit (0, 2, track.strip.compMode.load (std::memory_order_relaxed));
    switch (mode)
    {
        case 0:  track.strip.compOptoPeakRed.store (0.0f, std::memory_order_relaxed); break;
        case 1:  track.strip.compFetInput   .store (0.0f, std::memory_order_relaxed);
                  track.strip.compFetOutput  .store (0.0f, std::memory_order_relaxed); break;
        case 2:
        default: track.strip.compVcaThreshDb.store (12.0f, std::memory_order_relaxed); break;
    }
    syncKnobsFromMode();
    repaint();
}

void ChannelCompEditor::resized()
{
    auto area = getLocalBounds().reduced (12);

    // Reserve a vertical strip on the right for: threshold drag handle +
    // IN + GR. Each gets its own column. Handle is narrow (just enough for
    // the triangle); IN and GR are equal-width meter bars.
    constexpr int kHandleW = 14;
    constexpr int kMeterW = 28;
    constexpr int kMeterGap = 4;
    constexpr int kMeterStripW = kHandleW + kMeterGap + kMeterW * 2 + kMeterGap;
    constexpr int kMeterTopPadding    = 16;  // leaves room for "IN" / "GR" caption
    constexpr int kMeterBottomPadding = 14;  // leaves room for value text

    auto meterStrip = area.removeFromRight (kMeterStripW + 8);
    meterStrip.removeFromRight (4);

    area.removeFromRight (8);  // gap between knobs and meter strip

    auto header = area.removeFromTop (24);
    onButton.setBounds (header.removeFromRight (60).reduced (1));
    area.removeFromTop (8);

    auto modeRow = area.removeFromTop (24);
    const int modeColW = modeRow.getWidth() / 3;
    modeOpto.setBounds (modeRow.removeFromLeft (modeColW).reduced (2, 0));
    modeFet.setBounds  (modeRow.removeFromLeft (modeColW).reduced (2, 0));
    modeVca.setBounds  (modeRow.reduced (2, 0));
    area.removeFromTop (12);

    constexpr int kKnobBlockH = 56 + 18 + 4;

    auto layoutPair = [&] (juce::Slider& kA, juce::Label& lA,
                            juce::Slider* kB, juce::Label* lB)
    {
        auto labelRow = area.removeFromTop (14);
        auto knobRow  = area.removeFromTop (kKnobBlockH);

        // Single-knob row (kB == null) → centre the one knob across the row.
        if (kB == nullptr)
        {
            const int knobW = juce::jmin (knobRow.getWidth(), kKnobBlockH);
            const int knobX = knobRow.getX() + (knobRow.getWidth() - knobW) / 2;
            lA.setBounds (labelRow);
            kA.setBounds (knobX, knobRow.getY(), knobW, knobRow.getHeight());
        }
        else
        {
            const int colW = labelRow.getWidth() / 2;
            lA.setBounds (labelRow.removeFromLeft (colW));
            lB->setBounds (labelRow);
            kA.setBounds (knobRow.removeFromLeft (colW).reduced (4));
            kB->setBounds (knobRow.reduced (4));
        }
        area.removeFromTop (6);
    };

    const int mode = juce::jlimit (0, 2, track.strip.compMode.load (std::memory_order_relaxed));
    if (mode == 0)
    {
        // Opto: only Thresh (PEAK RED) + Makeup (GAIN). Layout as two rows
        // each centred so the controls feel deliberate, not "missing".
        layoutPair (threshKnob, threshLabel, nullptr, nullptr);
        area.removeFromTop (6);
        layoutPair (makeupKnob, makeupLabel, nullptr, nullptr);
    }
    else
    {
        // FET / VCA: 2x2 grid of Thresh/Ratio + Atk/Rel, then a centred Makeup.
        layoutPair (threshKnob, threshLabel,  &ratioKnob,   &ratioLabel);
        layoutPair (attackKnob, attackLabel,  &releaseKnob, &releaseLabel);
        area.removeFromTop (6);
        layoutPair (makeupKnob, makeupLabel, nullptr, nullptr);
    }

    // Vertical meters on the right side. Handle | IN | GR, with the
    // threshold triangle living in the handle column. Reserve top + bottom
    // padding so the captions / numeric readouts drawn by paint() fit.
    auto handleCol = meterStrip.removeFromLeft (kHandleW);
    meterStrip.removeFromLeft (kMeterGap);
    auto inMeter = meterStrip.removeFromLeft (kMeterW);
    meterStrip.removeFromLeft (kMeterGap);
    auto grMeter = meterStrip.removeFromLeft (kMeterW);

    handleCol = handleCol.withTrimmedTop (kMeterTopPadding)
                          .withTrimmedBottom (kMeterBottomPadding);
    inMeter   = inMeter  .withTrimmedTop (kMeterTopPadding)
                          .withTrimmedBottom (kMeterBottomPadding);
    grMeter   = grMeter  .withTrimmedTop (kMeterTopPadding)
                          .withTrimmedBottom (kMeterBottomPadding);

    threshHandleArea = handleCol;
    inputMeterArea   = inMeter;
    grMeterArea      = grMeter;
}
} // namespace duskstudio
