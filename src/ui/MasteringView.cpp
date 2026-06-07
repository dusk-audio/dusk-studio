#include "MasteringView.h"
#include "BounceDialog.h"
#include "CompHeaderButton.h"
#include "DuskFileBrowser.h"
#include "MasteringEqEditor.h"
#include "MasteringLimiterEditor.h"
#include "../dsp/MultibandCompPresets.h"
#include "../engine/BounceEngine.h"
#include "../engine/MasteringPlayer.h"
#if DUSKSTUDIO_HAS_DUSK_DSP
  #include "ModernCompressorPanels.h"   // multi-comp - MultibandCompressorPanel
#endif

namespace duskstudio
{
WaveformDisplay::WaveformDisplay (MasteringPlayer& p)
    : player (p),
      thumbnail (512, formatManager, thumbnailCache)
{
    formatManager.registerBasicFormats();
    setOpaque (true);
    startTimerHz (20);
}

WaveformDisplay::~WaveformDisplay() { thumbnail.setSource (nullptr); }

void WaveformDisplay::setSource (const juce::File& file)
{
    if (file == juce::File()) { thumbnail.setSource (nullptr); repaint(); return; }
    thumbnail.setSource (new juce::FileInputSource (file));
    repaint();
}

void WaveformDisplay::timerCallback()
{
    const auto p = player.getPlayhead();
    if (p == lastPlayhead) return;
    lastPlayhead = p;
    repaint();   // cheap - small region
}

void WaveformDisplay::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff0e0e10));
    g.setColour (juce::Colour (0xff2a2a32));
    g.drawRect (getLocalBounds(), 1);

    if (thumbnail.getNumChannels() == 0
        || thumbnail.getTotalLength() <= 0.0)
    {
        g.setColour (juce::Colour (0xff707074));
        g.setFont (juce::Font (juce::FontOptions (13.0f)));
        g.drawText (juce::CharPointer_UTF8 ("No mix loaded - pick one with Load mix..."),
                     getLocalBounds(), juce::Justification::centred, false);
        return;
    }

    auto bounds = getLocalBounds().reduced (4);
    g.setColour (juce::Colour (0xff5a8ad0));
    thumbnail.drawChannels (g, bounds, 0.0, thumbnail.getTotalLength(), 1.0f);

    // Playhead line.
    const double sr = player.getSourceSampleRate();
    const double total = thumbnail.getTotalLength();
    if (sr > 0.0 && total > 0.0)
    {
        const double sec  = (double) player.getPlayhead() / sr;
        const float  frac = (float) juce::jlimit (0.0, 1.0, sec / total);
        const int    x    = bounds.getX() + (int) (frac * bounds.getWidth());
        g.setColour (juce::Colour (0xffe04040));
        g.drawVerticalLine (x, (float) bounds.getY(), (float) bounds.getBottom());
    }
}

void WaveformDisplay::mouseDown (const juce::MouseEvent& e)
{
    const double sr = player.getSourceSampleRate();
    const double total = thumbnail.getTotalLength();
    if (sr <= 0.0 || total <= 0.0) return;
    auto bounds = getLocalBounds().reduced (4);
    if (bounds.getWidth() <= 0) return;
    const float frac = juce::jlimit (0.0f, 1.0f,
                                       (float) (e.x - bounds.getX()) / (float) bounds.getWidth());
    const auto target = (juce::int64) (frac * total * sr);
    player.setPlayhead (target);
    repaint();
}
namespace
{
// Streaming / broadcast target presets. Index 0 is the no-target case;
// indices 1..N correspond to the dropdown items. Values are LUFS (integrated
// program loudness) and true-peak ceiling in dBTP. Source: the platforms'
// published mastering recommendations as of 2024.
struct MasteringTarget { const char* name; float lufs; float ceilingDbTP; };
constexpr MasteringTarget kMasteringTargets[] =
{
    { "Off",                   0.0f,    0.0f },   // index 0: neutral display
    { "Spotify",              -14.0f,  -1.0f },
    { "Apple Music",          -16.0f,  -1.0f },
    { "YouTube",              -14.0f,  -1.0f },
    { "Tidal",                -14.0f,  -1.0f },
    { "Broadcast (EBU R128)", -23.0f,  -1.0f },
};
constexpr int kNumMasteringTargets = (int) (sizeof (kMasteringTargets) / sizeof (kMasteringTargets[0]));
} // namespace

MasteringView::MasteringView (Session& s, AudioEngine& e)
    : session (s), engine (e)
{
    // ── header ──
    sourceFileLabel.setText ("No mix loaded", juce::dontSendNotification);
    sourceFileLabel.setColour (juce::Label::textColourId, juce::Colour (0xffd0d0d0));
    sourceFileLabel.setJustificationType (juce::Justification::centredLeft);
    sourceFileLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
    addAndMakeVisible (sourceFileLabel);

    loadButton.onClick        = [this] { doLoadPrompt(); };
    loadLatestMixdown.onClick = [this] { doLoadLatestMixdown(); };
    addAndMakeVisible (loadButton);
    addAndMakeVisible (loadLatestMixdown);

    // ── transport ──
    playButton.onClick   = [this] { engine.getMasteringPlayer().play(); };
    stopButton.onClick   = [this] { engine.getMasteringPlayer().stop(); };
    rewindButton.onClick = [this] { engine.getMasteringPlayer().setPlayhead (0); };
    addAndMakeVisible (playButton);
    addAndMakeVisible (stopButton);
    addAndMakeVisible (rewindButton);

    clockLabel.setJustificationType (juce::Justification::centred);
    clockLabel.setColour (juce::Label::textColourId, juce::Colour (0xffe0e0e0));
    clockLabel.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                                                        16.0f, juce::Font::bold)));
    clockLabel.setText ("00:00.000", juce::dontSendNotification);
    addAndMakeVisible (clockLabel);

    grLabel.setJustificationType (juce::Justification::centred);
    grLabel.setColour (juce::Label::textColourId, juce::Colour (0xff909094));
    grLabel.setFont (juce::Font (juce::FontOptions (11.0f)));
    addAndMakeVisible (grLabel);

    auto& m = session.mastering();

    // ── Meter + LUFS ──
    auto styleMeter = [] (juce::Label& l)
    {
        l.setJustificationType (juce::Justification::centred);
        l.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                                                    11.0f, juce::Font::plain)));
        l.setColour (juce::Label::textColourId, juce::Colour (0xffd0d0d0));
        l.setColour (juce::Label::backgroundColourId, juce::Colour (0xff121214));
    };
    styleMeter (meterL);
    styleMeter (meterR);
    styleMeter (lufsM);
    styleMeter (lufsS);
    styleMeter (lufsI);
    styleMeter (truePeak);
    lufsI.setColour (juce::Label::backgroundColourId, juce::Colour (0xff1a2228));  // brighter - the target reading
    addAndMakeVisible (meterL);
    addAndMakeVisible (meterR);
    addAndMakeVisible (lufsM);
    addAndMakeVisible (lufsS);
    addAndMakeVisible (lufsI);
    addAndMakeVisible (truePeak);

    resetLoudness.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff202024));
    resetLoudness.setColour (juce::TextButton::textColourOffId, juce::Colour (0xffd0d0d0));
    resetLoudness.setTooltip ("Reset integrated LUFS history (Cmd+R inside this view)");
    resetLoudness.onClick = [this] { engine.getMasteringChain().resetLoudness(); };
    addAndMakeVisible (resetLoudness);

    // Streaming-target preset dropdown. Populated 1..N (index 0 is "Off").
    // ComboBox uses 1-based item IDs; we map item-id N → array index N-1.
    targetCaption.setText ("Target", juce::dontSendNotification);
    targetCaption.setJustificationType (juce::Justification::centredRight);
    targetCaption.setColour (juce::Label::textColourId, juce::Colour (0xff707074));
    targetCaption.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
    addAndMakeVisible (targetCaption);

    masteringTargetCombo.setTooltip (
        "Streaming / broadcast loudness target. When set, "
        "the I-LUFS cell glows green inside +/-0.5 LU of the "
        "target, yellow inside +/-2 LU, red beyond - and the "
        "TP cell glows red if the true peak exceeds the "
        "platform's ceiling.");
    for (int i = 0; i < kNumMasteringTargets; ++i)
    {
        const auto& t = kMasteringTargets[i];
        juce::String label (t.name);
        if (i > 0)
            label += juce::String::formatted ("  (%g LUFS / %g dBTP)", t.lufs, t.ceilingDbTP);
        masteringTargetCombo.addItem (label, i + 1);
    }
    const int idx = juce::jlimit (0, kNumMasteringTargets - 1,
                                    session.mastering().targetPresetIndex.load());
    masteringTargetCombo.setSelectedId (idx + 1, juce::dontSendNotification);
    masteringTargetCombo.onChange = [this]
    {
        const int newIdx = masteringTargetCombo.getSelectedId() - 1;
        session.mastering().targetPresetIndex.store (
            juce::jlimit (0, kNumMasteringTargets - 1, newIdx));
    };
    addAndMakeVisible (masteringTargetCombo);

    // ── Export ──
    exportButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff2a3a48));
    exportButton.setColour (juce::TextButton::textColourOffId, juce::Colour (0xffd0e0f0));
    exportButton.onClick = [this] { doExport(); };
    addAndMakeVisible (exportButton);

    // ── Waveform display (top of view) ──
    waveform = std::make_unique<WaveformDisplay> (engine.getMasteringPlayer());
    addAndMakeVisible (waveform.get());

    // ── Custom Digital EQ editor (curve + band controls) ──
    eqEditor = std::make_unique<MasteringEqEditor> (session.mastering());
    addAndMakeVisible (eqEditor.get());

    // ── Embedded Multiband Comp editor + header wrapper ──
    // The mastering chain's UniversalCompressor is in Multiband mode (7),
    // so the editor we get back is the Multi-Comp's multiband UI. Wrap it
    // in a panel that has its own title + ON toggle so the user can bypass
    // the multiband comp from the same place as the EQ / Limiter sections.
    compPanelWrapper = std::make_unique<juce::Component>();
    compPanelWrapper->setOpaque (true);
    addAndMakeVisible (compPanelWrapper.get());

    compHeaderBtn = std::make_unique<CompHeaderButton> (
        [this] { return session.mastering().compEnabled.load (std::memory_order_relaxed); },
        [this]
        {
            auto& mp = session.mastering();
            const bool now = ! mp.compEnabled.load (std::memory_order_relaxed);
            mp.compEnabled.store (now, std::memory_order_relaxed);
            if (compHeaderBtn != nullptr) compHeaderBtn->repaint();
        });
    compHeaderBtn->setLabelText ("MULTIBAND COMP");
    compHeaderBtn->setAccentColour (juce::Colour (0xffe0c050));   // gold — comp
    compHeaderEnabledSeen = session.mastering().compEnabled.load (std::memory_order_relaxed);
    compPanelWrapper->addAndMakeVisible (compHeaderBtn.get());

    // Embed ONLY the donor's MultibandCompressorPanel rather than the full
    // EnhancedCompressorEditor (which carries the mode selector + extra
    // mode panels we don't want surfaced in the mastering view). The
    // mastering chain has already pinned the UC into multiband mode, so
    // hosting the multiband panel by itself gives us the focused UI we
    // actually need.
#if DUSKSTUDIO_HAS_DUSK_DSP
    if (auto* compProc = engine.getMasteringChain().getCompProcessor())
    {
        compEditor = std::make_unique<MultibandCompressorPanel> (compProc->getParameters());
        compPanelWrapper->addAndMakeVisible (compEditor.get());
    }

    // DP-24-style 3-band preset picker in the comp header.
    compPresetCombo.addItem ("Presets...", 1);
    for (int i = 0; i < mbpresets::kNumPresets; ++i)
        compPresetCombo.addItem (mbpresets::kPresets[(size_t) i].name, i + 2);
    compPresetCombo.setSelectedId (1, juce::dontSendNotification);
    compPresetCombo.setTooltip ("Apply a DP-24-style 3-band mastering compressor preset. "
                                  "Maps onto the multiband comp with the high-mid band disabled.");
    compPresetCombo.onChange = [this]
    {
        const int id = compPresetCombo.getSelectedId();
        if (id >= 2)
        {
            applyMultibandPreset (id - 2);
            // One-shot: drop back to the placeholder so re-picking re-applies.
            compPresetCombo.setSelectedId (1, juce::dontSendNotification);
        }
    };
    compPanelWrapper->addAndMakeVisible (compPresetCombo);
#endif

    // ── Custom Limiter editor ──
    limiterEditor = std::make_unique<MasteringLimiterEditor> (
        session.mastering(), engine.getMasteringChain().getLimiter());
    addAndMakeVisible (limiterEditor.get());

    // Reflect the loaded source file from the session, if any.
    if (m.sourceFile != juce::File())
        loadFile (m.sourceFile);

    updateLabels();
    startTimerHz (20);
}

MasteringView::~MasteringView() = default;

void MasteringView::applyMultibandPreset (int presetIndex)
{
#if DUSKSTUDIO_HAS_DUSK_DSP
    if (presetIndex < 0 || presetIndex >= mbpresets::kNumPresets) return;
    auto* comp = engine.getMasteringChain().getCompProcessor();
    if (comp == nullptr) return;

    auto& apvts = comp->getParameters();
    const auto& preset = mbpresets::kPresets[(size_t) presetIndex];

    const auto setP = [&apvts] (const juce::String& id, float value)
    {
        if (auto* param = apvts.getParameter (id))
            param->setValueNotifyingHost (
                juce::jlimit (0.0f, 1.0f, apvts.getParameterRange (id).convertTo0to1 (value)));
    };
    const auto setBand = [&setP] (const char* band, const mbpresets::Band& b)
    {
        const juce::String pre = "mb_" + juce::String (band) + "_";
        setP (pre + "threshold", b.thresholdDb);
        setP (pre + "ratio",     b.ratio);
        setP (pre + "makeup",    b.makeupDb);
        setP (pre + "attack",    b.attackMs);
        setP (pre + "release",   b.releaseMs);
        setP (pre + "enabled",   1.0f);
        setP (pre + "bypass",    0.0f);
        setP (pre + "solo",      0.0f);
    };

    setBand ("low",    preset.low);
    setBand ("lowmid", preset.mid);
    setBand ("high",   preset.high);

    // 3-band emulation: disable the high-mid band and collapse its width so the
    // lowmid/high split lands on the chart's Hi-Xover. crossover_2 maxes at
    // 5 kHz, so presets with a higher Hi-Xover leave a thin uncompressed sliver.
    setP ("mb_highmid_enabled", 0.0f);
    setP ("mb_crossover_1", preset.lowXoverHz);
    const float c2 = juce::jmin (preset.hiXoverHz, 5000.0f);
    setP ("mb_crossover_2", c2);
    setP ("mb_crossover_3", juce::jmax (preset.hiXoverHz, c2));

    setP ("auto_makeup", 0.0f);   // Choice: 0 = Off, matching the chart
#else
    juce::ignoreUnused (presetIndex);
#endif
}

void MasteringView::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff0d0d0f));
}

void MasteringView::resized()
{
    auto area = getLocalBounds().reduced (12);

    // ── Header (load buttons + filename label) ──
    auto header = area.removeFromTop (28);
    loadButton.setBounds (header.removeFromLeft (110));
    header.removeFromLeft (4);
    loadLatestMixdown.setBounds (header.removeFromLeft (180));
    header.removeFromLeft (12);
    sourceFileLabel.setBounds (header);
    area.removeFromTop (8);

    // ── Transport row (now also hosts the LUFS readouts so the
    //    loudness target context sits next to the playback cluster
    //    instead of being buried at the bottom of the view). Width-
    //    clamped placement so narrow windows don't render widgets
    //    with negative widths. ──
    auto transportRow = area.removeFromTop (36);
    const auto place = [&transportRow] (juce::Component& c, int preferredW, int gapAfter = 0)
    {
        const int available = transportRow.getWidth();
        if (available <= 0) { c.setVisible (false); return; }
        c.setVisible (true);
        const int w = juce::jmin (preferredW, available);
        c.setBounds (transportRow.removeFromLeft (w));
        if (gapAfter > 0) transportRow.removeFromLeft (juce::jmin (gapAfter, transportRow.getWidth()));
    };
    place (rewindButton,    50,  4);
    place (playButton,      70,  4);
    place (stopButton,      70, 16);
    place (clockLabel,     140, 12);
    place (grLabel,        140, 12);
    place (truePeak,       100,  4);
    place (lufsM,           90,  4);
    place (lufsS,           90,  4);
    place (lufsI,          120,  6);
    place (resetLoudness,   60, 16);
    // Target sits with the loudness cluster it governs: the integrated-LUFS
    // (I) cell glows green when within range of this target.
    place (targetCaption,   46,  4);
    place (masteringTargetCombo, 260, 0);
    area.removeFromTop (8);

    // ── Bottom strip (export + L/R meters). LUFS readouts + target moved
    //    up to the transport row above; the reclaimed height goes to the
    //    plugin panels. ──
    auto bottom = area.removeFromBottom (32);
    area.removeFromBottom (6);

    auto meterRow = bottom.removeFromTop (28);
    exportButton.setBounds (meterRow.removeFromRight (160));
    meterRow.removeFromRight (12);
    meterR.setBounds (meterRow.removeFromRight (140));
    meterRow.removeFromRight (4);
    meterL.setBounds (meterRow.removeFromRight (140));

    // ── Waveform: a slim band at the top so the plugin row gets the bulk
    //    of the vertical real estate. Fixed height; what's left in `area`
    //    above the bottom strip and the panel row goes to the plugins.
    constexpr int kWaveH = 90;
    if (waveform != nullptr)
        waveform->setBounds (area.removeFromTop (kWaveH));
    area.removeFromTop (6);

    // ── 3-panel row: Digital EQ | Multiband Comp | Limiter ──
    // The EQ curve + 5-band knob row reads better with more width, and
    // the multiband panel's 4 columns are still legible at a smaller
    // share. Limiter is naturally narrow so it gets the leftover.
    auto panelsRow = area;

    constexpr int kPanelGap = 8;
    const int totalW    = juce::jmax (0, panelsRow.getWidth() - 2 * kPanelGap);
    const int eqW       = (int) std::round (totalW * 0.42);
    // Limiter shrunk 0.22 -> 0.14. Two sliders + a release knob don't
    // need a fifth of the row; the freed width goes to the multiband
    // comp panel which has 4 columns of GR bars + a row of knobs.
    const int limW      = (int) std::round (totalW * 0.14);
    const int compW     = totalW - eqW - limW;

    auto eqPanel   = panelsRow.removeFromLeft (eqW);
    panelsRow.removeFromLeft (kPanelGap);
    auto compPanel = panelsRow.removeFromLeft (compW);
    panelsRow.removeFromLeft (kPanelGap);
    auto limPanel  = panelsRow;
    juce::ignoreUnused (limW);

    // EQ panel - custom curve + band-controls editor.
    if (eqEditor != nullptr)
        eqEditor->setBounds (eqPanel);

    // Multiband Comp panel - wrapper hosts its own title + ON toggle on
    // top, then the embedded plugin editor takes the rest of the panel.
    if (compPanelWrapper != nullptr)
    {
        compPanelWrapper->setBounds (compPanel);
        auto inner = compPanelWrapper->getLocalBounds().reduced (8);
        auto headerRow = inner.removeFromTop (20);
#if DUSKSTUDIO_HAS_DUSK_DSP
        // Preset picker on the right of the header, the ON-toggle pill on the left.
        const int presetW = juce::jlimit (0, 130, headerRow.getWidth() / 2 - 2);
        compPresetCombo.setBounds (headerRow.removeFromRight (presetW));
        headerRow.removeFromRight (4);
#endif
        if (compHeaderBtn != nullptr) compHeaderBtn->setBounds (headerRow);
        inner.removeFromTop (4);
        if (compEditor != nullptr)
            compEditor->setBounds (inner);
    }

    // Limiter panel - custom Waves L4-style editor.
    if (limiterEditor != nullptr)
        limiterEditor->setBounds (limPanel);
}

void MasteringView::timerCallback()
{
    auto& player = engine.getMasteringPlayer();
    auto& m = session.mastering();

    // The comp header button only repaints itself on click; pick up external
    // toggles (e.g. session load) here.
    const bool compOn = m.compEnabled.load (std::memory_order_relaxed);
    if (compOn != compHeaderEnabledSeen)
    {
        compHeaderEnabledSeen = compOn;
        if (compHeaderBtn != nullptr) compHeaderBtn->repaint();
    }

    // Clock from the player playhead.
    const double sr = player.getSourceSampleRate();
    const double sec = (sr > 0.0) ? (double) player.getPlayhead() / sr : 0.0;
    const int mins = (int) (sec / 60.0);
    const int secs = (int) sec % 60;
    const int ms   = (int) ((sec - std::floor (sec)) * 1000.0);
    clockLabel.setText (juce::String::formatted ("%02d:%02d.%03d", mins, secs, ms),
                         juce::dontSendNotification);

    // Meters.
    auto fmt = [] (float dB)
    {
        if (dB <= -99.0f) return juce::String ("  -inf");
        return juce::String::formatted ("%6.1f dB", dB);
    };
    meterL.setText ("L " + fmt (m.meterPostMasterLDb.load()), juce::dontSendNotification);
    meterR.setText ("R " + fmt (m.meterPostMasterRDb.load()), juce::dontSendNotification);

    grLabel.setText (juce::String::formatted ("Comp GR %5.1f dB    Lim GR %5.1f dB",
                                                 m.meterCompGrDb.load(),
                                                 m.meterLimiterGrDb.load()),
                      juce::dontSendNotification);

    // LUFS / true peak. -100 sentinel renders as "-" (em-dash via UTF-8
    // so JUCE doesn't decode it as Latin-1 mojibake).
    auto fmtLufs = [] (float v)
    {
        if (v <= -99.0f) return juce::String (juce::CharPointer_UTF8 ("    -"));
        return juce::String::formatted ("%6.1f LUFS", v);
    };
    lufsM.setText ("M " + fmtLufs (m.meterMomentaryLufs.load()),  juce::dontSendNotification);
    lufsS.setText ("S " + fmtLufs (m.meterShortTermLufs.load()),  juce::dontSendNotification);

    const float iLufs = m.meterIntegratedLufs.load();
    lufsI.setText ("I " + fmtLufs (iLufs), juce::dontSendNotification);

    const float tp = m.meterTruePeakDb.load();
    truePeak.setText (tp <= -99.0f ? juce::String (juce::CharPointer_UTF8 ("TP   -"))
                                   : juce::String::formatted ("TP %5.1f dBTP", tp),
                       juce::dontSendNotification);

    // Streaming-target color coding. Off = neutral; otherwise:
    //   I-LUFS cell: green inside ±0.5 LU, yellow inside ±2 LU, red beyond.
    //   TP cell:     green at-or-below ceiling, red above.
    // No data (sentinel) keeps the neutral colour so a fresh load doesn't
    // flash green spuriously.
    const int targetIdx = juce::jlimit (0, kNumMasteringTargets - 1,
                                          m.targetPresetIndex.load());
    const auto neutralBg = juce::Colour (0xff121214);
    const auto greenBg   = juce::Colour (0xff1a3a1a);
    const auto yellowBg  = juce::Colour (0xff3a3a1a);
    const auto redBg     = juce::Colour (0xff3a1a1a);

    if (targetIdx == 0 || iLufs <= -99.0f)
    {
        lufsI.setColour (juce::Label::backgroundColourId,
                          juce::Colour (0xff1a2228));  // the "target" highlight
    }
    else
    {
        const float diff = std::fabs (iLufs - kMasteringTargets[targetIdx].lufs);
        const auto bg = (diff <= 0.5f) ? greenBg
                       : (diff <= 2.0f) ? yellowBg
                                        : redBg;
        lufsI.setColour (juce::Label::backgroundColourId, bg);
    }

    if (targetIdx == 0 || tp <= -99.0f)
    {
        truePeak.setColour (juce::Label::backgroundColourId, neutralBg);
    }
    else
    {
        const auto bg = (tp <= kMasteringTargets[targetIdx].ceilingDbTP)
                          ? greenBg : redBg;
        truePeak.setColour (juce::Label::backgroundColourId, bg);
    }

    // Reflect play-button state - the player auto-stops at EOF.
    playButton.setEnabled (player.isLoaded() && ! player.isPlaying());
    stopButton.setEnabled (player.isPlaying());
}

void MasteringView::updateLabels()
{
    const auto& m = session.mastering();
    if (m.sourceFile == juce::File())
        sourceFileLabel.setText ("No mix loaded", juce::dontSendNotification);
    else
        sourceFileLabel.setText (m.sourceFile.getFileName()
                                  + "  (" + m.sourceFile.getParentDirectory().getFileName() + "/)",
                                  juce::dontSendNotification);
}

bool MasteringView::loadFile (const juce::File& file)
{
    if (! engine.getMasteringPlayer().loadFile (file))
    {
        sourceFileLabel.setText ("Failed to load: " + file.getFullPathName(),
                                  juce::dontSendNotification);
        if (waveform != nullptr) waveform->setSource (juce::File());
        return false;
    }
    session.mastering().sourceFile = file;
    // Reset the integrated LUFS history so the reading reflects ONLY the
    // currently-loaded mix, not a mix-of-mixes from prior auditions.
    engine.getMasteringChain().resetLoudness();
    if (waveform != nullptr) waveform->setSource (file);
    updateLabels();
    return true;
}

void MasteringView::doLoadPrompt()
{
    auto startDir = session.mastering().sourceFile.getParentDirectory();
    if (! startDir.isDirectory())
        startDir = session.getSessionDirectory();

    filebrowser::open (*this, {
        /*title*/                  "Load mix to master",
        /*initialFileOrDirectory*/ startDir,
        /*filePatternsAllowed*/    "*.wav;*.aiff;*.flac",
        /*mode*/                   filebrowser::Mode::Open,
        /*warnAboutOverwriting*/   false,
        /*selectDirectories*/      false,
    },
    [this] (juce::File chosen)
    {
        if (chosen != juce::File()) loadFile (chosen);
    });
}

void MasteringView::doLoadLatestMixdown()
{
    // Convention: <sessionDir>/mixdown.wav is what the Mixing-stage
    // "Mixdown" button writes. Falls back to bounce.wav (the existing
    // free-form bounce target) if mixdown.wav doesn't exist.
    const auto dir = session.getSessionDirectory();
    auto candidate = dir.getChildFile ("mixdown.wav");
    if (! candidate.existsAsFile())
        candidate = dir.getChildFile ("bounce.wav");
    if (candidate.existsAsFile())
        loadFile (candidate);
    else
        sourceFileLabel.setText ("No mixdown.wav or bounce.wav in session dir",
                                  juce::dontSendNotification);
}

void MasteringView::doExport()
{
    if (! engine.getMasteringPlayer().isLoaded())
    {
        sourceFileLabel.setText ("Load a mix first, then export.",
                                  juce::dontSendNotification);
        return;
    }

    auto target = session.getSessionDirectory().getChildFile ("master.wav");

    auto panel = std::make_unique<BounceDialog> (engine, session,
                                                   engine.getDeviceManager(),
                                                   target, BounceEngine::Mode::MasteringChain);
    panel->setSize (520, 200);
    exportModal.show (*this, std::move (panel));
}
} // namespace duskstudio
