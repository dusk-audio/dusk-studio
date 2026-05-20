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

    // HPF row at the top of the popup, mirroring the strip's HPF control.
    // Same accent colour as the strip's HPF (kHpfBlue).
    const auto hpfAccent = juce::Colour (sslEqColors::kHpfBlue);
    hpfLabel.setText ("HPF", juce::dontSendNotification);
    hpfLabel.setJustificationType (juce::Justification::centred);
    hpfLabel.setColour (juce::Label::textColourId, hpfAccent.brighter (0.2f));
    hpfLabel.setFont (juce::Font (juce::FontOptions (16.0f, juce::Font::bold)));
    addAndMakeVisible (hpfLabel);

    hpfKnob.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    hpfKnob.setColour (juce::Slider::rotarySliderFillColourId, hpfAccent);
    hpfKnob.setColour (juce::Slider::rotarySliderOutlineColourId, juce::Colour (0xff404048));
    hpfKnob.setRange (ChannelStripParams::kHpfMinHz, ChannelStripParams::kHpfMaxHz, 1.0);
    hpfKnob.setSkewFactorFromMidPoint (80.0);
    hpfKnob.setDoubleClickReturnValue (true, ChannelStripParams::kHpfOffHz);
    hpfKnob.setTextBoxStyle (juce::Slider::TextBoxBelow, true, 80, 18);
    hpfKnob.setColour (juce::Slider::textBoxTextColourId, juce::Colour (0xffe0e0e0));
    hpfKnob.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colour (0));
    hpfKnob.setColour (juce::Slider::textBoxOutlineColourId, juce::Colour (0));
    hpfKnob.textFromValueFunction = [] (double v) -> juce::String
    {
        if (v <= ChannelStripParams::kHpfOffHz + 0.5) return "off";
        return formatFrequency (v);
    };
    hpfKnob.setValue (track.strip.hpfFreq.load (std::memory_order_relaxed),
                       juce::dontSendNotification);
    hpfKnob.onValueChange = [this]
    {
        const float freq = (float) hpfKnob.getValue();
        track.strip.hpfFreq.store (freq, std::memory_order_relaxed);
        // Bypass the HPF DSP entirely when the knob is at the floor, same
        // as the strip's HPF wiring.
        track.strip.hpfEnabled.store (freq > ChannelStripParams::kHpfOffHz + 0.5f,
                                       std::memory_order_relaxed);
    };
    addAndMakeVisible (hpfKnob);

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
            knob->onValueChange = [knob, atomicPtr]
            {
                atomicPtr->store ((float) knob->getValue(), std::memory_order_relaxed);
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
            knob->onValueChange = [knob, atomicPtr]
            {
                atomicPtr->store ((float) knob->getValue(), std::memory_order_relaxed);
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
                knob->onValueChange = [knob, qAtom]
                {
                    qAtom->store ((float) knob->getValue(), std::memory_order_relaxed);
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

    // Tight fit to the content: HPF (84) + HF (88) + HM (158) + LM (158)
    // + LF (84) = 572 + header/gaps (~32) + outer padding (24) = ~628.
    setSize (380, 628);
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

    // Header: just the E/G type toggle on the right.
    auto header = area.removeFromTop (24);
    typeButton.setBounds (header.removeFromRight (40));
    area.removeFromTop (8);

    // Layout matches the inline strip's EQ section so the SUMMARY popup
    // is visually identical: HPF row at top (single centred knob), then
    // each band row. Shelves (HF, LF) are gain | freq pairs; bell bands
    // (HM, LM) stack gain on top-left + Q below it on the left, with the
    // freq knob in the right column vertically centred between them.
    constexpr int kRowLabelW    = 36;
    constexpr int kKnobSize     = 56;
    constexpr int kValueH       = 18;
    constexpr int kKnobBlockH   = kKnobSize + kValueH + 6;            // 80
    constexpr int kQKnobSize    = 56;
    constexpr int kQBlockH      = kQKnobSize + kValueH;               // 74
    constexpr int kFreqYStagger = 2;
    constexpr int kHpfRowH      = kKnobBlockH;
    constexpr int kShelfRowH    = kKnobBlockH + kFreqYStagger + 2;
    constexpr int kBellRowH     = kKnobBlockH + kQBlockH;
    constexpr int kRowGap       = 4;

    // HPF row - single knob centred horizontally.
    {
        auto row = area.removeFromTop (kHpfRowH);
        hpfLabel.setBounds (row.removeFromLeft (kRowLabelW)
                              .withSizeKeepingCentre (kRowLabelW, kKnobSize));
        const int knobW = juce::jmin (96, row.getWidth());
        const int knobX = row.getX() + (row.getWidth() - knobW) / 2;
        hpfKnob.setBounds (knobX, row.getY(), knobW, kKnobBlockH);
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
