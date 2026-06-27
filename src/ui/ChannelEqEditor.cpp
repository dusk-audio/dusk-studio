#include "ChannelEqEditor.h"
#include "DuskStudioLookAndFeel.h"

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
    std::atomic<float>* (*freq) (ChannelStripParams&);
    // q is non-null only for bell bands (HM, LM). Shelves return nullptr.
    std::atomic<float>* (*q)    (ChannelStripParams&);
};

const std::array<BandSpec, 4>& bandSpecs()
{
    static const std::array<BandSpec, 4> specs {{
        { "HF", juce::Colour (sslEqColors::kHfRed),    ChannelStripParams::kHfFreqMin, ChannelStripParams::kHfFreqMax,
            [] (ChannelStripParams& s) -> std::atomic<float>* { return &s.hfGainDb; },
            [] (ChannelStripParams& s) -> std::atomic<float>* { return &s.hfFreq; },
            [] (ChannelStripParams&)   -> std::atomic<float>* { return nullptr; } },
        { "HM", juce::Colour (sslEqColors::kHmGreen),  ChannelStripParams::kHmFreqMin, ChannelStripParams::kHmFreqMax,
            [] (ChannelStripParams& s) -> std::atomic<float>* { return &s.hmGainDb; },
            [] (ChannelStripParams& s) -> std::atomic<float>* { return &s.hmFreq; },
            [] (ChannelStripParams& s) -> std::atomic<float>* { return &s.hmQ; } },
        { "LM", juce::Colour (sslEqColors::kLmBlue),   ChannelStripParams::kLmFreqMin, ChannelStripParams::kLmFreqMax,
            [] (ChannelStripParams& s) -> std::atomic<float>* { return &s.lmGainDb; },
            [] (ChannelStripParams& s) -> std::atomic<float>* { return &s.lmFreq; },
            [] (ChannelStripParams& s) -> std::atomic<float>* { return &s.lmQ; } },
        { "LF", juce::Colour (sslEqColors::kLfBlack),  ChannelStripParams::kLfFreqMin, ChannelStripParams::kLfFreqMax,
            [] (ChannelStripParams& s) -> std::atomic<float>* { return &s.lfGainDb; },
            [] (ChannelStripParams& s) -> std::atomic<float>* { return &s.lfFreq; },
            [] (ChannelStripParams&)   -> std::atomic<float>* { return nullptr; } },
    }};
    return specs;
}

inline juce::String formatFrequency (double hz)
{
    if (hz >= 1000.0) return juce::String (hz / 1000.0, 1) + " kHz";
    return juce::String ((int) std::round (hz)) + " Hz";
}
} // namespace

ChannelEqEditor::ChannelEqEditor (Track& t) : track (t)
{
    // Window title already shows the track + section; the inline label was
    // duplicating that. Keep the field zero-sized and unused.
    titleLabel.setVisible (false);

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

    // EQ section ON/OFF toggle — pill with green LED-style fill when
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

    // HPF + LPF — SSL 9000 J white-filter top section. Both knobs share
    // the white accent so they read as a filter pair (matches the
    // inline strip's filter row).
    const auto filterWhite = juce::Colour (sslEqColors::kFilterWhite);
    hpfLabel.setText ("HPF", juce::dontSendNotification);
    hpfLabel.setJustificationType (juce::Justification::centred);
    hpfLabel.setColour (juce::Label::textColourId, filterWhite);
    hpfLabel.setFont (juce::Font (juce::FontOptions (16.0f, juce::Font::bold)));
    addAndMakeVisible (hpfLabel);

    auto setupFilterKnob = [this] (juce::Slider& k, juce::Colour fill,
                                      double minHz, double maxHz, double offHz,
                                      double skewMid,
                                      bool offIsMax)
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
        k.textFromValueFunction = [offHz, offIsMax] (double v) -> juce::String
        {
            if (offIsMax ? (v >= offHz - 0.5) : (v <= offHz + 0.5)) return "off";
            return formatFrequency (v);
        };
    };
    setupFilterKnob (hpfKnob, filterWhite,
                      ChannelStripParams::kHpfMinHz, ChannelStripParams::kHpfMaxHz,
                      ChannelStripParams::kHpfOffHz, 80.0, /*offIsMax*/ false);
    hpfKnob.setValue (track.strip.hpfFreq.load (std::memory_order_relaxed),
                       juce::dontSendNotification);
    hpfKnob.onValueChange = [this]
    {
        const float freq = (float) hpfKnob.getValue();
        track.strip.hpfFreq.store (freq, std::memory_order_relaxed);
        const bool hpfOn = freq > ChannelStripParams::kHpfOffHz + 0.5f;
        track.strip.hpfEnabled.store (hpfOn, std::memory_order_relaxed);
        if (hpfOn)
        {
            track.strip.eqEnabled.store (true, std::memory_order_release);
            enableButton.setToggleState (true, juce::dontSendNotification);
        }
    };
    addAndMakeVisible (hpfKnob);

    // LPF — symmetric counterpart on the right side of the filter row.
    lpfLabel.setText ("LPF", juce::dontSendNotification);
    lpfLabel.setJustificationType (juce::Justification::centred);
    lpfLabel.setColour (juce::Label::textColourId, filterWhite);
    lpfLabel.setFont (juce::Font (juce::FontOptions (16.0f, juce::Font::bold)));
    addAndMakeVisible (lpfLabel);

    setupFilterKnob (lpfKnob, filterWhite,
                      ChannelStripParams::kLpfMinHz, ChannelStripParams::kLpfMaxHz,
                      ChannelStripParams::kLpfOffHz, 8000.0, /*offIsMax*/ true);
    lpfKnob.setValue (track.strip.lpfFreq.load (std::memory_order_relaxed),
                       juce::dontSendNotification);
    lpfKnob.onValueChange = [this]
    {
        const float freq = (float) lpfKnob.getValue();
        track.strip.lpfFreq.store (freq, std::memory_order_relaxed);
        const bool lpfOn = freq < ChannelStripParams::kLpfOffHz - 0.5f;
        track.strip.lpfEnabled.store (lpfOn, std::memory_order_relaxed);
        if (lpfOn)
        {
            track.strip.eqEnabled.store (true, std::memory_order_release);
            enableButton.setToggleState (true, juce::dontSendNotification);
        }
    };
    addAndMakeVisible (lpfKnob);

    for (size_t i = 0; i < bandSpecs().size(); ++i)
    {
        const auto& spec = bandSpecs()[i];
        auto& row = rows[i];

        row.nameLabel.setText (spec.name, juce::dontSendNotification);
        row.nameLabel.setJustificationType (juce::Justification::centred);
        row.nameLabel.setColour (juce::Label::textColourId, spec.accent.brighter (0.2f));
        row.nameLabel.setFont (juce::Font (juce::FontOptions (16.0f, juce::Font::bold)));
        addAndMakeVisible (row.nameLabel);

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
        row.gain->setValue (spec.gain (track.strip)->load (std::memory_order_relaxed),
                             juce::dontSendNotification);
        {
            auto* atomicPtr = spec.gain (track.strip);
            auto* knob = row.gain.get();
            auto* eqEnabledPtr = &track.strip.eqEnabled;
            knob->onValueChange = [knob, atomicPtr, eqEnabledPtr]
            {
                atomicPtr->store ((float) knob->getValue(), std::memory_order_relaxed);
                // Auto-arm — same UX as the inline strip-band knobs.
                eqEnabledPtr->store (true, std::memory_order_release);
            };
        }
        addAndMakeVisible (row.gain.get());

        const float defaultFreq = (i == 0 ? 8000.0f : i == 1 ? 2000.0f : i == 2 ? 600.0f : 100.0f);
        row.freq = std::make_unique<juce::Slider>();
        makeKnob (*row.freq, spec.accent, spec.freqMin, spec.freqMax,
                   defaultFreq, defaultFreq);
        row.freq->setNumDecimalPlacesToDisplay (0);
        row.freq->textFromValueFunction = [] (double v) { return formatFrequency (v); };
        row.freq->setValue (spec.freq (track.strip)->load (std::memory_order_relaxed),
                             juce::dontSendNotification);
        {
            auto* atomicPtr = spec.freq (track.strip);
            auto* knob = row.freq.get();
            auto* eqEnabledPtr = &track.strip.eqEnabled;
            knob->onValueChange = [knob, atomicPtr, eqEnabledPtr]
            {
                atomicPtr->store ((float) knob->getValue(), std::memory_order_relaxed);
                eqEnabledPtr->store (true, std::memory_order_release);
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
                row.q->setValue (qAtom->load (std::memory_order_relaxed),
                                  juce::dontSendNotification);
                auto* knob = row.q.get();
                auto* eqEnabledPtr = &track.strip.eqEnabled;
                knob->onValueChange = [knob, qAtom, eqEnabledPtr]
                {
                    qAtom->store ((float) knob->getValue(), std::memory_order_relaxed);
                    eqEnabledPtr->store (true, std::memory_order_release);
                };
                addAndMakeVisible (row.q.get());

                // "Q" caption next to the Q knob, in the band's accent
                // colour - matches the strip's Q label.
                row.qLabel.setText ("Q", juce::dontSendNotification);
                row.qLabel.setJustificationType (juce::Justification::centred);
                row.qLabel.setColour (juce::Label::textColourId, spec.accent.brighter (0.2f));
                row.qLabel.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
                addAndMakeVisible (row.qLabel);
            }
        }
    }

    // Tight fit to the content: HPF (84) + HF (88) + HM (160) + LM (160)
    // + LF (84) = 576 + header/gaps (~32) + outer padding (24) = ~640. The bell
    // rows are 2 px taller now that the Q block matches the gain/freq block.
    setSize (380, 640);
}

ChannelEqEditor::~ChannelEqEditor() = default;

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

    // Header: EQ enable pill on the LEFT, E/G type toggle on the RIGHT.
    auto header = area.removeFromTop (24);
    enableButton.setBounds (header.removeFromLeft (60));
    typeButton  .setBounds (header.removeFromRight (40));
    area.removeFromTop (8);

    // Layout matches the inline strip's EQ section so the TIMELINE popup
    // is visually identical: HPF row at top (single centred knob), then
    // each band row. Shelves (HF, LF) are gain | freq pairs; bell bands
    // (HM, LM) stack gain on top-left + Q below it on the left, with the
    // freq knob in the right column vertically centred between them.
    constexpr int kRowLabelW    = 36;
    constexpr int kKnobSize     = 56;
    constexpr int kValueH       = 18;
    constexpr int kKnobBlockH   = kKnobSize + kValueH + 6;            // 80
    constexpr int kQKnobSize    = 56;
    constexpr int kQBlockH      = kKnobBlockH;   // match gain/freq so the Q knob renders the same diameter
    constexpr int kFreqYStagger = 2;
    constexpr int kHpfRowH      = kKnobBlockH;
    constexpr int kShelfRowH    = kKnobBlockH + kFreqYStagger + 2;
    constexpr int kBellRowH     = kKnobBlockH + kQBlockH;
    constexpr int kRowGap       = 4;

    // Filter row — HPF | LPF side-by-side, white-faced SSL 9000 J top.
    {
        auto row = area.removeFromTop (kHpfRowH);
        // Carve same kRowLabelW gutter as the band rows so the filter
        // columns line up with the gain / freq columns below.
        row.removeFromLeft (kRowLabelW);
        const int colW = row.getWidth() / 2;
        auto hpfCell = row.removeFromLeft (colW);
        auto lpfCell = row;

        // Label sits at the top of each cell (centred, 16-pt bold) and
        // the knob takes the remaining height — matches the inline
        // strip's HPF/LPF layout grammar.
        constexpr int kFilterLabelH = 18;
        hpfLabel.setBounds (hpfCell.removeFromTop (kFilterLabelH));
        hpfKnob .setBounds (hpfCell);
        lpfLabel.setBounds (lpfCell.removeFromTop (kFilterLabelH));
        lpfKnob .setBounds (lpfCell);
        area.removeFromTop (kRowGap);
    }

    for (size_t i = 0; i < rows.size(); ++i)
    {
        const bool hasQ = rows[i].q != nullptr;
        const int  rowH = hasQ ? kBellRowH : kShelfRowH;

        auto row = area.removeFromTop (rowH);
        auto labelArea = row.removeFromLeft (kRowLabelW);

        // Band-name label - vertically centred to the gain rotary so it
        // sits next to the actual knob rather than the centre of the row.
        rows[i].nameLabel.setBounds (labelArea.getX(), row.getY(),
                                       labelArea.getWidth(), kKnobSize);

        if (hasQ)
        {
            const int qY = row.getY() + kKnobBlockH;
            rows[i].qLabel.setBounds (labelArea.getX(), qY,
                                        labelArea.getWidth(), kQKnobSize);
        }

        const int colW   = row.getWidth() / 2;
        const int leftX  = row.getX();
        const int rightX = row.getX() + colW;
        const int gainY  = row.getY();
        // Bell rows: freq is vertically centred inside the FULL bell row
        // (between gain and Q). Shelves keep the small SSL nudge.
        const int freqY  = hasQ
            ? row.getY() + (rowH - kKnobBlockH) / 2
            : gainY + kFreqYStagger;

        rows[i].gain->setBounds (leftX,  gainY, colW, kKnobBlockH);
        rows[i].freq->setBounds (rightX, freqY, colW, kKnobBlockH);

        if (hasQ)
        {
            const int qW = juce::jmin (colW, 80);
            const int qX = leftX + (colW - qW) / 2;
            const int qY = gainY + kKnobBlockH;
            rows[i].q->setBounds (qX, qY, qW, kQBlockH);
        }

        area.removeFromTop (kRowGap);
    }
}
} // namespace duskstudio
