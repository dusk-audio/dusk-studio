#include "ChannelEqEditor.h"
#include "DuskStudioLookAndFeel.h"
#include "../foundation/Text.h"

#include <algorithm>

namespace duskstudio
{
namespace
{
struct BandSpec
{
    const char* name;
    juce::Colour accent;
    float freqMin, freqMax;
    std::atomic<float>* (*gain) (ChannelStripParams&);
    ChannelStripParams::EqFreq freq;
    // q is non-null only for bell bands (HM, LM). Shelves return nullptr.
    std::atomic<float>* (*q)    (ChannelStripParams&);
};

const std::array<BandSpec, 4>& bandSpecs()
{
    static const std::array<BandSpec, 4> specs {{
        { "HF", juce::Colour (sslEqColors::kHfRed),    ChannelStripParams::kHfFreqMin, ChannelStripParams::kHfFreqMax,
            [] (ChannelStripParams& s) -> std::atomic<float>* { return &s.hfGainDb; },
            ChannelStripParams::EqFreq::Hf,
            [] (ChannelStripParams&)   -> std::atomic<float>* { return nullptr; } },
        { "HM", juce::Colour (sslEqColors::kHmGreen),  ChannelStripParams::kHmFreqMin, ChannelStripParams::kHmFreqMax,
            [] (ChannelStripParams& s) -> std::atomic<float>* { return &s.hmGainDb; },
            ChannelStripParams::EqFreq::Hm,
            [] (ChannelStripParams& s) -> std::atomic<float>* { return &s.hmQ; } },
        { "LM", juce::Colour (sslEqColors::kLmBlue),   ChannelStripParams::kLmFreqMin, ChannelStripParams::kLmFreqMax,
            [] (ChannelStripParams& s) -> std::atomic<float>* { return &s.lmGainDb; },
            ChannelStripParams::EqFreq::Lm,
            [] (ChannelStripParams& s) -> std::atomic<float>* { return &s.lmQ; } },
        { "LF", juce::Colour (sslEqColors::kLfBlack),  ChannelStripParams::kLfFreqMin, ChannelStripParams::kLfFreqMax,
            [] (ChannelStripParams& s) -> std::atomic<float>* { return &s.lfGainDb; },
            ChannelStripParams::EqFreq::Lf,
            [] (ChannelStripParams&)   -> std::atomic<float>* { return nullptr; } },
    }};
    return specs;
}

inline std::string formatFrequency (double hz)
{
    if (hz >= 1000.0) return dusk::text::format ("%.1f kHz", hz / 1000.0);
    return dusk::text::format ("%d Hz", (int) std::round (hz));
}

// What a frequency knob reads: a converted older session can hold a band or
// filter past the knob's range, where it keeps playing while the knob rests on
// its end stop.
inline double shownFrequency (double knob, float held, double lo, double hi)
{
    return (knob <= lo && held < lo) || (knob >= hi && held > hi) ? (double) held : knob;
}
} // namespace

ChannelEqEditor::ChannelEqEditor (Track& t) : track (t)
{
    typeButton.setClickingTogglesState (true);
    typeButton.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff5a3a20));
    typeButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff202020));
    typeButton.setColour (juce::TextButton::textColourOffId,  juce::Colours::white);
    typeButton.setColour (juce::TextButton::textColourOnId,   juce::Colours::white);
    typeButton.setTooltip ("Brown (E-series) / Black (G-series)");
    typeButton.setToggleState (track.strip.eqBlackMode.load (std::memory_order_relaxed),
                                juce::dontSendNotification);
    typeButton.onClick = [this]
    {
        track.strip.eqBlackMode.store (typeButton.getToggleState(), std::memory_order_relaxed);
        refreshTypeButton();
    };
    refreshTypeButton();
    addAndMakeVisible (typeButton);

    // EQ section ON/OFF toggle - pill with green LED-style fill when
    // engaged. Mirrors track.strip.eqEnabled so it stays in sync with
    // the strip's header pill + the band-knob auto-arm path.
    enableButton.setClickingTogglesState (true);
    enableButton.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff202024));
    enableButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff20603a));   // muted green
    enableButton.setColour (juce::TextButton::textColourOffId,  juce::Colour (0xffb0b0b8));
    enableButton.setColour (juce::TextButton::textColourOnId,   juce::Colours::white);
    enableButton.setTooltip ("EQ section on / off (bypasses HPF + LPF + all 4 bands).");
    enableButton.setToggleState (track.strip.eqEnabled.load (std::memory_order_relaxed),
                                   juce::dontSendNotification);
    enableButton.onClick = [this]
    {
        track.strip.eqEnabled.store (enableButton.getToggleState(),
                                       std::memory_order_release);
    };
    addAndMakeVisible (enableButton);

    auto setupLabel = [this] (juce::Label& label, const juce::String& text,
                               juce::Colour colour, float fontHeight)
    {
        label.setText (text, juce::dontSendNotification);
        label.setJustificationType (juce::Justification::centred);
        label.setColour (juce::Label::textColourId, colour);
        label.setFont (juce::Font (juce::FontOptions (fontHeight, juce::Font::bold)));
        addAndMakeVisible (label);
    };

    // Which strip this editor belongs to, centred between the EQ pill and the
    // E/G toggle. Fitted text shortens a long name rather than widening the
    // popup, and the strip pushes renames in through refreshTitle().
    setupLabel (titleLabel, track.name, juce::Colour (editorTitle::kColour),
                 editorTitle::kFontSize);
    titleLabel.setMinimumHorizontalScale (1.0f);

    const auto columnGrey = juce::Colour (0xff969aa2);
    setupLabel (gainColumnLabel, "GAIN", columnGrey, 12.0f);
    setupLabel (freqColumnLabel, "FREQ", columnGrey, 12.0f);
    setupLabel (qColumnLabel,    "Q",    columnGrey, 12.0f);

    // HPF + LPF - SSL 9000 J white-filter top section. Both knobs share
    // the white accent so they read as a filter pair (matches the
    // inline strip's filter row).
    const auto filterWhite = juce::Colour (sslEqColors::kFilterWhite);
    setupLabel (hpfLabel, "HPF", filterWhite, 16.0f);

    // OFF follows the switch, not the knob: a converted older session can
    // leave a filter on at its OFF end, where it still plays.
    auto setupFilterKnob = [this] (juce::Slider& k, juce::Colour fill,
                                      double minHz, double maxHz, double offHz,
                                      double skewMid, const std::atomic<bool>& enabled,
                                      const std::atomic<float>& held)
    {
        k.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        k.setColour (juce::Slider::rotarySliderFillColourId, fill);
        k.setColour (juce::Slider::rotarySliderOutlineColourId, juce::Colour (0xff404048));
        k.setRange (minHz, maxHz, 1.0);
        k.setSkewFactorFromMidPoint (skewMid);
        k.setDoubleClickReturnValue (true, offHz);
        k.setTextBoxStyle (juce::Slider::TextBoxBelow, true, 80, 18);
        k.setColour (juce::Slider::textBoxTextColourId, juce::Colour (0xffe0e0e0));
        k.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colour (0));
        k.setColour (juce::Slider::textBoxOutlineColourId, juce::Colour (0));
        k.textFromValueFunction = [minHz, maxHz, &enabled, &held] (double v)
        {
            if (! enabled.load (std::memory_order_relaxed)) return std::string ("off");
            return formatFrequency (shownFrequency (v, held.load (std::memory_order_relaxed), minHz, maxHz));
        };
    };
    setupFilterKnob (hpfKnob, filterWhite,
                      ChannelStripParams::kHpfMinHz, ChannelStripParams::kHpfMaxHz,
                      ChannelStripParams::kHpfOffHz, 80.0, track.strip.hpfEnabled, track.strip.hpfFreq);
    hpfKnob.setTitle ("EQ editor high-pass filter frequency");
    hpfKnob.setValue (track.strip.hpfFreq.load (std::memory_order_relaxed),
                       juce::dontSendNotification);
    hpfKnob.updateText();
    hpfKnob.onValueChange = [this]
    {
        const bool engaged = track.strip.moveEqFreq (ChannelStripParams::EqFreq::Hpf,
                                                     (float) hpfKnob.getValue());
        hpfKnob.updateText();
        if (engaged) enableButton.setToggleState (true, juce::dontSendNotification);
    };
    addAndMakeVisible (hpfKnob);

    // LPF - symmetric counterpart on the right side of the filter row.
    setupLabel (lpfLabel, "LPF", filterWhite, 16.0f);

    setupFilterKnob (lpfKnob, filterWhite,
                      ChannelStripParams::kLpfMinHz, ChannelStripParams::kLpfMaxHz,
                      ChannelStripParams::kLpfOffHz, 8000.0, track.strip.lpfEnabled, track.strip.lpfFreq);
    lpfKnob.setTitle ("EQ editor low-pass filter frequency");
    lpfKnob.setValue (track.strip.lpfFreq.load (std::memory_order_relaxed),
                       juce::dontSendNotification);
    lpfKnob.updateText();
    lpfKnob.onValueChange = [this]
    {
        const bool engaged = track.strip.moveEqFreq (ChannelStripParams::EqFreq::Lpf,
                                                     (float) lpfKnob.getValue());
        lpfKnob.updateText();
        if (engaged) enableButton.setToggleState (true, juce::dontSendNotification);
    };
    addAndMakeVisible (lpfKnob);

    for (size_t i = 0; i < bandSpecs().size(); ++i)
    {
        const auto& spec = bandSpecs()[i];
        auto& row = rows[i];

        setupLabel (row.nameLabel, spec.name, spec.accent.brighter (0.2f), 16.0f);

        auto makeKnob = [] (juce::Slider& k, juce::Colour fill, double mn, double mx,
                             double defaultVal, double skewMid,
                             double interval = 0.0)
        {
            k.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
            k.setColour (juce::Slider::rotarySliderFillColourId, fill);
            k.setColour (juce::Slider::rotarySliderOutlineColourId, juce::Colour (0xff404048));
            const double step = (interval > 0.0) ? interval : (mn < 0 ? 0.1 : 1.0);
            k.setRange (mn, mx, step);
            if (skewMid > 0) k.setSkewFactorFromMidPoint (skewMid);
            k.setDoubleClickReturnValue (true, defaultVal);
            k.setTextBoxStyle (juce::Slider::TextBoxBelow, true, 80, 18);
            k.setColour (juce::Slider::textBoxTextColourId,       juce::Colour (0xffe0e0e0));
            k.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colour (0));
            k.setColour (juce::Slider::textBoxOutlineColourId,    juce::Colour (0));
        };

        row.gain = std::make_unique<juce::Slider>();
        makeKnob (*row.gain, spec.accent,
                  ChannelStripParams::kBandGainMin, ChannelStripParams::kBandGainMax, 0.0, 0.0);
        row.gain->setNumDecimalPlacesToDisplay (1);
        row.gain->setTextValueSuffix (" dB");
        row.gain->setTitle ("EQ editor " + std::string (spec.name) + " gain");
        row.gain->setValue (spec.gain (track.strip)->load (std::memory_order_relaxed),
                             juce::dontSendNotification);
        {
            auto* strip = &track.strip;
            auto* atomicPtr = spec.gain (track.strip);
            auto* knob = row.gain.get();
            knob->onValueChange = [knob, strip, atomicPtr]
            {
                strip->moveEqBand (*atomicPtr, (float) knob->getValue());
            };
        }
        addAndMakeVisible (row.gain.get());

        const float defaultFreq = (i == 0 ? 8000.0f : i == 1 ? 2000.0f : i == 2 ? 600.0f : 100.0f);
        row.freq = std::make_unique<juce::Slider>();
        makeKnob (*row.freq, spec.accent, spec.freqMin, spec.freqMax,
                   defaultFreq, defaultFreq);
        row.freq->setNumDecimalPlacesToDisplay (0);
        row.freq->textFromValueFunction = [held = &track.strip.eqFreq (spec.freq), lo = spec.freqMin, hi = spec.freqMax] (double v)
        {
            return formatFrequency (shownFrequency (v, held->load (std::memory_order_relaxed), lo, hi));
        };
        row.freq->setTitle ("EQ editor " + std::string (spec.name) + " frequency");
        row.freq->setValue (track.strip.eqFreq (spec.freq).load (std::memory_order_relaxed),
                             juce::dontSendNotification);
        // setValue skips updateText() when the stored value clamps to where
        // the knob already sits, and the text was built before the formatter.
        row.freq->updateText();
        {
            auto* strip = &track.strip;
            auto* knob = row.freq.get();
            knob->onValueChange = [knob, strip, band = spec.freq]
            {
                strip->moveEqFreq (band, (float) knob->getValue());
            };
        }
        addAndMakeVisible (row.freq.get());

        // Q knob - only on bell bands (HM, LM). Shelves don't have one;
        // resized() leaves the Q row label blank for shelves and the freq
        // knob is centred vertically in the bell row instead.
        if (auto* qAtomGetter = spec.q)
        {
            if (auto* qAtom = qAtomGetter (track.strip))
            {
                row.q = std::make_unique<juce::Slider>();
                makeKnob (*row.q, spec.accent,
                          ChannelStripParams::kBandQMin, ChannelStripParams::kBandQMax,
                          0.7, 0.0, 0.01);
                row.q->setNumDecimalPlacesToDisplay (2);
                row.q->setTitle ("EQ editor " + std::string (spec.name) + " Q");
                row.q->setValue (qAtom->load (std::memory_order_relaxed),
                                  juce::dontSendNotification);
                auto* knob = row.q.get();
                auto* strip = &track.strip;
                knob->onValueChange = [knob, strip, qAtom]
                {
                    strip->moveEqBand (*qAtom, (float) knob->getValue());
                };
                addAndMakeVisible (row.q.get());

            }
        }
    }

    setSize (380, 500);
}

ChannelEqEditor::~ChannelEqEditor() = default;

void ChannelEqEditor::refreshTitle()
{
    if (titleLabel.getText (false) != track.name)
        titleLabel.setText (track.name, juce::dontSendNotification);
}

void ChannelEqEditor::refreshFilters()
{
    const auto sync = [] (juce::Slider& k, float hz)
    {
        if (! k.isMouseButtonDown()) k.setValue (hz, juce::dontSendNotification);
        k.updateText();
    };
    sync (hpfKnob, track.strip.hpfFreq.load (std::memory_order_relaxed));
    sync (lpfKnob, track.strip.lpfFreq.load (std::memory_order_relaxed));
}

std::string ChannelEqEditor::titleForScenario() const
{
    return titleLabel.getText (false).toStdString();
}

void ChannelEqEditor::refreshTypeButton()
{
    typeButton.setButtonText (typeButton.getToggleState() ? "G" : "E");
}

void ChannelEqEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff181820));
    g.setColour (juce::Colour (0xff2a2a30));
    g.drawRect (getLocalBounds(), 1);
}

void ChannelEqEditor::resized()
{
    auto area = getLocalBounds().reduced (12);

    // Header: EQ enable pill on the LEFT, E/G type toggle on the RIGHT,
    // the strip name centred between them.
    auto header = area.removeFromTop (24);
    // The title centres on the popup, not on the gap between the two pills,
    // so it is inset by the wider pill on both sides.
    titleLabel  .setBounds (header.withTrimmedLeft (60).withTrimmedRight (60));
    enableButton.setBounds (header.removeFromLeft (60));
    typeButton  .setBounds (header.removeFromRight (40));
    area.removeFromTop (8);

    // Match the inline strip's control order at a larger editing scale:
    // filter pair first, then one GAIN / FREQ / Q row per band.
    constexpr int kRowLabelW    = 36;
    constexpr int kKnobSize     = 56;
    constexpr int kValueH       = 18;
    constexpr int kKnobBlockH   = kKnobSize + kValueH + 6;
    constexpr int kHpfRowH      = kKnobBlockH;
    constexpr int kColumnHeaderH = 16;
    constexpr int kRowGap       = 4;

    // Filter row - HPF | LPF side-by-side, white-faced SSL 9000 J top.
    {
        auto row = area.removeFromTop (kHpfRowH);
        const int colW = row.getWidth() / 2;
        auto hpfCell = row.removeFromLeft (colW);
        auto lpfCell = row;

        // Label sits at the top of each cell (centred, 16-pt bold) and
        // the knob takes the remaining height - matches the inline
        // strip's HPF/LPF layout grammar.
        constexpr int kFilterLabelH = 18;
        hpfLabel.setBounds (hpfCell.removeFromTop (kFilterLabelH));
        hpfKnob .setBounds (hpfCell);
        lpfLabel.setBounds (lpfCell.removeFromTop (kFilterLabelH));
        lpfKnob .setBounds (lpfCell);
        area.removeFromTop (kRowGap);
    }

    auto columnHeader = area.removeFromTop (kColumnHeaderH);
    columnHeader.removeFromLeft (kRowLabelW);
    const int headerColW = columnHeader.getWidth() / 3;
    gainColumnLabel.setBounds (columnHeader.removeFromLeft (headerColW));
    freqColumnLabel.setBounds (columnHeader.removeFromLeft (headerColW));
    qColumnLabel   .setBounds (columnHeader);
    area.removeFromTop (2);

    for (size_t i = 0; i < rows.size(); ++i)
    {
        auto row = area.removeFromTop (kKnobBlockH);
        auto labelArea = row.removeFromLeft (kRowLabelW);

        rows[i].nameLabel.setBounds (labelArea.getX(), row.getY(),
                                       labelArea.getWidth(), kKnobSize);

        const int colW = row.getWidth() / 3;
        auto gainCell = row.removeFromLeft (colW);
        auto freqCell = row.removeFromLeft (colW);
        auto qCell = row;
        rows[i].gain->setBounds (gainCell);
        rows[i].freq->setBounds (freqCell);
        if (rows[i].q != nullptr)
            rows[i].q->setBounds (qCell);

        if (i + 1 < rows.size())
            area.removeFromTop (kRowGap);
    }
}
} // namespace duskstudio
