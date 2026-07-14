#include "ChannelCompEditor.h"
#include "DuskStudioLookAndFeel.h"

#include <algorithm>

namespace duskstudio
{
namespace
{
// clamp with jlimit's argument order (lo, hi, value).
template <typename T>
inline T jlimit (T lo, T hi, T value) noexcept { return std::clamp (value, lo, hi); }
} // namespace
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

// Per-mode attack / release knob ranges and log-skew midpoints. The hardware
// reference units have very different timing characters: an 1176 spends most
// of its useful attack travel in the 20 µs..1 ms region (impossible to dial
// in on a linear slider), whereas a VCA-style mix-bus comp lives in the
// 1..30 ms range. Without per-mode skew the bottom 80 % of the FET attack
// knob is dead travel.
void applyTimingRanges (juce::Slider& attack, juce::Slider& release, int mode)
{
    switch (mode)
    {
        case 1:  // FET - 1176-shaped: 0.02..80 ms attack, 50..1100 ms release
            attack .setRange (0.02, 80.0,   0.01);
            attack .setSkewFactorFromMidPoint (0.8);    // midpoint = 0.8 ms (1176 sweet spot)
            release.setRange (50.0, 1100.0, 1.0);
            release.setSkewFactorFromMidPoint (200.0);
            break;

        case 2:  // VCA - dbx/SSL-shaped: 0.1..50 ms attack, 10..5000 ms release
            attack .setRange (0.1,  50.0,   0.01);
            attack .setSkewFactorFromMidPoint (3.0);
            release.setRange (10.0, 5000.0, 1.0);
            release.setSkewFactorFromMidPoint (200.0);
            break;

        default:
            // OPTO hides these knobs entirely; ranges left at the values
            // styleKnob applied at construction.
            break;
    }
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

    // VCA knee toggle. Same gold styling as the mode buttons but standalone
    // (not part of the mode radio group); writes the OverEasy session atom.
    vcaOverEasyBtn.setClickingTogglesState (true);
    vcaOverEasyBtn.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff181820));
    vcaOverEasyBtn.setColour (juce::TextButton::buttonOnColourId, juce::Colour (fourKColors::kCompGold));
    vcaOverEasyBtn.setColour (juce::TextButton::textColourOffId,  juce::Colour (0xffd09060));
    vcaOverEasyBtn.setColour (juce::TextButton::textColourOnId,   juce::Colour (0xff121214));
    vcaOverEasyBtn.setTooltip ("Soft knee: parabolic (soft) knee. Off = hard knee.");
    vcaOverEasyBtn.setToggleState (track.strip.compVcaOverEasy.load (std::memory_order_relaxed),
                                    juce::dontSendNotification);
    vcaOverEasyBtn.onClick = [this]
    {
        track.strip.compVcaOverEasy.store (vcaOverEasyBtn.getToggleState(),
                                            std::memory_order_relaxed);
    };
    addAndMakeVisible (vcaOverEasyBtn);

    // VCA detector mode toggle - when ON, switches the donor's RMS detector
    // to a fixed 10 ms TC (dbx 160 spec). When OFF, the level-adaptive
    // 35 ms -> 5 ms curve (donor default).
    vcaDetectorBtn.setClickingTogglesState (true);
    vcaDetectorBtn.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff181820));
    vcaDetectorBtn.setColour (juce::TextButton::buttonOnColourId, juce::Colour (fourKColors::kCompGold));
    vcaDetectorBtn.setColour (juce::TextButton::textColourOffId,  juce::Colour (0xffd09060));
    vcaDetectorBtn.setColour (juce::TextButton::textColourOnId,   juce::Colour (0xff121214));
    vcaDetectorBtn.setTooltip ("Classic: fixed 10 ms RMS time constant. Off = adaptive 35-5 ms.");
    vcaDetectorBtn.setToggleState (track.strip.compVcaDetectorClassic.load (std::memory_order_relaxed),
                                    juce::dontSendNotification);
    vcaDetectorBtn.onClick = [this]
    {
        track.strip.compVcaDetectorClassic.store (vcaDetectorBtn.getToggleState(),
                                                    std::memory_order_relaxed);
    };
    addAndMakeVisible (vcaDetectorBtn);

    const auto gold = juce::Colour (fourKColors::kCompGold);
    styleKnob (threshKnob,  gold, ChannelStripParams::kCompThreshMin,  ChannelStripParams::kCompThreshMax,  -12.0,  -24.0, " dB", 1);
    // threshKnob stays hidden in the new layout - threshold (and the
    // OPTO peak-reduction / FET input equivalents) are set via the
    // triangle handle on the GR meter strip. The knob is kept as a
    // backing value-source for syncKnobsFromMode / writeThresholdToMode
    // but never added to the visible component tree.
    styleKnob (ratioKnob,   gold, ChannelStripParams::kCompRatioMin,   ChannelStripParams::kCompRatioMax,    4.0,    4.0, ":1",  1);
    styleKnob (attackKnob,  gold, ChannelStripParams::kCompAttackMin,  ChannelStripParams::kCompAttackMax,  10.0,   10.0, " ms", 1);
    styleKnob (releaseKnob, gold, ChannelStripParams::kCompReleaseMin, ChannelStripParams::kCompReleaseMax,100.0,  200.0, " ms", 0);
    styleKnob (makeupKnob,  gold, ChannelStripParams::kCompMakeupMin,  ChannelStripParams::kCompMakeupMax,   0.0,    0.0, " dB", 1);

    threshKnob.onValueChange  = [this] { writeThresholdToMode(); };
    ratioKnob.onValueChange   = [this] { writeRatioToMode(); };
    attackKnob.onValueChange  = [this] { writeAttackToMode(); };
    releaseKnob.onValueChange = [this] { writeReleaseToMode(); };
    makeupKnob.onValueChange  = [this] { writeMakeupToMode(); };

    // threshKnob deliberately NOT addAndMakeVisible - value is read +
    // written via the triangle handle, not a knob.
    addAndMakeVisible (ratioKnob);
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

    refreshLabelsForMode();   // applies per-mode ranges + sets size
    // Sync AFTER refreshLabelsForMode so the per-mode slider range (set by
    // applyTimingRanges) is already in place when the stored atom value is
    // pushed into the slider - otherwise the default construction range
    // would clamp values that are valid in the active mode.
    syncKnobsFromMode();

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
    const int m = jlimit (0, 2, track.strip.compMode.load (std::memory_order_relaxed));

    // Uniform RAT / ATK / REL / MAKEUP labels across every mode. Matches
    // the inline strip + bus + master comp labels so every comp in the
    // mixer reads identically. FET still calls this "output gain"
    // internally and VCA still calls it "makeup gain" - the UX cost of
    // two different labels for the same control isn't worth the
    // technical accuracy.
    const char* thresh  = "THRESHOLD";
    const char* ratio   = "RATIO";
    const char* attack  = "ATTACK";
    const char* release = "RELEASE";
    const char* makeup  = "MAKEUP";

    switch (m)
    {
        case 0:  // Opto: peak-reduction style. Attack/release/ratio are
                 // fixed by the optical model - hide those knobs entirely
                 // (don't show "(fixed)" placeholders).
            thresh  = "PEAK RED";
            break;
        case 1:  // FET (1176): real threshold (donor fet_threshold).
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

    // Per-mode threshold knob range. FET / Opto top out at 0 dB; VCA's
    // compVcaThreshDb runs -38..+12 (where +12 is the "no compression"
    // sentinel), so VCA needs the knob to reach +12 or the stored value gets
    // clamped to 0 on display / double-click reset and the sentinel is lost.
    threshKnob.setRange (-60.0, (m == 2) ? 12.0 : 0.0, 0.1);

    // OPTO hides the fixed knobs (ratio / attack / release) entirely -
    // showing them greyed out with "(fixed)" labels added visual noise.
    // FET / VCA expose the full 2×2 grid.
    const bool optoMode = (m == 0);
    ratioKnob  .setVisible (! optoMode);   ratioLabel  .setVisible (! optoMode);
    attackKnob .setVisible (! optoMode);   attackLabel .setVisible (! optoMode);
    releaseKnob.setVisible (! optoMode);   releaseLabel.setVisible (! optoMode);
    ratioKnob  .setEnabled (! optoMode);
    attackKnob .setEnabled (! optoMode);
    releaseKnob.setEnabled (! optoMode);

    // Per-mode timing ranges + log skew. Must run before any setValue() that
    // pushes a stored atom into the slider - the slider will clamp the
    // incoming value against whichever range is currently active.
    applyTimingRanges (attackKnob, releaseKnob, m);

    // OverEasy + detector-mode toggles: VCA only.
    const bool vcaMode = (m == 2);
    vcaOverEasyBtn.setVisible (vcaMode);
    vcaOverEasyBtn.setEnabled (vcaMode);
    vcaOverEasyBtn.setToggleState (track.strip.compVcaOverEasy.load (std::memory_order_relaxed),
                                    juce::dontSendNotification);
    vcaDetectorBtn.setVisible (vcaMode);
    vcaDetectorBtn.setEnabled (vcaMode);
    vcaDetectorBtn.setToggleState (track.strip.compVcaDetectorClassic.load (std::memory_order_relaxed),
                                    juce::dontSendNotification);

    // Uniform height - same grid across every mode, no resize on
    // mode swap.
    constexpr int kKnobBlockH = 56 + 18 + 4;
    constexpr int kRowGap     = 6;
    constexpr int kHeaderH    = 24 + 8 + 24 + 12;
    constexpr int kFooterPad  = 16;
    const int contentH = kHeaderH + kKnobBlockH + kRowGap + kKnobBlockH + kFooterPad;
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
    const int mode = jlimit (0, 2, track.strip.compMode.load (std::memory_order_relaxed));
    switch (mode)
    {
        case 0: // Opto: -60 dB -> 100% peak reduction, 0 dB -> 0%
        {
            const float peakRed = jlimit (0.0f, 100.0f, -threshDb * (100.0f / 60.0f));
            track.strip.compOptoPeakRed.store (peakRed, std::memory_order_relaxed);
            break;
        }
        case 1: // FET
        {
            // Adjustable threshold (donor's fet_threshold param). Original
            // 1176 hardware had no threshold control - the input knob
            // drove signal into a fixed -10 dBFS detector. Dusk exposes a
            // real threshold so the FET's UX matches Opto / VCA; the
            // characteristic saturation / transformer colouration is still
            // baked into the donor's FET stage.
            track.strip.compFetThresholdDb.store (jlimit (-60.0f, 0.0f, threshDb),
                                                    std::memory_order_relaxed);
            break;
        }
        case 2: // VCA: direct
        default:
            track.strip.compVcaThreshDb.store (jlimit (-38.0f, 12.0f, threshDb),
                                                std::memory_order_relaxed);
            break;
    }
}

void ChannelCompEditor::writeRatioToMode()
{
    const float r = (float) ratioKnob.getValue();
    const int mode = jlimit (0, 2, track.strip.compMode.load (std::memory_order_relaxed));
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
            track.strip.compVcaRatio.store (jlimit (1.0f, 120.0f, r),
                                             std::memory_order_relaxed);
            break;
    }
}

void ChannelCompEditor::writeAttackToMode()
{
    const float a = (float) attackKnob.getValue();
    const int mode = jlimit (0, 2, track.strip.compMode.load (std::memory_order_relaxed));
    switch (mode)
    {
        case 0: break;  // Opto attack is the optical lag - fixed
        case 1:
            track.strip.compFetAttack.store (jlimit (0.02f, 80.0f, a),
                                              std::memory_order_relaxed);
            break;
        case 2:
        default:
            track.strip.compVcaAttack.store (jlimit (0.1f, 50.0f, a),
                                              std::memory_order_relaxed);
            break;
    }
}

void ChannelCompEditor::writeReleaseToMode()
{
    const float r = (float) releaseKnob.getValue();
    const int mode = jlimit (0, 2, track.strip.compMode.load (std::memory_order_relaxed));
    switch (mode)
    {
        case 0: break;  // Opto release is the optical decay - fixed
        case 1:
            track.strip.compFetRelease.store (jlimit (50.0f, 1100.0f, r),
                                               std::memory_order_relaxed);
            break;
        case 2:
        default:
            track.strip.compVcaRelease.store (jlimit (10.0f, 5000.0f, r),
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

    const int mode = jlimit (0, 2, track.strip.compMode.load (std::memory_order_relaxed));
    switch (mode)
    {
        case 0: // Opto: 0..100 % gain, 50 % = unity
        {
            const float pct = jlimit (0.0f, 100.0f, 50.0f + dB * 2.5f);
            track.strip.compOptoGain.store (pct, std::memory_order_relaxed);
            break;
        }
        case 1: // FET: independent OUTPUT knob, no chain coupling.
        {
            track.strip.compFetOutput.store (jlimit (-20.0f, 20.0f, dB),
                                              std::memory_order_relaxed);
            break;
        }
        case 2:
        default:
            track.strip.compVcaOutput.store (jlimit (-20.0f, 20.0f, dB),
                                              std::memory_order_relaxed);
            break;
    }
}

// Read back per-mode params and update the knob displays (called when the
// mode changes so the user sees the calibration for the active mode).
void ChannelCompEditor::syncKnobsFromMode()
{
    const int mode = jlimit (0, 2, track.strip.compMode.load (std::memory_order_relaxed));
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
            // THRESHOLD knob now mirrors compFetThresholdDb directly
            // (-60..0 dB). Output / attack / release / ratio unchanged.
            threshKnob.setValue (track.strip.compFetThresholdDb.load (std::memory_order_relaxed),
                                    juce::dontSendNotification);
            // Map the discrete FET ratio index back to the unified ratio knob's
            // continuous scale. Mirrors the inverse of writeRatioToMode():
            //   0=4:1, 1=8:1, 2=12:1, 3=20:1, 4=All (clamped to display max).
            static const float kFetRatioDisplay[] = { 4.0f, 8.0f, 12.0f, 20.0f, 20.0f };
            const int ratioIdx = jlimit (0, 4,
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
    const int m = jlimit (0, 2, track.strip.compMode.load (std::memory_order_relaxed));
    modeOpto.setToggleState (m == 0, juce::dontSendNotification);
    modeFet.setToggleState  (m == 1, juce::dontSendNotification);
    modeVca.setToggleState  (m == 2, juce::dontSendNotification);
}

void ChannelCompEditor::timerCallback()
{
    // GR - peak-style meter: fast-down (snap), slow release-up.
    const float gr = track.meterGrDb.load (std::memory_order_relaxed);
    if (gr < displayedGrDb)
        displayedGrDb = gr;
    else
        displayedGrDb += (gr - displayedGrDb) * 0.18f;

    // Input - peak-style on the way up, slower on the way down so the
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

        const float clamped = jlimit (minDb, maxDb, dB);
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

        // Caption above and value below - both in small grey type so the
        // bar stays the dominant element.
        g.setColour (juce::Colour (0xffa0a0a0));
        g.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
        g.drawText (caption, area.withY (area.getY() - 14).withHeight (12),
                     juce::Justification::centred, false);
        g.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
        g.drawText (valueText, area.withY (area.getBottom() + 1).withHeight (16),
                     juce::Justification::centred, false);
    };

    // Input meter (left of pair) - green at low, yellow at -6, red at -1.
    drawVerticalMeter (inputMeterArea, displayedInputDb, -60.0f, 0.0f,
                        juce::Colour (0xffd05a5a),  // top (loud)
                        juce::Colour (0xff60c060),  // bottom (quiet)
                        "IN",
                        displayedInputDb <= -99.0f ? juce::String ("-inf")
                                                   : juce::String::formatted ("%.1f", displayedInputDb));

    // GR meter (right of pair) - gradient from gold at low GR to red at deep GR.
    // GR is negative dB; map -20 .. 0 onto the bar with 0 = empty, -20 = full.
    drawVerticalMeter (grMeterArea, -displayedGrDb, 0.0f, 20.0f,
                        juce::Colour (fourKColors::kHfRed).brighter (0.1f),
                        juce::Colour (fourKColors::kCompGold).brighter (0.2f),
                        "GR",
                        juce::String::formatted ("%.1f", displayedGrDb));

    // Threshold marker on the IN bar. Read the per-mode atomic the engine
    //    uses, then convert back to a unified threshold-dB axis for display.
    //    Mirrors CompMeterStrip's drawing exactly so the inline strip and
    //    the popup look the same.
    if (! inputMeterArea.isEmpty() && ! threshHandleArea.isEmpty())
    {
        const int mode = jlimit (0, 2,
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
                thresh = track.strip.compFetThresholdDb.load (std::memory_order_relaxed);
                break;
            }
            case 2:
            default:
                thresh = track.strip.compVcaThreshDb.load (std::memory_order_relaxed);
                break;
        }
        const float clamped = jlimit (-60.0f, 0.0f, thresh);
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

// Threshold drag (mirrors CompMeterStrip's mouse handling). The triangle
//    in threshHandleArea / clicks on the IN bar set threshold for the
//    currently-active comp mode, using the same per-mode mapping the engine
//    reads (see writeThresholdToMode for routing).
namespace
{
float dbForYInBar (int y, juce::Rectangle<int> bar)
{
    const float relY = (float) (bar.getBottom() - 2 - y) / (float) (bar.getHeight() - 4);
    return jlimit (-60.0f, 0.0f, -60.0f + jlimit (0.0f, 1.0f, relY) * 60.0f);
}

void writeThresholdForMode (Track& t, float threshDb)
{
    const int mode = jlimit (0, 2, t.strip.compMode.load (std::memory_order_relaxed));
    switch (mode)
    {
        case 0:
        {
            const float peakRed = jlimit (0.0f, 100.0f, -threshDb * (100.0f / 60.0f));
            t.strip.compOptoPeakRed.store (peakRed, std::memory_order_relaxed);
            break;
        }
        case 1:
        {
            t.strip.compFetThresholdDb.store (jlimit (-60.0f, 0.0f, threshDb),
                                                std::memory_order_relaxed);
            break;
        }
        case 2:
        default:
            t.strip.compVcaThreshDb.store (jlimit (-38.0f, 12.0f, threshDb),
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
    const int mode = jlimit (0, 2, track.strip.compMode.load (std::memory_order_relaxed));
    switch (mode)
    {
        case 0:  track.strip.compOptoPeakRed.store (0.0f, std::memory_order_relaxed); break;
        case 1:  track.strip.compFetThresholdDb.store (0.0f, std::memory_order_relaxed);
                  track.strip.compFetInput      .store (0.0f, std::memory_order_relaxed);
                  track.strip.compFetOutput     .store (0.0f, std::memory_order_relaxed); break;
        case 2:
        default: track.strip.compVcaThreshDb.store (12.0f, std::memory_order_relaxed); break;
    }
    syncKnobsFromMode();
    repaint();
}

void ChannelCompEditor::resized()
{
    auto area = getLocalBounds().reduced (12);

    // Mixbus-style modal layout mirrors the channel-strip COMP section:
    //   ┌─────────────────────────────────────────┐
    //   │ onButton                                │  ← header row
    //   │ [Opto] [FET] [VCA]                       │  ← mode row
    //   │ ┌────┐ ┌──────────┐ ┌────┐ ┌────┐       │
    //   │ │THR │ │GR meters │ │RAT │ │OUT │       │
    //   │ │slid│ │IN  GR    │ │ATK │ │REL │       │
    //   │ │    │ │          │ │    │ │MAK │       │
    //   │ └────┘ └──────────┘ └────┘ └────┘       │
    //   └─────────────────────────────────────────┘

    constexpr int kHandleW = 14;
    constexpr int kMeterW = 28;
    constexpr int kMeterGap = 4;
    constexpr int kMeterStripW = kHandleW + kMeterGap + kMeterW * 2 + kMeterGap;
    constexpr int kMeterTopPadding    = 16;
    constexpr int kMeterBottomPadding = 14;

    // Top: ON button + mode row.
    auto header = area.removeFromTop (24);
    onButton.setBounds (header.removeFromRight (60).reduced (1));
    area.removeFromTop (8);
    auto modeRow = area.removeFromTop (24);
    const int modeColW = modeRow.getWidth() / 3;
    modeOpto.setBounds (modeRow.removeFromLeft (modeColW).reduced (2, 0));
    modeFet.setBounds  (modeRow.removeFromLeft (modeColW).reduced (2, 0));
    modeVca.setBounds  (modeRow.reduced (2, 0));
    area.removeFromTop (6);

    // VCA knee + detector-mode toggles. Two side-by-side buttons below the
    // mode row, visible only in VCA mode (space reclaimed otherwise so the
    // rotaries grid doesn't shift around when switching modes).
    if (vcaOverEasyBtn.isVisible())
    {
        auto kneeRow = area.removeFromTop (20);
        const int colW = kneeRow.getWidth() / 2;
        vcaOverEasyBtn.setBounds (kneeRow.removeFromLeft (colW).reduced (4, 0));
        vcaDetectorBtn.setBounds (kneeRow.reduced (4, 0));
        area.removeFromTop (6);
    }
    else
    {
        area.removeFromTop (6);
    }

    // BODY: LEFT (meter strip with threshold drag handle) | RIGHT (rotaries grid).
    // No dedicated threshold knob/slider - the triangle on the meter
    // handle column drives threshold / peak-red / FET input.
    constexpr int kColGap = 12;

    auto body = area;
    auto meterStrip = body.removeFromLeft (kMeterStripW);
    body.removeFromLeft (kColGap);
    auto rotariesCol = body;     // remaining width

    // threshLabel is no longer rendered as a column header; hide it.
    threshLabel.setVisible (false);
    threshKnob .setVisible (false);

    // Meter strip: handle | IN | GR.
    auto handleCol = meterStrip.removeFromLeft (kHandleW);
    meterStrip.removeFromLeft (kMeterGap);
    auto inMeter = meterStrip.removeFromLeft (kMeterW);
    meterStrip.removeFromLeft (kMeterGap);
    auto grMeter = meterStrip.removeFromLeft (kMeterW);
    handleCol = handleCol.withTrimmedTop (kMeterTopPadding).withTrimmedBottom (kMeterBottomPadding);
    inMeter   = inMeter  .withTrimmedTop (kMeterTopPadding).withTrimmedBottom (kMeterBottomPadding);
    grMeter   = grMeter  .withTrimmedTop (kMeterTopPadding).withTrimmedBottom (kMeterBottomPadding);
    threshHandleArea = handleCol;
    inputMeterArea   = inMeter;
    grMeterArea      = grMeter;

    // Rotaries grid on the right. FET/VCA show 2×2 (RAT/ATK over
    // REL/MAK). OPTO is sparse: only MAKEUP (rendered as "GAIN" by the
    // label refresh), centred at the top of the column.
    const int mode = jlimit (0, 2, track.strip.compMode.load (std::memory_order_relaxed));
    constexpr int kRowLabelH = 14;
    const int rowH = (rotariesCol.getHeight() - kRowLabelH * 2 - 12) / 2;
    auto layoutCell = [&] (juce::Rectangle<int> cell,
                             juce::Slider& k, juce::Label& l)
    {
        l.setBounds (cell.removeFromTop (kRowLabelH));
        k.setBounds (cell.reduced (4));
    };

    // FET / VCA: full 2×2 grid (RAT / ATK on top, REL / GAIN on bottom).
    // OPTO: only GAIN visible - centered in the rotaries column. The
    // visibility flags are driven by refreshLabelsForMode; we just lay
    // out whichever knobs are visible here.
    makeupKnob.setVisible (true);   makeupLabel.setVisible (true);

    if (mode == 0)  // OPTO - single GAIN knob, centred vertically.
    {
        const int cellH = kRowLabelH + rowH;
        const int cellW = std::min (rotariesCol.getWidth(), 120);
        const auto centred = juce::Rectangle<int> (
            rotariesCol.getCentreX() - cellW / 2,
            rotariesCol.getCentreY() - cellH / 2,
            cellW, cellH);
        layoutCell (centred, makeupKnob, makeupLabel);
    }
    else
    {
        auto rowTop    = rotariesCol.removeFromTop (kRowLabelH + rowH);
        rotariesCol.removeFromTop (12);
        auto rowBottom = rotariesCol.removeFromTop (kRowLabelH + rowH);
        const int colW = rowTop.getWidth() / 2;
        layoutCell (rowTop   .removeFromLeft (colW), ratioKnob,   ratioLabel);
        layoutCell (rowTop,                          attackKnob,  attackLabel);
        layoutCell (rowBottom.removeFromLeft (colW), releaseKnob, releaseLabel);
        layoutCell (rowBottom,                        makeupKnob,  makeupLabel);
    }
}
} // namespace duskstudio
