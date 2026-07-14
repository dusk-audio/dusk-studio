#include "MasteringLimiterEditor.h"
#include "CompHeaderButton.h"
#include "../dsp/BrickwallLimiter.h"

namespace duskstudio
{
namespace
{
constexpr float kThreshMinDb  = -20.0f;
constexpr float kThreshMaxDb  =   0.0f;
constexpr float kCeilingMinDb = -12.0f;
constexpr float kCeilingMaxDb =   0.0f;
constexpr float kAttenMaxDb   =  20.0f;   // GR axis max

// Map a dB value to a y-coordinate inside a vertical meter, top = max dB.
float dbToY (float db, float minDb, float maxDb, juce::Rectangle<float> bar)
{
    const float frac = juce::jlimit (0.0f, 1.0f, (db - minDb) / (maxDb - minDb));
    return bar.getBottom() - 2.0f - frac * (bar.getHeight() - 4.0f);
}

float yToDb (int y, float minDb, float maxDb, juce::Rectangle<float> bar)
{
    const float relY = (float) (bar.getBottom() - 2 - y) / juce::jmax (1.0f, bar.getHeight() - 4.0f);
    return juce::jlimit (minDb, maxDb,
                          minDb + juce::jlimit (0.0f, 1.0f, relY) * (maxDb - minDb));
}

void styleKnob (juce::Slider& s, juce::Colour fill,
                  double mn, double mx, double defaultVal,
                  const juce::String& suffix, int decimals)
{
    s.setSliderStyle (juce::Slider::RotaryVerticalDrag);
    s.setColour (juce::Slider::rotarySliderFillColourId, fill);
    s.setColour (juce::Slider::rotarySliderOutlineColourId, juce::Colour (0xff404048));
    s.setRange (mn, mx, 0.01);
    s.setDoubleClickReturnValue (true, defaultVal);
    s.setTextBoxStyle (juce::Slider::TextBoxBelow, true, 70, 16);
    s.setColour (juce::Slider::textBoxTextColourId, juce::Colour (0xffd0d0d0));
    s.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colour (0));
    s.setColour (juce::Slider::textBoxOutlineColourId,    juce::Colour (0));
    s.setNumDecimalPlacesToDisplay (decimals);
    s.setTextValueSuffix (suffix);
}

} // namespace

MasteringLimiterEditor::MasteringLimiterEditor (MasteringParams& p,
                                                  BrickwallLimiter& l)
    : params (p), limiter (l)
{
    setOpaque (true);

    headerBtn = std::make_unique<CompHeaderButton> (
        [this] { return params.limiterEnabled.load (std::memory_order_relaxed); },
        [this]
        {
            const bool now = ! params.limiterEnabled.load (std::memory_order_relaxed);
            params.limiterEnabled.store (now, std::memory_order_relaxed);
            if (headerBtn != nullptr) headerBtn->repaint();
        });
    headerBtn->setLabelText ("LIMITER");
    headerBtn->setAccentColour (juce::Colour (0xffe05050));   // red - brickwall
    addAndMakeVisible (headerBtn.get());

    // Mode shapes the limiter's hold + release: Modern (balanced), Transparent
    // (fast recovery, minimal pumping), Punchy (longer hold, denser).
    modeCaption.setJustificationType (juce::Justification::centredRight);
    modeCaption.setFont (juce::Font (juce::FontOptions (10.5f, juce::Font::bold)));
    modeCaption.setColour (juce::Label::textColourId, juce::Colour (0xffa0a0a8));
    addAndMakeVisible (modeCaption);

    modeCombo.addItem ("Modern", 1);
    modeCombo.addItem ("Transparent", 2);
    modeCombo.addItem ("Punchy", 3);
    // Clamp at the param boundary so a stale / corrupt limiterMode can't select
    // an ID the combo doesn't have (which renders it blank).
    const int storedMode = juce::jlimit (0, modeCombo.getNumItems() - 1,
                                          params.limiterMode.load (std::memory_order_relaxed));
    modeCombo.setSelectedId (storedMode + 1, juce::dontSendNotification);
    modeCombo.setColour (juce::ComboBox::backgroundColourId, juce::Colour (0xff1a1a22));
    modeCombo.setColour (juce::ComboBox::textColourId,       juce::Colour (0xffe0e0e8));
    modeCombo.setColour (juce::ComboBox::outlineColourId,    juce::Colour (0xff404048));
    modeCombo.onChange = [this]
    {
        const int candidate = juce::jlimit (0, modeCombo.getNumItems() - 1,
                                             modeCombo.getSelectedId() - 1);
        params.limiterMode.store (candidate, std::memory_order_relaxed);
    };
    addAndMakeVisible (modeCombo);

    const auto accent = juce::Colour (0xff5a8ad0);
    styleKnob (releaseKnob, accent, 10.0, 1000.0, 100.0, " ms", 0);
    releaseKnob.setValue (params.limiterReleaseMs.load (std::memory_order_relaxed),
                            juce::dontSendNotification);
    releaseKnob.onValueChange = [this]
    {
        params.limiterReleaseMs.store ((float) releaseKnob.getValue(), std::memory_order_relaxed);
    };
    addAndMakeVisible (releaseKnob);

    releaseLabel.setJustificationType (juce::Justification::centred);
    releaseLabel.setFont (juce::Font (juce::FontOptions (10.5f, juce::Font::bold)));
    releaseLabel.setColour (juce::Label::textColourId, juce::Colour (0xffa0a0a8));
    addAndMakeVisible (releaseLabel);

    styleKnob (lookaheadKnob, accent, 0.1, 10.0, 2.0, " ms", 1);
    lookaheadKnob.setValue (params.limiterLookaheadMs.load (std::memory_order_relaxed),
                              juce::dontSendNotification);
    lookaheadKnob.setTooltip ("More lookahead catches transients more cleanly but adds latency.");
    lookaheadKnob.onValueChange = [this]
    {
        params.limiterLookaheadMs.store ((float) lookaheadKnob.getValue(), std::memory_order_relaxed);
    };
    addAndMakeVisible (lookaheadKnob);

    lookaheadLabel.setJustificationType (juce::Justification::centred);
    lookaheadLabel.setFont (juce::Font (juce::FontOptions (10.5f, juce::Font::bold)));
    lookaheadLabel.setColour (juce::Label::textColourId, juce::Colour (0xffa0a0a8));
    addAndMakeVisible (lookaheadLabel);

    stereoLinkToggle.setMouseClickGrabsKeyboardFocus (false);
    stereoLinkToggle.setColour (juce::ToggleButton::textColourId, juce::Colour (0xffd0d0d0));
    stereoLinkToggle.setToggleState (params.limiterStereoLink.load (std::memory_order_relaxed),
                                      juce::dontSendNotification);
    stereoLinkToggle.setTooltip ("When on, gain reduction is matched across L/R "
                                  "to preserve the stereo image. Off limits each "
                                  "channel independently.");
    stereoLinkToggle.onClick = [this]
    {
        params.limiterStereoLink.store (stereoLinkToggle.getToggleState(), std::memory_order_relaxed);
    };
    addAndMakeVisible (stereoLinkToggle);

    startTimerHz (30);
}

MasteringLimiterEditor::~MasteringLimiterEditor() { stopTimer(); }

float MasteringLimiterEditor::yToDriveDb (int y) const noexcept
{
    // The handle is a threshold (0..-20 dB top-to-bottom) but the limiter has
    // no threshold - it drives the input into the ceiling. Lower threshold =
    // more drive = louder, so drive = -threshold (0..+20 dB).
    return -yToDb (y, kThreshMinDb, kThreshMaxDb, threshMeterArea.toFloat());
}

float MasteringLimiterEditor::yToCeilingDb (int y) const noexcept
{
    return yToDb (y, kCeilingMinDb, kCeilingMaxDb, ceilingMeterArea.toFloat());
}

void MasteringLimiterEditor::timerCallback()
{
    // Mode / stereo-link / release are set once in the ctor, so a session load
    // (which rewrites the params in place) would leave them stale. Re-sync from
    // the params here - dontSendNotification so this never re-fires the onChange
    // write-back. Skip the knob while the user is dragging it.
    const int modeId = juce::jlimit (0, modeCombo.getNumItems() - 1,
                                     params.limiterMode.load (std::memory_order_relaxed)) + 1;
    if (modeCombo.getSelectedId() != modeId)
        modeCombo.setSelectedId (modeId, juce::dontSendNotification);

    const bool link = params.limiterStereoLink.load (std::memory_order_relaxed);
    if (stereoLinkToggle.getToggleState() != link)
        stereoLinkToggle.setToggleState (link, juce::dontSendNotification);

    if (! releaseKnob.isMouseButtonDown())
    {
        const float rel = params.limiterReleaseMs.load (std::memory_order_relaxed);
        if (std::abs ((float) releaseKnob.getValue() - rel) > 0.5f)
            releaseKnob.setValue (rel, juce::dontSendNotification);
    }

    const bool editingLookahead = lookaheadKnob.isMouseButtonDown()
                               || lookaheadKnob.hasKeyboardFocus (true);
    if (! editingLookahead)
    {
        const float la = params.limiterLookaheadMs.load (std::memory_order_relaxed);
        if (std::abs ((float) lookaheadKnob.getValue() - la) > 0.01f)
            lookaheadKnob.setValue (la, juce::dontSendNotification);
    }
    const float gr = limiter.getCurrentGrDb();
    if (gr < displayedGrDb) displayedGrDb = gr;
    else                    displayedGrDb += (gr - displayedGrDb) * 0.18f;

    // Pre / post limiter levels - approximated from the post-master meters
    // on MasteringParams. The mastering chain writes those at the end of
    // each block, so they're a reliable proxy for what the limiter is
    // seeing/producing.
    const float postL = params.meterPostMasterLDb.load (std::memory_order_relaxed);
    const float postR = params.meterPostMasterRDb.load (std::memory_order_relaxed);
    const float post  = juce::jmax (postL, postR);
    if (post > displayedOutDb) displayedOutDb = post;
    else                        displayedOutDb += (post - displayedOutDb) * 0.15f;

    // Pre-limiter (driven input) approximation: post-limiter peak minus the GR
    // (which the limiter pulled out). With 0 GR this just matches the post
    // value; with active limiting the column shows the driven level the
    // threshold line sits against.
    const float preApprox = post - displayedGrDb;
    if (preApprox > displayedInDb) displayedInDb = preApprox;
    else                            displayedInDb += (preApprox - displayedInDb) * 0.15f;

    repaint();
}

void MasteringLimiterEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff20202a));   // raised panel surface
    g.setColour (juce::Colour (0xff3a3a46));
    g.drawRect (getLocalBounds(), 1);

    // LED-segment well matching the multiband-comp GR meters: dark gradient
    // body, inner shadow, and a 20-rung ladder of unlit segments. The live
    // signal fill draws over this.
    auto drawMeterBg = [&] (juce::Rectangle<float> bar)
    {
        juce::ColourGradient bg (juce::Colour (0xff0c0c0c), bar.getX(), bar.getY(),
                                  juce::Colour (0xff181818), bar.getX(), bar.getBottom(), false);
        g.setGradientFill (bg);
        g.fillRoundedRectangle (bar, 5.0f);
        g.setColour (juce::Colour (0xff000000).withAlpha (0.5f));
        g.drawRoundedRectangle (bar.reduced (1.0f), 4.0f, 1.0f);

        auto inner = bar.reduced (4.0f);
        constexpr int numSegments = 20;
        constexpr float gap = 2.0f;
        const float segH = (inner.getHeight() - (numSegments - 1) * gap) / numSegments;
        // Skip the ladder entirely when the meter is too short to fit positive-
        // height rungs (panel resized very small), so we never draw negative rects.
        if (segH > 0.0f)
            for (int s = 0; s < numSegments; ++s)
            {
                juce::Rectangle<float> seg (inner.getX(), inner.getY() + s * (segH + gap),
                                             inner.getWidth(), segH);
                g.setColour (juce::Colour (0xff242429));
                g.fillRoundedRectangle (seg, 2.0f);
                g.setColour (juce::Colour (0xff303036));
                if (seg.getWidth() > 4.0f)
                    g.fillRect (seg.getX() + 2.0f, seg.getY() + 1.0f, seg.getWidth() - 4.0f, 1.0f);
            }
    };

    auto drawCaption = [&] (juce::Rectangle<int> meter, const juce::String& caption)
    {
        g.setColour (juce::Colour (0xffa0a0a8));
        g.setFont (juce::Font (juce::FontOptions (10.5f, juce::Font::bold)));
        g.drawText (caption,
                     juce::Rectangle<int> (meter.getX() - 14, meter.getY() - 18,
                                            meter.getWidth() + 28, 14),
                     juce::Justification::centred, false);
    };

    // Threshold meter (live signal fills it; handle = drive setting)
    if (! threshMeterArea.isEmpty())
    {
        const auto bar = threshMeterArea.toFloat();
        drawMeterBg (bar);

        // Live fill - cyan gradient, fills upward.
        const float frac = juce::jlimit (0.0f, 1.0f,
            (juce::jlimit (kThreshMinDb, kThreshMaxDb, displayedInDb) - kThreshMinDb)
              / (kThreshMaxDb - kThreshMinDb));
        if (frac > 0.001f)
        {
            const float fillH = (bar.getHeight() - 4.0f) * frac;
            auto fillRect = juce::Rectangle<float> (bar.getX() + 2.0f,
                                                      bar.getBottom() - 2.0f - fillH,
                                                      bar.getWidth() - 4.0f, fillH);
            juce::ColourGradient grad (juce::Colour (0xffd06060), bar.getX(), bar.getY(),
                                         juce::Colour (0xff5ac8e0), bar.getX(), bar.getBottom(),
                                         false);
            g.setGradientFill (grad);
            g.fillRect (fillRect);
        }

        // Threshold handle (drag triangle on left). drive is 0..+20; the
        // handle sits at the threshold position = -drive (0..-20 dB).
        const float drive = params.limiterDriveDb.load (std::memory_order_relaxed);
        const float handleY = dbToY (-drive, kThreshMinDb, kThreshMaxDb, bar);

        // Handle = the full-width level line; the whole bar is the drag target.
        g.setColour (juce::Colour (0xff80b0e0).withAlpha (0.9f));
        g.drawLine (bar.getX(), handleY, bar.getRight(), handleY, 1.4f);

        drawCaption (threshMeterArea, "Threshold");

        // Threshold value box rides just under the handle line so the line,
        // triangle, and readout read as one control (shows -drive). Clamped to
        // stay inside the bar at the extremes.
        const float boxY = juce::jlimit (bar.getY() + 1.0f, bar.getBottom() - 15.0f,
                                          handleY + 1.0f);
        const auto valBox = juce::Rectangle<float> (bar.getX() + 2.0f, boxY,
                                                       bar.getWidth() - 4.0f, 14.0f);
        g.setColour (juce::Colour (0xff181820));
        g.fillRoundedRectangle (valBox, 2.0f);
        g.setColour (juce::Colour (0xff5a8ad0));
        g.drawRoundedRectangle (valBox, 2.0f, 0.6f);
        g.setColour (juce::Colour (0xff80b0e0));
        g.setFont (juce::Font (juce::FontOptions (9.5f, juce::Font::bold)));
        g.drawText (juce::String::formatted ("%.2f", -drive),
                     valBox, juce::Justification::centred, false);
    }

    // Ceiling meter
    if (! ceilingMeterArea.isEmpty())
    {
        const auto bar = ceilingMeterArea.toFloat();
        drawMeterBg (bar);

        const float frac = juce::jlimit (0.0f, 1.0f,
            (juce::jlimit (kCeilingMinDb, kCeilingMaxDb, displayedOutDb) - kCeilingMinDb)
              / (kCeilingMaxDb - kCeilingMinDb));
        if (frac > 0.001f)
        {
            const float fillH = (bar.getHeight() - 4.0f) * frac;
            auto fillRect = juce::Rectangle<float> (bar.getX() + 2.0f,
                                                      bar.getBottom() - 2.0f - fillH,
                                                      bar.getWidth() - 4.0f, fillH);
            juce::ColourGradient grad (juce::Colour (0xffd05050), bar.getX(), bar.getY(),
                                         juce::Colour (0xff5ac8e0), bar.getX(), bar.getBottom(),
                                         false);
            g.setGradientFill (grad);
            g.fillRect (fillRect);
        }

        const float ceiling = params.limiterCeilingDb.load (std::memory_order_relaxed);
        const float handleY = dbToY (ceiling, kCeilingMinDb, kCeilingMaxDb, bar);

        // Handle = the full-width level line; the whole bar is the drag target.
        g.setColour (juce::Colour (0xffe05050).withAlpha (0.9f));
        g.drawLine (bar.getX(), handleY, bar.getRight(), handleY, 1.4f);

        drawCaption (ceilingMeterArea, "Ceiling");

        // Ceiling value box rides just above the handle line (one control).
        const float boxY = juce::jlimit (bar.getY() + 1.0f, bar.getBottom() - 15.0f,
                                          handleY - 15.0f);
        const auto valBox = juce::Rectangle<float> (bar.getX() + 2.0f, boxY,
                                                       bar.getWidth() - 4.0f, 14.0f);
        g.setColour (juce::Colour (0xff181820));
        g.fillRoundedRectangle (valBox, 2.0f);
        g.setColour (juce::Colour (0xffe05050));
        g.drawRoundedRectangle (valBox, 2.0f, 0.6f);
        g.setColour (juce::Colour (0xffff8080));
        g.setFont (juce::Font (juce::FontOptions (9.5f, juce::Font::bold)));
        g.drawText (juce::String::formatted ("%.2f", ceiling),
                     valBox, juce::Justification::centred, false);
    }

    // Atten meter (live GR; fills downward from top)
    if (! attenMeterArea.isEmpty())
    {
        const auto bar = attenMeterArea.toFloat();
        drawMeterBg (bar);

        const float grAbs = juce::jlimit (0.0f, kAttenMaxDb, std::abs (displayedGrDb));
        const float frac = grAbs / kAttenMaxDb;
        if (frac > 0.001f)
        {
            const float fillH = (bar.getHeight() - 4.0f) * frac;
            auto fillRect = juce::Rectangle<float> (bar.getX() + 2.0f,
                                                      bar.getY() + 2.0f,
                                                      bar.getWidth() - 4.0f, fillH);
            juce::ColourGradient grad (juce::Colour (0xffe04040), bar.getX(), bar.getY(),
                                         juce::Colour (0xffe0c050), bar.getX(), bar.getBottom(),
                                         false);
            g.setGradientFill (grad);
            g.fillRect (fillRect);
        }
        drawCaption (attenMeterArea, "Atten");

        // GR scale ticks on the right (small vertical strip).
        g.setColour (juce::Colour (0xff707074));
        g.setFont (juce::Font (juce::FontOptions (8.5f)));
        for (auto entry : { std::pair<float, const char*> { 3.0f,  "3"  },
                            std::pair<float, const char*> { 6.0f,  "6"  },
                            std::pair<float, const char*> { 12.0f, "12" } })
        {
            const float frac01 = entry.first / kAttenMaxDb;
            const float y = bar.getY() + 2.0f + frac01 * (bar.getHeight() - 4.0f);
            g.drawText (entry.second,
                          juce::Rectangle<float> (bar.getRight() + 2.0f, y - 5.0f, 18.0f, 10.0f),
                          juce::Justification::centredLeft, false);
        }

        // GR value below the meter.
        g.setColour (juce::Colour (0xfff09060));
        g.setFont (juce::Font (juce::FontOptions (9.5f, juce::Font::bold)));
        g.drawText (juce::String::formatted ("%.1f", displayedGrDb),
                     juce::Rectangle<int> (attenMeterArea.getX(), attenMeterArea.getBottom() + 2,
                                            attenMeterArea.getWidth(), 12),
                     juce::Justification::centred, false);
    }

    // LUFS readout box
    if (! lufsBoxArea.isEmpty())
    {
        const auto box = lufsBoxArea.toFloat().reduced (1.0f);
        g.setColour (juce::Colour (0xff0d1218));
        g.fillRoundedRectangle (box, 3.0f);
        g.setColour (juce::Colour (0xff5ac8e0));
        g.drawRoundedRectangle (box, 3.0f, 0.8f);

        g.setColour (juce::Colour (0xff80c0d0));
        g.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
        g.drawText ("LUFS Long",
                     box.withHeight (16.0f).withTrimmedTop (4.0f),
                     juce::Justification::centred, false);

        const float iLufs = params.meterIntegratedLufs.load (std::memory_order_relaxed);
        g.setColour (juce::Colour (0xfff0f0f0));
        g.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                                                    24.0f, juce::Font::bold)));
        const auto valArea = box.withTrimmedTop (20.0f).withTrimmedBottom (20.0f);
        g.drawText (iLufs <= -99.0f ? juce::String ("--") : juce::String::formatted ("%.1f", iLufs),
                     valArea, juce::Justification::centred, false);

        // Bottom subtitles - dBTP + Short.
        const float tp = params.meterTruePeakDb.load (std::memory_order_relaxed);
        const float sLufs = params.meterShortTermLufs.load (std::memory_order_relaxed);
        g.setColour (juce::Colour (0xff909094));
        g.setFont (juce::Font (juce::FontOptions (8.5f)));
        const auto subArea = box.withTrimmedTop (box.getHeight() - 18.0f);
        g.drawText (tp <= -99.0f ? juce::String ("dBTP --")
                                  : juce::String::formatted ("dBTP %.1f", tp),
                     subArea.withWidth (subArea.getWidth() * 0.5f),
                     juce::Justification::centred, false);
        g.drawText (sLufs <= -99.0f ? juce::String ("Short --")
                                     : juce::String::formatted ("Short %.1f", sLufs),
                     subArea.withTrimmedLeft (subArea.getWidth() * 0.5f),
                     juce::Justification::centred, false);
    }
}

void MasteringLimiterEditor::resized()
{
    auto area = getLocalBounds().reduced (8);

    auto header = area.removeFromTop (22);
    if (headerBtn != nullptr) headerBtn->setBounds (header);
    area.removeFromTop (8);

    // Right column: Mode selector at top, Release knob, Stereo-link toggle.
    constexpr int kRightColW = 110;
    auto rightCol = area.removeFromRight (kRightColW);
    area.removeFromRight (8);
    {
        auto col = rightCol.reduced (4, 0);
        modeCaption.setBounds (col.removeFromTop (14));
        col.removeFromTop (2);
        modeCombo.setBounds   (col.removeFromTop (24));
        col.removeFromTop (12);

        const int kRelKnobH = 70;
        releaseLabel.setBounds (col.removeFromTop (14));
        const int knobY = col.getY();
        const int knobW = juce::jmin (col.getWidth(), kRelKnobH);
        const int knobX = col.getX() + (col.getWidth() - knobW) / 2;
        releaseKnob.setBounds (knobX, knobY, knobW, kRelKnobH);
        col.removeFromTop (kRelKnobH + 2);

        col.removeFromTop (8);
        lookaheadLabel.setBounds (col.removeFromTop (14));
        const int laY = col.getY();
        const int laW = juce::jmin (col.getWidth(), kRelKnobH);
        const int laX = col.getX() + (col.getWidth() - laW) / 2;
        lookaheadKnob.setBounds (laX, laY, laW, kRelKnobH);
        col.removeFromTop (kRelKnobH + 2);

        col.removeFromTop (8);
        stereoLinkToggle.setBounds (col.removeFromTop (22));
    }

    // LUFS readout sits at the bottom-right of the meter area, above the
    // Atten meter is the live LUFS box. Reserve a fixed-height block.
    constexpr int kLufsBoxH = 80;
    auto lufsRow = area.removeFromBottom (kLufsBoxH);
    area.removeFromBottom (4);

    // Three meter columns - Threshold, Ceiling, Atten. Equal widths with
    // small gaps; reserve top padding for "Threshold/Ceiling/Atten" caption
    // and bottom padding for the GR numeric readout.
    constexpr int kMeterTopPad    = 22;
    constexpr int kMeterBottomPad = 18;
    auto meters = area;
    meters.removeFromTop (kMeterTopPad);
    meters.removeFromBottom (kMeterBottomPad);

    constexpr int kMeterGap = 8;
    constexpr int kAttenW   = 18;   // narrower than threshold / ceiling
    constexpr int kHandlePad = 8;   // room on left for triangle handle
    const int twoColW = (meters.getWidth() - kAttenW - 2 * kMeterGap - kHandlePad * 2);
    const int colW = juce::jmax (12, twoColW / 2);

    meters.removeFromLeft (kHandlePad);
    threshMeterArea  = meters.removeFromLeft (colW);
    meters.removeFromLeft (kMeterGap + kHandlePad);
    ceilingMeterArea = meters.removeFromLeft (colW);
    meters.removeFromLeft (kMeterGap);
    attenMeterArea   = meters.removeFromLeft (kAttenW);

    // LUFS box centred under the meters.
    const int lufsBoxW = juce::jmin (180, lufsRow.getWidth());
    const int lufsX = lufsRow.getX() + (lufsRow.getWidth() - lufsBoxW) / 2;
    lufsBoxArea = juce::Rectangle<int> (lufsX, lufsRow.getY(), lufsBoxW, kLufsBoxH);
}

void MasteringLimiterEditor::mouseDown (const juce::MouseEvent& e)
{
    const auto p = e.getPosition();
    if (threshMeterArea.expanded (10, 4).contains (p))
    {
        currentDrag = DragMode::Threshold;
        params.limiterDriveDb.store (yToDriveDb (e.y), std::memory_order_relaxed);
        repaint();
    }
    else if (ceilingMeterArea.expanded (10, 4).contains (p))
    {
        currentDrag = DragMode::Ceiling;
        params.limiterCeilingDb.store (yToCeilingDb (e.y), std::memory_order_relaxed);
        repaint();
    }
    else
    {
        currentDrag = DragMode::None;
    }
}

void MasteringLimiterEditor::mouseDrag (const juce::MouseEvent& e)
{
    switch (currentDrag)
    {
        case DragMode::Threshold:
            params.limiterDriveDb.store (yToDriveDb (e.y), std::memory_order_relaxed);
            repaint();
            break;
        case DragMode::Ceiling:
            params.limiterCeilingDb.store (yToCeilingDb (e.y), std::memory_order_relaxed);
            repaint();
            break;
        case DragMode::None:
            break;
    }
}

void MasteringLimiterEditor::mouseUp (const juce::MouseEvent&)
{
    currentDrag = DragMode::None;
}

void MasteringLimiterEditor::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (threshMeterArea.expanded (10, 4).contains (e.getPosition()))
        params.limiterDriveDb.store (0.0f, std::memory_order_relaxed);
    else if (ceilingMeterArea.expanded (10, 4).contains (e.getPosition()))
        params.limiterCeilingDb.store (-0.3f, std::memory_order_relaxed);
    repaint();
}
} // namespace duskstudio
