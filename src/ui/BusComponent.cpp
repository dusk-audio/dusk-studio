#include "BusComponent.h"
#include "../engine/AudioEngine.h"
#include "../engine/CompModeMap.h"
#include "DuskContextMenu.h"
#include "DuskLabelEditor.h"
#include "DuskStudioLookAndFeel.h"  // fourKColors palette
#include "DimOverlay.h"
#include "DuskAlerts.h"
#if DUSKSTUDIO_HAS_NATIVE_UI
 #include "NativeEditorEmbedScale.h"
#endif
#include "../foundation/Text.h"
#include "../session/MidiBindings.h"
#include "../session/ParamEditAction.h"
#include <algorithm>
#include <cmath>
#include <string>

namespace duskstudio
{
namespace
{
// clamp with jlimit's argument order (lo, hi, value).
template <typename T>
inline T jlimit (T lo, T hi, T value) noexcept { return std::clamp (value, lo, std::max (lo, hi)); }

float busMeterFrac (float db) noexcept
{
    return faderSkewMeterFrac (db, ChannelStripParams::kFaderMinDb, ChannelStripParams::kFaderMaxDb);
}

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
    // Double-click -> reset to midPt (canonical "default" for skewed
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

// The bus highpass readout: OFF at the floor, then Hz, "1.2k" from 1 kHz.
std::string busHpfText (double hz)
{
    if (hz <= BusParams::kHpfOffHz + 0.5) return "OFF";
    if (hz >= 1000.0)
    {
        const double khz = hz / 1000.0;
        return std::abs (khz - std::round (khz)) < 0.05
                   ? dusk::text::format ("%dk", (int) std::round (khz))
                   : dusk::text::format ("%.1fk", khz);
    }
    return dusk::text::format ("%d", (int) std::round (hz));
}

// Reads what busHpfText writes, so a typed "1.2k" is 1200 Hz, not 1.2.
template <typename Text>
double busHpfFromText (const Text& raw)
{
    auto text = raw.trim().toLowerCase();
    if (text == "off") return BusParams::kHpfOffHz;
    if (text.endsWith ("hz")) text = text.dropLastCharacters (2).trimEnd();
    const bool isKhz = text.endsWithChar ('k');
    if (isKhz) text = text.dropLastCharacters (1);
    return text.getDoubleValue() * (isKhz ? 1000.0 : 1.0);
}

// Where the knob sits for the session's highpass: a disengaged one reads OFF
// whatever corner it last had.
double busHpfShownHz (const BusParams& strip)
{
    return strip.hpfEnabled.load (std::memory_order_relaxed)
               ? (double) strip.hpfFreq.load (std::memory_order_relaxed)
               : (double) BusParams::kHpfOffHz;
}

// Range, skew (245 Hz, the sweep's geometric middle, at twelve o'clock),
// double-click to OFF and the readout, for the strip knob and the editor's.
void setUpBusHpfKnob (juce::Slider& s, const BusParams& strip)
{
    s.setRange (BusParams::kHpfMinHz, BusParams::kHpfMaxHz, 1.0);
    s.setSkewFactorFromMidPoint (245.0);
    s.setDoubleClickReturnValue (true, BusParams::kHpfOffHz);
    s.textFromValueFunction = [] (double v) { return busHpfText (v); };
    s.valueFromTextFunction = [] (const auto& text) { return busHpfFromText (text); };
    s.setValue (busHpfShownHz (strip), juce::dontSendNotification);
    s.updateText();
}

// Dusk editor-modal styling helpers - match ChannelEqEditor (380-wide popups,
// 56-px rotary knobs, 80×18 text boxes, 16-pt bold accent band labels), so the
// bus and master EQ popups conform with the channel strip's.
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

// Centred caption: at editorTitle::kFontSize the strip name every editor popup
// carries on its top row.
void styleEditorAccentLabel (juce::Label& l, const juce::String& text,
                              float fontHeight = 11.0f,
                              std::uint32_t colour = 0xffb07050)
{
    l.setText (text, juce::dontSendNotification);
    l.setJustificationType (juce::Justification::centred);
    l.setColour (juce::Label::textColourId, juce::Colour (colour));
    l.setFont (juce::Font (juce::FontOptions (fontHeight, juce::Font::bold)));
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

        // Which bus this editor belongs to, centred left of the EQ pill.
        styleEditorAccentLabel (titleLbl, bus.name, editorTitle::kFontSize, editorTitle::kColour);
        titleLbl.setMinimumHorizontalScale (1.0f);
        addAndMakeVisible (titleLbl);

        styleEditorKnob (lf,  eqGreen, -9.0, 9.0, 0.0, 0.0, " dB", 1);
        // Tooltips set after the styleEditorKnob trio below so the
        // hints carry the same range / reset semantics as the inline.
        styleEditorKnob (mid, eqGreen, -9.0, 9.0, 0.0, 0.0, " dB", 1);
        styleEditorKnob (hf,  eqGreen, -9.0, 9.0, 0.0, 0.0, " dB", 1);
        lf .setValue (bus.strip.eqLfGainDb .load(), juce::dontSendNotification);
        mid.setValue (bus.strip.eqMidGainDb.load(), juce::dontSendNotification);
        hf .setValue (bus.strip.eqHfGainDb .load(), juce::dontSendNotification);

        auto armed = [this] { enableBtn.setToggleState (true, juce::dontSendNotification); };
        lf .onValueChange = [this, armed] { bus.strip.moveEqGain (bus.strip.eqLfGainDb,  (float) lf .getValue()); armed(); };
        mid.onValueChange = [this, armed] { bus.strip.moveEqGain (bus.strip.eqMidGainDb, (float) mid.getValue()); armed(); };
        hf .onValueChange = [this, armed] { bus.strip.moveEqGain (bus.strip.eqHfGainDb,  (float) hf .getValue()); armed(); };
        lf .setTooltip ("Bus EQ low shelf @ 300 Hz (-9..+9 dB). Double-click to reset; Shift-drag for fine.");
        mid.setTooltip ("Bus EQ mid bell @ 800 Hz, Q 0.7 (-9..+9 dB). Double-click to reset; Shift-drag for fine.");
        hf .setTooltip ("Bus EQ high shelf @ 2 kHz (-9..+9 dB). Double-click to reset; Shift-drag for fine.");
        addAndMakeVisible (lf); addAndMakeVisible (mid); addAndMakeVisible (hf);

        const auto filterWhite = juce::Colour (sslEqColors::kFilterWhite);
        styleEditorKnob (hpf, filterWhite, BusParams::kHpfMinHz, BusParams::kHpfMaxHz,
                         BusParams::kHpfOffHz, 245.0, "", 0);
        setUpBusHpfKnob (hpf, bus.strip);
        hpf.onValueChange = [this, armed] { if (bus.strip.moveHpf ((float) hpf.getValue())) armed(); };
        hpf.setTooltip ("Bus highpass, 12 dB/oct, 20 Hz..3 kHz; fully down is OFF. Double-click for OFF; Shift-drag for fine.");
        addAndMakeVisible (hpf);

        styleEditorLabel (hpfLbl, "HPF", filterWhite);
        styleEditorLabel (lfLbl,  "LF",  eqGreen);
        styleEditorLabel (midLbl, "MID", eqGreen);
        styleEditorLabel (hfLbl,  "HF",  eqGreen);
        addAndMakeVisible (hpfLbl);
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
        titleLbl.setBounds (header.withTrimmedLeft (60).withTrimmedRight (60));
        enableBtn.setBounds (header.removeFromRight (60));
        area.removeFromTop (kEditorHeaderGap);

        auto labelRow = area.removeFromTop (kEditorLabelRowH);
        auto knobRow  = area.removeFromTop (kEditorKnobBlockH);
        const int colW = knobRow.getWidth() / 4;
        hpfLbl.setBounds (labelRow.removeFromLeft (colW));
        lfLbl .setBounds (labelRow.removeFromLeft (colW));
        midLbl.setBounds (labelRow.removeFromLeft (colW));
        hfLbl .setBounds (labelRow);
        hpf.setBounds (knobRow.removeFromLeft (colW));
        lf .setBounds (knobRow.removeFromLeft (colW));
        mid.setBounds (knobRow.removeFromLeft (colW));
        hf .setBounds (knobRow);
    }

    // A bus rename lands while the popup is open, pushed in by the strip's poll.
    void refreshTitle()
    {
        if (titleLbl.getText (false) != bus.name)
            styleEditorAccentLabel (titleLbl, bus.name, editorTitle::kFontSize, editorTitle::kColour);
    }

private:
    Bus& bus;
    juce::TextButton enableBtn;
    juce::Label titleLbl;
    juce::Slider lf, mid, hf, hpf;
    juce::Label  lfLbl, midLbl, hfLbl, hpfLbl;
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
    disableLabelEditorPopup (nameLabel);
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
    // Bus VUs use the full numbered scale (like the master), not the compact
    // "-" / "+" endpoints. Warm-cream rich face matches the master so the
    // visual family reads as one unit.
    vuMeter->setRichStyle (true);
    addAndMakeVisible (*vuMeter);

    const auto eqGreen = juce::Colour (0xff80c090);
    const auto compGold = juce::Colour (0xff7da8c5);   // SSL G-bus comp powder blue (legacy var name)
    // Pan red - matches the channel-strip pan colour (0xffc04040) so the
    // pan-control language is consistent across channels and buses.
    const auto panRed   = juce::Colour (0xffc04040);

    // Split EQ header: status-light click toggles bypass, label click opens
    // the editor, and a right-click anywhere opens the section menu.
    eqHeaderBtn = std::make_unique<SplitModuleButton> ("EQ");
    eqHeaderBtn->setAccentColour (eqGreen);
    eqHeaderBtn->setCallbacks (
        [this] { return ! bus.strip.eqEnabled.load (std::memory_order_relaxed); },
        [this]
                        {
                            const bool now = ! bus.strip.eqEnabled.load (std::memory_order_relaxed);
                            bus.strip.eqEnabled.store (now, std::memory_order_release);
                            if (eqHeaderBtn != nullptr) eqHeaderBtn->refresh();
                        },
        [this] { openEqEditorPopup(); },
        [this] { showEqSectionMenu(); });
    eqHeaderBtn->setTooltip ("Click the status light to bypass/engage EQ; click EQ to open the editor; right-click for the EQ menu.");
    addAndMakeVisible (eqHeaderBtn.get());

    // Suffixes empty - same rationale as master: 38-px textbox can't fit
    // " dB"/" ms" without truncating, and the L/M/H labels already imply dB.
    styleSmallKnob (eqLfGain,  -9.0, 9.0, 0.0, bus.strip.eqLfGainDb.load(),  eqGreen, "", 1);
    styleSmallKnob (eqMidGain, -9.0, 9.0, 0.0, bus.strip.eqMidGainDb.load(), eqGreen, "", 1);
    styleSmallKnob (eqHfGain,  -9.0, 9.0, 0.0, bus.strip.eqHfGainDb.load(),  eqGreen, "", 1);
    eqLfGain .setTooltip ("Bus EQ low shelf @ 300 Hz (-9..+9 dB). Double-click to reset; Shift-drag for fine.");
    eqMidGain.setTooltip ("Bus EQ mid bell @ 800 Hz, Q 0.7 (-9..+9 dB). Double-click to reset; Shift-drag for fine.");
    eqHfGain .setTooltip ("Bus EQ high shelf @ 2 kHz (-9..+9 dB). Double-click to reset; Shift-drag for fine.");
    const auto busTitle = "Bus " + std::to_string (busIndex + 1);
    eqLfGain .setTitle (busTitle + " EQ low shelf gain");
    eqMidGain.setTitle (busTitle + " EQ mid bell gain");
    eqHfGain .setTitle (busTitle + " EQ high shelf gain");
    auto busEqArmed = [this] { if (eqHeaderBtn != nullptr) eqHeaderBtn->refresh(); };
    eqLfGain .onValueChange = [this, busEqArmed] { bus.strip.moveEqGain (bus.strip.eqLfGainDb,  (float) eqLfGain .getValue()); busEqArmed(); };
    eqMidGain.onValueChange = [this, busEqArmed] { bus.strip.moveEqGain (bus.strip.eqMidGainDb, (float) eqMidGain.getValue()); busEqArmed(); };
    eqHfGain .onValueChange = [this, busEqArmed] { bus.strip.moveEqGain (bus.strip.eqHfGainDb,  (float) eqHfGain .getValue()); busEqArmed(); };
    addAndMakeVisible (eqLfGain); addAndMakeVisible (eqMidGain); addAndMakeVisible (eqHfGain);

    // Highpass, white-faced like the channel strip's filters. Turning it up
    // off the floor arms the EQ, whose status light bypasses it with the bands.
    const auto filterWhite = juce::Colour (sslEqColors::kFilterWhite);
    styleSmallKnob (eqHpfFreq, BusParams::kHpfMinHz, BusParams::kHpfMaxHz, 245.0,
                    busHpfShownHz (bus.strip), filterWhite, "", 0);
    setUpBusHpfKnob (eqHpfFreq, bus.strip);
    eqHpfFreq.setTooltip ("Bus highpass, 12 dB/oct, 20 Hz..3 kHz; fully down is OFF. Double-click for OFF; Shift-drag for fine.");
    eqHpfFreq.setTitle (busTitle + " high-pass filter frequency");
    eqHpfFreq.setHelpText ("Bus high-pass filter corner, in hertz; OFF when fully down.");
    eqHpfFreq.onValueChange = [this, busEqArmed] { if (bus.strip.moveHpf ((float) eqHpfFreq.getValue())) busEqArmed(); };
    addAndMakeVisible (eqHpfFreq);

    styleSmallLabel (eqHpfLbl, "HPF", filterWhite);
    // L = low shelf, M = bell, H = high shelf - standard 3-band EQ labelling.
    styleSmallLabel (eqLfLbl,  "L", eqGreen);
    styleSmallLabel (eqMidLbl, "M", eqGreen);
    styleSmallLabel (eqHfLbl,  "H", eqGreen);
    addAndMakeVisible (eqHpfLbl);
    addAndMakeVisible (eqLfLbl); addAndMakeVisible (eqMidLbl); addAndMakeVisible (eqHfLbl);

    // Comp section. The split header mirrors the channel-strip COMP and the
    // processor remains a fixed SSL-style glue topology.
    compHeaderBtn = std::make_unique<SplitModuleButton> ("COMP");
    compHeaderBtn->setAccentColour (compGold);
    compHeaderBtn->setCallbacks (
        [this] { return ! bus.strip.compEnabled.load (std::memory_order_relaxed); },
        [this]
                        {
                            const bool now = ! bus.strip.compEnabled.load (std::memory_order_relaxed);
                            bus.strip.compEnabled.store (now, std::memory_order_release);
                            if (compHeaderBtn != nullptr) compHeaderBtn->refresh();
                        },
        [this] { openCompEditorPopup(); },
        [this] { showCompSectionMenu(); });
    compHeaderBtn->setTooltip ("Click the status light to bypass/engage compression; click COMP to open the editor; right-click for the COMP menu.");
    addAndMakeVisible (compHeaderBtn.get());

    CompMeterStrip::Source compSrc;
    compSrc.getInputDb     = [this]
                              {
                                  const float l = bus.strip.meterPostBusLDb.load (std::memory_order_relaxed);
                                  const float r = bus.strip.meterPostBusRDb.load (std::memory_order_relaxed);
                                  return std::max (l, r);
                              };
    compSrc.getGrDb        = [this] { return bus.strip.meterGrDb.load (std::memory_order_relaxed); };
    compSrc.getThresholdDb = [this] { return comp::thresholdAxisValue (bus.strip.comp, comp::kBusMode); };
    compSrc.setThresholdDb = [this] (float db)
                              {
                                  comp::applyThresholdAxisValue (bus.strip.comp, comp::kBusMode, db);
                              };
    compSrc.resetThreshold = [this]
                              {
                                  comp::resetRole (bus.strip.comp, comp::kBusMode, comp::Role::Threshold);
                              };
    compSrc.isEngaged      = [this] { return bus.strip.compEnabled.load (std::memory_order_relaxed); };
    compSrc.autoEnable     = [this]
                              {
                                  bus.strip.compEnabled.store (true, std::memory_order_release);
                                  if (compHeaderBtn != nullptr) compHeaderBtn->refresh();
                              };
    // Fader-side GR LED (matches channel-strip grammar): slim widget that
    // shows only the GR bar + threshold-drag handle, glued next to the
    // level meter to the right of the fader. The handle travels the bus
    // threshold's whole host range, -30 dB at the bottom to +15 dB at the
    // top, so its 0 dB default is not the level meter's 0 dB tick.
    const auto busThresholdAxis = comp::thresholdAxis (comp::kBusMode);
    compSrc.floorDb        = busThresholdAxis.lo;
    compSrc.ceilingDb      = busThresholdAxis.hi;
    compMeter = std::make_unique<CompMeterStrip> (std::move (compSrc));
    compMeter->setShowInputBar (false);
    compMeter->setHandleVisible (true);
    compMeter->setHandleOnRight (true);
    addAndMakeVisible (compMeter.get());

    // RAT / ATK / REL / MAK are Multi-Comp 2's Bus-mode controls: each travels
    // the plug-in's host range (ratio, attack and release by step), returns to
    // the plug-in's default on double-click and reads as the plug-in prints the
    // value. Moving one leaves the engage switch as it is.
    const auto bindCompKnob = [this, compGold] (auto& knob, CompParamId id, const std::string& tooltip)
    {
        const auto& spec = compParamSpec (id);
        styleSmallKnob (knob, spec.hostMin, spec.hostMax, spec.hostDefault, bus.strip.comp.get (id),
                        compGold, "", 0);
        knob.setRange (spec.hostMin, spec.hostMax, spec.integer ? 1.0 : 0.0);
        knob.setSkewFactor (1.0);
        knob.setDoubleClickReturnValue (true, spec.hostDefault);
        knob.textFromValueFunction = [eng = &engine, id] (double v) -> std::string
        {
            return eng->compValueText (CompStage::Bus, id, (float) v);
        };
        knob.valueFromTextFunction = [eng = &engine, &knob, id] (const auto& text)
        {
            float value = 0.0f;
            return eng->compValueFromText (CompStage::Bus, id, text.toStdString(), value)
                       ? (double) value : knob.getValue();
        };
        knob.onValueChange = [this, &knob, id] { bus.strip.comp.set (id, (float) knob.getValue()); };
        knob.setTooltip (tooltip);
        knob.updateText();
    };
    const auto steps = [this] (CompParamId id) { return engine.compStepsText (CompStage::Bus, id, " / "); };
    bindCompKnob (compRatio,   CompParamId::BusRatio,   "Bus comp ratio (stepped: " + steps (CompParamId::BusRatio) + ").");
    bindCompKnob (compAttack,  CompParamId::BusAttack,  "Bus comp attack (stepped: " + steps (CompParamId::BusAttack) + ").");
    bindCompKnob (compRelease, CompParamId::BusRelease, "Bus comp release (stepped: " + steps (CompParamId::BusRelease) + ").");
    bindCompKnob (compMakeup,  CompParamId::BusMakeup,
                  "Bus comp make-up gain (" + engine.compStepsText (CompStage::Bus, CompParamId::BusMakeup, " to ")
                      + "). Double-click for "
                      + engine.compValueText (CompStage::Bus, CompParamId::BusMakeup,
                                              compParamSpec (CompParamId::BusMakeup).hostDefault)
                      + "; Shift-drag for fine.");
    // White pointer on the powder-blue body - matches the SSL G-bus
    // comp's painted-line indicator. styleSmallKnob's default thumb
    // (fill-brightened) washes out against the light blue.
    for (auto* k : { &compRatio, &compAttack, &compRelease, &compMakeup })
        k->setColour (juce::Slider::thumbColourId, juce::Colours::white);

    addAndMakeVisible (compRatio);  addAndMakeVisible (compAttack);
    addAndMakeVisible (compRelease); addAndMakeVisible (compMakeup);
    styleSmallLabel (compRatLbl, "RAT", compGold);
    styleSmallLabel (compAtkLbl, "ATK", compGold);
    styleSmallLabel (compRelLbl, "REL", compGold);
    styleSmallLabel (compMakLbl, "MAK", compGold);
    addAndMakeVisible (compRatLbl); addAndMakeVisible (compAtkLbl);
    addAndMakeVisible (compRelLbl); addAndMakeVisible (compMakLbl);

    // Pan. Format matches channel-strip pan: "C" at centre,
    // "L<pct>" / "R<pct>" otherwise - no decimals, no raw -1..1 number.
    styleSmallKnob (panKnob, -1.0, 1.0, 0.0, bus.strip.pan.load(), panRed, "", 0);
    panKnob.setNumDecimalPlacesToDisplay (0);
    panKnob.textFromValueFunction = [] (double v) -> std::string
    {
        if (std::abs (v) < 0.01) return "C";
        const int pct = (int) std::round (std::abs (v) * 100.0);
        return (v < 0 ? "L" : "R") + std::to_string (pct);
    };
    panKnob.updateText();
    panKnob.setTooltip ("Bus pan (L100..C..R100). Double-click for centre; Shift-drag for fine.");
    panKnob.onValueChange = [this] { bus.strip.pan.store ((float) panKnob.getValue(), std::memory_order_relaxed); };
    panKnob.onDragStart   = [this] { bus.strip.panTouched.store (true,  std::memory_order_release); };
    panKnob.onDragEnd     = [this] { bus.strip.panTouched.store (false, std::memory_order_release); };
    addAndMakeVisible (panKnob);
    // Match the channel-strip PAN label (10.5 pt bold) instead of the
    // smaller 8.5 pt used by the bus's L/M/H knob captions - PAN sits
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
    // No textbox - the standalone faderValueLabel below shows the value at
    // the channel-strip's font weight + size (cap can fully reach min/max).
    faderSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    faderSlider.setTooltip ("Bus fader (-90..+12 dB). Double-click for 0 dB; Shift-drag for fine.");
    faderSlider.setVelocityBasedMode (false);
    faderSlider.setVelocityModeParameters (1.0, 1, 0.0, true);
    auto formatFaderDb = [] (double db) -> juce::String
    {
        return (db <= -89.95) ? juce::String (juce::CharPointer_UTF8 ("\xe2\x88\x9e"))
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
        // The timer records mute in WRITE too; recording the click as well
        // lands the change where it was made, not up to a tick later.
        if (engine.getTransport().isPlaying()
            && bus.strip.automationMode.load (std::memory_order_relaxed) == (int) AutomationMode::Write)
            recordAutomation (AutomationParam::Mute, true, newState ? 1.0f : 0.0f);
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

    // Automation-mode button - same grammar as the channel strips: borderless
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
    applyAutoMode (bus.strip.automationMode.load (std::memory_order_relaxed));

    // Mouse listeners so the strip's mouseDown sees right-clicks on each
    // child (e.eventComponent identifies which control was hit). Matches
    // the ChannelStripComponent pattern - the strip body handles all
    // right-click routing in one place rather than each control owning
    // its own MIDI Learn menu.
    faderSlider.addMouseListener (this, false);
    muteButton .addMouseListener (this, false);
    soloButton .addMouseListener (this, false);
    panKnob    .addMouseListener (this, false);
    for (auto* knob : { &eqHpfFreq, &eqLfGain, &eqMidGain, &eqHfGain })
        knob->addMouseListener (this, false);

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

    // Compact split buttons use the same two primary hitboxes as the expanded
    // headers, while retaining their section-specific accent colours.
    eqCompactButton.setAccentColour (juce::Colour (0xff5fc46f));
    compCompactButton.setAccentColour (juce::Colour (0xffe0c050));
    eqCompactButton.setCallbacks (
        [this] { return ! bus.strip.eqEnabled.load (std::memory_order_relaxed); },
        [this]
        {
            const bool now = ! bus.strip.eqEnabled.load (std::memory_order_relaxed);
            bus.strip.eqEnabled.store (now, std::memory_order_release);
        },
        [this] { openEqEditorPopup(); },
        [this] { showEqSectionMenu(); });
    compCompactButton.setCallbacks (
        [this] { return ! bus.strip.compEnabled.load (std::memory_order_relaxed); },
        [this]
        {
            const bool now = ! bus.strip.compEnabled.load (std::memory_order_relaxed);
            bus.strip.compEnabled.store (now, std::memory_order_release);
        },
        [this] { openCompEditorPopup(); },
        [this] { showCompSectionMenu(); });
    eqCompactButton  .setTooltip ("Click the light to bypass/engage EQ; click EQ to open the editor; right-click for the EQ menu.");
    compCompactButton.setTooltip ("Click the light to bypass/engage compression; click COMP to open the editor; right-click for the COMP menu.");
    addChildComponent (eqCompactButton);
    addChildComponent (compCompactButton);

    startTimerHz (30);
}

BusComponent::~BusComponent()
{
    // stopTimer() must run before any member destructor so the timer
    // thread can't fire timerCallback on freed atomics. dusk::Timer's
    // own destructor calls stopTimer too but that's the BASE-class
    // destructor - it runs AFTER derived-class members destruct.
    stopTimer();
    for (int p = 0; p < kNumAutomationParams; ++p)
        recordAutomation ((AutomationParam) p, false, 0.0f);
   #if DUSKSTUDIO_HAS_NATIVE_UI
    if (compEditor != nullptr)
        compEditor->shutdown();
    compEditorDim.reset();
   #endif
}

void BusComponent::recordAutomation (AutomationParam param, bool recording, float value)
{
    auto& recorder = passRecorders[(size_t) param];
    auto& lane = bus.strip.automationLanes[(size_t) param];
    if (recording)
    {
        const auto position = engine.getTransport().getAutomationPosition();
        const bool touch = bus.strip.automationMode.load (std::memory_order_relaxed) == (int) AutomationMode::Touch;
        recorder.record (lane, position.samples, value,
                         sessionRef.tempoBpm.load (std::memory_order_relaxed),
                         position.locateSerial,
                         touch ? (std::int64_t) (engine.getCurrentSampleRate()
                                                 * AutomationPassRecorder::kTouchReturnSeconds)
                               : 0);
    }
    else
    {
        recorder.finish (lane);
    }
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
    // A pass this mode change ends is spliced on the next timer tick.
    bus.strip.automationMode.store ((int) mode, std::memory_order_release);
    applyAutoMode ((int) mode);
}

void BusComponent::applyAutoMode (int mode)
{
    // READ locks the automated controls (fader / pan / mute). Solo isn't
    // automated, so it stays interactive in every mode.
    const bool interactive = mode != (int) AutomationMode::Read;
    faderSlider.setEnabled (interactive);
    panKnob    .setEnabled (interactive);
    muteButton .setEnabled (interactive);

    if (mode == appliedAutoMode) return;
    appliedAutoMode = mode;
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
    outputL.step (bus.strip.meterPostBusLDb.load (std::memory_order_relaxed));
    outputR.step (bus.strip.meterPostBusRDb.load (std::memory_order_relaxed));

    // An open editor is titled with the bus name, so the rename has to reach it.
    if (auto* eq = dynamic_cast<BusEqEditorPanel*> (eqEditorModal.getBody()))
        eq->refreshTitle();

    // The levels above step while the strip is hidden, so its first paint after
    // showing draws them as they are; showing it refreshes the rest at once.
    if (shownInLayout (*this))
    {
        syncShownView();
    }
    else
    {
        meterGateL.forget();
        meterGateR.forget();
    }

    // The automation mode's control lock and Write/Touch capture run whether or
    // not the strip shows: MIDI, a control surface or the lane can move a
    // hidden bus.
    const int  amode   = bus.strip.automationMode.load (std::memory_order_relaxed);
    applyAutoMode (amode);
    const bool isWrite  = amode == (int) AutomationMode::Write;
    const bool isTouch  = amode == (int) AutomationMode::Touch;
    const bool playing  = engine.getTransport().isPlaying();
    recordAutomation (AutomationParam::FaderDb,
                      playing && (isWrite || (isTouch && bus.strip.faderTouched.load (std::memory_order_relaxed))),
                      bus.strip.faderDb.load (std::memory_order_relaxed));
    recordAutomation (AutomationParam::Pan,
                      playing && (isWrite || (isTouch && bus.strip.panTouched.load (std::memory_order_relaxed))),
                      bus.strip.pan.load (std::memory_order_relaxed));
    recordAutomation (AutomationParam::Mute, playing && isWrite,
                      bus.strip.mute.load (std::memory_order_relaxed) ? 1.0f : 0.0f);
}

void BusComponent::visibilityChanged()
{
    refreshForShowState();
}

void BusComponent::refreshForShowState()
{
    if (shownInLayout (*this))
        syncShownView();
}

void BusComponent::syncShownView()
{
    if (vuMeter != nullptr) vuMeter->showStateChanged();
    if (compMeter != nullptr) compMeter->showStateChanged();

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
        const int eqOn   = bus.strip.eqEnabled  .load (std::memory_order_relaxed) ? 1 : 0;
        const int compOn = bus.strip.compEnabled.load (std::memory_order_relaxed) ? 1 : 0;
        if (eqOn != lastCompactEqOn)
        {
            lastCompactEqOn = eqOn;
            eqCompactButton.refresh();
        }
        if (compOn != lastCompactCompOn)
        {
            lastCompactCompOn = compOn;
            compCompactButton.refresh();
        }
    }

    const float maxHold = std::max (outputL.peak, outputR.peak);
    if (peakReadout.update (maxHold))
    {
        countGatedRepaint();
        outputPeakLabel.setText (peakReadout.text, juce::dontSendNotification);
    }
    if (paintedPeakColour.changed (maxHold >= -3.0f ? 2.0f : maxHold >= -12.0f ? 1.0f : 0.0f, 0.0f))
        outputPeakLabel.setColour (juce::Label::textColourId,
            maxHold >= -3.0f  ? juce::Colour (0xffff5050) :
            maxHold >= -12.0f ? juce::Colour (0xffe0c050) :
                                 juce::Colour (0xffd0d0d0));

    if (! meterArea.isEmpty())
    {
        const float span = (float) meterArea.getHeight() - 4.0f;
        const bool left  = meterGateL.changed (outputColumnDraw (outputL.displayed, busMeterFrac (outputL.displayed), span));
        const bool right = meterGateR.changed (outputColumnDraw (outputR.displayed, busMeterFrac (outputR.displayed), span));
        if (left || right)
        {
            countGatedRepaint();
            repaint (meterArea);
        }
    }

    // Motor-fader / motor-pan animation - same grammar as
    // ChannelStripComponent. We poll the live* atoms (the engine writes
    // them every block: Off mirrors the setpoint, so MIDI-bound moves still
    // sync here; Read/Touch-untouched carry the lane value). Gate the visual
    // update on the user NOT grabbing that control so we never fight a
    // gesture.
    const int amode = bus.strip.automationMode.load (std::memory_order_relaxed);
    {
        const float live    = bus.strip.liveFaderDb.load (std::memory_order_relaxed);
        const bool  touched = bus.strip.faderTouched.load (std::memory_order_relaxed);
        if (! touched && std::abs (live - displayedLiveFaderDb) > 0.05f)
        {
            displayedLiveFaderDb = live;
            faderSlider.setValue (live, juce::dontSendNotification);
            faderValueLabel.setText ((live <= -89.95f) ? juce::String (juce::CharPointer_UTF8 ("\xe2\x88\x9e"))
                                                        : juce::String (live, 1),
                                       juce::dontSendNotification);
        }
        else if (touched)
            displayedLiveFaderDb = live;
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
    syncEqKnobs();
    syncCompKnobs();

    // Refresh both split headers so external atom changes (session reload,
    // editor actions, MIDI bindings) are reflected without a click.
    if (eqHeaderBtn != nullptr) eqHeaderBtn->refresh();
    if (compHeaderBtn != nullptr) compHeaderBtn->refresh();
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
        if (e.eventComponent == &eqHpfFreq)
        {
            midilearn::showLearnMenu (eqHpfFreq, sessionRef,
                                        MidiBindingTarget::BusHpfFreq, busIndex);
            return;
        }
        int band = 0;
        for (auto* knob : { &eqLfGain, &eqMidGain, &eqHfGain })
        {
            if (e.eventComponent == knob)
            {
                midilearn::showLearnMenu (*knob, sessionRef, MidiBindingTarget::BusEqGain,
                                            packBusEqBand (busIndex, band));
                return;
            }
            ++band;
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
    const std::pair<const char*, std::uint32_t> presets[] = {
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
    std::vector<std::pair<juce::String, std::uint32_t>> presetCopy;
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
        // SSL G-bus comp chassis - near-black with a soft powder-blue
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
        // 73 % - they read as two different scales for the same signal.
        constexpr float kBarGap = 1.0f;

        auto drawColumn = [&] (juce::Rectangle<float> bar, const LevelBallistics& level)
        {
            g.setColour (juce::Colour (0xff0c0c0e));
            g.fillRoundedRectangle (bar, 1.5f);
            g.setColour (juce::Colour (0xff2a2a2e));
            g.drawRoundedRectangle (bar, 1.5f, 0.6f);

            // LED-style hard zones - match the channel-strip meter look.
            const juce::Colour kLedGreen  (0xff20d040);
            const juce::Colour kLedYellow (0xfff0e020);
            const juce::Colour kLedRed    (0xffff2020);
            auto fracForDb = [] (float db) { return busMeterFrac (db); };
            auto colourForZone = [&] (int zone) -> juce::Colour
            {
                return zone == 2 ? kLedRed : zone == 1 ? kLedYellow : kLedGreen;
            };

            const auto draw = outputColumnDraw (level.displayed, fracForDb (level.displayed),
                                                bar.getHeight() - 4.0f);
            if (draw.fill.shown)
            {
                const float fillH = draw.fill.px;
                const float x = bar.getX() + 1.5f;
                const float w = bar.getWidth() - 3.0f;
                const float yFillTop = bar.getBottom() - 2.0f - fillH;
                const float yFillBot = bar.getBottom() - 2.0f;
                const auto fillRect = juce::Rectangle<float> (x, yFillTop, w, fillH);

                const auto tipCol = colourForZone (draw.fillZone);
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
                fillBand (std::max (yFillTop, bar.getY()),
                            std::min (yRedTop, yFillBot),
                            kLedRed);
                fillBand (std::max (yFillTop, yRedTop),
                            std::min (yYellowTop, yFillBot),
                            kLedYellow);
                fillBand (std::max (yFillTop, yYellowTop),
                            yFillBot,
                            kLedGreen);
            }
        };

        const auto fullBar = meterArea.toFloat();
        const float colW   = (fullBar.getWidth() - kBarGap) * 0.5f;
        drawColumn (juce::Rectangle<float> (fullBar.getX(), fullBar.getY(),
                                              colW, fullBar.getHeight()),
                     outputL);
        drawColumn (juce::Rectangle<float> (fullBar.getX() + colW + kBarGap, fullBar.getY(),
                                              colW, fullBar.getHeight()),
                     outputR);
    }

    // Fader dB scale labels - drawn LEFT of the slider's track (track-3
    // grammar). Each label has a short tick line stub extending toward
    // the track; the label glyph sits a few px further left for breathing
    // room.
    {
        const auto& range = faderSlider.getNormalisableRange();
        const auto sliderB = faderSlider.getBounds().toFloat();
        const float trackCx = sliderB.getCentreX();
        const float trackW  = std::min (4.0f, sliderB.getWidth() * 0.18f);
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
            // ∞ renders visually smaller than the digits at a given point size,
            // so upsize it to match their height.
            const juce::Font font (juce::FontOptions (isBottom ? 16.0f : 10.5f, juce::Font::plain));
            g.setFont (font);
            const juce::String label = isBottom ? juce::String (juce::CharPointer_UTF8 ("\xe2\x88\x9e"))   /* ∞ = -inf dB / fully off */
                                                : juce::String (t.label);
            const float ascent  = font.getAscent();
            const float descent = font.getDescent();
            const float baselineY = y + (ascent - descent) * 0.5f - 2.0f;
            const float textW = juce::GlyphArrangement::getStringWidth (font, label);
            g.drawSingleLineText (label,
                                    (int) std::lround (labelRight - textW),
                                    (int) std::lround (baselineY),
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
    // setCompSectionVisible - no per-section toggle, the whole block
    // collapses behind the compact placeholder button.
    const bool sec = ! compact;
    if (eqHeaderBtn != nullptr) eqHeaderBtn->setVisible (sec);
    eqHpfFreq .setVisible (sec);  eqHpfLbl.setVisible (sec);
    eqLfGain  .setVisible (sec);  eqLfLbl .setVisible (sec);
    eqMidGain .setVisible (sec);  eqMidLbl.setVisible (sec);
    eqHfGain  .setVisible (sec);  eqHfLbl .setVisible (sec);
    if (compHeaderBtn != nullptr) compHeaderBtn->setVisible (sec);
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
        // Width matches the EQ/COMP pill row below (reduced(4,0)) so the meter
        // and the section pills line up. The dial inside is aspect-locked and
        // centres itself, so the box can take the full pill width without
        // distorting (and the dial grows to its height cap). Height still drives
        // the dial size: it shrinks when the TIMELINE tape strip is expanded
        // (the fader needs the room).
        const int stripW = area.getWidth();
        const int heightDriver = compactVu ? stripW * 6 / 12 : stripW * 7 / 12;
        const int minH = compactVu ? 32 : 36;
        const int vuH  = std::max (minH, heightDriver);
        const int vuW  = std::max (1, stripW - 8);
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
        const int leftPad = std::max (0, (labelRow.getWidth() - totalW) / 2);
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
        // Four blocks fill the narrowest bus strip exactly; below that they
        // share what there is.
        const int blockW = std::min (kKnobBlockW, s.getWidth() / 4);
        auto rows = layKnobRow (s, 4);
        auto& lblRow  = rows.first;
        auto& knobRow = rows.second;
        eqHpfLbl.setBounds (lblRow .removeFromLeft (blockW));
        eqLfLbl .setBounds (lblRow .removeFromLeft (blockW));
        eqMidLbl.setBounds (lblRow .removeFromLeft (blockW));
        eqHfLbl .setBounds (lblRow .removeFromLeft (blockW));
        eqHpfFreq.setBounds (knobRow.removeFromLeft (blockW));
        eqLfGain .setBounds (knobRow.removeFromLeft (blockW));
        eqMidGain.setBounds (knobRow.removeFromLeft (blockW));
        eqHfGain .setBounds (knobRow.removeFromLeft (blockW));
    }
    area.removeFromTop (3);

    {
        constexpr int kCompKnobLabelH = 10;
        constexpr int kCompKnobRowH   = kCompKnobLabelH + kKnobBlockH;

        // Single-row COMP body (matches channel-strip track-3 grammar):
        // 4 knobs (RAT / ATK / REL / MAK) across one row - no 2×2 grid.
        constexpr int kCompBodyH = kCompKnobRowH;
        constexpr int kCompSectionH = 16 + 2 + kCompBodyH + 2;
        compArea = area.removeFromTop (kCompSectionH);
        auto s = compArea.reduced (3, 2);

        auto headerRow = s.removeFromTop (16);
        if (compHeaderBtn != nullptr) compHeaderBtn->setBounds (headerRow);
        s.removeFromTop (2);

        auto body = s.removeFromTop (kCompBodyH);

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
    // sits directly above the slider thumb - matches ChannelStripComponent's
    // pan-on-fader-top grammar instead of floating in the middle of the strip.

    // Mute / solo at the bottom.
    auto buttons = area.removeFromBottom (24);
    muteButton.setBounds (buttons.removeFromLeft (buttons.getWidth() / 2).reduced (2));
    soloButton.setBounds (buttons.reduced (2));
    area.removeFromBottom (2);

    // Automation-mode row directly above M/S - thin full-width row, same
    // 16 px grammar as the channel strips.
    auto autoRow = area.removeFromBottom (16);
    autoModeButton.setBounds (autoRow.reduced (1, 0));
    area.removeFromBottom (4);

    // Output-peak readout row. Positioned under the meter cluster further
    // down (once meterArea is known) rather than full-width, so it sits under
    // the LED, not the fader. Just reserve the row height here.
    auto peakRow = area.removeFromBottom (14);
    area.removeFromBottom (2);

    // Right-side stack - fader-side grammar matching the channel strip:
    //   [right pad] [GR LED (compMeter)] [gap] [level meter] [gap] [fader]
    // Scale labels (+6 / +3 / 0 / ... / off) are drawn by paint() to the LEFT
    // of the fader track.
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
    // Trim meter top so it lines up with the slider's +6 tick - same
    // grammar as channel strips (meter scale = fader scale 1:1, peaks
    // above +6 read as clip, never floating in unlabeled space).
    // Bottom trimmed by the same amount the slider is (kFaderValueH + 8
    // reserved for the standalone value label) so the meter's "off" /
    // -inf bottom lines up with the fader's "off" tick - without this
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
    const int leftPad = jlimit (0, std::max (0, area.getWidth() - 20),
                                         kRightStackW);
    area.removeFromLeft (leftPad);
    // Right-bias the slider bounds inside the fader column so the cap
    // sits visually adjacent to the level meter - matches the
    // cap-to-LED distance the channel strip has by construction.
    // Without this narrowing, the cap floats in the centre of a wide
    // column with a large empty gap to the meter that reads as
    // "disconnected".
    constexpr int kFaderColW = 50;
    if (area.getWidth() > kFaderColW)
        area = area.removeFromRight (kFaderColW);

    // Pan knob/label centred on the FADER column X.
    const int faderCentreX = area.getCentreX();
    const int knobX        = faderCentreX - kPanBlockW / 2;
    const int labelBlockW  = std::max (kPanBlockW, 40);
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

    // Share the bottom readout row with the output peak value, as channel
    // strips do. Keeping the fader value here avoids the bus-group "0.0"
    // floating one row above every other strip's readout.
    faderValueLabel.setBounds (sliderBounds.getX(),
                                 peakRow.getY(),
                                 sliderBounds.getWidth(),
                                 peakRow.getHeight());

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
                        - (int) std::lround (zeroFrac * (float) (meterArea.getHeight() - 2));
        constexpr int kGrCaptionReserve = 10;
        const int compTop = zeroY - kGrCaptionReserve;
        compMeter->setBounds (compMeterCol.withY (compTop)
                                          .withHeight (meterArea.getBottom() - compTop));
    }
}

void BusComponent::openEqEditorPopup()
{
    if (eqEditorModal.isOpen()) { eqEditorModal.close(); return; }
    closeCompEditorPopup();
    auto* host = getTopLevelComponent();
    if (host == nullptr) host = this;
    eqEditorModal.show (*host, std::make_unique<BusEqEditorPanel> (bus),
                        /*onDismiss*/ {}, /*dismissOnClickOutside*/ true,
                        /*dismissOnEscape*/ true, kEditorDimAlpha);
}

#if DUSKSTUDIO_HAS_NATIVE_UI
imgui::DafEditorHost::Geometry BusComponent::compEditorGeometry()
{
    auto* const topLevel = getTopLevelComponent();
    if (topLevel == nullptr || compEditor == nullptr)
        return {};
    const auto fitted = embedscale::fittedChildGeometry (*topLevel, (int) compEditor->designWidth(),
                                                         (int) compEditor->designHeight());
    if (compEditorDim != nullptr)
    {
        compEditorDim->setBounds (topLevel->getLocalBounds());
        compEditorDim->setNativeChildArea (fitted.logical.expanded (1));
    }
    const auto& g = fitted.geometry;
    return { g.x, g.y, g.width, g.height, g.scale };
}

void BusComponent::finishCompEditorClose()
{
    compEditorDim.reset();
    compEditorHider.restore();
    if (auto* target = EmbeddedModal::focusRestoreTarget().getComponent())
        target->grabKeyboardFocus();
}
#endif

void BusComponent::openCompEditorPopup()
{
   #if ! DUSKSTUDIO_HAS_NATIVE_UI
    showDuskAlert (*this, "Compressor",
                   "The compressor editor needs the native UI, which this build was made without.");
   #else
    if (isCompEditorOpen())
    {
        closeCompEditorPopup();
        return;
    }
    if (eqEditorModal.isOpen()) eqEditorModal.close();

    auto* topLevel = getTopLevelComponent();
    if (topLevel == nullptr) topLevel = this;
    const auto parentHandle = embedscale::nativeParentHandle (*topLevel);
    if (parentHandle == 0)
    {
        showDuskAlert (*topLevel, "Compressor",
                       "The compressor editor cannot open: the main window is not ready.");
        return;
    }

    if (auto hook = EmbeddedModal::beforeModalShown())
        hook();

    if (compEditor == nullptr)
    {
        // The bus face engages the compressor from its threshold handle only;
        // its knobs leave the switch alone, and so do the editor's.
        imgui::CompEditorTarget binding;
        binding.comp = &bus.strip.comp;
        binding.engaged = &bus.strip.compEnabled;
        binding.engagedOnlyBy = comp::roleParam (comp::kBusMode, comp::Role::Threshold).id;
        compEditor = std::make_unique<imgui::CompEditorHost> (
            [this]() -> MultiCompStage& { return engine.getBusStrip (busIndex).getCompressor(); },
            binding);

        // Raw `this`: the strip owns the host and shuts it down before its own
        // teardown, so none of these outlives the strip.
        imgui::DafEditorHost::Callbacks callbacks;
        callbacks.closed = [this] { finishCompEditorClose(); };
        callbacks.gestureEnded = []
        {
            if (auto* target = EmbeddedModal::focusRestoreTarget().getComponent())
                target->grabKeyboardFocus();
        };
        callbacks.geometry = [this] { return compEditorGeometry(); };
        compEditor->setCallbacks (std::move (callbacks));
    }

    compEditorDim = std::make_unique<DimOverlay> (kEditorDimAlpha);
    compEditorDim->setBounds (topLevel->getLocalBounds());
    compEditorDim->onClick = [this] { closeCompEditorPopup(); };
    topLevel->addAndMakeVisible (compEditorDim.get());
    compEditorHider.hideUnder (*topLevel, { compEditorDim.get() });

    const auto geometry = compEditorGeometry();
    if (geometry.width >= 2 && geometry.height >= 2 && compEditor->open (parentHandle, geometry))
        return;

    compEditorDim.reset();
    compEditorHider.restore();
    const auto& why = compEditor->lastOpenFailure();
    showDuskAlert (*topLevel, "Compressor",
                   why.empty() ? "The compressor editor cannot open on this display backend."
                               : why.c_str());
   #endif
}

void BusComponent::closeCompEditorPopup()
{
   #if DUSKSTUDIO_HAS_NATIVE_UI
    if (compEditor != nullptr && compEditor->isOpen())
        compEditor->close();
   #endif
}

bool BusComponent::isCompEditorOpen() const noexcept
{
   #if DUSKSTUDIO_HAS_NATIVE_UI
    return compEditor != nullptr && compEditor->isOpen();
   #else
    return false;
   #endif
}

void BusComponent::showEqSectionMenu()
{
    juce::PopupMenu menu;
    menu.addItem (10, "Reset EQ");
    menu.addSeparator();
    menu.addItem (11, "Open editor...");
    juce::Component& anchor = compactMode ? (juce::Component&) eqCompactButton
                                          : (juce::Component&) *eqHeaderBtn;
    showContextMenu (menu, anchor,
        [safeThis = juce::Component::SafePointer<BusComponent> (this)] (int chosen)
        {
            auto* self = safeThis.getComponent();
            if (self == nullptr) return;
            if (chosen == 10) self->resetEqSection();
            else if (chosen == 11) self->openEqEditorPopup();
        });
}

void BusComponent::showCompSectionMenu()
{
    juce::PopupMenu menu;
    menu.addItem (10, "Reset comp");
    menu.addSeparator();
    menu.addItem (11, "Open editor...");
    juce::Component& anchor = compactMode ? (juce::Component&) compCompactButton
                                          : (juce::Component&) *compHeaderBtn;
    showContextMenu (menu, anchor,
        [safeThis = juce::Component::SafePointer<BusComponent> (this)] (int chosen)
        {
            auto* self = safeThis.getComponent();
            if (self == nullptr) return;
            if (chosen == 10) self->resetCompSection();
            else if (chosen == 11) self->openCompEditorPopup();
        });
}

// The EQ editor, MIDI bindings and a reset all write the atoms directly, so
// the strip knobs follow them here. A knob held by the mouse is left alone so
// a drag never snaps back mid-gesture.
void BusComponent::syncEqKnobs()
{
    auto sync = [] (auto& knob, double target)
    {
        if (knob.isMouseButtonDown() || std::abs (knob.getValue() - target) < 1.0e-4) return;
        knob.setValue (target, juce::dontSendNotification);
    };
    sync (eqHpfFreq, busHpfShownHz (bus.strip));
    sync (eqLfGain,  bus.strip.eqLfGainDb .load (std::memory_order_relaxed));
    sync (eqMidGain, bus.strip.eqMidGainDb.load (std::memory_order_relaxed));
    sync (eqHfGain,  bus.strip.eqHfGainDb .load (std::memory_order_relaxed));
}

// Flattens the bands and turns the highpass OFF, leaving the EQ engaged or
// bypassed as it was.
void BusComponent::resetEqSection()
{
    auto& strip = bus.strip;
    strip.hpfEnabled .store (false, std::memory_order_relaxed);
    strip.hpfFreq    .store (BusParams::kHpfOffHz, std::memory_order_relaxed);
    strip.eqLfGainDb .store (0.0f, std::memory_order_relaxed);
    strip.eqMidGainDb.store (0.0f, std::memory_order_relaxed);
    strip.eqHfGainDb .store (0.0f, std::memory_order_relaxed);
    syncEqKnobs();
    if (eqHeaderBtn != nullptr) eqHeaderBtn->refresh();
}

// The bus compressor editor, the strip's own threshold handle and a reset all
// write the CompParams directly, so the knobs follow them here, a knob held by
// the mouse left alone.
void BusComponent::syncCompKnobs()
{
    const auto& c = bus.strip.comp;
    auto sync = [&c] (auto& knob, CompParamId id)
    {
        const double target = c.get (id);
        if (knob.isMouseButtonDown() || std::abs (knob.getValue() - target) < 1.0e-4) return;
        knob.setValue (target, juce::dontSendNotification);
    };
    sync (compRatio,   CompParamId::BusRatio);
    sync (compAttack,  CompParamId::BusAttack);
    sync (compRelease, CompParamId::BusRelease);
    sync (compMakeup,  CompParamId::BusMakeup);
}

// Every Bus-mode control, the threshold handle's included, back to the
// plug-in's default, the compressor engaged or bypassed as it was.
void BusComponent::resetCompSection()
{
    bus.strip.comp.resetToDefaults (CompStage::Bus);
    syncCompKnobs();
    if (compMeter != nullptr) compMeter->repaint();
    if (compHeaderBtn != nullptr) compHeaderBtn->refresh();
}
} // namespace duskstudio
