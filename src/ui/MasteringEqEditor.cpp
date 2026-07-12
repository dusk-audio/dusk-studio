#include "MasteringEqEditor.h"
#include "CompHeaderButton.h"
#include <cmath>

namespace duskstudio
{
namespace
{
constexpr float kFreqMinHz = 20.0f;
constexpr float kFreqMaxHz = 20000.0f;
constexpr float kCurveDbRange = 18.0f;   // ±18 dB on the vertical axis

// Spectrum overlay vertical scale (dBFS). The whole plot height maps this
// range; the EQ ±18 dB curve sits on top in its own scale.
constexpr float kSpecFloorDb = -96.0f;
constexpr float kSpecTopDb   =   6.0f;

// Per-band accent colours. Same hue family as the per-channel-strip EQ
// colours so the user's eye-mapping carries between mixing and mastering.
const juce::Colour kBandColours[5] = {
    juce::Colour (0xff60c060),   // Low shelf  - green
    juce::Colour (0xffe0c050),   // Low mid    - amber
    juce::Colour (0xffe09050),   // Mid        - orange
    juce::Colour (0xffd07070),   // High mid   - rose
    juce::Colour (0xff5a8ad0),   // High shelf - blue
};
const char* kBandNames[5] = { "Low", "Lo-Mid", "Mid", "Hi-Mid", "High" };

// Default freq ranges per band. Overlapping ranges so a band can be tuned
// either side of its nominal centre - typical for mastering EQs.
struct BandRange { float minHz, maxHz, defaultHz; };
constexpr BandRange kBandRanges[5] = {
    {  20.0f,    400.0f,    50.0f },   // low shelf
    {  60.0f,   1500.0f,   250.0f },   // low mid
    { 200.0f,   6000.0f,  1000.0f },   // mid
    { 800.0f,  12000.0f,  4000.0f },   // high mid
    {2000.0f,  20000.0f, 12000.0f },   // high shelf
};

void styleKnob (juce::Slider& s, juce::Colour fill, double mn, double mx,
                 double defaultVal, double skewMid,
                 const juce::String& suffix, int decimals)
{
    s.setSliderStyle (juce::Slider::RotaryVerticalDrag);
    s.setColour (juce::Slider::rotarySliderFillColourId, fill);
    s.setColour (juce::Slider::rotarySliderOutlineColourId, juce::Colour (0xff404048));
    s.setRange (mn, mx, 0.01);
    if (skewMid > 0) s.setSkewFactorFromMidPoint (skewMid);
    s.setDoubleClickReturnValue (true, defaultVal);
    // Wider + taller textbox so values like "12k" or "0.0 dB" don't clip
    // and the auto-derived font height is large enough to be legible.
    s.setTextBoxStyle (juce::Slider::TextBoxBelow, true, 80, 18);
    s.setColour (juce::Slider::textBoxTextColourId, juce::Colour (0xffe0e0e0));
    s.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colour (0));
    s.setColour (juce::Slider::textBoxOutlineColourId,    juce::Colour (0));
    s.setNumDecimalPlacesToDisplay (decimals);
    s.setTextValueSuffix (suffix);
}

juce::String formatFreq (double v)
{
    // Compact format - drops the space + " Hz" suffix so the string fits the
    // value-textbox without truncation. ">=10000" rounds to integer kHz so
    // 12000 reads as "12k" not "12.0k".
    if (v >= 10000.0) return juce::String ((int) std::round (v / 1000.0)) + "k";
    if (v >= 1000.0)  return juce::String (v / 1000.0, 1) + "k";
    return juce::String ((int) std::round (v));
}
} // namespace

MasteringEqEditor::MasteringEqEditor (MasteringParams& p, MasteringChain* c)
    : params (p), chain (c)
{
    setOpaque (true);

    specDb.fill (kSpecFloorDb);

    // Spectrum on/off - a small unobtrusive pill in the curve's top-right.
    specToggle.setClickingTogglesState (true);
    specToggle.setToggleState (showSpectrum, juce::dontSendNotification);
    specToggle.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff202028));
    specToggle.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff2a4a60));
    specToggle.setColour (juce::TextButton::textColourOffId,  juce::Colour (0xff707078));
    specToggle.setColour (juce::TextButton::textColourOnId,   juce::Colour (0xff8aceee));
    specToggle.onClick = [this]
    {
        showSpectrum = specToggle.getToggleState();
        if (! curveArea.isEmpty()) repaint (curveArea);
    };
    addAndMakeVisible (specToggle);

    headerBtn = std::make_unique<CompHeaderButton> (
        [this] { return params.eqEnabled.load (std::memory_order_relaxed); },
        [this]
        {
            const bool now = ! params.eqEnabled.load (std::memory_order_relaxed);
            params.eqEnabled.store (now, std::memory_order_relaxed);
            if (headerBtn != nullptr) headerBtn->repaint();
        });
    headerBtn->setLabelText ("MASTERING EQ");
    headerBtn->setAccentColour (juce::Colour (0xff5fa8d0));   // blue - transparent EQ
    addAndMakeVisible (headerBtn.get());

    for (int i = 0; i < 5; ++i)
    {
        auto& b = bandUI[(size_t) i];
        b.accent = kBandColours[i];
        b.bandLabel.setText (kBandNames[i], juce::dontSendNotification);
        b.bandLabel.setJustificationType (juce::Justification::centred);
        b.bandLabel.setFont (juce::Font (juce::FontOptions (12.5f, juce::Font::bold)));
        b.bandLabel.setColour (juce::Label::textColourId, b.accent.brighter (0.25f));
        addAndMakeVisible (b.bandLabel);

        const auto& r = kBandRanges[i];
        // Empty suffix - formatFreq() includes the unit ("k") and we don't
        // want a stray " Hz" appended to short readouts like "80".
        styleKnob (b.freqKnob, b.accent, r.minHz, r.maxHz, r.defaultHz, r.defaultHz,
                    "", 0);
        b.freqKnob.textFromValueFunction = [] (double v) { return formatFreq (v); };
        b.freqKnob.setValue (params.eqBandFreq[i].load (std::memory_order_relaxed),
                              juce::dontSendNotification);
        b.freqKnob.updateText();
        b.freqKnob.onValueChange = [this, i]
        {
            params.eqBandFreq[i].store ((float) bandUI[(size_t) i].freqKnob.getValue(),
                                          std::memory_order_relaxed);
        };
        addAndMakeVisible (b.freqKnob);

        styleKnob (b.gainKnob, b.accent, -12.0, 12.0, 0.0, 0.0, " dB", 1);
        b.gainKnob.setValue (params.eqBandGainDb[i].load (std::memory_order_relaxed),
                              juce::dontSendNotification);
        b.gainKnob.onValueChange = [this, i]
        {
            params.eqBandGainDb[i].store ((float) bandUI[(size_t) i].gainKnob.getValue(),
                                            std::memory_order_relaxed);
        };
        addAndMakeVisible (b.gainKnob);

        // Shelf bands (0 and 4) hide their Q knob - the user-facing dial
        // isn't musically useful for the gentle slopes mastering uses.
        const bool isShelf = (i == 0 || i == 4);
        styleKnob (b.qKnob, b.accent, 0.3, 6.0, isShelf ? 0.7 : 1.0, 1.0, "", 2);
        b.qKnob.setValue (params.eqBandQ[i].load (std::memory_order_relaxed),
                           juce::dontSendNotification);
        b.qKnob.onValueChange = [this, i]
        {
            params.eqBandQ[i].store ((float) bandUI[(size_t) i].qKnob.getValue(),
                                       std::memory_order_relaxed);
        };
        if (isShelf) addChildComponent (b.qKnob);  // tracked but hidden
        else         addAndMakeVisible (b.qKnob);
    }

    rebuildKnobValues();
    startTimerHz (30);
}

MasteringEqEditor::~MasteringEqEditor() { stopTimer(); }

void MasteringEqEditor::rebuildKnobValues()
{
    for (int i = 0; i < 5; ++i)
    {
        lastFreq[(size_t) i] = params.eqBandFreq[i]   .load (std::memory_order_relaxed);
        lastGain[(size_t) i] = params.eqBandGainDb[i] .load (std::memory_order_relaxed);
        lastQ   [(size_t) i] = params.eqBandQ[i]      .load (std::memory_order_relaxed);
    }
    lastEnabled = params.eqEnabled.load (std::memory_order_relaxed);
    lastScopeSampleRate = (chain != nullptr) ? chain->getScopeSampleRate() : 0.0;
}

void MasteringEqEditor::timerCallback()
{
    const bool specChanged = updateSpectrum();

    bool changed = false;
    for (int i = 0; i < 5; ++i)
    {
        const float f = params.eqBandFreq[i]   .load (std::memory_order_relaxed);
        const float g = params.eqBandGainDb[i] .load (std::memory_order_relaxed);
        const float q = params.eqBandQ[i]      .load (std::memory_order_relaxed);
        if (f != lastFreq[(size_t) i] || g != lastGain[(size_t) i] || q != lastQ[(size_t) i])
            changed = true;
    }
    const bool en = params.eqEnabled.load (std::memory_order_relaxed);
    const bool enabledChanged = (en != lastEnabled);
    if (enabledChanged) changed = true;

    // Re-evaluate the curve when the audio device's scope rate changes (bandResponseDb
    // depends on it), even if no band parameter moved.
    const double scopeSr = (chain != nullptr) ? chain->getScopeSampleRate() : 0.0;
    if (scopeSr != lastScopeSampleRate) changed = true;

    if (changed)
    {
        rebuildKnobValues();
        if (! curveArea.isEmpty())
            repaint (curveArea);
        // Header LED reflects eqEnabled; repaint it so MIDI / other-surface
        // toggles light it, not just local header clicks.
        if (enabledChanged && headerBtn != nullptr)
            headerBtn->repaint();
    }
    else if (specChanged && ! curveArea.isEmpty())
        repaint (curveArea);
}

float MasteringEqEditor::bandResponseDb (int idx, float freqHz) const noexcept
{
    // Plot the actual biquad magnitude (shared coefficient math with the DSP).
    // The DSP runs the biquads oversampled, so evaluate the curve at the same
    // oversampled rate - that's the de-crammed response the audio applies.
    // Guard a null chain AND an invalid scope rate (0 / negative / non-finite) -
    // either would push a bad sample rate into magnitudeDb. Matches drawSpectrum.
    double base = (chain != nullptr) ? chain->getScopeSampleRate() : 48000.0;
    if (! (base > 0.0) || ! std::isfinite (base)) base = 48000.0;
    const double sr = base * (double) duskstudio::MasteringDigitalEq::kOversample;
    return duskstudio::MasteringDigitalEq::magnitudeDb (
        idx, sr, lastFreq[(size_t) idx], lastQ[(size_t) idx], lastGain[(size_t) idx], freqHz);
}

float MasteringEqEditor::totalResponseDb (float freqHz) const noexcept
{
    if (! lastEnabled) return 0.0f;
    float total = 0.0f;
    for (int i = 0; i < 5; ++i) total += bandResponseDb (i, freqHz);
    return total;
}

bool MasteringEqEditor::updateSpectrum()
{
    if (! showSpectrum || chain == nullptr) return false;

    if (chain->readScopeLatest (fftScratch.data(), kFftSize) < kFftSize)
        return false;   // not enough audio buffered yet

    // performFrequencyOnlyForwardTransform wants a 2×size scratch; the
    // windowed time data goes in the low half, magnitudes come back in it.
    std::copy (fftScratch.begin(), fftScratch.end(), fftWork.begin());
    fftWindow.multiplyWithWindowingTable (fftWork.data(), (size_t) kFftSize);
    fft.performFrequencyOnlyForwardTransform (fftWork.data());

    // ×4/N: single-sided scaling (2/N) times the 2× that compensates the Hann
    // window's 0.5 coherent gain, so a bin-centred 0 dBFS sine reads near 0 dBFS
    // (with 2/N alone it sat ~6 dB low).
    constexpr float kRef = 4.0f / (float) kFftSize;
    for (int i = 0; i < kNumBins; ++i)
    {
        const float db = juce::jlimit (kSpecFloorDb, kSpecTopDb,
            juce::Decibels::gainToDecibels (fftWork[(size_t) i] * kRef, kSpecFloorDb));
        float& s = specDb[(size_t) i];
        s = (db > s) ? db : s + (db - s) * 0.25f;   // instant attack, slow release
    }
    return true;
}

void MasteringEqEditor::drawSpectrum (juce::Graphics& g, juce::Rectangle<float> plot) const
{
    const double sr = (chain != nullptr) ? chain->getScopeSampleRate() : 48000.0;
    if (sr <= 0.0) return;

    const float logMin = std::log10 (kFreqMinHz);
    const float logMax = std::log10 (kFreqMaxHz);
    auto fToX = [&] (float fHz)
    {
        return plot.getX() + (std::log10 (fHz) - logMin) / (logMax - logMin) * plot.getWidth();
    };
    auto dbToY = [&] (float db)
    {
        const float frac = juce::jlimit (0.0f, 1.0f,
            (db - kSpecFloorDb) / (kSpecTopDb - kSpecFloorDb));
        return plot.getBottom() - frac * plot.getHeight();
    };

    juce::Path contour;
    float firstX = 0.0f, lastX = 0.0f;
    bool started = false;
    const float binHz = (float) sr / (float) kFftSize;
    for (int i = 1; i < kNumBins; ++i)
    {
        const float f = i * binHz;
        if (f < kFreqMinHz) continue;
        if (f > kFreqMaxHz) break;
        const float x = fToX (f);
        const float y = dbToY (specDb[(size_t) i]);
        if (! started) { contour.startNewSubPath (x, y); firstX = x; started = true; }
        else           contour.lineTo (x, y);
        lastX = x;
    }
    if (! started) return;

    juce::Path fill = contour;
    fill.lineTo (lastX, plot.getBottom());
    fill.lineTo (firstX, plot.getBottom());
    fill.closeSubPath();

    g.setColour (juce::Colour (0xff39c0c8).withAlpha (0.16f));
    g.fillPath (fill);
    g.setColour (juce::Colour (0xff5fd6dd).withAlpha (0.45f));
    g.strokePath (contour, juce::PathStrokeType (1.0f));
}

void MasteringEqEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff20202a));   // raised panel surface
    g.setColour (juce::Colour (0xff3a3a46));
    g.drawRect (getLocalBounds(), 1);

    if (curveArea.isEmpty()) return;

    // Curve plot background.
    auto plot = curveArea.toFloat().reduced (1.0f);
    g.setColour (juce::Colour (0xff0d0d11));
    g.fillRoundedRectangle (plot, 3.0f);

    // Grid: 0 dB centre line + ±6 / ±12 horizontal guides.
    const float midY = plot.getCentreY();
    const float halfH = plot.getHeight() * 0.5f;

    auto dbToY = [&] (float db)
    {
        return midY - (db / kCurveDbRange) * halfH;
    };

    g.setColour (juce::Colour (0xff20202a));
    for (float db : { -12.0f, -6.0f, 6.0f, 12.0f })
        g.drawHorizontalLine ((int) dbToY (db), plot.getX(), plot.getRight());
    g.setColour (juce::Colour (0xff404048));
    g.drawHorizontalLine ((int) midY, plot.getX(), plot.getRight());

    // Vertical decade gridlines at 100 / 1k / 10k.
    auto fToX = [&] (float fHz)
    {
        const float frac = (std::log10 (fHz) - std::log10 (kFreqMinHz))
                           / (std::log10 (kFreqMaxHz) - std::log10 (kFreqMinHz));
        return plot.getX() + frac * plot.getWidth();
    };
    g.setColour (juce::Colour (0xff20202a));
    for (float f : { 100.0f, 1000.0f, 10000.0f })
        g.drawVerticalLine ((int) fToX (f), plot.getY(), plot.getBottom());

    // Decade labels.
    g.setColour (juce::Colour (0xff606068));
    g.setFont (juce::Font (juce::FontOptions (9.0f)));
    for (auto entry : { std::pair<float, const char*> { 100.0f,   "100" },
                        std::pair<float, const char*> { 1000.0f,  "1k"  },
                        std::pair<float, const char*> { 10000.0f, "10k" } })
    {
        const auto x = fToX (entry.first);
        g.drawText (entry.second,
                     juce::Rectangle<float> (x - 18.0f, plot.getBottom() - 11.0f, 36.0f, 10.0f),
                     juce::Justification::centred, false);
    }

    // Live spectrum (post-EQ), behind the EQ curves
    if (showSpectrum)
        drawSpectrum (g, plot);

    // Per-band response curves (dim)
    constexpr int kNumPoints = 220;
    std::array<float, kNumPoints> freqs;
    for (int i = 0; i < kNumPoints; ++i)
    {
        const float t = (float) i / (float) (kNumPoints - 1);
        freqs[(size_t) i] = std::pow (10.0f, std::log10 (kFreqMinHz)
                                              + t * (std::log10 (kFreqMaxHz) - std::log10 (kFreqMinHz)));
    }

    // Render the curve regardless of enable state - just dim when bypassed
    // so the user can dial in band gains and watch the shape build up before
    // engaging the EQ.
    const float curveAlpha = lastEnabled ? 1.0f : 0.35f;

    for (int b = 0; b < 5; ++b)
    {
        juce::Path bandPath;
        for (int i = 0; i < kNumPoints; ++i)
        {
            // Force every band to use its real (last-edited) response even
            // when bypassed so the per-band lines render. Gain==0 just gives
            // a flat line at 0 dB which is visually correct.
            const float db = bandResponseDb (b, freqs[(size_t) i]);
            const float x = fToX (freqs[(size_t) i]);
            const float y = juce::jlimit (plot.getY(), plot.getBottom(), dbToY (db));
            if (i == 0) bandPath.startNewSubPath (x, y);
            else        bandPath.lineTo (x, y);
        }
        g.setColour (kBandColours[b].withAlpha (0.30f * curveAlpha));
        g.strokePath (bandPath, juce::PathStrokeType (1.2f));
    }

    // Total response (bold).
    juce::Path totalPath;
    juce::Path fillPath;
    for (int i = 0; i < kNumPoints; ++i)
    {
        // Sum all bands regardless of enable so the curve "tracks" the
        // user's edits. The DSP path still respects lastEnabled - this is
        // purely the visual curve.
        float total = 0.0f;
        for (int b = 0; b < 5; ++b) total += bandResponseDb (b, freqs[(size_t) i]);
        const float x = fToX (freqs[(size_t) i]);
        const float y = juce::jlimit (plot.getY(), plot.getBottom(), dbToY (total));
        if (i == 0)
        {
            totalPath.startNewSubPath (x, y);
            fillPath.startNewSubPath (x, midY);
            fillPath.lineTo (x, y);
        }
        else
        {
            totalPath.lineTo (x, y);
            fillPath.lineTo (x, y);
        }
    }
    fillPath.lineTo (plot.getRight(), midY);
    fillPath.closeSubPath();

    g.setColour (juce::Colour (0xff5a8ad0).withAlpha (0.18f * curveAlpha));
    g.fillPath (fillPath);
    g.setColour (juce::Colour (0xff8aafee).withAlpha (curveAlpha));
    g.strokePath (totalPath, juce::PathStrokeType (1.6f));

    if (! lastEnabled)
    {
        g.setColour (juce::Colour (0xff707074));
        g.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
        g.drawText ("BYPASS",
                     plot.withSizeKeepingCentre (60.0f, 16.0f).withY (plot.getY() + 6.0f),
                     juce::Justification::centred, false);
    }

    // dB scale labels on the right edge of the plot
    g.setColour (juce::Colour (0xff909094));
    g.setFont (juce::Font (juce::FontOptions (9.5f, juce::Font::bold)));
    for (float db : { 12.0f, 6.0f, 0.0f, -6.0f, -12.0f })
    {
        const float y = dbToY (db);
        const auto rect = juce::Rectangle<float> (plot.getRight() - 30.0f, y - 7.0f,
                                                    28.0f, 14.0f);
        const juce::String txt = (db > 0.0f ? "+" : "") + juce::String ((int) db);
        g.drawText (txt, rect, juce::Justification::centredRight, false);
    }

    // Band handle dots on the curve at each band's centre frequency
    // Larger dots (16 px) so they're easy to grab; the dragged band gets a
    // brighter halo so the user always knows which one they're moving.
    constexpr float kDotR = 8.0f;
    for (int b = 0; b < 5; ++b)
    {
        const float fc = lastFreq[(size_t) b];
        float total = 0.0f;
        for (int bi = 0; bi < 5; ++bi) total += bandResponseDb (bi, fc);
        const float x = fToX (fc);
        const float y = juce::jlimit (plot.getY(), plot.getBottom(), dbToY (total));
        const bool isDragging = (b == draggingBand);
        const auto fill = kBandColours[b].withAlpha (lastEnabled ? 0.95f : 0.55f);

        // Soft halo behind the dragged dot.
        if (isDragging)
        {
            g.setColour (kBandColours[b].withAlpha (0.30f));
            g.fillEllipse (x - kDotR - 4.0f, y - kDotR - 4.0f,
                            (kDotR + 4.0f) * 2.0f, (kDotR + 4.0f) * 2.0f);
        }

        g.setColour (fill);
        g.fillEllipse (x - kDotR, y - kDotR, kDotR * 2.0f, kDotR * 2.0f);
        g.setColour (juce::Colour (0xff0a0a0a));
        g.drawEllipse (x - kDotR, y - kDotR, kDotR * 2.0f, kDotR * 2.0f,
                        isDragging ? 1.6f : 1.0f);
        // Band number inside the dot - larger so it's readable on the
        // bigger handle.
        g.setColour (juce::Colour (0xff0a0a0a));
        g.setFont (juce::Font (juce::FontOptions (10.5f, juce::Font::bold)));
        g.drawText (juce::String (b + 1),
                      juce::Rectangle<float> (x - kDotR, y - kDotR, kDotR * 2.0f, kDotR * 2.0f),
                      juce::Justification::centred, false);

        // Live dB readout above the dragged dot - shows the band's gain.
        if (isDragging)
        {
            const float gain = lastGain[(size_t) b];
            const auto badge = juce::Rectangle<float> (x - 28.0f, y - kDotR - 22.0f,
                                                         56.0f, 16.0f);
            g.setColour (juce::Colour (0xff0d0d11));
            g.fillRoundedRectangle (badge, 3.0f);
            g.setColour (kBandColours[b].withAlpha (0.85f));
            g.drawRoundedRectangle (badge, 3.0f, 0.8f);
            g.setColour (juce::Colour (0xfff0f0f0));
            g.setFont (juce::Font (juce::FontOptions (10.5f, juce::Font::bold)));
            g.drawText ((gain >= 0.0f ? "+" : "") + juce::String (gain, 1) + " dB",
                         badge, juce::Justification::centred, false);
        }
    }
}

// Plot <-> param coordinate helpers
//
// All of these stay coordinate-correct against curveArea so paint() and the
// drag path don't drift. dbToY uses the same midY/halfH math as paint();
// freqToX uses the log10 mapping. The inverse pair (yToDb / xToFreq)
// drives mouseDrag.

float MasteringEqEditor::dbToY (float db) const noexcept
{
    if (curveArea.isEmpty()) return 0.0f;
    const auto plot = curveArea.toFloat().reduced (1.0f);
    const float midY  = plot.getCentreY();
    const float halfH = plot.getHeight() * 0.5f;
    return midY - (db / kCurveDbRange) * halfH;
}

float MasteringEqEditor::yToDb (float y) const noexcept
{
    if (curveArea.isEmpty()) return 0.0f;
    const auto plot = curveArea.toFloat().reduced (1.0f);
    const float midY  = plot.getCentreY();
    const float halfH = plot.getHeight() * 0.5f;
    if (halfH <= 0.0f) return 0.0f;
    return juce::jlimit (-kCurveDbRange, kCurveDbRange,
                          (midY - y) / halfH * kCurveDbRange);
}

float MasteringEqEditor::freqToX (float hz) const noexcept
{
    if (curveArea.isEmpty()) return 0.0f;
    const auto plot = curveArea.toFloat().reduced (1.0f);
    const float frac = (std::log10 (hz) - std::log10 (kFreqMinHz))
                       / (std::log10 (kFreqMaxHz) - std::log10 (kFreqMinHz));
    return plot.getX() + frac * plot.getWidth();
}

float MasteringEqEditor::xToFreq (float x) const noexcept
{
    if (curveArea.isEmpty()) return kFreqMinHz;
    const auto plot = curveArea.toFloat().reduced (1.0f);
    if (plot.getWidth() <= 0.0f) return kFreqMinHz;
    const float frac = juce::jlimit (0.0f, 1.0f,
                                       (x - plot.getX()) / plot.getWidth());
    const float logF = std::log10 (kFreqMinHz)
                        + frac * (std::log10 (kFreqMaxHz) - std::log10 (kFreqMinHz));
    return std::pow (10.0f, logF);
}

int MasteringEqEditor::hitTestBandDot (juce::Point<int> p) const noexcept
{
    constexpr float kHitR = 14.0f;   // a bit larger than the dot itself for forgiving hits
    int best = -1;
    float bestDist = kHitR;
    for (int b = 0; b < 5; ++b)
    {
        const float fc = lastFreq[(size_t) b];
        float total = 0.0f;
        for (int bi = 0; bi < 5; ++bi) total += bandResponseDb (bi, fc);
        const auto plot = curveArea.toFloat().reduced (1.0f);
        const float x = freqToX (fc);
        const float y = juce::jlimit (plot.getY(), plot.getBottom(), dbToY (total));
        const float dx = (float) p.x - x;
        const float dy = (float) p.y - y;
        const float dist = std::sqrt (dx * dx + dy * dy);
        if (dist < bestDist) { best = b; bestDist = dist; }
    }
    return best;
}

void MasteringEqEditor::mouseDown (const juce::MouseEvent& e)
{
    if (! curveArea.contains (e.getPosition())) { draggingBand = -1; return; }
    draggingBand = hitTestBandDot (e.getPosition());
    if (draggingBand >= 0)
        setMouseCursor (juce::MouseCursor::DraggingHandCursor);
    repaint (curveArea);
}

void MasteringEqEditor::mouseDrag (const juce::MouseEvent& e)
{
    if (draggingBand < 0) return;
    const int b = draggingBand;

    // Y -> gain (clamped to the gain knob's range so the slider doesn't
    // jump outside its scale and so a wild drag past the plot edge doesn't
    // store a value that no longer makes audible sense).
    const float gainDb = juce::jlimit (-12.0f, 12.0f, yToDb ((float) e.y));
    params.eqBandGainDb[b].store (gainDb, std::memory_order_relaxed);
    bandUI[(size_t) b].gainKnob.setValue (gainDb, juce::dontSendNotification);

    // X -> freq, clamped to that band's UI range so dragging doesn't
    // teleport bands out of order (e.g. dragging Mid past High).
    const auto& range = bandUI[(size_t) b].freqKnob.getRange();
    const float hz = juce::jlimit ((float) range.getStart(),
                                     (float) range.getEnd(),
                                     xToFreq ((float) e.x));
    params.eqBandFreq[b].store (hz, std::memory_order_relaxed);
    bandUI[(size_t) b].freqKnob.setValue (hz, juce::dontSendNotification);
    bandUI[(size_t) b].freqKnob.updateText();

    // Refresh local cache so paint() picks up the new dot position THIS
    // frame rather than waiting for the timer.
    lastGain[(size_t) b] = gainDb;
    lastFreq[(size_t) b] = hz;
    repaint (curveArea);
}

void MasteringEqEditor::mouseUp (const juce::MouseEvent&)
{
    if (draggingBand >= 0)
    {
        draggingBand = -1;
        setMouseCursor (juce::MouseCursor::NormalCursor);
        repaint (curveArea);
    }
}

void MasteringEqEditor::mouseDoubleClick (const juce::MouseEvent& e)
{
    // Reset a band's gain to 0 dB by double-clicking its dot. Faster than
    // the gain-knob's double-click reset for users working from the curve.
    const int b = hitTestBandDot (e.getPosition());
    if (b < 0) return;
    params.eqBandGainDb[b].store (0.0f, std::memory_order_relaxed);
    bandUI[(size_t) b].gainKnob.setValue (0.0f, juce::dontSendNotification);
    lastGain[(size_t) b] = 0.0f;
    repaint (curveArea);
}

void MasteringEqEditor::resized()
{
    auto area = getLocalBounds().reduced (8);

    auto header = area.removeFromTop (20);
    if (headerBtn != nullptr) headerBtn->setBounds (header);
    area.removeFromTop (4);

    // Curve plot uses ~55 % of remaining height; the controls row uses the
    // rest. With a tall panel the user reads the curve clearly; with a
    // narrower panel both halves shrink proportionally.
    const int controlsH = juce::jlimit (130, 210, (int) (area.getHeight() * 0.48f));
    controlsArea = area.removeFromBottom (controlsH);
    area.removeFromBottom (4);
    curveArea = area;

    // FFT toggle - small pill in the plot's top-left (top-right holds the
    // +12 dB scale label, centre holds the BYPASS badge).
    specToggle.setBounds (curveArea.getX() + 6, curveArea.getY() + 5, 34, 15);

    // Band controls: 5 equal columns. Each column has band label,
    //    Freq knob, Gain knob, and Q knob (mid bands only). Shelf columns
    //    drop the Q row and centre Freq+Gain instead.
    const int colW = controlsArea.getWidth() / 5;
    const int knobSize = juce::jlimit (34, 60, (controlsArea.getHeight() - 16) / 3 - 8);

    for (int i = 0; i < 5; ++i)
    {
        auto col = juce::Rectangle<int> (controlsArea.getX() + i * colW,
                                            controlsArea.getY(),
                                            colW, controlsArea.getHeight());
        col = col.reduced (3, 0);

        auto labelRow = col.removeFromTop (14);
        bandUI[(size_t) i].bandLabel.setBounds (labelRow);
        col.removeFromTop (2);

        const bool isShelf = (i == 0 || i == 4);
        const int rows = isShelf ? 2 : 3;
        const int rowH = col.getHeight() / rows;
        auto layoutKnob = [&] (juce::Slider& k, juce::Rectangle<int> row)
        {
            const int x = row.getX() + (row.getWidth() - knobSize) / 2;
            const int y = row.getY() + 2;
            k.setBounds (x, y, knobSize, juce::jmin (knobSize + 14, row.getHeight() - 2));
        };
        layoutKnob (bandUI[(size_t) i].freqKnob, col.removeFromTop (rowH));
        layoutKnob (bandUI[(size_t) i].gainKnob, col.removeFromTop (rowH));
        if (! isShelf)
            layoutKnob (bandUI[(size_t) i].qKnob, col);
    }
}
} // namespace duskstudio
