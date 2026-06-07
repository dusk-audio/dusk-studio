#include "BusComponent.h"
#include "../engine/AudioEngine.h"
#include "DuskContextMenu.h"
#include "SteppedKnob.h"
#include "CompBypassLed.h"
#include "DuskStudioLookAndFeel.h"  // fourKColors palette
#include "../session/MidiBindings.h"
#include "../session/ParamEditAction.h"
#include <algorithm>

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
    s.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 38, 14);
    s.setColour (juce::Slider::rotarySliderFillColourId, col);
    s.setColour (juce::Slider::rotarySliderOutlineColourId, juce::Colour (0xff2a2a2e));
    s.setColour (juce::Slider::thumbColourId, col.brighter (0.3f));
    s.setColour (juce::Slider::textBoxTextColourId, juce::Colour (0xffd0d0d0));
    s.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    s.setColour (juce::Slider::textBoxOutlineColourId,    juce::Colours::transparentBlack);
    s.setNumDecimalPlacesToDisplay (decimals);
    s.setTextValueSuffix (suffix);
    // Double-click → reset to midPt (canonical "default" for skewed
    // knobs). Velocity-based mode w/ shift-swap = drag is positional
    // by default, holding Shift while dragging engages JUCE's slow
    // velocity-precision mode for fine adjust. Mirrors what channel-
    // strip knobs already get via enableValueLabel.
    s.setDoubleClickReturnValue (true, midPt);
    s.setVelocityBasedMode (false);
    s.setVelocityModeParameters (1.0, 1, 0.0, /*userCanPressKeyToSwap*/ true);
}

void styleSmallLabel (juce::Label& lbl, const juce::String& text, juce::Colour col)
{
    lbl.setText (text, juce::dontSendNotification);
    lbl.setJustificationType (juce::Justification::centred);
    lbl.setColour (juce::Label::textColourId, col);
    lbl.setFont (juce::Font (juce::FontOptions (8.5f, juce::Font::bold)));
}

// Dusk editor-modal styling helpers — match ChannelEqEditor / ChannelCompEditor
// (380-wide popups, 56-px rotary knobs, 80×18 text boxes, 16-pt bold accent
// band labels). Used by Bus / Master EQ + COMP modals so the UI conforms
// across every strip type.
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

// 11-pt comp-knob label — matches ChannelCompEditor's styleLabel exactly.
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

class BusEqEditorPanel final : public juce::Component
{
public:
    BusEqEditorPanel (Bus& busRef) : bus (busRef)
    {
        const auto eqGreen = juce::Colour (0xff80c090);
        styleEditorEnableBtn (enableBtn, eqGreen);
        enableBtn.setButtonText ("EQ");
        enableBtn.setToggleState (bus.strip.eqEnabled.load (std::memory_order_relaxed),
                                    juce::dontSendNotification);
        enableBtn.onClick = [this]
        {
            bus.strip.eqEnabled.store (enableBtn.getToggleState(),
                                         std::memory_order_release);
        };
        addAndMakeVisible (enableBtn);

        styleEditorKnob (lf,  eqGreen, -9.0, 9.0, 0.0, 0.0, " dB", 1);
        // Tooltips set after the styleEditorKnob trio below so the
        // hints carry the same range / reset semantics as the inline.
        styleEditorKnob (mid, eqGreen, -9.0, 9.0, 0.0, 0.0, " dB", 1);
        styleEditorKnob (hf,  eqGreen, -9.0, 9.0, 0.0, 0.0, " dB", 1);
        lf .setValue (bus.strip.eqLfGainDb .load(), juce::dontSendNotification);
        mid.setValue (bus.strip.eqMidGainDb.load(), juce::dontSendNotification);
        hf .setValue (bus.strip.eqHfGainDb .load(), juce::dontSendNotification);

        auto arm = [this]
        {
            bus.strip.eqEnabled.store (true, std::memory_order_release);
            enableBtn.setToggleState (true, juce::dontSendNotification);
        };
        lf .onValueChange = [this, arm] { bus.strip.eqLfGainDb .store ((float) lf .getValue(), std::memory_order_relaxed); arm(); };
        mid.onValueChange = [this, arm] { bus.strip.eqMidGainDb.store ((float) mid.getValue(), std::memory_order_relaxed); arm(); };
        hf .onValueChange = [this, arm] { bus.strip.eqHfGainDb .store ((float) hf .getValue(), std::memory_order_relaxed); arm(); };
        lf .setTooltip ("Bus EQ low shelf @ 300 Hz (-9..+9 dB). Double-click to reset; Shift-drag for fine.");
        mid.setTooltip ("Bus EQ mid bell @ 800 Hz, Q 0.7 (-9..+9 dB). Double-click to reset; Shift-drag for fine.");
        hf .setTooltip ("Bus EQ high shelf @ 2 kHz (-9..+9 dB). Double-click to reset; Shift-drag for fine.");
        addAndMakeVisible (lf); addAndMakeVisible (mid); addAndMakeVisible (hf);

        styleEditorLabel (lfLbl,  "LF",  eqGreen);
        styleEditorLabel (midLbl, "MID", eqGreen);
        styleEditorLabel (hfLbl,  "HF",  eqGreen);
        addAndMakeVisible (lfLbl); addAndMakeVisible (midLbl); addAndMakeVisible (hfLbl);

        setSize (380, kEditorOuterPad * 2 + kEditorHeaderH + kEditorHeaderGap
                       + kEditorLabelRowH + kEditorKnobBlockH);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff181820));
        g.setColour (juce::Colour (0xff2a2a30));
        g.drawRect (getLocalBounds(), 1);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (kEditorOuterPad);
        auto header = area.removeFromTop (kEditorHeaderH);
        enableBtn.setBounds (header.removeFromRight (60));
        area.removeFromTop (kEditorHeaderGap);

        auto labelRow = area.removeFromTop (kEditorLabelRowH);
        auto knobRow  = area.removeFromTop (kEditorKnobBlockH);
        const int colW = knobRow.getWidth() / 3;
        lfLbl .setBounds (labelRow.removeFromLeft (colW));
        midLbl.setBounds (labelRow.removeFromLeft (colW));
        hfLbl .setBounds (labelRow);
        lf .setBounds (knobRow.removeFromLeft (colW));
        mid.setBounds (knobRow.removeFromLeft (colW));
        hf .setBounds (knobRow);
    }

private:
    Bus& bus;
    juce::TextButton enableBtn;
    juce::Slider lf, mid, hf;
    juce::Label  lfLbl, midLbl, hfLbl;
};

// Bus COMP modal. Mirrors ChannelCompEditor:
//   - ON button top-right
//   - LEFT: handle column (threshold drag) | IN meter | GR meter
//   - RIGHT: 2×2 knob grid (RAT / MAK top, ATK / REL bottom)
// No mode picker (bus comp is a fixed SSL-style glue topology). No
// standalone THR knob — threshold is set via the triangle handle.
class BusCompEditorPanel final : public juce::Component, private juce::Timer
{
public:
    BusCompEditorPanel (Bus& busRef) : bus (busRef)
    {
        // SSL G-bus comp palette: powder-blue knob body + near-black
        // chassis. Variable name kept for callsite compatibility.
        const auto compGold = juce::Colour (0xff7da8c5);
        styleEditorEnableBtn (enableBtn, compGold);
        enableBtn.setButtonText ("ON");
        enableBtn.setToggleState (bus.strip.compEnabled.load (std::memory_order_relaxed),
                                    juce::dontSendNotification);
        enableBtn.onClick = [this]
        {
            bus.strip.compEnabled.store (enableBtn.getToggleState(),
                                           std::memory_order_release);
        };
        addAndMakeVisible (enableBtn);

        styleEditorKnob (rat, compGold,   1.0,   10.0,    4.0,    4.0, ":1",  1);
        styleEditorKnob (atk, compGold,   0.1,   50.0,    5.0,   10.0, " ms", 1);
        styleEditorKnob (rel, compGold,  50.0, 1000.0,  200.0,  200.0, " ms", 0);
        styleEditorKnob (mak, compGold, -10.0,   20.0,    0.0,    0.0, " dB", 1);
        for (auto* k : { &rat, &atk, &rel, &mak })
            k->setColour (juce::Slider::thumbColourId, juce::Colours::white);
        mak.setValue (bus.strip.compMakeupDb .load(), juce::dontSendNotification);

        rat.setTooltip ("Bus comp ratio (stepped: 2:1 / 4:1 / 10:1).");
        atk.setTooltip ("Bus comp attack (stepped: 0.1 / 0.3 / 1 / 3 / 10 / 30 ms).");
        rel.setTooltip ("Bus comp release (stepped: 0.1 / 0.3 / 0.6 / 1.2 s, top = AUTO).");
        mak.setTooltip ("Bus comp make-up gain (-10..+20 dB). Double-click for 0 dB; Shift-drag for fine.");
        mak.onValueChange = [this] { bus.strip.compMakeupDb .store ((float) mak.getValue(), std::memory_order_relaxed); };

        // Stepped SSL selectors (see the inline-strip comp setup).
        configureSteppedKnob (rat, sslsteps::ratioValues(), sslsteps::ratioLabels(),
                              bus.strip.compRatio.load(),
                              [this] (double v) { bus.strip.compRatio.store ((float) v, std::memory_order_relaxed); });
        configureSteppedKnob (atk, sslsteps::attackValues(), sslsteps::attackLabels(),
                              bus.strip.compAttackMs.load(),
                              [this] (double v) { bus.strip.compAttackMs.store ((float) v, std::memory_order_relaxed); });
        configureSteppedKnob (rel, sslsteps::releaseValues(), sslsteps::releaseLabels(),
                              bus.strip.compReleaseAuto.load() ? -1.0 : (double) bus.strip.compReleaseMs.load(),
                              [this] (double v)
                              {
                                  const bool autoOn = v < 0.0;
                                  bus.strip.compReleaseAuto.store (autoOn, std::memory_order_relaxed);
                                  if (! autoOn)
                                      bus.strip.compReleaseMs.store ((float) v, std::memory_order_relaxed);
                              });

        addAndMakeVisible (rat); addAndMakeVisible (atk);
        addAndMakeVisible (rel); addAndMakeVisible (mak);

        styleEditorCompLabel (ratLbl, "RATIO");
        styleEditorCompLabel (atkLbl, "ATTACK");
        styleEditorCompLabel (relLbl, "RELEASE");
        styleEditorCompLabel (makLbl, "GAIN");
        addAndMakeVisible (ratLbl); addAndMakeVisible (atkLbl);
        addAndMakeVisible (relLbl); addAndMakeVisible (makLbl);

        // Same fixed size as ChannelCompEditor's grid mode for visual
        // parity. Channel computes: kHeaderH(24)+gap(8)+modeRow(24)+gap(12)
        // + 2×knobBlockH(80) + footerPad(16) + 24 = 268. Bus has no
        // mode row — drop those rows + use ~244h.
        setSize (380, 268);
        startTimerHz (30);
    }

    ~BusCompEditorPanel() override { stopTimer(); }

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

        // Threshold triangle handle, drawn on the LEFT of the IN meter.
        if (! inputMeterArea.isEmpty() && ! threshHandleArea.isEmpty())
        {
            const float thresh = juce::jlimit (-60.0f, 0.0f,
                bus.strip.compThreshDb.load (std::memory_order_relaxed));
            const float frac = (thresh - (-60.0f)) / 60.0f;
            const auto inBar = inputMeterArea.toFloat();
            const float y = inBar.getBottom() - 2.0f - frac * (inBar.getHeight() - 4.0f);

            const float halfH = 9.0f;
            const float baseX = (float) threshHandleArea.getX();
            const float tipX  = (float) threshHandleArea.getRight() + 2.0f;
            juce::Path tri;
            tri.addTriangle (baseX, y - halfH, baseX, y + halfH, tipX, y);

            const bool engaged = bus.strip.compEnabled.load (std::memory_order_relaxed);
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
            bus.strip.compEnabled.store (true, std::memory_order_release);
            enableBtn.setToggleState (true, juce::dontSendNotification);
            repaint();
        }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! draggingThreshold) return;
        writeThresholdFromY (e.y);
        bus.strip.compEnabled.store (true, std::memory_order_release);
        enableBtn.setToggleState (true, juce::dontSendNotification);
        repaint();
    }

    void mouseUp (const juce::MouseEvent&) override { draggingThreshold = false; }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        const auto hitArea = threshHandleArea.getUnion (inputMeterArea).expanded (2);
        if (! hitArea.contains (e.getPosition())) return;
        // Reset to 0 dB (no compression).
        bus.strip.compThreshDb.store (0.0f, std::memory_order_relaxed);
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
        // Smooth IN meter (fast-up, slow-down).
        const float l = bus.strip.meterPostBusLDb.load (std::memory_order_relaxed);
        const float r = bus.strip.meterPostBusRDb.load (std::memory_order_relaxed);
        const float in = juce::jmax (l, r);
        if (in > displayedInputDb) displayedInputDb = in;
        else                        displayedInputDb += (in - displayedInputDb) * 0.10f;

        // Smooth GR (fast-down on hit, slow release back to 0).
        const float gr = bus.strip.meterGrDb.load (std::memory_order_relaxed);
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
        bus.strip.compThreshDb.store (juce::jlimit (-60.0f, 0.0f, dbOnInAxis),
                                        std::memory_order_relaxed);
    }

    Bus& bus;
    juce::TextButton enableBtn;
    juce::Slider rat, atk, rel, mak;
    juce::Label  ratLbl, atkLbl, relLbl, makLbl;
    juce::Rectangle<int> inputMeterArea, grMeterArea, threshHandleArea;
    float displayedInputDb = -100.0f;
    float displayedGrDb    = 0.0f;
    bool  draggingThreshold = false;
};
} // namespace

BusComponent::BusComponent (Bus& b, Session& s, AudioEngine& e, int idx)
    : bus (b), sessionRef (s), engine (e), busIndex (idx)
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
        if (txt.isEmpty()) { nameLabel.setText (bus.name, juce::dontSendNotification); return; }
        if (txt == bus.name) return;
        const auto oldName = bus.name;
        auto& um = engine.getUndoManager();
        um.beginNewTransaction ("Bus name");
        um.perform (new ParamEditAction (
            [&s = sessionRef, idx = busIndex, txt]     { s.bus (idx).name = txt; },
            [&s = sessionRef, idx = busIndex, oldName] { s.bus (idx).name = oldName; }));
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
    const auto compGold = juce::Colour (0xff7da8c5);   // SSL G-bus comp powder blue (legacy var name)
    // Pan red - matches the channel-strip pan colour (0xffc04040) so the
    // pan-control language is consistent across channels and buses.
    const auto panRed   = juce::Colour (0xffc04040);

    // EQ section header — CompHeaderButton (LED + label pill) matches the
    // COMP header below and the channel-strip EQ header. Left-click toggles
    // enable; no right-click (bus EQ has fixed topology, no mode picker).
    eqHeaderBtn = std::make_unique<CompHeaderButton> (
        /*getEnabled*/ [this] { return bus.strip.eqEnabled.load (std::memory_order_relaxed); },
        /*onToggle*/   [this]
                        {
                            const bool now = ! bus.strip.eqEnabled.load (std::memory_order_relaxed);
                            bus.strip.eqEnabled.store (now, std::memory_order_release);
                            if (eqHeaderBtn != nullptr) eqHeaderBtn->repaint();
                        });
    eqHeaderBtn->setLabelText ("EQ");
    eqHeaderBtn->setTooltip ("3-band British-style EQ on/off");
    addAndMakeVisible (eqHeaderBtn.get());

    // Suffixes empty - same rationale as master: 38-px textbox can't fit
    // " dB"/" ms" without truncating, and the L/M/H labels already imply dB.
    styleSmallKnob (eqLfGain,  -9.0, 9.0, 0.0, bus.strip.eqLfGainDb.load(),  eqGreen, "", 1);
    styleSmallKnob (eqMidGain, -9.0, 9.0, 0.0, bus.strip.eqMidGainDb.load(), eqGreen, "", 1);
    styleSmallKnob (eqHfGain,  -9.0, 9.0, 0.0, bus.strip.eqHfGainDb.load(),  eqGreen, "", 1);
    eqLfGain .setTooltip ("Bus EQ low shelf @ 300 Hz (-9..+9 dB). Double-click to reset; Shift-drag for fine.");
    eqMidGain.setTooltip ("Bus EQ mid bell @ 800 Hz, Q 0.7 (-9..+9 dB). Double-click to reset; Shift-drag for fine.");
    eqHfGain .setTooltip ("Bus EQ high shelf @ 2 kHz (-9..+9 dB). Double-click to reset; Shift-drag for fine.");
    // Auto-arm the bus EQ on any band touch (same UX as the channel
    // strip): EQ defaults to off and the LED only lights once the
    // engineer shapes the sound. release ordering pairs with audio-
    // thread's relaxed read in the DSP gate.
    auto armBusEq = [this]
    {
        bus.strip.eqEnabled.store (true, std::memory_order_release);
        if (eqHeaderBtn != nullptr) eqHeaderBtn->repaint();
    };
    eqLfGain .onValueChange = [this, armBusEq] { bus.strip.eqLfGainDb .store ((float) eqLfGain .getValue(), std::memory_order_relaxed); armBusEq(); };
    eqMidGain.onValueChange = [this, armBusEq] { bus.strip.eqMidGainDb.store ((float) eqMidGain.getValue(), std::memory_order_relaxed); armBusEq(); };
    eqHfGain .onValueChange = [this, armBusEq] { bus.strip.eqHfGainDb .store ((float) eqHfGain .getValue(), std::memory_order_relaxed); armBusEq(); };
    addAndMakeVisible (eqLfGain); addAndMakeVisible (eqMidGain); addAndMakeVisible (eqHfGain);
    // L = low shelf, M = bell, H = high shelf - standard 3-band EQ labelling.
    styleSmallLabel (eqLfLbl,  "L", eqGreen);
    styleSmallLabel (eqMidLbl, "M", eqGreen);
    styleSmallLabel (eqHfLbl,  "H", eqGreen);
    addAndMakeVisible (eqLfLbl); addAndMakeVisible (eqMidLbl); addAndMakeVisible (eqHfLbl);

    // Comp section. Shell now mirrors the channel-strip COMP visually:
    //   - CompHeaderButton on top (single button, white text, green LED,
    //     left-click toggles enable). No mode picker (bus comp is a
    //     fixed SSL-style glue topology, not OPTO/FET/VCA).
    //   - CompMeterStrip on the LEFT (handle + IN bar + dB scale + GR
    //     bar). Threshold drag writes bus.strip.compThreshDb (-60..0).
    //   - Knob grid on the RIGHT: RAT / ATK + REL / MAK across two rows.
    //     The standalone THR knob is gone — threshold is set via the
    //     triangle handle on the meter.
    compHeaderBtn = std::make_unique<CompHeaderButton> (
        /*getEnabled*/ [this] { return bus.strip.compEnabled.load (std::memory_order_relaxed); },
        /*onToggle*/   [this]
                        {
                            const bool now = ! bus.strip.compEnabled.load (std::memory_order_relaxed);
                            bus.strip.compEnabled.store (now, std::memory_order_release);
                            if (compHeaderBtn != nullptr) compHeaderBtn->repaint();
                        });
    addAndMakeVisible (compHeaderBtn.get());

    CompMeterStrip::Source compSrc;
    compSrc.getInputDb     = [this]
                              {
                                  const float l = bus.strip.meterPostBusLDb.load (std::memory_order_relaxed);
                                  const float r = bus.strip.meterPostBusRDb.load (std::memory_order_relaxed);
                                  return juce::jmax (l, r);
                              };
    compSrc.getGrDb        = [this] { return bus.strip.meterGrDb.load (std::memory_order_relaxed); };
    compSrc.getThresholdDb = [this] { return bus.strip.compThreshDb.load (std::memory_order_relaxed); };
    compSrc.setThresholdDb = [this] (float db)
                              {
                                  bus.strip.compThreshDb.store (juce::jlimit (-60.0f, 0.0f, db),
                                                                  std::memory_order_relaxed);
                              };
    compSrc.resetThreshold = [this]
                              {
                                  bus.strip.compThreshDb.store (0.0f, std::memory_order_relaxed);
                              };
    compSrc.isEngaged      = [this] { return bus.strip.compEnabled.load (std::memory_order_relaxed); };
    compSrc.autoEnable     = [this]
                              {
                                  bus.strip.compEnabled.store (true, std::memory_order_release);
                                  if (compHeaderBtn != nullptr) compHeaderBtn->repaint();
                              };
    // Fader-side GR LED (matches channel-strip grammar): slim widget that
    // shows only the GR bar + threshold-drag handle, glued next to the
    // level meter to the right of the fader. setRangeDb(-60, 0) maps the
    // drag y position 1:1 with the level-meter scale so dragging to -18 dB
    // lands the triangle on the level meter's -18 tick.
    compSrc.floorDb        = -60.0f;
    compSrc.ceilingDb      =   0.0f;
    compMeter = std::make_unique<CompMeterStrip> (std::move (compSrc));
    compMeter->setShowInputBar (false);
    compMeter->setHandleVisible (true);
    compMeter->setHandleOnRight (true);
    compMeter->setRangeDb (-60.0f, 0.0f);
    addAndMakeVisible (compMeter.get());

    styleSmallKnob (compRatio,     1.0,   10.0,    4.0, bus.strip.compRatio.load(),     compGold, ":1", 1);
    styleSmallKnob (compAttack,    0.1,   50.0,    5.0, bus.strip.compAttackMs.load(),  compGold, "",   1);
    styleSmallKnob (compRelease,  50.0, 1000.0,  200.0, bus.strip.compReleaseMs.load(), compGold, "",   0);
    styleSmallKnob (compMakeup,  -10.0,   20.0,    0.0, bus.strip.compMakeupDb.load(),  compGold, "",   1);
    compRatio  .setTooltip ("Bus comp ratio (stepped: 2:1 / 4:1 / 10:1).");
    compAttack .setTooltip ("Bus comp attack (stepped: 0.1 / 0.3 / 1 / 3 / 10 / 30 ms).");
    compRelease.setTooltip ("Bus comp release (stepped: 0.1 / 0.3 / 0.6 / 1.2 s, top = AUTO).");
    compMakeup .setTooltip ("Bus comp make-up gain (-10..+20 dB). Double-click for 0 dB; Shift-drag for fine.");
    // White pointer on the powder-blue body — matches the SSL G-bus
    // comp's painted-line indicator. styleSmallKnob's default thumb
    // (fill-brightened) washes out against the light blue.
    for (auto* k : { &compRatio, &compAttack, &compRelease, &compMakeup })
        k->setColour (juce::Slider::thumbColourId, juce::Colours::white);
    compMakeup .onValueChange = [this] { bus.strip.compMakeupDb .store ((float) compMakeup .getValue(), std::memory_order_relaxed); };

    // Ratio / attack / release are SSL-style stepped selectors — the donor's
    // bus_* params are Choices, so the knob detents on the real values and the
    // readout shows them (no more continuous knob landing between steps).
    configureSteppedKnob (compRatio, sslsteps::ratioValues(), sslsteps::ratioLabels(),
                          bus.strip.compRatio.load(),
                          [this] (double v) { bus.strip.compRatio.store ((float) v, std::memory_order_relaxed); });
    configureSteppedKnob (compAttack, sslsteps::attackValues(), sslsteps::attackLabels(),
                          bus.strip.compAttackMs.load(),
                          [this] (double v) { bus.strip.compAttackMs.store ((float) v, std::memory_order_relaxed); });
    configureSteppedKnob (compRelease, sslsteps::releaseValues(), sslsteps::releaseLabels(),
                          bus.strip.compReleaseAuto.load() ? -1.0 : (double) bus.strip.compReleaseMs.load(),
                          [this] (double v)
                          {
                              const bool autoOn = v < 0.0;
                              bus.strip.compReleaseAuto.store (autoOn, std::memory_order_relaxed);
                              if (! autoOn)
                                  bus.strip.compReleaseMs.store ((float) v, std::memory_order_relaxed);
                          });

    addAndMakeVisible (compRatio);  addAndMakeVisible (compAttack);
    addAndMakeVisible (compRelease); addAndMakeVisible (compMakeup);
    styleSmallLabel (compRatLbl, "RAT", compGold);
    styleSmallLabel (compAtkLbl, "ATK", compGold);
    styleSmallLabel (compRelLbl, "REL", compGold);
    styleSmallLabel (compMakLbl, "MAK", compGold);
    addAndMakeVisible (compRatLbl); addAndMakeVisible (compAtkLbl);
    addAndMakeVisible (compRelLbl); addAndMakeVisible (compMakLbl);

    // Pan. Format matches channel-strip pan: "C" at centre,
    // "L<pct>" / "R<pct>" otherwise — no decimals, no raw -1..1 number.
    styleSmallKnob (panKnob, -1.0, 1.0, 0.0, bus.strip.pan.load(), panRed, "", 0);
    panKnob.setNumDecimalPlacesToDisplay (0);
    panKnob.textFromValueFunction = [] (double v) -> juce::String
    {
        if (std::abs (v) < 0.01) return "C";
        const int pct = (int) std::round (std::abs (v) * 100.0);
        return (v < 0 ? "L" : "R") + juce::String (pct);
    };
    panKnob.updateText();
    panKnob.setTooltip ("Bus pan (L100..C..R100). Double-click for centre; Shift-drag for fine.");
    panKnob.onValueChange = [this] { bus.strip.pan.store ((float) panKnob.getValue(), std::memory_order_relaxed); };
    panKnob.onDragStart   = [this] { bus.strip.panTouched.store (true,  std::memory_order_release); };
    panKnob.onDragEnd     = [this] { bus.strip.panTouched.store (false, std::memory_order_release); };
    addAndMakeVisible (panKnob);
    // Match the channel-strip PAN label (10.5 pt bold) instead of the
    // smaller 8.5 pt used by the bus's L/M/H knob captions — PAN sits
    // alone above its knob and benefits from the larger size for parity
    // with the track strips.
    panLbl.setText ("PAN", juce::dontSendNotification);
    panLbl.setJustificationType (juce::Justification::centred);
    panLbl.setColour (juce::Label::textColourId, panRed);
    panLbl.setFont (juce::Font (juce::FontOptions (10.5f, juce::Font::bold)));
    addAndMakeVisible (panLbl);

    faderSlider.setRange (ChannelStripParams::kFaderMinDb, ChannelStripParams::kFaderMaxDb, 0.1);
    faderSlider.setSkewFactorFromMidPoint (-12.0);
    faderSlider.setValue (bus.strip.faderDb.load (std::memory_order_relaxed), juce::dontSendNotification);
    faderSlider.setDoubleClickReturnValue (true, 0.0);
    // No textbox — the standalone faderValueLabel below shows the value at
    // the channel-strip's font weight + size (cap can fully reach min/max).
    faderSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    faderSlider.setTooltip ("Bus fader (-90..+12 dB). Double-click for 0 dB; Shift-drag for fine.");
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
        bus.strip.faderDb.store (v, std::memory_order_relaxed);
        faderValueLabel.setText (formatFaderDb (v), juce::dontSendNotification);
    };
    faderSlider.onDragStart = [this] { bus.strip.faderTouched.store (true,  std::memory_order_release); };
    faderSlider.onDragEnd   = [this] { bus.strip.faderTouched.store (false, std::memory_order_release); };
    addAndMakeVisible (faderSlider);

    // Standalone fader-value readout below the slider (matches channel
    // strip's fader-side value label).
    faderValueLabel.setJustificationType (juce::Justification::centred);
    faderValueLabel.setColour (juce::Label::textColourId, juce::Colour (0xffd8d8d8));
    faderValueLabel.setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);
    faderValueLabel.setColour (juce::Label::outlineColourId,    juce::Colours::transparentBlack);
    faderValueLabel.setFont (juce::Font (juce::FontOptions (
        juce::Font::getDefaultMonospacedFontName(), 14.0f, juce::Font::bold)));
    faderValueLabel.setText (formatFaderDb (faderSlider.getValue()), juce::dontSendNotification);
    addAndMakeVisible (faderValueLabel);

    muteButton.setClickingTogglesState (true);
    muteButton.setColour (juce::TextButton::buttonOnColourId, juce::Colours::orangered);
    muteButton.setToggleState (bus.strip.mute.load (std::memory_order_relaxed), juce::dontSendNotification);
    muteButton.onClick = [this]
    {
        const bool newState = muteButton.getToggleState();
        bus.strip.mute.store (newState, std::memory_order_release);
        // WRITE only: the audio thread reads the discrete mute lane in Touch
        // (routeDiscrete has no `touched` gate), so a Touch-mode capture would
        // push_back into a vector it is mid-read on. Write reads `manual`, so
        // the append never overlaps a lane read.
        const int m = bus.strip.automationMode.load (std::memory_order_relaxed);
        const bool capturing = engine.getTransport().isPlaying()
                             && m == (int) AutomationMode::Write;
        if (capturing)
            captureWritePoint (AutomationParam::Mute, newState ? 1.0f : 0.0f);
    };
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

    // Automation-mode button — same grammar as the channel strips: borderless
    // text, mode colour applied via refreshAutoModeButton(), in-window menu.
    autoModeButton.setTooltip ("Automation mode (click to pick: Off / Read / Write / Touch). "
                                "READ replays the recorded ride; WRITE captures fader / pan / "
                                "mute moves; TOUCH replays until you grab a control, then "
                                "captures while held.");
    autoModeButton.onClick = [this] { showAutoModeMenu(); };
    autoModeButton.setColour (juce::TextButton::buttonColourId,   juce::Colours::transparentBlack);
    autoModeButton.setColour (juce::TextButton::buttonOnColourId, juce::Colours::transparentBlack);
    addAndMakeVisible (autoModeButton);
    // Apply the restored mode (not just the button visuals) so a bus loaded in
    // READ opens with its fader / pan / mute already disabled.
    setAutoMode ((AutomationMode) bus.strip.automationMode.load (std::memory_order_relaxed));

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
        lbl.setColour (juce::Label::backgroundColourId,  juce::Colours::transparentBlack);
        // No outline - same rationale as the channel strip: the 1 px border
        // drew on top of the adjacent fader textbox edge and looked like overlap.
        lbl.setColour (juce::Label::outlineColourId,     juce::Colours::transparentBlack);
        lbl.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                                                      10.0f, juce::Font::bold)));
    };
    outputPeakLabel.setText ("-inf", juce::dontSendNotification);
    styleReadout (outputPeakLabel, juce::Colour (0xffd0d0d0));
    addAndMakeVisible (outputPeakLabel);

    // Compact placeholder buttons (hidden until setCompactMode(true)).
    // Same visual grammar as ChannelStripComponent's eq/comp compact
    // pills so the bus + master strips read as the same compact form
    // when the tape TIMELINE shrinks the strip.
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
    eqCompactButton  .setTooltip ("Open the bus EQ editor (3-band British-style EQ).");
    compCompactButton.setTooltip ("Open the bus comp editor (console-style glue compressor).");
    eqCompactButton  .onClick = [this] { openEqEditorPopup(); };
    compCompactButton.onClick = [this] { openCompEditorPopup(); };
    addChildComponent (eqCompactButton);
    addChildComponent (compCompactButton);

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

void BusComponent::captureWritePoint (AutomationParam param, float denormValue)
{
    // Denormalize -> 0..1 lane storage. Mirrors ChannelStripComponent's
    // capture; buses only automate FaderDb / Pan / Mute.
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
                return v >= 0.5f ? 1.0f : 0.0f;
            // Buses don't automate solo or aux sends.
            case AutomationParam::Solo:
            case AutomationParam::AuxSend1:
            case AutomationParam::AuxSend2:
            case AutomationParam::AuxSend3:
            case AutomationParam::AuxSend4:
            case AutomationParam::kCount:
                break;
        }
        return 0.0f;
    };

    auto& lane = bus.strip.automationLanes[(size_t) param];
    AutomationPoint pt;
    pt.timeSamples   = engine.getTransport().getPlayhead();
    pt.value         = normalize (param, denormValue);
    pt.recordedAtBPM = sessionRef.tempoBpm.load (std::memory_order_relaxed);

    // Pre-filter: drop near-identical continuous samples close in time
    // (timer noise when the control hasn't moved). Discrete params keep
    // every transition.
    if (isContinuousParam (param) && ! lane.points.empty())
    {
        constexpr float kDeltaEps = 0.001f;
        constexpr juce::int64 kMaxSpanSamples = 22050;   // ~500 ms @ 44.1 k
        const auto& last = lane.points.back();
        if (std::abs (pt.value - last.value) < kDeltaEps
            && pt.timeSamples >= last.timeSamples
            && (pt.timeSamples - last.timeSamples) < kMaxSpanSamples)
            return;
    }

    // Keep the ascending-time invariant evaluateLane's binary search needs.
    // A backward playhead (loop wrap / rewind) truncates the now-future tail
    // then appends; a same-sample hit replaces in place.
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

void BusComponent::showAutoModeMenu()
{
    const int cur = bus.strip.automationMode.load (std::memory_order_relaxed);
    juce::PopupMenu menu;
    menu.addItem (1, "Off",   true, cur == (int) AutomationMode::Off);
    menu.addItem (2, "Read",  true, cur == (int) AutomationMode::Read);
    menu.addItem (3, "Write", true, cur == (int) AutomationMode::Write);
    menu.addItem (4, "Touch", true, cur == (int) AutomationMode::Touch);
    showContextMenu (menu, autoModeButton,
        [safeThis = juce::Component::SafePointer<BusComponent> (this)] (int chosen)
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

void BusComponent::setAutoMode (AutomationMode mode)
{
    // release-store so the audio thread's acquire-load of the new mode sees
    // every lane append made during the Write/Touch pass that just ended.
    bus.strip.automationMode.store ((int) mode, std::memory_order_release);

    // Read disables the automated controls (fader / pan / mute). Solo isn't
    // automated, so it stays interactive in every mode.
    const bool interactive = mode != AutomationMode::Read;
    faderSlider.setEnabled (interactive);
    panKnob    .setEnabled (interactive);
    muteButton .setEnabled (interactive);

    refreshAutoModeButton();
}

void BusComponent::refreshAutoModeButton()
{
    const int m = bus.strip.automationMode.load (std::memory_order_relaxed);
    const char* label = "OFF";
    juce::Colour bg = juce::Colours::transparentBlack;
    juce::Colour fg (0xff707074);
    switch (m)
    {
        case (int) AutomationMode::Read:
            label = "READ";
            bg = juce::Colour (0xff20603a);   // muted green
            fg = juce::Colour (0xffd0e8d0);
            break;
        case (int) AutomationMode::Write:
            label = "WRITE";
            bg = juce::Colour (0xff803020);   // muted red
            fg = juce::Colour (0xfff0d0c8);
            break;
        case (int) AutomationMode::Touch:
            label = "TOUCH";
            bg = juce::Colour (0xff806020);   // muted amber
            fg = juce::Colour (0xfff0e0c0);
            break;
        case (int) AutomationMode::Off:
        default: break;
    }
    autoModeButton.setButtonText (label);
    autoModeButton.setColour (juce::TextButton::buttonColourId, bg);
    autoModeButton.setColour (juce::TextButton::textColourOffId, fg);
}

void BusComponent::timerCallback()
{
    // Refresh the name label when something external rewrites it (undo,
    // session reload) so the strip doesn't show a stale name.
    if (! nameLabel.isBeingEdited() && nameLabel.getText (false) != bus.name)
        nameLabel.setText (bus.name, juce::dontSendNotification);

    if (lastBusColour != bus.colour)
    {
        lastBusColour = bus.colour;
        repaint();
    }

    // Compact-mode button illumination: the EQ + COMP pills light up
    // when their corresponding section is engaged (eqEnabled /
    // compEnabled). Mirrors ChannelStripComponent's compact-button
    // refresh so all three strip types read the same way under TIMELINE.
    if (compactMode)
    {
        const auto eqAccent   = juce::Colour (0xff5fc46f);
        const auto compAccent = juce::Colour (0xffe0c050);
        const int eqOn   = bus.strip.eqEnabled  .load (std::memory_order_relaxed) ? 1 : 0;
        const int compOn = bus.strip.compEnabled.load (std::memory_order_relaxed) ? 1 : 0;
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

    // GR is shown by the graphical GR meter; displayedGrDb still feeds it.
    const float gr = bus.strip.meterGrDb.load (std::memory_order_relaxed);
    if (gr < displayedGrDb) displayedGrDb = gr;
    else                    displayedGrDb += (gr - displayedGrDb) * 0.5f;  // ~48 ms recovery (was ~167 ms)

    if (! meterArea.isEmpty())   repaint (meterArea);
    if (! grMeterArea.isEmpty()) repaint (grMeterArea.expanded (2, 10));  // include "GR" caption

    // Motor-fader / motor-pan animation + Write/Touch capture — same grammar
    // as ChannelStripComponent. We poll the live* atoms (the engine writes
    // them every block: Off mirrors the setpoint, so MIDI-bound moves still
    // sync here; Read/Touch-untouched carry the lane value). Gate the visual
    // update on the user NOT grabbing that control so we never fight a
    // gesture. Capture appends to the lane when playing in Write, or in Touch
    // while the control is held.
    const int  amode   = bus.strip.automationMode.load (std::memory_order_relaxed);
    const bool isWrite  = amode == (int) AutomationMode::Write;
    const bool isTouch  = amode == (int) AutomationMode::Touch;
    const bool playing  = engine.getTransport().isPlaying();
    {
        const float live    = bus.strip.liveFaderDb.load (std::memory_order_relaxed);
        const bool  touched = bus.strip.faderTouched.load (std::memory_order_relaxed);
        if (! touched && std::abs (live - displayedLiveFaderDb) > 0.05f)
        {
            displayedLiveFaderDb = live;
            faderSlider.setValue (live, juce::dontSendNotification);
            faderValueLabel.setText ((live <= -89.95f) ? juce::String ("off")
                                                        : juce::String (live, 1),
                                       juce::dontSendNotification);
        }
        else if (touched)
            displayedLiveFaderDb = live;

        if (playing && (isWrite || (isTouch && touched)))
            captureWritePoint (AutomationParam::FaderDb,
                                bus.strip.faderDb.load (std::memory_order_relaxed));
    }
    {
        const float live    = bus.strip.livePan.load (std::memory_order_relaxed);
        const bool  touched = bus.strip.panTouched.load (std::memory_order_relaxed);
        if (! touched && std::abs (live - displayedLivePan) > 0.005f)
        {
            displayedLivePan = live;
            panKnob.setValue (live, juce::dontSendNotification);
        }
        else if (touched)
            displayedLivePan = live;

        if (playing && (isWrite || (isTouch && touched)))
            captureWritePoint (AutomationParam::Pan,
                                bus.strip.pan.load (std::memory_order_relaxed));
    }
    {
        // Mute display follows the active source: the lane value in
        // Read/Touch, the manual setpoint in Off/Write. liveMute trails the
        // setpoint by a block in Off, so reading it there made the button
        // look stuck right after a click. dontSendNotification avoids
        // re-triggering the click capture.
        const bool laneDriven = (amode == (int) AutomationMode::Read)
                              || (amode == (int) AutomationMode::Touch);
        const bool m = laneDriven ? bus.strip.liveMute.load (std::memory_order_relaxed)
                                   : bus.strip.mute.load (std::memory_order_relaxed);
        if (muteButton.getToggleState() != m)
            muteButton.setToggleState (m, juce::dontSendNotification);
    }
    {
        const bool s = bus.strip.solo.load (std::memory_order_relaxed);
        if (soloButton.getToggleState() != s)
            soloButton.setToggleState (s, juce::dontSendNotification);
    }
    // EQ LED reads via CompHeaderButton's getEnabled lambda; nudge a
    // repaint each tick so the LED reflects external atom changes (e.g.
    // session reload, MIDI bindings) without a click.
    if (eqHeaderBtn != nullptr) eqHeaderBtn->repaint();
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

void BusComponent::applyBusColour (juce::Colour c)
{
    if (bus.colour == c) return;
    const auto oldColour = bus.colour;
    auto& um = engine.getUndoManager();
    um.beginNewTransaction ("Bus colour");
    um.perform (new ParamEditAction (
        [&s = sessionRef, idx = busIndex, c]         { s.bus (idx).colour = c; },
        [&s = sessionRef, idx = busIndex, oldColour] { s.bus (idx).colour = oldColour; }));
    repaint();
}

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
    showContextMenu (menu, *this,
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
    // Inset the card 4 px so adjacent bus strips show a clear gap
    // between their outlines AND so the inner content (with its 6 px
    // resized() margin) has a 2 px gutter between knobs/faders and the
    // frame. Earlier 3 px inset put the frame nearly on top of the
    // content, which read as cramped.
    auto r = getLocalBounds().toFloat().reduced (4.0f);
    g.setColour (juce::Colour (0xff181820));
    g.fillRoundedRectangle (r, 5.0f);
    g.setColour (bus.colour.withAlpha (0.85f));
    g.fillRoundedRectangle (r.removeFromTop (4.0f), 2.0f);
    g.setColour (bus.colour.withAlpha (0.45f));
    g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (4.0f), 5.0f, 1.5f);

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
        // SSL G-bus comp chassis — near-black with a soft powder-blue
        // outline. Matches the EQ section's hardware-grammar treatment.
        g.setColour (juce::Colour (0xff0a0a0a));
        g.fillRoundedRectangle (compArea.toFloat(), 3.0f);
        g.setColour (juce::Colour (0xff7da8c5).withAlpha (0.40f));
        g.drawRoundedRectangle (compArea.toFloat().reduced (0.5f), 3.0f, 0.8f);
    }

    // Stereo LED meter - two columns side by side inside meterArea.
    if (! meterArea.isEmpty())
    {
        // Match the fader's range + skewed scale so the LED tick
        // positions line up with the fader tick labels (+6 / +3 / 0 / 3
        // / 6 / 12 / 24 / 40 / off). Without the skew the LED's "0 dB"
        // sat at 91 % of bar height while the fader's "0" tick lived at
        // 73 % — they read as two different scales for the same signal.
        constexpr float kMinDb = ChannelStripParams::kFaderMinDb;   // -100
        constexpr float kMaxDb = ChannelStripParams::kFaderMaxDb;   // +12
        constexpr float kFaderSkewMidDb = -12.0f;
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
                // JUCE setSkewFactorFromMidPoint formula —
                // skewFactor = log(0.5) / log(midFrac).
                const float midFrac = (kFaderSkewMidDb - kMinDb)
                                    / (kMaxDb - kMinDb);
                const float skewFactor = std::log (0.5f) / std::log (midFrac);
                const float t = juce::jlimit (0.0f, 1.0f,
                                                  (db - kMinDb) / (kMaxDb - kMinDb));
                return std::pow (t, skewFactor);
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

    // Fader dB scale labels — drawn LEFT of the slider's track (track-3
    // grammar). Each label has a short tick line stub extending toward
    // the track; the label glyph sits a few px further left for breathing
    // room. Skipped when the fader scale column was carved out (legacy
    // layout fallback).
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

void BusComponent::setCompactVu (bool compact)
{
    if (compactVu == compact) return;
    compactVu = compact;
    resized();
}

void BusComponent::setCompactMode (bool compact)
{
    if (compactMode == compact) return;
    compactMode = compact;

    // Reset the gated-repaint sentinels so the next timer tick after
    // re-entering compact mode unconditionally publishes the pill
    // colours; otherwise a previous compact-mode cache value would
    // suppress the first refresh when the underlying EQ/COMP state
    // hasn't flipped between toggles.
    lastCompactEqOn   = -1;
    lastCompactCompOn = -1;

    // Hide / show every EQ + COMP child in lockstep with the mode.
    // Same approach as ChannelStripComponent::setEqSectionVisible /
    // setCompSectionVisible — no per-section toggle, the whole block
    // collapses behind the compact placeholder button.
    const bool sec = ! compact;
    if (eqHeaderBtn != nullptr) eqHeaderBtn->setVisible (sec);
    eqLfGain  .setVisible (sec);  eqLfLbl .setVisible (sec);
    eqMidGain .setVisible (sec);  eqMidLbl.setVisible (sec);
    eqHfGain  .setVisible (sec);  eqHfLbl .setVisible (sec);
    if (compHeaderBtn != nullptr) compHeaderBtn->setVisible (sec);
    if (compMeter     != nullptr) compMeter    ->setVisible (sec);
    compRatio  .setVisible (sec);  compRatLbl.setVisible (sec);
    compAttack .setVisible (sec);  compAtkLbl.setVisible (sec);
    compRelease.setVisible (sec);  compRelLbl.setVisible (sec);
    compMakeup .setVisible (sec);  compMakLbl.setVisible (sec);

    eqCompactButton  .setVisible (compact);
    compCompactButton.setVisible (compact);

    resized();
    repaint();
}

void BusComponent::resized()
{
    // 6 px inner inset matches MasterStripComponent so the bus row +
    // master share the same visual gutter between content and frame.
    auto area = getLocalBounds().reduced (6);
    area.removeFromTop (6);
    nameLabel.setBounds (area.removeFromTop (20));
    area.removeFromTop (3);

    // Analog VU meter at ~12:7 (~1.71:1) aspect ratio. Sits between the
    // name label and the EQ block so the user reads level first (the
    // most common monitoring task). The squatter 12:7 ratio (vs the
    // 12:5 photo aspect) leaves more vertical room for the EQ + comp
    // section below at narrow strip widths.
    if (vuMeter != nullptr)
    {
        // When the TIMELINE tape strip is expanded the bus fader needs
        // more room, so the VU shrinks. Shrink BOTH dimensions so the
        // dial keeps the same 12:7 aspect ratio as the expanded form
        // (just smaller) - earlier code only reduced the height which
        // left a wide-and-flat box with a tiny dial floating inside.
        constexpr int kRatioW = 12;
        constexpr int kRatioH = 7;
        const int stripW = area.getWidth();
        // Compact heightDriver = stripW * 6/12; gives vuW = stripW * 6/7
        // (~86% of strip) so the VU fills most of the horizontal room
        // without crowding the fader stack. Expanded uses 7/12 height
        // -> vuW = full strip width.
        const int heightDriver = compactVu ? stripW * 6 / 12 : stripW * 7 / 12;
        const int minH = compactVu ? 32 : 36;
        const int vuH  = juce::jmax (minH, heightDriver);
        const int vuW  = juce::jmin (stripW, vuH * kRatioW / kRatioH);
        auto slot = area.removeFromTop (vuH);
        vuMeter->setBounds (slot.withSizeKeepingCentre (vuW, vuH));
        area.removeFromTop (3);
    }

    // No plugin slots on buses - reserved space stays a thin gap so the
    // strip's vertical layout matches the channel strips' rhythm. Plugins
    // live on AUX return lanes (the AUX stage), not on bus subgroups.
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

    if (compactMode)
    {
        // Compact: collapse EQ + COMP to a single button each, stacked
        // vertically so the strip's vertical rhythm still reads but the
        // section knobs are off-screen. paint() suppresses the eqArea /
        // compArea tinted frame when these rects are empty.
        constexpr int kCompactBtnH = 20;
        auto eqBtnRow = area.removeFromTop (kCompactBtnH);
        eqCompactButton.setBounds (eqBtnRow.reduced (4, 0));
        eqArea = juce::Rectangle<int>();
        area.removeFromTop (3);
        auto compBtnRow = area.removeFromTop (kCompactBtnH);
        compCompactButton.setBounds (compBtnRow.reduced (4, 0));
        compArea = juce::Rectangle<int>();
        area.removeFromTop (3);
    }
    else
    {
    // EQ section. Pre-compute area so paint() can draw the green-tinted
    // background band around the knob block.
    {
        constexpr int kEqSectionH = 16 + 1 + 10 + kKnobBlockH;
        eqArea = area.removeFromTop (kEqSectionH);
        auto s = eqArea;
        if (eqHeaderBtn != nullptr) eqHeaderBtn->setBounds (s.removeFromTop (16).reduced (4, 0));
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

    // Comp section. Mirrors the channel strip:
    //   Header  : CompHeaderButton (full width, 16 px)
    //   Body    : CompMeterStrip on the LEFT (36 px), 2×2 knob grid on
    //             the RIGHT (RAT / MAK on top, ATK / REL on bottom).
    //             Threshold is set via the triangle handle on the meter
    //             — no dedicated THR knob.
    {
        constexpr int kCompKnobLabelH = 10;
        constexpr int kCompKnobRowH   = kCompKnobLabelH + kKnobBlockH;
        constexpr int kCompMeterW     = 36;
        constexpr int kCompMeterGap   = 4;

        // Single-row COMP body (matches channel-strip track-3 grammar):
        // 4 knobs (RAT / ATK / REL / MAK) across one row — no 2×2 grid.
        constexpr int kCompBodyH = kCompKnobRowH;
        constexpr int kCompSectionH = 16 + 2 + kCompBodyH + 2;
        compArea = area.removeFromTop (kCompSectionH);
        auto s = compArea.reduced (3, 2);

        auto headerRow = s.removeFromTop (16);
        if (compHeaderBtn != nullptr) compHeaderBtn->setBounds (headerRow);
        s.removeFromTop (2);

        auto body = s.removeFromTop (kCompBodyH);
        // compMeter no longer lives inside the COMP section — it's the
        // fader-side GR LED now (placed beside the level meter further
        // below). The 4-knob row uses the full body width.
        juce::ignoreUnused (kCompMeterW, kCompMeterGap);

        auto layoutCell = [&] (juce::Rectangle<int> cell,
                                 juce::Slider& knob, juce::Label& label)
        {
            label.setBounds (cell.removeFromTop (kCompKnobLabelH));
            knob.setBounds  (cell.getX(), cell.getY(), cell.getWidth(), kKnobBlockH);
        };

        const int colW = body.getWidth() / 4;
        layoutCell (body.removeFromLeft (colW), compRatio,   compRatLbl);
        layoutCell (body.removeFromLeft (colW), compAttack,  compAtkLbl);
        layoutCell (body.removeFromLeft (colW), compRelease, compRelLbl);
        layoutCell (body,                        compMakeup,  compMakLbl);
    }
    area.removeFromTop (3);
    } // end !compactMode EQ + COMP branch

    // Pan is positioned later (below) at the TOP of the fader column so it
    // sits directly above the slider thumb — matches ChannelStripComponent's
    // pan-on-fader-top grammar instead of floating in the middle of the strip.

    // Mute / solo at the bottom.
    auto buttons = area.removeFromBottom (24);
    muteButton.setBounds (buttons.removeFromLeft (buttons.getWidth() / 2).reduced (2));
    soloButton.setBounds (buttons.reduced (2));
    area.removeFromBottom (2);

    // Automation-mode row directly above M/S — thin full-width row, same
    // 16 px grammar as the channel strips.
    auto autoRow = area.removeFromBottom (16);
    autoModeButton.setBounds (autoRow.reduced (1, 0));
    area.removeFromBottom (4);

    // Output-peak readout row. Positioned under the meter cluster further
    // down (once meterArea is known) rather than full-width, so it sits under
    // the LED, not the fader. Just reserve the row height here.
    auto peakRow = area.removeFromBottom (14);
    area.removeFromBottom (2);

    // Right-side stack — fader-side grammar matching the channel strip:
    //   [right pad] [GR LED (compMeter)] [gap] [level meter] [gap] [fader]
    // Scale labels (+6 / +3 / 0 / ... / off) drawn by paint() to the LEFT
    // of the fader track (same kSharedXOver math as ChannelStripComponent).
    constexpr int kMeterW          = 14;   // 2 × ~6 px columns + 1 px gap (stereo LED)
    constexpr int kGrLedW          = 20;   // GR bar + threshold-drag handle
    constexpr int kMeterToGrGap    = 1;
    constexpr int kFaderToMeterGap = 1;
    constexpr int kRightPad        = 14;
    constexpr int kPanKnobSize = 26;
    constexpr int kPanValueH   = 12;
    constexpr int kPanBlockH   = kPanKnobSize + kPanValueH + 2;
    constexpr int kPanLabelH   = 11;
    constexpr int kPanBlockW   = 56;
    constexpr int kPanFaderGap = 4;        // small visible gap between pan "C" and cap top
    constexpr int kFaderValueH = 18;       // standalone value label below the slider

    auto panSlice = area.removeFromTop (kPanLabelH + kPanBlockH);

    area.removeFromRight (kRightPad);
    auto compMeterCol = area.removeFromRight (kGrLedW);
    area.removeFromRight (kMeterToGrGap);
    meterArea = area.removeFromRight (kMeterW);
    area.removeFromRight (kFaderToMeterGap);
    // Trim meter top so it lines up with the slider's +6 tick — same
    // grammar as channel strips (meter scale = fader scale 1:1, peaks
    // above +6 read as clip, never floating in unlabeled space).
    // Bottom trimmed by the same amount the slider is (kFaderValueH + 8
    // reserved for the standalone value label) so the meter's "off" /
    // -inf bottom lines up with the fader's "off" tick — without this
    // the meter overhangs the fader by 26 px and looks disconnected
    // from the strip's level scale.
    const int meterTopY = panSlice.getBottom() + kPanFaderGap
                        + (int) duskstudio::kFaderTrackPad;
    meterArea = meterArea.withTop (meterTopY)
                          .withTrimmedBottom (kFaderValueH + 8);
    // Centre the output-peak readout under the meter + GR-LED cluster (not the
    // full strip width, which read as sitting under the fader).
    outputPeakLabel.setBounds (meterArea.getX(), peakRow.getY(),
                                compMeterCol.getRight() - meterArea.getX(),
                                peakRow.getHeight());
    // Mirror padding so the fader's centre lands on the strip's centre.
    constexpr int kRightStackW = kRightPad + kGrLedW + kMeterToGrGap
                                  + kMeterW + kFaderToMeterGap;
    const int leftPad = juce::jlimit (0, juce::jmax (0, area.getWidth() - 20),
                                         kRightStackW);
    area.removeFromLeft (leftPad);
    // Right-bias the slider bounds inside the fader column so the cap
    // sits visually adjacent to the level meter — matches the
    // cap-to-LED distance the channel strip has by construction.
    // Without this narrowing, the cap floats in the centre of a wide
    // column with a large empty gap to the meter that reads as
    // "disconnected".
    constexpr int kFaderColW = 50;
    if (area.getWidth() > kFaderColW)
        area = area.removeFromRight (kFaderColW);
    // Scale labels no longer use a dedicated carved column — they're drawn
    // in paint() to the left of the fader track via kSharedXOver math.
    faderScaleArea = juce::Rectangle<int>();
    // grMeterArea was the old standalone GR bar; compMeter now owns the
    // GR display (placed beside the level meter below). Empty rect tells
    // paint() to skip drawing the legacy bar + caption.
    grMeterArea = juce::Rectangle<int>();

    // Pan knob/label centred on the FADER column X.
    const int faderCentreX = area.getCentreX();
    const int knobX        = faderCentreX - kPanBlockW / 2;
    const int labelBlockW  = juce::jmax (kPanBlockW, 40);
    panLbl .setBounds (faderCentreX - labelBlockW / 2,
                         panSlice.getY(),
                         labelBlockW, kPanLabelH);
    panKnob.setBounds (knobX, panSlice.getY() + kPanLabelH,
                         kPanBlockW, kPanBlockH);

    // Slider top sits kPanFaderGap below the pan-slice bottom (cap-at-max
    // top has small visible gap below the "C" textbox); bottom is trimmed
    // so the standalone value label fits cleanly under the cap-at-min.
    auto sliderBounds = area.withTrimmedTop (kPanFaderGap)
                            .withTrimmedBottom (kFaderValueH + 8);
    faderSlider.setBounds (sliderBounds);

    faderValueLabel.setBounds (sliderBounds.getX(),
                                 sliderBounds.getBottom() + 6,
                                 sliderBounds.getWidth(),
                                 kFaderValueH);

    // Position the fader-side compMeter so its GR bar top (= compTop +
    // 10 px caption reserve) lines up with the level meter's "0" dB tick.
    // Mirrors ChannelStripComponent's same-name math.
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

void BusComponent::openEqEditorPopup()
{
    if (eqEditorModal.isOpen()) { eqEditorModal.close(); return; }
    if (compEditorModal.isOpen()) compEditorModal.close();
    auto* host = getTopLevelComponent();
    if (host == nullptr) host = this;
    eqEditorModal.show (*host, std::make_unique<BusEqEditorPanel> (bus),
                        /*onDismiss*/ {}, /*dismissOnClickOutside*/ true,
                        /*dismissOnEscape*/ true, kEditorDimAlpha);
}

void BusComponent::openCompEditorPopup()
{
    if (compEditorModal.isOpen()) { compEditorModal.close(); return; }
    if (eqEditorModal.isOpen()) eqEditorModal.close();
    auto* host = getTopLevelComponent();
    if (host == nullptr) host = this;
    compEditorModal.show (*host, std::make_unique<BusCompEditorPanel> (bus),
                          /*onDismiss*/ {}, /*dismissOnClickOutside*/ true,
                          /*dismissOnEscape*/ true, kEditorDimAlpha);
}
} // namespace duskstudio
