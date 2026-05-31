#include "MasterStripComponent.h"
#include "CompBypassLed.h"
#include "DuskContextMenu.h"
#include "DuskComboBox.h"
#include "../engine/AudioEngine.h"
#include "DimOverlay.h"
#include "DuskStudioLookAndFeel.h"
#include "SteppedKnob.h"
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
    // Double-click → midPt reset, Shift-drag → velocity-precision
    // fine adjust. Mirrors channel-strip knob behaviour.
    s.setDoubleClickReturnValue (true, midPt);
    s.setVelocityBasedMode (false);
    s.setVelocityModeParameters (1.0, 1, 0.0, /*userCanPressKeyToSwap*/ true);
}

void styleSmallLabel (juce::Label& lbl, const juce::String& text, juce::Colour col)
{
    lbl.setText (text, juce::dontSendNotification);
    lbl.setJustificationType (juce::Justification::centred);
    lbl.setColour (juce::Label::textColourId, col);
    // Bumped from 8.5 pt → 10.5 pt + minimum horizontal scale so the
    // Pultec captions ("HF BOOST FREQ", "HF− ATTEN FREQ") stay legible
    // against the new blue section background without clipping at the
    // narrow column width.
    lbl.setFont (juce::Font (juce::FontOptions (10.5f, juce::Font::bold)));
    lbl.setMinimumHorizontalScale (0.6f);
}

void styleToggleNS (juce::TextButton& b, juce::Colour onColour)
{
    b.setClickingTogglesState (true);
    b.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff202024));
    b.setColour (juce::TextButton::buttonOnColourId, onColour);
    b.setColour (juce::TextButton::textColourOffId,  juce::Colour (0xffb0a080));
    b.setColour (juce::TextButton::textColourOnId,   juce::Colour (0xff121214));
}

// Match ChannelEqEditor / ChannelCompEditor visual grammar so all four
// strip types (channel / aux / bus / master) share one editor-modal look.
constexpr int kEditorKnobSize    = 56;
constexpr int kEditorValueH      = 18;
constexpr int kEditorKnobBlockH  = kEditorKnobSize + kEditorValueH + 6;   // 80
constexpr int kEditorOuterPad    = 12;
constexpr int kEditorHeaderH     = 24;
constexpr int kEditorHeaderGap   = 8;
constexpr int kEditorLabelRowH   = 18;

void styleEditorKnob (juce::Slider& k, juce::Colour fill,
                       double mn, double mx, double defaultVal, double skewMid,
                       const juce::String& suffix, int decimals)
{
    k.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    k.setColour (juce::Slider::rotarySliderFillColourId, fill);
    k.setColour (juce::Slider::rotarySliderOutlineColourId, juce::Colour (0xff404048));
    const double step = (mn < 0 ? 0.1 : 0.01);
    k.setRange (mn, mx, step);
    if (skewMid > mn && skewMid < mx) k.setSkewFactorFromMidPoint (skewMid);
    k.setDoubleClickReturnValue (true, defaultVal);
    k.setTextBoxStyle (juce::Slider::TextBoxBelow, true, 80, 18);
    k.setColour (juce::Slider::textBoxTextColourId,       juce::Colour (0xffe0e0e0));
    k.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colour (0));
    k.setColour (juce::Slider::textBoxOutlineColourId,    juce::Colour (0));
    k.setNumDecimalPlacesToDisplay (decimals);
    k.setTextValueSuffix (suffix);
    // Shift-drag fine-adjust (velocity-precision swap on shift hold).
    k.setVelocityBasedMode (false);
    k.setVelocityModeParameters (1.0, 1, 0.0, /*userCanPressKeyToSwap*/ true);
}

void styleEditorLabel (juce::Label& l, const juce::String& text, juce::Colour accent)
{
    l.setText (text, juce::dontSendNotification);
    l.setJustificationType (juce::Justification::centred);
    l.setColour (juce::Label::textColourId, accent.brighter (0.2f));
    l.setFont (juce::Font (juce::FontOptions (16.0f, juce::Font::bold)));
}

void styleEditorCompLabel (juce::Label& l, const juce::String& text)
{
    l.setText (text, juce::dontSendNotification);
    l.setJustificationType (juce::Justification::centred);
    l.setColour (juce::Label::textColourId, juce::Colour (0xffb07050));
    l.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
}

void styleEditorEnableBtn (juce::TextButton& b, juce::Colour onColour)
{
    b.setClickingTogglesState (true);
    b.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff202024));
    b.setColour (juce::TextButton::buttonOnColourId, onColour);
    b.setColour (juce::TextButton::textColourOffId,  juce::Colour (0xffd0c0a0));
    b.setColour (juce::TextButton::textColourOnId,   juce::Colour (0xff121214));
}

// Master EQ modal — full(er) Pultec surface mirroring the donor's
// TubeEQProcessor parameter set. 2×4 grid:
//   Row 1 (LF + OUT):  LF BOOST | LF ATTEN | LF FREQ | OUT
//   Row 2 (HF):        HF BOOST | HF ATTEN | HF BOOST FREQ | HF ATTEN FREQ
// Pultec freq pickers are discrete (donor accepts float; we expose the
// canonical Pultec-hardware values via DuskComboBox so the UX matches
// what engineers expect from EQP-1A clones). Bandwidth + Mid Dip/Peak
// + Tube Drive + Input gain stay hidden — bandwidth/drive are fixed
// per spec; Mid section belongs on the dedicated mastering view.
class MasterEqEditorPanel final : public juce::Component
{
public:
    MasterEqEditorPanel (MasterBusParams& p) : params (p)
    {
        // Hardware-grammar palette: black knobs + cream stencils + teal
        // blue chassis. Variable name kept for callsite compatibility.
        const auto pultecGold  = juce::Colour (0xff1a1a1c);   // bakelite black
        const auto pultecCream = juce::Colour (0xfff0e8d0);   // stencil cream
        styleEditorEnableBtn (enableBtn, juce::Colour (0xff7090a0));
        (void) pultecCream;
        enableBtn.setButtonText ("EQ");
        enableBtn.setToggleState (params.eqEnabled.load (std::memory_order_relaxed),
                                    juce::dontSendNotification);
        enableBtn.onClick = [this]
        {
            params.eqEnabled.store (enableBtn.getToggleState(), std::memory_order_relaxed);
        };
        addAndMakeVisible (enableBtn);

        styleEditorKnob (lfBoost,   pultecGold,   0.0,  10.0,   3.0,  3.0, "", 1);
        styleEditorKnob (lfAtten,   pultecGold,   0.0,  10.0,   0.0,  3.0, "", 1);
        styleEditorKnob (hfBoost,   pultecGold,   0.0,  10.0,   3.0,  3.0, "", 1);
        styleEditorKnob (hfAtten,   pultecGold,   0.0,  10.0,   3.0,  3.0, "", 1);
        styleEditorKnob (hfBandwidth, pultecGold, 0.0,  10.0,   0.5,  5.0, "", 1);
        lfBoost     .setValue (params.eqLfBoost         .load(), juce::dontSendNotification);
        lfAtten     .setValue (params.eqLfAtten         .load(), juce::dontSendNotification);
        hfBoost     .setValue (params.eqHfBoost         .load(), juce::dontSendNotification);
        hfAtten     .setValue (params.eqHfAtten         .load(), juce::dontSendNotification);
        hfBandwidth .setValue (params.eqHfBoostBandwidth.load(), juce::dontSendNotification);

        auto arm = [this]
        {
            params.eqEnabled.store (true, std::memory_order_release);
            enableBtn.setToggleState (true, juce::dontSendNotification);
        };
        lfBoost    .setTooltip ("Pultec LF boost (0..10). Double-click for 3; Shift-drag for fine.");
        lfAtten    .setTooltip ("Pultec LF attenuate (0..10). Double-click for 0; Shift-drag for fine.");
        hfBoost    .setTooltip ("Pultec HF boost (0..10). Double-click for 3; Shift-drag for fine.");
        hfAtten    .setTooltip ("Pultec HF attenuate (0..10). Double-click for 3; Shift-drag for fine.");
        hfBandwidth.setTooltip ("Pultec HF bandwidth (Sharp..Broad, 0..10). Double-click for 0.5; Shift-drag for fine.");
        lfBoost    .onValueChange = [this, arm] { params.eqLfBoost         .store ((float) lfBoost    .getValue(), std::memory_order_relaxed); arm(); };
        lfAtten    .onValueChange = [this, arm] { params.eqLfAtten         .store ((float) lfAtten    .getValue(), std::memory_order_relaxed); arm(); };
        hfBoost    .onValueChange = [this, arm] { params.eqHfBoost         .store ((float) hfBoost    .getValue(), std::memory_order_relaxed); arm(); };
        hfAtten    .onValueChange = [this, arm] { params.eqHfAtten         .store ((float) hfAtten    .getValue(), std::memory_order_relaxed); arm(); };
        hfBandwidth.onValueChange = [this, arm] { params.eqHfBoostBandwidth.store ((float) hfBandwidth.getValue(), std::memory_order_relaxed); arm(); };

        addAndMakeVisible (lfBoost); addAndMakeVisible (lfAtten);
        addAndMakeVisible (hfBoost); addAndMakeVisible (hfAtten);
        addAndMakeVisible (hfBandwidth);

        // Thumb override — styleEditorKnob doesn't explicitly set thumb
        // colour; cream pointer reads against the black bakelite body.
        for (auto* k : { &lfBoost, &lfAtten, &hfBoost, &hfAtten, &hfBandwidth })
            k->setColour (juce::Slider::thumbColourId, pultecCream);

        // Pultec discrete-frequency rotaries (dented knobs). Same range
        // / format logic as the inline strip version — snaps to the
        // hardware-canonical Hz positions and renders the label in the
        // textbox below.
        auto setupFreqKnob = [this, arm, pultecGold, pultecCream] (juce::Slider& k, const int* hz, int count,
                                                                       std::atomic<float>& atom)
        {
            k.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
            k.setColour (juce::Slider::rotarySliderFillColourId,    pultecGold);
            k.setColour (juce::Slider::rotarySliderOutlineColourId, juce::Colour (0xff0a0a0c));
            k.setColour (juce::Slider::thumbColourId,               pultecCream);
            k.setColour (juce::Slider::textBoxTextColourId,         pultecCream);
            k.setColour (juce::Slider::textBoxBackgroundColourId,   juce::Colour (0));
            k.setColour (juce::Slider::textBoxOutlineColourId,      juce::Colour (0));
            k.setRange (0.0, (double) (count - 1), 1.0);
            k.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 80, 18);
            k.textFromValueFunction = [hz, count] (double v) -> juce::String
            {
                const int idx = juce::jlimit (0, count - 1, (int) std::round (v));
                const int h = hz[idx];
                return h >= 1000 ? juce::String (h / 1000) + " kHz"
                                  : juce::String (h)        + " Hz";
            };
            const float current = atom.load (std::memory_order_relaxed);
            int bestIdx = 0;
            float bestDist = std::abs (current - (float) hz[0]);
            for (int i = 1; i < count; ++i)
            {
                const float d = std::abs (current - (float) hz[i]);
                if (d < bestDist) { bestDist = d; bestIdx = i; }
            }
            k.setValue ((double) bestIdx, juce::dontSendNotification);
            k.updateText();
            k.onValueChange = [&k, hz, count, &atom, arm]
            {
                const int idx = juce::jlimit (0, count - 1, (int) std::round (k.getValue()));
                atom.store ((float) hz[idx], std::memory_order_relaxed);
                arm();
            };
            addAndMakeVisible (k);
        };
        static const int kLfHz[]      = { 20, 30, 60, 100 };
        static const int kHfBoostHz[] = { 3000, 4000, 5000, 8000, 10000, 12000, 16000 };
        static const int kHfAttenHz[] = { 5000, 10000, 20000 };
        setupFreqKnob (lfFreqKnob,      kLfHz,      4, params.eqLfFreq);
        setupFreqKnob (hfBoostFreqKnob, kHfBoostHz, 7, params.eqHfBoostFreq);
        setupFreqKnob (hfAttenFreqKnob, kHfAttenHz, 3, params.eqHfAttenFreq);
        lfFreqKnob     .setTooltip ("Pultec LF frequency (20/30/60/100 Hz). Detent-snapped.");
        hfBoostFreqKnob.setTooltip ("Pultec HF boost frequency (3/4/5/8/10/12/16 kHz). Detent-snapped.");
        hfAttenFreqKnob.setTooltip ("Pultec HF attenuate frequency (5/10/20 kHz). Detent-snapped.");

        styleEditorLabel (lfBoostLbl,     "LF BOOST",      pultecCream);
        styleEditorLabel (lfAttenLbl,     "LF ATTEN",      pultecCream);
        styleEditorLabel (lfFreqLbl,      "LF FREQ",       pultecCream);
        styleEditorLabel (hfBandwidthLbl, "HF BANDWIDTH",  pultecCream);
        styleEditorLabel (hfBoostLbl,     "HF BOOST",      pultecCream);
        styleEditorLabel (hfAttenLbl,     "HF ATTEN",      pultecCream);
        styleEditorLabel (hfBoostFreqLbl, "HF BOOST FREQ", pultecCream);
        styleEditorLabel (hfAttenFreqLbl, "HF ATTEN FREQ", pultecCream);
        // Smaller font on the combo-row labels so the multi-word freq
        // captions don't overflow the column width.
        auto shrinkLabel = [] (juce::Label& l)
        {
            l.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
        };
        shrinkLabel (lfFreqLbl); shrinkLabel (hfBoostFreqLbl);
        shrinkLabel (hfAttenFreqLbl); shrinkLabel (lfAttenLbl);
        shrinkLabel (hfBoostLbl); shrinkLabel (hfAttenLbl); shrinkLabel (lfBoostLbl);
        shrinkLabel (hfBandwidthLbl);
        addAndMakeVisible (lfBoostLbl); addAndMakeVisible (lfAttenLbl);
        addAndMakeVisible (lfFreqLbl);  addAndMakeVisible (hfBandwidthLbl);
        addAndMakeVisible (hfBoostLbl); addAndMakeVisible (hfAttenLbl);
        addAndMakeVisible (hfBoostFreqLbl); addAndMakeVisible (hfAttenFreqLbl);

        // Single-column 4-row stack: LF row + HF BOOST row + HF ATTEN
        // row + HF BANDWIDTH row. No captions, no separator.
        const int bandsH = 4 * (kEditorLabelRowH + kEditorKnobBlockH)
                          + 3 * kSubRowGap;
        setSize (520, kEditorOuterPad * 2 + kEditorHeaderH + kEditorHeaderGap
                       + bandsH + 8);
    }

    void paint (juce::Graphics& g) override
    {
        // Pultec EQP-1A chassis blue (teal) — same paint as the inline
        // strip's eqArea so the popup reads as the same hardware unit
        // scaled up.
        g.fillAll (juce::Colour (0xff2c5060));
        g.setColour (juce::Colour (0xff90c0d0).withAlpha (0.45f));
        g.drawRect (getLocalBounds(), 1);
    }

    static constexpr int kSectionCaptionH = 18;
    static constexpr int kSubRowGap       = 4;

    void resized() override
    {
        auto area = getLocalBounds().reduced (kEditorOuterPad);
        auto header = area.removeFromTop (kEditorHeaderH);
        enableBtn.setBounds (header.removeFromRight (60));
        area.removeFromTop (kEditorHeaderGap);

        // Combo cells: vertically centred 24-px height inside the knob slot.
        auto centredComboBounds = [] (juce::Rectangle<int> cell)
        {
            return cell.withSizeKeepingCentre (juce::jmin (cell.getWidth() - 8, 160), 24);
        };

        const int bandRowH = kEditorLabelRowH + kEditorKnobBlockH;

        // Row 1: LF BOOST | LF ATTEN | LF FREQ (3 equal cells)
        {
            auto rLbl  = area.removeFromTop (kEditorLabelRowH);
            auto rKnob = area.removeFromTop (kEditorKnobBlockH);
            const int colW = rLbl.getWidth() / 3;
            lfBoostLbl.setBounds (rLbl .removeFromLeft (colW));
            lfAttenLbl.setBounds (rLbl .removeFromLeft (colW));
            lfFreqLbl .setBounds (rLbl);
            lfBoost   .setBounds (rKnob.removeFromLeft (colW));
            lfAtten   .setBounds (rKnob.removeFromLeft (colW));
            lfFreqKnob.setBounds (rKnob);
            area.removeFromTop (kSubRowGap);
        }

        // Row 2: HF BOOST | HF BOOST FREQ (2 equal cells)
        {
            auto rLbl  = area.removeFromTop (kEditorLabelRowH);
            auto rKnob = area.removeFromTop (kEditorKnobBlockH);
            const int colW = rLbl.getWidth() / 2;
            hfBoostLbl     .setBounds (rLbl .removeFromLeft (colW));
            hfBoostFreqLbl .setBounds (rLbl);
            hfBoost         .setBounds (rKnob.removeFromLeft (colW));
            hfBoostFreqKnob .setBounds (rKnob);
            area.removeFromTop (kSubRowGap);
        }

        // Row 3: HF ATTEN | HF ATTEN FREQ (2 equal cells)
        {
            auto rLbl  = area.removeFromTop (kEditorLabelRowH);
            auto rKnob = area.removeFromTop (kEditorKnobBlockH);
            const int colW = rLbl.getWidth() / 2;
            hfAttenLbl     .setBounds (rLbl .removeFromLeft (colW));
            hfAttenFreqLbl .setBounds (rLbl);
            hfAtten         .setBounds (rKnob.removeFromLeft (colW));
            hfAttenFreqKnob .setBounds (rKnob);
            area.removeFromTop (kSubRowGap);
        }

        // Row 4: HF BANDWIDTH centred (single knob).
        {
            auto rLbl  = area.removeFromTop (kEditorLabelRowH);
            auto rKnob = area.removeFromTop (kEditorKnobBlockH);
            const int knobW = juce::jmin (rKnob.getWidth() / 3, 120);
            const int knobX = rKnob.getX() + (rKnob.getWidth() - knobW) / 2;
            hfBandwidthLbl.setBounds (knobX, rLbl .getY(), knobW, rLbl .getHeight());
            hfBandwidth   .setBounds (knobX, rKnob.getY(), knobW, rKnob.getHeight());
        }

        (void) bandRowH;   // unused now (no column stacking)
    }

private:
    MasterBusParams& params;
    juce::TextButton enableBtn;
    juce::Slider lfBoost, lfAtten, hfBoost, hfAtten, hfBandwidth;
    juce::Slider lfFreqKnob      { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Slider hfBoostFreqKnob { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Slider hfAttenFreqKnob { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Label  lfBoostLbl, lfAttenLbl, lfFreqLbl, hfBandwidthLbl;
    juce::Label  hfBoostLbl, hfAttenLbl, hfBoostFreqLbl, hfAttenFreqLbl;
};

// Master COMP modal — same layout grammar as ChannelCompEditor + the new
// BusCompEditorPanel: ON top-right, LEFT meter strip (triangle threshold
// drag + IN + GR bars), RIGHT 2×2 knob grid (RAT/MAK over ATK/REL). No
// THR knob — threshold via the triangle on the IN bar.
class MasterCompEditorPanel final : public juce::Component, private juce::Timer
{
public:
    MasterCompEditorPanel (MasterBusParams& p) : params (p)
    {
        // SSL G-bus comp palette — powder-blue knobs + near-black
        // chassis. Variable name kept for callsite compatibility.
        const auto compGold = juce::Colour (0xff7da8c5);
        styleEditorEnableBtn (enableBtn, compGold);
        enableBtn.setButtonText ("ON");
        enableBtn.setToggleState (params.compEnabled.load (std::memory_order_relaxed),
                                    juce::dontSendNotification);
        enableBtn.onClick = [this]
        {
            params.compEnabled.store (enableBtn.getToggleState(), std::memory_order_relaxed);
        };
        addAndMakeVisible (enableBtn);

        styleEditorKnob (rat, compGold,   1.0,   10.0,    4.0,   4.0, ":1",  1);
        styleEditorKnob (atk, compGold,   0.1,   50.0,    5.0,  10.0, " ms", 1);
        styleEditorKnob (rel, compGold,  50.0, 1000.0,  200.0, 200.0, " ms", 0);
        styleEditorKnob (mak, compGold, -10.0,   20.0,    0.0,   0.0, " dB", 1);
        for (auto* k : { &rat, &atk, &rel, &mak })
            k->setColour (juce::Slider::thumbColourId, juce::Colours::white);
        mak.setValue (params.compMakeupDb .load(), juce::dontSendNotification);

        rat.setTooltip ("Master bus comp ratio (SSL-stepped: 2:1 / 4:1 / 10:1).");
        atk.setTooltip ("Master bus comp attack (SSL-stepped: 0.1 / 0.3 / 1 / 3 / 10 / 30 ms).");
        rel.setTooltip ("Master bus comp release (SSL-stepped: 0.1 / 0.3 / 0.6 / 1.2 s, top = AUTO).");
        mak.setTooltip ("Master bus comp make-up gain (-10..+20 dB). Double-click for 0 dB; Shift-drag for fine.");
        mak.onValueChange = [this] { params.compMakeupDb .store ((float) mak.getValue(), std::memory_order_relaxed); };

        // Stepped SSL selectors — donor bus_* params are Choices.
        configureSteppedKnob (rat, sslsteps::ratioValues(), sslsteps::ratioLabels(),
                              params.compRatio.load(),
                              [this] (double v) { params.compRatio.store ((float) v, std::memory_order_relaxed); });
        configureSteppedKnob (atk, sslsteps::attackValues(), sslsteps::attackLabels(),
                              params.compAttackMs.load(),
                              [this] (double v) { params.compAttackMs.store ((float) v, std::memory_order_relaxed); });
        configureSteppedKnob (rel, sslsteps::releaseValues(), sslsteps::releaseLabels(),
                              params.compReleaseAuto.load() ? -1.0 : (double) params.compReleaseMs.load(),
                              [this] (double v)
                              {
                                  const bool autoOn = v < 0.0;
                                  params.compReleaseAuto.store (autoOn, std::memory_order_relaxed);
                                  if (! autoOn)
                                      params.compReleaseMs.store ((float) v, std::memory_order_relaxed);
                              });

        addAndMakeVisible (rat); addAndMakeVisible (atk);
        addAndMakeVisible (rel); addAndMakeVisible (mak);

        styleEditorCompLabel (ratLbl, "RATIO");
        styleEditorCompLabel (atkLbl, "ATTACK");
        styleEditorCompLabel (relLbl, "RELEASE");
        styleEditorCompLabel (makLbl, "GAIN");
        addAndMakeVisible (ratLbl); addAndMakeVisible (atkLbl);
        addAndMakeVisible (relLbl); addAndMakeVisible (makLbl);

        setSize (380, 268);
        startTimerHz (30);
    }

    ~MasterCompEditorPanel() override { stopTimer(); }

    void paint (juce::Graphics& g) override
    {
        // SSL G-bus comp chassis — near-black with powder-blue accent.
        g.fillAll (juce::Colour (0xff0a0a0a));
        g.setColour (juce::Colour (0xff7da8c5).withAlpha (0.40f));
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

            g.setColour (juce::Colour (0xffa0a0a0));
            g.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
            g.drawText (caption, area.withY (area.getY() - 14).withHeight (12),
                         juce::Justification::centred, false);
            g.setFont (juce::Font (juce::FontOptions (9.5f)));
            g.drawText (valueText, area.withY (area.getBottom()).withHeight (14),
                         juce::Justification::centred, false);
        };

        drawVerticalMeter (inputMeterArea, displayedInputDb, -60.0f, 0.0f,
                            juce::Colour (0xffd05a5a),
                            juce::Colour (0xff60c060),
                            "IN",
                            displayedInputDb <= -99.0f ? juce::String ("-inf")
                                                       : juce::String::formatted ("%.1f", displayedInputDb));

        drawVerticalMeter (grMeterArea, -displayedGrDb, 0.0f, 20.0f,
                            juce::Colour (0xffff7060),
                            juce::Colour (0xffe0c050).brighter (0.2f),
                            "GR",
                            juce::String::formatted ("%.1f", displayedGrDb));

        if (! inputMeterArea.isEmpty() && ! threshHandleArea.isEmpty())
        {
            const float thresh = juce::jlimit (-60.0f, 0.0f,
                params.compThreshDb.load (std::memory_order_relaxed));
            const float frac = (thresh - (-60.0f)) / 60.0f;
            const auto inBar = inputMeterArea.toFloat();
            const float y = inBar.getBottom() - 2.0f - frac * (inBar.getHeight() - 4.0f);

            const float halfH = 9.0f;
            const float baseX = (float) threshHandleArea.getX();
            const float tipX  = (float) threshHandleArea.getRight() + 2.0f;
            juce::Path tri;
            tri.addTriangle (baseX, y - halfH, baseX, y + halfH, tipX, y);

            const bool engaged = params.compEnabled.load (std::memory_order_relaxed);
            const auto fill = engaged ? juce::Colour (0xffe0c050).brighter (0.30f)
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

    void mouseDown (const juce::MouseEvent& e) override
    {
        const auto hitArea = threshHandleArea.getUnion (inputMeterArea).expanded (2);
        draggingThreshold = hitArea.contains (e.getPosition());
        if (draggingThreshold)
        {
            writeThresholdFromY (e.y);
            params.compEnabled.store (true, std::memory_order_relaxed);
            enableBtn.setToggleState (true, juce::dontSendNotification);
            repaint();
        }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! draggingThreshold) return;
        writeThresholdFromY (e.y);
        params.compEnabled.store (true, std::memory_order_relaxed);
        enableBtn.setToggleState (true, juce::dontSendNotification);
        repaint();
    }

    void mouseUp (const juce::MouseEvent&) override { draggingThreshold = false; }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        const auto hitArea = threshHandleArea.getUnion (inputMeterArea).expanded (2);
        if (! hitArea.contains (e.getPosition())) return;
        params.compThreshDb.store (0.0f, std::memory_order_relaxed);
        repaint();
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (kEditorOuterPad);

        auto header = area.removeFromTop (kEditorHeaderH);
        enableBtn.setBounds (header.removeFromRight (60));
        area.removeFromTop (kEditorHeaderGap);

        constexpr int kHandleW    = 14;
        constexpr int kMeterW     = 28;
        constexpr int kMeterGap   = 4;
        constexpr int kMeterStripW = kHandleW + kMeterGap + kMeterW * 2 + kMeterGap;
        constexpr int kMeterTopPad = 16;
        constexpr int kMeterBotPad = 14;
        constexpr int kColGap = 12;

        auto body = area;
        auto meterStrip = body.removeFromLeft (kMeterStripW);
        body.removeFromLeft (kColGap);
        auto rotariesCol = body;

        auto handleCol = meterStrip.removeFromLeft (kHandleW);
        meterStrip.removeFromLeft (kMeterGap);
        auto inMeter = meterStrip.removeFromLeft (kMeterW);
        meterStrip.removeFromLeft (kMeterGap);
        auto grMeter = meterStrip.removeFromLeft (kMeterW);
        handleCol = handleCol.withTrimmedTop (kMeterTopPad).withTrimmedBottom (kMeterBotPad);
        inMeter   = inMeter  .withTrimmedTop (kMeterTopPad).withTrimmedBottom (kMeterBotPad);
        grMeter   = grMeter  .withTrimmedTop (kMeterTopPad).withTrimmedBottom (kMeterBotPad);
        threshHandleArea = handleCol;
        inputMeterArea   = inMeter;
        grMeterArea      = grMeter;

        constexpr int kRowLabelH = 14;
        const int rowH = (rotariesCol.getHeight() - kRowLabelH * 2 - 12) / 2;
        auto layoutCell = [&] (juce::Rectangle<int> cell,
                                 juce::Slider& k, juce::Label& l)
        {
            l.setBounds (cell.removeFromTop (kRowLabelH));
            k.setBounds (cell.reduced (4));
        };
        auto rowTop    = rotariesCol.removeFromTop (kRowLabelH + rowH);
        rotariesCol.removeFromTop (12);
        auto rowBottom = rotariesCol.removeFromTop (kRowLabelH + rowH);
        const int colW = rowTop.getWidth() / 2;
        layoutCell (rowTop   .removeFromLeft (colW), rat, ratLbl);
        layoutCell (rowTop,                          mak, makLbl);
        layoutCell (rowBottom.removeFromLeft (colW), atk, atkLbl);
        layoutCell (rowBottom,                        rel, relLbl);
    }

private:
    void timerCallback() override
    {
        const float l = params.meterPostMasterLDb.load (std::memory_order_relaxed);
        const float r = params.meterPostMasterRDb.load (std::memory_order_relaxed);
        const float in = juce::jmax (l, r);
        if (in > displayedInputDb) displayedInputDb = in;
        else                        displayedInputDb += (in - displayedInputDb) * 0.10f;

        const float gr = params.meterGrDb.load (std::memory_order_relaxed);
        if (gr < displayedGrDb) displayedGrDb = gr;
        else                     displayedGrDb += (gr - displayedGrDb) * 0.5f;  // ~48 ms recovery (was ~167 ms)

        if (! grMeterArea  .isEmpty()) repaint (grMeterArea  .expanded (0, 14));
        if (! inputMeterArea.isEmpty()) repaint (inputMeterArea.expanded (0, 14));
        if (! threshHandleArea.isEmpty()) repaint (threshHandleArea.expanded (4, 14));
    }

    void writeThresholdFromY (int y)
    {
        const int height = inputMeterArea.getHeight() - 4;
        if (height <= 0) return;
        const float relY = (float) (inputMeterArea.getBottom() - 2 - y)
                            / (float) height;
        const float dbOnInAxis = juce::jlimit (-60.0f, 0.0f,
            -60.0f + juce::jlimit (0.0f, 1.0f, relY) * 60.0f);
        params.compThreshDb.store (juce::jlimit (-60.0f, 0.0f, dbOnInAxis),
                                     std::memory_order_relaxed);
    }

    MasterBusParams& params;
    juce::TextButton enableBtn;
    juce::Slider rat, atk, rel, mak;
    juce::Label  ratLbl, atkLbl, relLbl, makLbl;
    juce::Rectangle<int> inputMeterArea, grMeterArea, threshHandleArea;
    float displayedInputDb = -100.0f;
    float displayedGrDb    = 0.0f;
    bool  draggingThreshold = false;
};
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

    // EQ + comp section toggles + their parameter knobs. Pultec EQP-1A
    // hardware grammar: black bakelite knobs against the teal-blue
    // chassis, with cream-stencilled labels. Knobs share `pultecBlack`;
    // labels use `pultecCream`; the eqArea background paints in
    // `pultecBlue` (see paint()).
    const auto pultecBlack = juce::Colour (0xff1a1a1c);
    const auto pultecCream = juce::Colour (0xfff0e8d0);
    const auto pultecGold  = pultecBlack;   // legacy variable name; now black
    const auto compGold    = juce::Colour (0xff7da8c5);   // SSL G-bus comp powder blue (legacy var name)

    // EQ header — shared CompHeaderButton pill (green LED + bold label).
    // No pickFn: master EQ has a single Pultec topology, no mode picker.
    eqHeaderBtn = std::make_unique<CompHeaderButton> (
        /*getEnabled*/ [this] { return params.eqEnabled.load (std::memory_order_relaxed); },
        /*onToggle*/   [this]
                        {
                            const bool now = ! params.eqEnabled.load (std::memory_order_relaxed);
                            params.eqEnabled.store (now, std::memory_order_release);
                            if (eqHeaderBtn != nullptr) eqHeaderBtn->repaint();
                        });
    eqHeaderBtn->setLabelText ("EQ");
    eqHeaderBtn->setTooltip ("Pultec-style Tube EQ on/off");
    addAndMakeVisible (eqHeaderBtn.get());

    // Suffixes intentionally empty - section labels already convey the
    // unit, and a 6-char " dB"/" ms" suffix won't fit the 38-px text
    // box without truncating to "...".
    styleSmallKnob (eqLfBoost,    0.0,  10.0,   3.0, params.eqLfBoost.load(),       pultecGold, "", 1);
    styleSmallKnob (eqLfAtten,    0.0,  10.0,   3.0, params.eqLfAtten.load(),       pultecGold, "", 1);
    styleSmallKnob (eqHfBoost,    0.0,  10.0,   3.0, params.eqHfBoost.load(),       pultecGold, "", 1);
    styleSmallKnob (eqHfAtten,    0.0,  10.0,   3.0, params.eqHfAtten.load(),       pultecGold, "", 1);
    eqLfBoost.setTooltip ("Pultec LF boost (0..10). Double-click for 3; Shift-drag for fine.");
    eqLfAtten.setTooltip ("Pultec LF attenuate (0..10). Double-click for 3; Shift-drag for fine.");
    eqHfBoost.setTooltip ("Pultec HF boost (0..10). Double-click for 3; Shift-drag for fine.");
    eqHfAtten.setTooltip ("Pultec HF attenuate (0..10). Double-click for 3; Shift-drag for fine.");
    // styleSmallKnob picks the thumb colour by brightening the fill;
    // black brightened ~0.3 is too dark to read against the black knob
    // body. Override thumb to cream so the pointer reads as the white
    // stencilled indicator on a Pultec EQP-1A knob.
    for (auto* k : { &eqLfBoost, &eqLfAtten, &eqHfBoost, &eqHfAtten })
        k->setColour (juce::Slider::thumbColourId, pultecCream);

    // Auto-arm the master Pultec EQ on any band touch. EQ defaults to
    // off (Session.h) so the LED only lights once the engineer shapes
    // the sound — same UX as channel + bus EQ headers.
    auto armMasterEq = [this]
    {
        params.eqEnabled.store (true, std::memory_order_release);
        // Repaint the header so the LED reflects the auto-armed state.
        if (eqHeaderBtn != nullptr) eqHeaderBtn->repaint();
    };
    eqLfBoost   .onValueChange = [this, armMasterEq] { params.eqLfBoost     .store ((float) eqLfBoost   .getValue(), std::memory_order_relaxed); armMasterEq(); };
    eqLfAtten   .onValueChange = [this, armMasterEq] { params.eqLfAtten     .store ((float) eqLfAtten   .getValue(), std::memory_order_relaxed); armMasterEq(); };
    eqHfBoost   .onValueChange = [this, armMasterEq] { params.eqHfBoost     .store ((float) eqHfBoost   .getValue(), std::memory_order_relaxed); armMasterEq(); };
    eqHfAtten   .onValueChange = [this, armMasterEq] { params.eqHfAtten     .store ((float) eqHfAtten   .getValue(), std::memory_order_relaxed); armMasterEq(); };

    addAndMakeVisible (eqLfBoost); addAndMakeVisible (eqLfAtten);
    addAndMakeVisible (eqHfBoost); addAndMakeVisible (eqHfAtten);

    // HF Bandwidth lives in the popup editor only — set-once setup
    // control. See MasterEqEditorPanel for the knob.

    // Discrete Pultec-position freq pickers (LF FREQ, HF BOOST FREQ,
    // HF ATTEN FREQ). Lists + format mirror MasterEqEditorPanel so the
    // inline surface stays in lock-step with the popup.
    // Stepped rotary knob — snaps to the discrete Pultec position
    // indices (0..N-1). textFromValueFunction maps the index back to
    // the Hz / kHz string so the knob's textbox reads like a freq
    // picker. Dragging the knob feels detent-like because the range
    // step is 1 and JUCE rounds to the nearest integer.
    auto setupStripFreqKnob = [this, armMasterEq, pultecBlack, pultecCream]
                                  (juce::Slider& k, const int* hz, int count,
                                   std::atomic<float>& atom,
                                   std::function<juce::String (int)> fmt)
    {
        k.setRange (0.0, (double) (count - 1), 1.0);
        k.setColour (juce::Slider::rotarySliderFillColourId,    pultecBlack);
        k.setColour (juce::Slider::rotarySliderOutlineColourId, juce::Colour (0xff0a0a0c));
        k.setColour (juce::Slider::thumbColourId,               pultecCream);
        k.setColour (juce::Slider::textBoxTextColourId,         pultecCream);
        k.setColour (juce::Slider::textBoxBackgroundColourId,   juce::Colours::transparentBlack);
        k.setColour (juce::Slider::textBoxOutlineColourId,      juce::Colours::transparentBlack);
        k.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 56, 14);
        // Capture hz array + count by value into the formatter so the
        // lambda stays valid after the setup helper returns.
        k.textFromValueFunction = [hz, count] (double v) -> juce::String
        {
            const int idx = juce::jlimit (0, count - 1, (int) std::round (v));
            const int h = hz[idx];
            return h >= 1000 ? juce::String (h / 1000) + " kHz"
                              : juce::String (h)        + " Hz";
        };
        // Initial value: nearest discrete index for the stored atom Hz.
        const float current = atom.load (std::memory_order_relaxed);
        int bestIdx = 0;
        float bestDist = std::abs (current - (float) hz[0]);
        for (int i = 1; i < count; ++i)
        {
            const float d = std::abs (current - (float) hz[i]);
            if (d < bestDist) { bestDist = d; bestIdx = i; }
        }
        k.setValue ((double) bestIdx, juce::dontSendNotification);
        k.updateText();
        k.onValueChange = [&k, hz, count, &atom, armMasterEq]
        {
            const int idx = juce::jlimit (0, count - 1, (int) std::round (k.getValue()));
            atom.store ((float) hz[idx], std::memory_order_relaxed);
            armMasterEq();
        };
        addAndMakeVisible (k);
        (void) fmt;   // fmt no longer used — textFromValueFunction owns formatting
    };
    static const int kLfHz[]      = { 20, 30, 60, 100 };
    static const int kHfBoostHz[] = { 3000, 4000, 5000, 8000, 10000, 12000, 16000 };
    static const int kHfAttenHz[] = { 5000, 10000, 20000 };
    auto fmtLfHz = [] (int hz) { return juce::String (hz) + " Hz"; };
    auto fmtHfHz = [] (int hz) { return hz >= 1000
                                          ? juce::String (hz / 1000) + " kHz"
                                          : juce::String (hz) + " Hz"; };
    setupStripFreqKnob (eqLfFreqKnob,      kLfHz,      4, params.eqLfFreq,      fmtLfHz);
    setupStripFreqKnob (eqHfBoostFreqKnob, kHfBoostHz, 7, params.eqHfBoostFreq, fmtHfHz);
    setupStripFreqKnob (eqHfAttenFreqKnob, kHfAttenHz, 3, params.eqHfAttenFreq, fmtHfHz);
    eqLfFreqKnob     .setTooltip ("Pultec LF frequency (20/30/60/100 Hz). Dragging snaps to the hardware-canonical detents.");
    eqHfBoostFreqKnob.setTooltip ("Pultec HF boost frequency (3/4/5/8/10/12/16 kHz). Detent-snapped.");
    eqHfAttenFreqKnob.setTooltip ("Pultec HF attenuate frequency (5/10/20 kHz). Detent-snapped.");

    // Inline now shows the full Pultec surface (mirrors the popup editor):
    // Row 1 = 4 gain knobs, Row 2 = LF FREQ / HF BANDWIDTH / HF BOOST FREQ
    // / HF ATTEN FREQ, Row 3 = OUT centred. The "FREQS…" button is gone.
    styleSmallLabel (eqLfBoostLabel, "LF BOOST", pultecCream);
    styleSmallLabel (eqLfAttenLabel, "LF ATTEN", pultecCream);
    styleSmallLabel (eqHfBoostLabel, "HF BOOST", pultecCream);
    // U+2212 minus. Construct via CharPointer_UTF8 - juce::String's const char*
    // ctor uses the system locale, which mangles UTF-8 into garbage on Linux
    // ("HFâ\x88\x92" instead of "HF−").
    styleSmallLabel (eqHfAttenLabel,
                      juce::String (juce::CharPointer_UTF8 ("HF\xe2\x88\x92 ATTEN")),
                      pultecCream);
    styleSmallLabel (eqLfFreqLabel,      "LF FREQ",       pultecCream);
    styleSmallLabel (eqHfBoostFreqLabel, "HF BOOST FREQ", pultecCream);
    styleSmallLabel (eqHfAttenFreqLabel,
                      juce::String (juce::CharPointer_UTF8 ("HF\xe2\x88\x92 ATTEN FREQ")),
                      pultecCream);
    addAndMakeVisible (eqLfBoostLabel); addAndMakeVisible (eqLfAttenLabel);
    addAndMakeVisible (eqHfBoostLabel); addAndMakeVisible (eqHfAttenLabel);
    addAndMakeVisible (eqLfFreqLabel);
    addAndMakeVisible (eqHfBoostFreqLabel);
    addAndMakeVisible (eqHfAttenFreqLabel);

    // Comp section. Same shell as bus + channel strips:
    //   - CompHeaderButton (white text, green LED, left-click toggles
    //     enable). No mode picker — fixed SSL-style glue topology.
    //   - CompMeterStrip on the LEFT (triangle-handle threshold, IN bar
    //     + dB scale + GR bar). Threshold drag writes compThreshDb.
    //   - Knob grid (RAT / MAK on top, ATK / REL on bottom) on the
    //     RIGHT. The standalone THR knob is gone.
    compHeaderBtn = std::make_unique<CompHeaderButton> (
        /*getEnabled*/ [this] { return params.compEnabled.load (std::memory_order_relaxed); },
        /*onToggle*/   [this]
                        {
                            const bool now = ! params.compEnabled.load (std::memory_order_relaxed);
                            params.compEnabled.store (now, std::memory_order_relaxed);
                            if (compHeaderBtn != nullptr) compHeaderBtn->repaint();
                        });
    addAndMakeVisible (compHeaderBtn.get());

    CompMeterStrip::Source compSrc;
    compSrc.getInputDb     = [this]
                              {
                                  const float l = params.meterPostMasterLDb.load (std::memory_order_relaxed);
                                  const float r = params.meterPostMasterRDb.load (std::memory_order_relaxed);
                                  return juce::jmax (l, r);
                              };
    compSrc.getGrDb        = [this] { return params.meterGrDb.load (std::memory_order_relaxed); };
    compSrc.getThresholdDb = [this] { return params.compThreshDb.load (std::memory_order_relaxed); };
    compSrc.setThresholdDb = [this] (float db)
                              {
                                  params.compThreshDb.store (juce::jlimit (-60.0f, 0.0f, db),
                                                              std::memory_order_relaxed);
                              };
    compSrc.resetThreshold = [this]
                              {
                                  params.compThreshDb.store (0.0f, std::memory_order_relaxed);
                              };
    compSrc.isEngaged      = [this] { return params.compEnabled.load (std::memory_order_relaxed); };
    compSrc.autoEnable     = [this]
                              {
                                  params.compEnabled.store (true, std::memory_order_relaxed);
                                  if (compHeaderBtn != nullptr) compHeaderBtn->repaint();
                              };
    // Fader-side GR LED (matches channel + bus grammar). Slim GR-only widget
    // with the threshold-drag handle on the right side, glued next to the
    // level meter. setRangeDb(-60, 0) maps drag y 1:1 with the level meter
    // scale so the triangle sits on the same dB tick as it's set to.
    compSrc.floorDb        = -60.0f;
    compSrc.ceilingDb      =   0.0f;
    compMeter = std::make_unique<CompMeterStrip> (std::move (compSrc));
    compMeter->setShowInputBar (false);
    compMeter->setHandleVisible (true);
    compMeter->setHandleOnRight (true);
    compMeter->setRangeDb (-60.0f, 0.0f);
    addAndMakeVisible (compMeter.get());

    styleSmallKnob (compRatio,       1.0, 10.0,    4.0, params.compRatio.load(),       compGold, ":1", 1);
    styleSmallKnob (compAttack,      0.1, 50.0,    5.0, params.compAttackMs.load(),    compGold, "",   1);
    styleSmallKnob (compRelease,    50.0,1000.0, 200.0, params.compReleaseMs.load(),   compGold, "",   0);
    styleSmallKnob (compMakeup,    -10.0, 20.0,    0.0, params.compMakeupDb.load(),    compGold, "",   1);
    compRatio  .setTooltip ("Master bus comp ratio (SSL-stepped: 2:1 / 4:1 / 10:1).");
    compAttack .setTooltip ("Master bus comp attack (SSL-stepped: 0.1 / 0.3 / 1 / 3 / 10 / 30 ms).");
    compRelease.setTooltip ("Master bus comp release (SSL-stepped: 0.1 / 0.3 / 0.6 / 1.2 s, top = AUTO).");
    compMakeup .setTooltip ("Master bus comp make-up gain (-10..+20 dB). Double-click for 0 dB; Shift-drag for fine.");
    for (auto* k : { &compRatio, &compAttack, &compRelease, &compMakeup })
        k->setColour (juce::Slider::thumbColourId, juce::Colours::white);

    compMakeup   .onValueChange = [this] { params.compMakeupDb .store ((float) compMakeup   .getValue(), std::memory_order_relaxed); };

    // Ratio / attack / release are SSL-style stepped selectors — the donor's
    // bus_* params are Choices, so the knob detents on the real values.
    configureSteppedKnob (compRatio, sslsteps::ratioValues(), sslsteps::ratioLabels(),
                          params.compRatio.load(),
                          [this] (double v) { params.compRatio.store ((float) v, std::memory_order_relaxed); });
    configureSteppedKnob (compAttack, sslsteps::attackValues(), sslsteps::attackLabels(),
                          params.compAttackMs.load(),
                          [this] (double v) { params.compAttackMs.store ((float) v, std::memory_order_relaxed); });
    configureSteppedKnob (compRelease, sslsteps::releaseValues(), sslsteps::releaseLabels(),
                          params.compReleaseAuto.load() ? -1.0 : (double) params.compReleaseMs.load(),
                          [this] (double v)
                          {
                              const bool autoOn = v < 0.0;
                              params.compReleaseAuto.store (autoOn, std::memory_order_relaxed);
                              if (! autoOn)
                                  params.compReleaseMs.store ((float) v, std::memory_order_relaxed);
                          });

    addAndMakeVisible (compRatio);    addAndMakeVisible (compAttack);
    addAndMakeVisible (compRelease);  addAndMakeVisible (compMakeup);

    // Compact placeholder buttons (hidden until setCompactMode(true)).
    auto styleCompactSectionBtn = [] (juce::TextButton& b, juce::Colour accent)
    {
        b.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff282830));
        b.setColour (juce::TextButton::buttonOnColourId, accent.withMultipliedAlpha (0.85f));
        b.setColour (juce::TextButton::textColourOffId,  accent.withMultipliedBrightness (0.8f));
        b.setColour (juce::TextButton::textColourOnId,   juce::Colours::white);
        b.setMouseClickGrabsKeyboardFocus (false);
        b.setClickingTogglesState (false);
    };
    styleCompactSectionBtn (eqCompactButton,   juce::Colour (0xff5fc46f));
    styleCompactSectionBtn (compCompactButton, juce::Colour (0xffe0c050));
    eqCompactButton  .setTooltip ("Open the master EQ editor (Pultec-style Tube EQ).");
    compCompactButton.setTooltip ("Open the master comp editor (SSL-style glue compressor).");
    eqCompactButton  .onClick = [this] { openEqEditorPopup(); };
    compCompactButton.onClick = [this] { openCompEditorPopup(); };
    addChildComponent (eqCompactButton);
    addChildComponent (compCompactButton);

    styleSmallLabel (compRatLabel, "RAT", compGold);
    styleSmallLabel (compAtkLabel, "ATK", compGold);
    styleSmallLabel (compRelLabel, "REL", compGold);
    styleSmallLabel (compMakLabel, "MAK", compGold);
    addAndMakeVisible (compRatLabel); addAndMakeVisible (compAtkLabel);
    addAndMakeVisible (compRelLabel); addAndMakeVisible (compMakLabel);

    // TAPE pill — left-click toggles tapeEnabled (gives the timeline
    // compact view a way to bypass without expanding the strip);
    // right-click opens the popup editor. Lit state reflects the
    // tapeEnabled atom (synced from timerCallback) so the engine's
    // auto-arm-on-edit still shows up here. Matches the expanded-
    // mode CompHeaderButton's left/right grammar — different from
    // EQ / COMP compact pills, which only have the popup editor as
    // their bypass route via the in-popup enable LED.
    {
        const auto tapeAccent = juce::Colour (0xffd0a060);
        tapeButton.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff282830));
        tapeButton.setColour (juce::TextButton::buttonOnColourId, tapeAccent.withMultipliedAlpha (0.85f));
        tapeButton.setColour (juce::TextButton::textColourOffId,  tapeAccent.withMultipliedBrightness (0.8f));
        tapeButton.setColour (juce::TextButton::textColourOnId,   juce::Colours::white);
        tapeButton.setMouseClickGrabsKeyboardFocus (false);
        tapeButton.setClickingTogglesState (false);   // toggle path goes through the atom, not the button state
        tapeButton.setToggleState (params.tapeEnabled.load (std::memory_order_relaxed),
                                    juce::dontSendNotification);
        tapeButton.setTooltip ("Left-click: open tape editor. Right-click: bypass / engage tape.");
        tapeButton.onClick = [this]
        {
            openTapeMachineModal();
        };
        tapeButton.onRightClick = [this] (const juce::MouseEvent&)
        {
            const bool now = ! params.tapeEnabled.load (std::memory_order_relaxed);
            params.tapeEnabled.store (now, std::memory_order_relaxed);
            tapeButton.setToggleState (now, juce::dontSendNotification);
        };
        addAndMakeVisible (tapeButton);
    }

    // Regular-mode TAPE header — CompHeaderButton (LED + label). Tape has no
    // inline controls, so its primary action (left-click) OPENS the editor and
    // right-click toggles enable; the LED still reflects tapeEnabled. setCompactMode
    // swaps visibility between this header and the compact tapeButton pill above.
    tapeHeaderBtn = std::make_unique<CompHeaderButton> (
        /*getEnabled*/            [this] { return params.tapeEnabled.load (std::memory_order_relaxed); },
        /*onToggle (left-click)*/ [this] { openTapeMachineModal(); },
        /*onPickMode (right-click)*/ [this]
                        {
                            const bool now = ! params.tapeEnabled.load (std::memory_order_relaxed);
                            params.tapeEnabled.store (now, std::memory_order_relaxed);
                            if (tapeHeaderBtn != nullptr) tapeHeaderBtn->repaint();
                        });
    tapeHeaderBtn->setLabelText ("TAPE");
    tapeHeaderBtn->setTooltip ("Left-click to open the tape-machine editor. Right-click to bypass / engage tape.");
    addAndMakeVisible (tapeHeaderBtn.get());

    // Default mode is regular (compactMode = false). Hide the compact
    // tapeButton initially so the header CompHeaderButton owns the
    // slot. setCompactMode swaps the two whenever the TIMELINE tape
    // strip toggles.
    tapeButton.setVisible (false);

    // Listen for parameter changes on the donor TapeMachine processor
    // so any edit in the popup auto-arms tapeEnabled (matches the EQ
    // / COMP arm-on-touch behaviour).
   #if DUSKSTUDIO_HAS_DUSK_DSP
    if (tapeProcessorPtr != nullptr)
        tapeProcessorPtr->addListener (this);
   #endif

    faderSlider.setRange (ChannelStripParams::kFaderMinDb, ChannelStripParams::kFaderMaxDb, 0.1);
    faderSlider.setSkewFactorFromMidPoint (-12.0);
    faderSlider.setValue (params.faderDb.load (std::memory_order_relaxed), juce::dontSendNotification);
    faderSlider.setDoubleClickReturnValue (true, 0.0);
    // No textbox — standalone faderValueLabel below shows the value (matches
    // channel + bus strip's fader-side grammar; lets the cap fully reach the
    // slider's min / max ticks without clipping into the textbox).
    faderSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    faderSlider.setTooltip ("Master fader (-90..+12 dB). Double-click for 0 dB; Shift-drag for fine. Right-click for MIDI Learn.");
    faderSlider.setVelocityBasedMode (false);
    faderSlider.setVelocityModeParameters (1.0, 1, 0.0, true);
    auto formatFaderDb = [] (double db) -> juce::String
    {
        return (db <= -89.95) ? juce::String ("off")
                              : juce::String (db, 1);
    };
    faderSlider.onValueChange = [this, formatFaderDb]
    {
        const float v = (float) faderSlider.getValue();
        params.faderDb.store (v, std::memory_order_relaxed);
        faderValueLabel.setText (formatFaderDb (v), juce::dontSendNotification);
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

    // Standalone fader-value readout below the slider.
    faderValueLabel.setJustificationType (juce::Justification::centred);
    faderValueLabel.setColour (juce::Label::textColourId, juce::Colour (0xffd8d8d8));
    faderValueLabel.setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);
    faderValueLabel.setColour (juce::Label::outlineColourId,    juce::Colours::transparentBlack);
    faderValueLabel.setFont (juce::Font (juce::FontOptions (
        juce::Font::getDefaultMonospacedFontName(), 14.0f, juce::Font::bold)));
    faderValueLabel.setText (formatFaderDb (faderSlider.getValue()), juce::dontSendNotification);
    addAndMakeVisible (faderValueLabel);

    // Automation mode button — cycles Off / R / W / T via popup.
    // Transparent fill so the bottom row reads as plain text rather
    // than a boxed pill (matches the channel-strip auto button's
    // off-state look + the user's "no boxes around bottom labels" pass).
    autoModeButton.setTooltip ("Master automation: Off / Read / Write / Touch");
    autoModeButton.setColour (juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
    autoModeButton.setColour (juce::TextButton::textColourOffId, juce::Colour (0xff909094));
    autoModeButton.onClick = [this] { showAutoModeMenu(); };
    {
        const int amode = params.automationMode.load (std::memory_order_relaxed);
        autoModeButton.setButtonText (amode == (int) AutomationMode::Off   ? "Off"
                                       : amode == (int) AutomationMode::Read  ? "R"
                                       : amode == (int) AutomationMode::Write ? "W"
                                                                              : "T");
    }
    addAndMakeVisible (autoModeButton);

    muteButton.setClickingTogglesState (true);
    muteButton.setColour (juce::TextButton::buttonOnColourId, juce::Colours::orangered);
    muteButton.setTooltip ("Mute master output");
    muteButton.setToggleState (params.mute.load (std::memory_order_relaxed),
                                juce::dontSendNotification);
    muteButton.onClick = [this]
    {
        params.mute.store (muteButton.getToggleState(), std::memory_order_release);
    };
    addAndMakeVisible (muteButton);

    monoStereoButton.setClickingTogglesState (true);
    monoStereoButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff806020));
    monoStereoButton.setTooltip ("Mono-sum the master output (L+R)*0.5 - "
                                 "for mono-compat checks. Stereo when off.");
    const bool monoInit = params.monoSum.load (std::memory_order_relaxed);
    monoStereoButton.setToggleState (monoInit, juce::dontSendNotification);
    monoStereoButton.setButtonText (monoInit ? "MONO" : "STEREO");
    monoStereoButton.onClick = [this]
    {
        const bool on = monoStereoButton.getToggleState();
        params.monoSum.store (on, std::memory_order_release);
        monoStereoButton.setButtonText (on ? "MONO" : "STEREO");
    };
    addAndMakeVisible (monoStereoButton);

    // Output meter readouts (peak dBFS + GR dB).
    auto styleReadout = [] (juce::Label& lbl, juce::Colour col)
    {
        lbl.setJustificationType (juce::Justification::centred);
        lbl.setColour (juce::Label::textColourId,        col);
        lbl.setColour (juce::Label::backgroundColourId,  juce::Colours::transparentBlack);
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

void MasterStripComponent::audioProcessorParameterChanged (juce::AudioProcessor*,
                                                             int, float)
{
    // Fires from any thread (incl. audio). Defer to the message
    // thread before touching atoms / UI. The store itself is atomic
    // but we also call setToggleState which is message-thread-only.
    juce::Component::SafePointer<MasterStripComponent> safe (this);
    juce::MessageManager::callAsync ([safe]()
    {
        if (auto* self = safe.getComponent())
        {
            self->params.tapeEnabled.store (true, std::memory_order_relaxed);
            self->tapeButton.setToggleState (true, juce::dontSendNotification);
            self->tapeButton.repaint();
            if (self->tapeHeaderBtn != nullptr) self->tapeHeaderBtn->repaint();
        }
    });
}

void MasterStripComponent::audioProcessorChanged (juce::AudioProcessor*,
                                                    const juce::AudioProcessorListener::ChangeDetails&)
{
    // Layout / preset changes — no-op for the tape arm-on-touch UX.
}

MasterStripComponent::~MasterStripComponent()
{
    // See BusComponent::~BusComponent - explicit stopTimer() runs
    // before any modal / member teardown so the timer thread can't
    // fire on objects we're about to clean up.
    stopTimer();
   #if DUSKSTUDIO_HAS_DUSK_DSP
    if (tapeProcessorPtr != nullptr)
        tapeProcessorPtr->removeListener (this);
   #endif
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
    // Compact-mode button illumination — same grammar as bus + channel
    // strips. EQ + COMP pills light up when their corresponding
    // section is engaged so TIMELINE users still see at-a-glance state.
    if (compactMode)
    {
        const auto eqAccent   = juce::Colour (0xff5fc46f);
        const auto compAccent = juce::Colour (0xffe0c050);
        const int eqOn   = params.eqEnabled  .load (std::memory_order_relaxed) ? 1 : 0;
        const int compOn = params.compEnabled.load (std::memory_order_relaxed) ? 1 : 0;
        if (eqOn != lastCompactEqOn)
        {
            lastCompactEqOn = eqOn;
            eqCompactButton.setColour (juce::TextButton::buttonColourId,
                                         eqOn != 0 ? eqAccent.darker (0.55f)
                                                    : juce::Colour (0xff282830));
            eqCompactButton.repaint();
        }
        if (compOn != lastCompactCompOn)
        {
            lastCompactCompOn = compOn;
            compCompactButton.setColour (juce::TextButton::buttonColourId,
                                           compOn != 0 ? compAccent.darker (0.55f)
                                                        : juce::Colour (0xff282830));
            compCompactButton.repaint();
        }
    }

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
    else                    displayedGrDb += (gr - displayedGrDb) * 0.5f;  // ~48 ms recovery (was ~167 ms)

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
    // Animate whenever liveFaderDb diverges and the user isn't dragging —
    // mode-agnostic so MIDI-bound master fader moves the on-screen slider
    // in Off mode too.
    {
        const int amode = params.automationMode.load (std::memory_order_relaxed);
        const bool isWrite = amode == (int) AutomationMode::Write;
        const bool isTouch = amode == (int) AutomationMode::Touch;
        const bool playing = engine.getTransport().isPlaying();

        const float live    = params.liveFaderDb.load (std::memory_order_relaxed);
        const bool  touched = params.faderTouched.load (std::memory_order_relaxed);
        if (! touched && std::abs (live - displayedLiveFaderDb) > 0.05f)
        {
            displayedLiveFaderDb = live;
            faderSlider.setValue (live, juce::dontSendNotification);
            faderValueLabel.setText ((live <= -89.95f) ? juce::String ("off")
                                                       : juce::String (live, 1),
                                       juce::dontSendNotification);
        }
        else if (touched)
        {
            displayedLiveFaderDb = live;
        }

        const bool capturing = playing && (isWrite || (isTouch && touched));
        if (capturing)
            captureFaderWritePoint (params.faderDb.load (std::memory_order_relaxed));
    }

    // Master mute / mono toggle visual sync — poll the atoms so any MIDI
    // path (or future binding target) that flips them is reflected on
    // screen. No automation lane on these so liveMute isn't needed.
    {
        const bool mute = params.mute.load (std::memory_order_relaxed);
        if (muteButton.getToggleState() != mute)
            muteButton.setToggleState (mute, juce::dontSendNotification);
    }
    {
        const bool mono = params.monoSum.load (std::memory_order_relaxed);
        if (monoStereoButton.getToggleState() != mono)
        {
            monoStereoButton.setToggleState (mono, juce::dontSendNotification);
            monoStereoButton.setButtonText (mono ? "MONO" : "STEREO");
        }
    }
    {
        const bool eqOn = params.eqEnabled.load (std::memory_order_relaxed);
        if (eqHeaderBtn != nullptr) eqHeaderBtn->repaint();   // LED reflects atom
        (void) eqOn;
    }
    {
        // TAPE pill state — sync toggle from atom so MIDI bind / future
        // automation reflects the lit state.
        const bool tapeOn = params.tapeEnabled.load (std::memory_order_relaxed);
        if (tapeButton.getToggleState() != tapeOn)
            tapeButton.setToggleState (tapeOn, juce::dontSendNotification);
        if (tapeHeaderBtn != nullptr) tapeHeaderBtn->repaint();   // LED reflects atom
    }

    // Cross-surface sync: when the popup EQ editor mutates an atom, the
    // inline knob / combo lags until something else triggers a repaint.
    // Poll every atom against the inline widget and snap if the values
    // diverge AND the user isn't actively dragging that widget (so we
    // never fight a live gesture).
    auto syncKnob = [] (juce::Slider& k, float atomVal, float eps = 0.01f)
    {
        if (k.isMouseButtonDown()) return;
        if (std::abs ((float) k.getValue() - atomVal) > eps)
            k.setValue (atomVal, juce::dontSendNotification);
    };
    syncKnob (eqLfBoost,     params.eqLfBoost         .load (std::memory_order_relaxed));
    syncKnob (eqLfAtten,     params.eqLfAtten         .load (std::memory_order_relaxed));
    syncKnob (eqHfBoost,     params.eqHfBoost         .load (std::memory_order_relaxed));
    syncKnob (eqHfAtten,     params.eqHfAtten         .load (std::memory_order_relaxed));
    // HF Bandwidth no longer inline; popup syncs from atom on open.

    // Snap stepped freq knobs to nearest discrete Pultec position so
    // popup edits / MIDI-bound writes reflect on the inline knob.
    auto syncFreqKnob = [] (juce::Slider& k, const int* hz, int count, float atomHz)
    {
        if (k.isMouseButtonDown()) return;
        int bestIdx = 0;
        float bestDist = std::abs (atomHz - (float) hz[0]);
        for (int i = 1; i < count; ++i)
        {
            const float d = std::abs (atomHz - (float) hz[i]);
            if (d < bestDist) { bestDist = d; bestIdx = i; }
        }
        if ((int) std::round (k.getValue()) != bestIdx)
            k.setValue ((double) bestIdx, juce::dontSendNotification);
    };
    static const int kLfHzPoll[]      = { 20, 30, 60, 100 };
    static const int kHfBoostHzPoll[] = { 3000, 4000, 5000, 8000, 10000, 12000, 16000 };
    static const int kHfAttenHzPoll[] = { 5000, 10000, 20000 };
    syncFreqKnob (eqLfFreqKnob,      kLfHzPoll,      4, params.eqLfFreq.load (std::memory_order_relaxed));
    syncFreqKnob (eqHfBoostFreqKnob, kHfBoostHzPoll, 7, params.eqHfBoostFreq.load (std::memory_order_relaxed));
    syncFreqKnob (eqHfAttenFreqKnob, kHfAttenHzPoll, 3, params.eqHfAttenFreq.load (std::memory_order_relaxed));
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
    showContextMenu (m, autoModeButton,
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
    // group, separate from the bus row to its left. Larger inset (4 vs
    // the old 3) gives the frame breathing room from the inner content
    // grid so the strip stops feeling cramped against its own outline.
    auto r = getLocalBounds().toFloat().reduced (4.0f);
    g.setColour (juce::Colour (0xff202024));
    g.fillRoundedRectangle (r, 5.0f);
    g.setColour (juce::Colour (0xffd0a060));
    g.fillRoundedRectangle (r.removeFromTop (4.0f), 2.0f);
    g.setColour (juce::Colour (0xffd0a060).withAlpha (0.55f));
    g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (4.0f), 5.0f, 2.0f);

    // EQ / COMP framed bands - same grammar as channel + bus strips.
    if (! eqArea.isEmpty())
    {
        // Pultec EQP-1A chassis blue (teal-ish) — black bakelite knobs
        // + cream stencilling read as the real-hardware grammar.
        g.setColour (juce::Colour (0xff2c5060));
        g.fillRoundedRectangle (eqArea.toFloat(), 3.0f);
        g.setColour (juce::Colour (0xff90c0d0).withAlpha (0.45f));
        g.drawRoundedRectangle (eqArea.toFloat().reduced (0.5f), 3.0f, 0.8f);

        // No section captions / vertical separator — single-column
        // stacked layout (see resized() for the row breakdown).
    }
    if (! compArea.isEmpty())
    {
        // SSL G-bus comp chassis — near-black with powder-blue accent.
        g.setColour (juce::Colour (0xff0a0a0a));
        g.fillRoundedRectangle (compArea.toFloat(), 3.0f);
        g.setColour (juce::Colour (0xff7da8c5).withAlpha (0.40f));
        g.drawRoundedRectangle (compArea.toFloat().reduced (0.5f), 3.0f, 0.8f);
    }

    // Stereo LED meter - two vertical bars (L | R) inside meterArea.
    if (! meterArea.isEmpty())
    {
        constexpr float kBarGap = 1.0f;

        // Meter dB → y uses the fader's NormalisableRange (same skew,
        // same range) so "0 dB" on the meter lands at exactly the same
        // Y as the fader's "0" tick. Linear -60..+6 was the prior
        // approach and produced a visible scale mismatch with the fader.
        const auto& faderRange = faderSlider.getNormalisableRange();

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
                const double clamped = juce::jlimit ((double) faderRange.start,
                                                      (double) faderRange.end,
                                                      (double) db);
                return (float) juce::jlimit (0.0, 1.0,
                                              faderRange.convertTo0to1 (clamped));
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

    // Fader dB scale labels — track-3 grammar, drawn LEFT of the slider's
    // track (no separate carved column). Same kFaderTicks set as channel +
    // bus strips so the entire mixer reads identically.
    {
        const auto& range = faderSlider.getNormalisableRange();
        const auto sliderB = faderSlider.getBounds().toFloat();
        const float trackCx = sliderB.getCentreX();
        const float trackW  = juce::jmin (4.0f, sliderB.getWidth() * 0.18f);
        const float trackLx = trackCx - trackW * 0.5f;
        constexpr float kSharedXOver = 24.0f;
        const float labelRight = trackLx - kSharedXOver - 6.0f;

        for (const auto& t : kFaderTicks)
        {
            if (t.db < range.start - 0.01f || t.db > range.end + 0.01f) continue;
            const float y = faderYForDb (faderSlider, t.db);
            const bool isZero   = (std::abs (t.db) < 0.01f);
            const bool isBottom = (t.db <= -89.0f);

            g.setColour (isZero ? juce::Colour (0xffffffff)
                                : juce::Colour (0xffb8b8c0));
            const juce::Font font (juce::FontOptions (10.5f, juce::Font::plain));
            g.setFont (font);
            const juce::String label = isBottom ? juce::String ("off")
                                                : juce::String (t.label);
            const float ascent  = font.getAscent();
            const float descent = font.getDescent();
            const float baselineY = y + (ascent - descent) * 0.5f - 2.0f;
            const float textW = juce::GlyphArrangement::getStringWidth (font, label);
            g.drawSingleLineText (label,
                                    juce::roundToInt (labelRight - textW),
                                    juce::roundToInt (baselineY),
                                    juce::Justification::left);
        }
    }
}

void MasterStripComponent::setCompactMode (bool compact)
{
    if (compactMode == compact) return;
    compactMode = compact;

    // Reset the gated-repaint sentinels so the next timer tick republishes
    // the pill colours after re-entering compact mode (matches the
    // BusComponent fix — without this a previous cache value would
    // suppress the first refresh when EQ/COMP state hasn't flipped).
    lastCompactEqOn   = -1;
    lastCompactCompOn = -1;

    const bool sec = ! compact;
    if (eqHeaderBtn != nullptr) eqHeaderBtn->setVisible (sec);
    eqLfBoost   .setVisible (sec);  eqLfBoostLabel.setVisible (sec);
    eqLfAtten   .setVisible (sec);  eqLfAttenLabel.setVisible (sec);
    eqHfBoost   .setVisible (sec);  eqHfBoostLabel.setVisible (sec);
    eqHfAtten   .setVisible (sec);  eqHfAttenLabel.setVisible (sec);
    eqLfFreqKnob     .setVisible (sec);  eqLfFreqLabel     .setVisible (sec);
    eqHfBoostFreqKnob.setVisible (sec);  eqHfBoostFreqLabel.setVisible (sec);
    eqHfAttenFreqKnob.setVisible (sec);  eqHfAttenFreqLabel.setVisible (sec);
    if (compHeaderBtn != nullptr) compHeaderBtn->setVisible (sec);
    if (compMeter     != nullptr) compMeter    ->setVisible (sec);
    compRatio   .setVisible (sec);  compRatLabel.setVisible (sec);
    compAttack  .setVisible (sec);  compAtkLabel.setVisible (sec);
    compRelease .setVisible (sec);  compRelLabel.setVisible (sec);
    compMakeup  .setVisible (sec);  compMakLabel.setVisible (sec);

    eqCompactButton  .setVisible (compact);
    compCompactButton.setVisible (compact);

    // TAPE: compact shows the amber pill, regular shows the CompHeaderButton.
    tapeButton.setVisible (compact);
    if (tapeHeaderBtn != nullptr) tapeHeaderBtn->setVisible (sec);

    resized();
    repaint();
}

void MasterStripComponent::setCompactVu (bool compact)
{
    if (compactVu == compact) return;
    compactVu = compact;
    resized();
}

void MasterStripComponent::resized()
{
    // Inner content inset of 6 px sits 2 px inside the 4 px outer-frame
    // inset - enough gutter to keep knobs/labels off the frame, light
    // enough to not steal noticeable layout room.
    auto area = getLocalBounds().reduced (6);
    area.removeFromTop (6);
    {
        auto headerRow = area.removeFromTop (22);
        nameLabel.setBounds (headerRow);
    }
    area.removeFromTop (6);

    // Bottom row: M / STEREO / Auto compressed onto a single 24 px row
    // (was 24+2+16+4 = 46 px). Frees ~22 px for the fader.
    {
        auto bottomBtns = area.removeFromBottom (24);
        const int third = bottomBtns.getWidth() / 3;
        muteButton      .setBounds (bottomBtns.removeFromLeft (third).reduced (1));
        autoModeButton  .setBounds (bottomBtns.removeFromLeft (third).reduced (1));
        monoStereoButton.setBounds (bottomBtns.reduced (1));
        area.removeFromBottom (4);
    }

    // Analog VU meter at the top - same proportions as the bus strips so
    // the meter row reads consistently across the console. Shrink both
    // dimensions in compact mode (TIMELINE tape strip expanded) so the
    // dial keeps its 12:7 aspect ratio instead of stretching wide and
    // flat with a tiny dial inside.
    if (vuMeter != nullptr)
    {
        constexpr int kRatioW = 12;
        constexpr int kRatioH = 7;
        const int stripW = area.getWidth();
        const int heightDriver = compactVu ? stripW * 6 / 12 : stripW * 7 / 12;
        const int minH = compactVu ? 36 : 40;
        const int vuH  = juce::jmax (minH, heightDriver);
        const int vuW  = juce::jmin (stripW, vuH * kRatioW / kRatioH);
        auto slot = area.removeFromTop (vuH);
        vuMeter->setBounds (slot.withSizeKeepingCentre (vuW, vuH));
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

    if (compactMode)
    {
        constexpr int kCompactBtnH = 20;
        eqCompactButton.setBounds (area.removeFromTop (kCompactBtnH).reduced (4, 0));
        eqArea = juce::Rectangle<int>();
        area.removeFromTop (4);
        compCompactButton.setBounds (area.removeFromTop (kCompactBtnH).reduced (4, 0));
        compArea = juce::Rectangle<int>();
        area.removeFromTop (6);
    }
    else
    {
    // EQ section: 3-row stack mirroring the Pultec's grouped sections. The HF
    // controls split across two 2-cell rows (instead of one 4-cell row) so
    // each cell stays wide enough for the full "HF BOOST FREQ" / "HF− ATTEN
    // FREQ" labels even at a much narrower strip width. HF Bandwidth lives in
    // the popup editor only (set-once setup control).
    //   Row 1:  LF BOOST | LF ATTEN | LF FREQ   (3 equal cells)
    //   Row 2:  HF BOOST | HF BOOST FREQ        (2 equal cells)
    //   Row 3:  HF− ATTEN | HF− ATTEN FREQ      (2 equal cells)
    {
        constexpr int kRowGap     = 4;
        constexpr int kBandRowH   = 10 + kKnobBlockH;   // 52
        constexpr int kEqSectionH = 20 + 2 + 3 * kBandRowH + 2 * kRowGap;
        eqArea = area.removeFromTop (kEqSectionH);
        auto s = eqArea;
        if (eqHeaderBtn != nullptr) eqHeaderBtn->setBounds (s.removeFromTop (20).reduced (4, 0));
        else                         s.removeFromTop (20);
        s.removeFromTop (2);

        // Row 1: LF BOOST | LF ATTEN | LF FREQ (3 equal cells).
        {
            auto lbl  = s.removeFromTop (10);
            auto knob = s.removeFromTop (kKnobBlockH);
            const int colW = lbl.getWidth() / 3;
            eqLfBoostLabel.setBounds (lbl .removeFromLeft (colW));
            eqLfAttenLabel.setBounds (lbl .removeFromLeft (colW));
            eqLfFreqLabel .setBounds (lbl);
            eqLfBoost     .setBounds (knob.removeFromLeft (colW));
            eqLfAtten     .setBounds (knob.removeFromLeft (colW));
            eqLfFreqKnob  .setBounds (knob);
            s.removeFromTop (kRowGap);
        }

        // Row 2: HF BOOST | HF BOOST FREQ (2 equal cells).
        {
            auto lbl  = s.removeFromTop (10);
            auto knob = s.removeFromTop (kKnobBlockH);
            const int colW = lbl.getWidth() / 2;
            eqHfBoostLabel     .setBounds (lbl .removeFromLeft (colW));
            eqHfBoostFreqLabel .setBounds (lbl);
            eqHfBoost          .setBounds (knob.removeFromLeft (colW));
            eqHfBoostFreqKnob  .setBounds (knob);
            s.removeFromTop (kRowGap);
        }

        // Row 3: HF− ATTEN | HF− ATTEN FREQ (2 equal cells).
        {
            auto lbl  = s.removeFromTop (10);
            auto knob = s.removeFromTop (kKnobBlockH);
            const int colW = lbl.getWidth() / 2;
            eqHfAttenLabel     .setBounds (lbl .removeFromLeft (colW));
            eqHfAttenFreqLabel .setBounds (lbl);
            eqHfAtten          .setBounds (knob.removeFromLeft (colW));
            eqHfAttenFreqKnob  .setBounds (knob);
        }
    }
    area.removeFromTop (4);

    // Comp section: header + meter strip on the LEFT + 2×2 knob grid on
    // the RIGHT, mirroring channel + bus strips.
    {
        constexpr int kCompKnobLabelH = 10;
        constexpr int kCompKnobRowH   = kCompKnobLabelH + kKnobBlockH;
        constexpr int kCompMeterW     = 40;
        constexpr int kCompMeterGap   = 4;

        // Single-row COMP body (matches channel + bus strip grammar):
        // 4 knobs (RAT / ATK / REL / MAK) across one row.
        constexpr int kCompBodyH = kCompKnobRowH;
        constexpr int kCompSectionH = 20 + 2 + kCompBodyH + 2;
        compArea = area.removeFromTop (kCompSectionH);
        auto s = compArea.reduced (4, 2);

        auto headerRow = s.removeFromTop (20);
        if (compHeaderBtn != nullptr) compHeaderBtn->setBounds (headerRow);
        s.removeFromTop (2);

        auto body = s.removeFromTop (kCompBodyH);
        // compMeter moved out of the COMP section — now the fader-side GR
        // LED (placed below). 4-knob row uses the full body width.
        juce::ignoreUnused (kCompMeterW, kCompMeterGap);

        auto layoutCell = [&] (juce::Rectangle<int> cell,
                                 juce::Slider& knob, juce::Label& label)
        {
            label.setBounds (cell.removeFromTop (kCompKnobLabelH));
            knob.setBounds  (cell.getX(), cell.getY(), cell.getWidth(), kKnobBlockH);
        };

        const int colW = body.getWidth() / 4;
        layoutCell (body.removeFromLeft (colW), compRatio,   compRatLabel);
        layoutCell (body.removeFromLeft (colW), compAttack,  compAtkLabel);
        layoutCell (body.removeFromLeft (colW), compRelease, compRelLabel);
        layoutCell (body,                        compMakeup,  compMakLabel);
    }
    area.removeFromTop (6);
    } // end !compactMode EQ + COMP branch

    // TAPE row — compact mode uses the pill-style tapeButton (click
    // opens editor, atom drives lit state); regular mode swaps in
    // tapeHeaderBtn (CompHeaderButton with green LED, matches the EQ
    // / COMP header grammar). Visibility set in setCompactMode.
    // Compact size matches the master EQ + COMP pills above
    // (20h × reduced(4,0)) so the three-pill stack reads as one
    // visual unit; non-compact tapeHeaderBtn keeps its taller native
    // header dimensions for the LED + label pairing.
    const int tapeRowH = compactMode ? 20 : 22;
    const int tapeReduceX = compactMode ? 4 : 2;
    auto buttonRow = area.removeFromTop (tapeRowH).reduced (tapeReduceX, 0);
    if (compactMode)                              tapeButton.setBounds (buttonRow);
    else if (tapeHeaderBtn != nullptr)            tapeHeaderBtn->setBounds (buttonRow);

    // Extra headroom below the tape row so the "GR" caption painted
    // above the gain-reduction meter doesn't collide with the gear
    // button. The caption sits at meterY - 9 px in paint(); 12 px of
    // breathing room keeps it clear and gives the strip a quieter
    // visual break between the comp/tape band and the fader/meter band.
    area.removeFromTop (12);

    // Numeric peak / GR readouts dropped from the strip — the LED
    // meter + GR bar communicate the same info, and the readouts ate
    // 18 px of vertical that the fader needs more. Labels still exist
    // (timer updates them) but are unparented here so they don't draw.
    outputPeakLabel.setVisible (false);
    grPeakLabel    .setVisible (false);

    // Centred [fader | level meter | GR LED] cluster — fader-side grammar
    // matching channel + bus strips, but centred in the (wider) master strip
    // instead of right-pinned. Scale labels drawn by paint() to the LEFT of
    // the slider's track via kSharedXOver math. No separate carved column.
    constexpr int kMeterW          = 16;
    constexpr int kGrLedW          = 20;
    constexpr int kMeterToGrGap    = 1;
    constexpr int kFaderToMeterGap = 1;
    constexpr int kFaderColW       = 50;
    constexpr int kFaderValueH     = 18;   // standalone value label below the slider

    // The master strip has no bus column or pan knob to balance against, so
    // the old right-pinned layout left a large dead gap on the left. Centre
    // the cluster; lay it out L->R: fader, level meter, GR LED.
    const int kClusterW = kFaderColW + kFaderToMeterGap + kMeterW
                        + kMeterToGrGap + kGrLedW;
    const int leftMargin = juce::jmax (0, (area.getWidth() - kClusterW) / 2);
    area.removeFromLeft (leftMargin);

    auto faderColArea = area.removeFromLeft (kFaderColW);
    area.removeFromLeft (kFaderToMeterGap);
    meterArea = area.removeFromLeft (kMeterW);
    area.removeFromLeft (kMeterToGrGap);
    auto compMeterCol = area.removeFromLeft (kGrLedW);

    // Trim meter top so it lines up with the slider's +6 tick — same grammar
    // as channel + bus strips. Bottom trimmed to match the fader's bottom
    // (kFaderValueH + 8 value-label reserve) so the meter doesn't overhang
    // past the "off" tick.
    const int meterTopY = faderColArea.getY() + (int) duskstudio::kFaderTrackPad;
    meterArea = meterArea.withTop (meterTopY)
                          .withTrimmedBottom (kFaderValueH + 8);
    // Legacy carves cleared — paint() short-circuits on empty rects.
    faderScaleArea = juce::Rectangle<int>();
    grMeterArea    = juce::Rectangle<int>();

    // Slider bottom trimmed for the standalone value label.
    auto sliderBounds = faderColArea.withTrimmedBottom (kFaderValueH + 8);
    faderSlider.setBounds (sliderBounds);

    faderValueLabel.setBounds (sliderBounds.getX(),
                                 sliderBounds.getBottom() + 6,
                                 sliderBounds.getWidth(),
                                 kFaderValueH);

    // compMeter top aligned with level meter's 0 dB tick (10 px caption
    // reserve above grBarArea pulled into account). Same math as channel
    // + bus strips.
    if (compMeter != nullptr)
    {
        // Match the level-meter draw mapping (fader's NormalisableRange)
        // so the GR threshold handle anchors on the visible 0 dB tick.
        const auto& faderRange = faderSlider.getNormalisableRange();
        const float zeroFrac = (float) faderRange.convertTo0to1 (0.0);
        const int zeroY = meterArea.getBottom() - 1
                        - juce::roundToInt (zeroFrac * (float) (meterArea.getHeight() - 2));
        constexpr int kGrCaptionReserve = 10;
        const int compTop = zeroY - kGrCaptionReserve;
        compMeter->setBounds (compMeterCol.withY (compTop)
                                          .withHeight (meterArea.getBottom() - compTop));
    }
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
    // Tape pill left-click opens the editor via onClick now; no need
    // for a right-click route here.
    juce::Component::mouseDown (e);
}

void MasterStripComponent::openEqEditorPopup()
{
    if (eqEditorModal.isOpen()) { eqEditorModal.close(); return; }
    if (compEditorModal.isOpen()) compEditorModal.close();
    auto* host = getTopLevelComponent();
    if (host == nullptr) host = this;
    eqEditorModal.show (*host, std::make_unique<MasterEqEditorPanel> (params));
}

void MasterStripComponent::openCompEditorPopup()
{
    if (compEditorModal.isOpen()) { compEditorModal.close(); return; }
    if (eqEditorModal.isOpen()) eqEditorModal.close();
    auto* host = getTopLevelComponent();
    if (host == nullptr) host = this;
    compEditorModal.show (*host, std::make_unique<MasterCompEditorPanel> (params));
}
} // namespace duskstudio
