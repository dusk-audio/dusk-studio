#include "MasteringView.h"
#include "BounceDialog.h"
#include "DimOverlay.h"
#include "DuskAlerts.h"
#include "DuskContextMenu.h"
#include "DuskFileBrowser.h"
#include "EmbeddedModal.h"
#include "SaveTargetChecks.h"
#include "SplitModuleButton.h"
#include "../dsp/MultibandCompPresets.h"
#include "../engine/BounceEngine.h"
#include "../engine/MasteringPlayer.h"
#include "../engine/audiofile/FileReader.h"
#include "../session/CompParams.h"
#if DUSKSTUDIO_HAS_NATIVE_UI
 #include "NativeEditorEmbedScale.h"
 #include "imgui/DuskPanelWindow.h"
 #include "imgui/MasteringEqView.h"
 #include "imgui/MasteringLimiterView.h"
 #include "imgui/MasteringPhaseView.h"
#endif
#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace duskstudio
{
namespace
{
template <typename T>
inline T jlimit (T lo, T hi, T value) noexcept { return std::clamp (value, lo, std::max (lo, hi)); }

#if DUSKSTUDIO_HAS_NATIVE_UI
// Where a panel's framework child belongs, from the rectangle its proxy occupies in the
// stage.
imgui::DuskPanelWindow::Geometry nativePanelGeometry (const juce::Component& view,
                                                      const juce::Component& proxy)
{
    auto* const topLevel = view.getTopLevelComponent();
    if (topLevel == nullptr)
        return {};
    const auto logical = topLevel->getLocalArea (&view, proxy.getBounds());
    const auto g = embedscale::childGeometryFor (*topLevel, logical);
    return { g.x, g.y, g.width, g.height, g.scale };
}
#endif
}

WaveformDisplay::WaveformDisplay (MasteringPlayer& p)
    : player (p)
{
    setOpaque (true);
    startTimerHz (20);
}

WaveformDisplay::~WaveformDisplay() { stopTimer(); }

void WaveformDisplay::followPlayer()
{
    const auto generation = player.getSourceGeneration();
    if (generation == shownSourceGeneration) return;
    shownSourceGeneration = generation;
    shownFile = std::filesystem::u8path (player.getLoadedFile().getFullPathName().toStdString());
    waveformSource.setFile (shownFile);
    waveformSnapshot = waveformSource.snapshot();
    repaint();
}

void WaveformDisplay::timerCallback()
{
    followPlayer();
    const auto p = player.getPlayhead();
    auto next = waveformSource.snapshot();
    if (p == lastPlayhead && next.state == waveformSnapshot.state
        && next.peaks == waveformSnapshot.peaks) return;
    waveformSnapshot = std::move (next);
    lastPlayhead = p;
    repaint();   // cheap - small region
}

void WaveformDisplay::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff0e0e10));
    g.setColour (juce::Colour (0xff2a2a32));
    g.drawRect (getLocalBounds(), 1);

    const auto& peaks = waveformSnapshot.peaks;
    if (! peaks || peaks->info().numFrames == 0)
    {
        g.setColour (juce::Colour (0xff707074));
        g.setFont (juce::Font (juce::FontOptions (13.0f)));
        const auto message = waveformSnapshot.state == dusk::audio::WaveformSource::State::Loading
            ? "Loading waveform..."
            : waveformSnapshot.state == dusk::audio::WaveformSource::State::Failed
                ? "Waveform unavailable" : "No mix loaded - pick one with Load mix...";
        g.drawText (message,
                     getLocalBounds(), juce::Justification::centred, false);
        return;
    }

    auto bounds = getLocalBounds().reduced (4);
    if (bounds.isEmpty()) return;

    const double sr      = player.getSourceSampleRate();
    const double total   = peaks->durationSeconds();
    const double playSecRaw = (sr > 0.0) ? (double) player.getPlayhead() / sr : 0.0;
    const double playSec = jlimit (0.0, total, playSecRaw);
    const float  frac    = (total > 0.0) ? (float) (playSec / total) : 0.0f;
    const int    splitX  = bounds.getX() + (int) (frac * bounds.getWidth());

    // Centre line.
    g.setColour (juce::Colour (0xff202028));
    g.drawHorizontalLine (bounds.getCentreY(), (float) bounds.getX(), (float) bounds.getRight());

    // Keep the time-to-pixel mapping fixed as the played colour boundary moves.
    const auto frames = peaks->info().numFrames;
    const int channels = peaks->info().numChannels;
    for (int channel = 0; channel < channels; ++channel)
    {
        const float top = (float) (bounds.getY() + channel * bounds.getHeight() / channels);
        const float bottom = (float) (bounds.getY() + (channel + 1) * bounds.getHeight() / channels);
        const float centre = (top + bottom) * 0.5f;
        const float scale = (bottom - top) * 0.5f;
        for (int pixel = 0; pixel < bounds.getWidth(); ++pixel)
        {
            const auto firstFrame = (std::int64_t) ((double) pixel * (double) frames / bounds.getWidth());
            const auto endFrame = std::max (firstFrame + 1,
                (std::int64_t) std::ceil ((double) (pixel + 1) * (double) frames / bounds.getWidth()));
            const auto peak = peaks->query (channel, firstFrame, endFrame);
            if (! peak || (peak->minimum >= 0.0f && peak->maximum <= 0.0f)) continue;
            const int x = bounds.getX() + pixel;
            g.setColour (x < splitX ? juce::Colour (0xff7ab0ee) : juce::Colour (0xff3c5c84));
            const float y1 = std::clamp (centre - std::clamp (peak->maximum, -1.0f, 1.0f) * scale - 0.3f, top, bottom);
            const float y2 = std::clamp (centre - std::clamp (peak->minimum, -1.0f, 1.0f) * scale + 0.3f, top, bottom);
            g.drawVerticalLine (x, y1, y2);
        }
    }

    // Time ruler along the bottom - a handful of mm:ss ticks.
    if (total > 0.0)
    {
        double step = 30.0;
        for (double s : { 1.0, 2.0, 5.0, 10.0, 15.0, 30.0, 60.0, 120.0, 300.0 })
            if (total / s <= 10.0) { step = s; break; }
        g.setFont (juce::Font (juce::FontOptions (9.0f)));
        for (double t = 0.0; t <= total; t += step)
        {
            const int x = bounds.getX() + (int) ((t / total) * bounds.getWidth());
            g.setColour (juce::Colour (0xff34343e));
            g.drawVerticalLine (x, (float) bounds.getBottom() - 7.0f, (float) bounds.getBottom());
            g.setColour (juce::Colour (0xff707078));
            g.drawText (juce::String::formatted ("%d:%02d", (int) (t / 60.0), (int) t % 60),
                         x + 2, bounds.getBottom() - 12, 38, 10,
                         juce::Justification::left, false);
        }
    }

    // Playhead - bright line + a small flag at the top.
    if (sr > 0.0 && total > 0.0)
    {
        g.setColour (juce::Colour (0xffff5555));
        g.drawVerticalLine (splitX, (float) bounds.getY(), (float) bounds.getBottom());
        juce::Path flag;
        flag.addTriangle ((float) splitX - 4.0f, (float) bounds.getY(),
                          (float) splitX + 4.0f, (float) bounds.getY(),
                          (float) splitX,        (float) bounds.getY() + 6.0f);
        g.setColour (juce::Colour (0xffff7777));
        g.fillPath (flag);
    }
}

void WaveformDisplay::mouseDown (const juce::MouseEvent& e)
{
    const auto frames = player.getLengthSamples();
    if (! player.isLoaded() || frames <= 0
        || waveformSnapshot.state == dusk::audio::WaveformSource::State::Empty) return;
    auto bounds = getLocalBounds().reduced (4);
    if (bounds.getWidth() <= 0) return;
    const float frac = jlimit (0.0f, 1.0f,
                                       (float) (e.x - bounds.getX()) / (float) bounds.getWidth());
    const auto target = (std::int64_t) ((double) frac * (double) frames);
    player.setPlayhead (target);
    repaint();
}
namespace
{
// Reference loudness targets, one row per distinct LUFS / dBTP pair; the
// services named on a row share its target. Index 0 is the no-target case.
// Values are integrated program loudness in LUFS and the true-peak ceiling in
// dBTP.
struct MasteringTarget { const char* name; float lufs; float ceilingDbTP; };
constexpr MasteringTarget kMasteringTargets[] =
{
    { "Off",                        0.0f,    0.0f },   // index 0: neutral display
    { "Spotify / YouTube / Tidal", -14.0f,  -1.0f },
    { "Apple Music",               -16.0f,  -1.0f },
    { "Broadcast (EBU R128)",      -23.0f,  -1.0f },
};
constexpr int kNumMasteringTargets = (int) (sizeof (kMasteringTargets) / sizeof (kMasteringTargets[0]));
// The session loader clamps the saved index without seeing this table; adding a
// preset here without widening the clamp would silently drop it on reload.
static_assert (kNumMasteringTargets == MasteringParams::kNumTargetPresets,
               "kMasteringTargets and MasteringParams::kNumTargetPresets must agree");
} // namespace

MasteringView::MasteringView (Session& s, AudioEngine& e)
    : session (s), engine (e)
{
    // A clicked button would otherwise take keyboard focus and eat Space
    // (juce::Button triggers on it), so the global play/stop toggle in
    // MainComponent::keyPressed would go dead after any click in here.
    loadButton.setMouseClickGrabsKeyboardFocus (false);
    loadLatestMixdown.setMouseClickGrabsKeyboardFocus (false);
    playButton.setMouseClickGrabsKeyboardFocus (false);
    stopButton.setMouseClickGrabsKeyboardFocus (false);
    rewindButton.setMouseClickGrabsKeyboardFocus (false);
    resetLoudness.setMouseClickGrabsKeyboardFocus (false);
    exportButton.setMouseClickGrabsKeyboardFocus (false);

    // header
    sourceFileLabel.setText ("No mix loaded", juce::dontSendNotification);
    sourceFileLabel.setColour (juce::Label::textColourId, juce::Colour (0xffd0d0d0));
    sourceFileLabel.setJustificationType (juce::Justification::centredLeft);
    sourceFileLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
    addAndMakeVisible (sourceFileLabel);

    loadButton.onClick        = [this] { doLoadPrompt(); };
    loadLatestMixdown.onClick = [this] { doLoadLatestMixdown(); };
    addAndMakeVisible (loadButton);
    addAndMakeVisible (loadLatestMixdown);

    // transport
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
    grLabel.setFont (juce::Font (juce::FontOptions (13.0f)));
    addAndMakeVisible (grLabel);

    // Meter + LUFS
    auto styleMeter = [] (juce::Label& l)
    {
        l.setJustificationType (juce::Justification::centred);
        l.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                                                    13.0f, juce::Font::plain)));
        l.setColour (juce::Label::textColourId, juce::Colour (0xffd0d0d0));
        l.setColour (juce::Label::backgroundColourId, juce::Colour (0xff121214));
    };
    styleMeter (lufsM);
    styleMeter (lufsS);
    styleMeter (lufsI);
    styleMeter (truePeak);
    lufsI.setColour (juce::Label::backgroundColourId, juce::Colour (0xff1a2228));  // brighter - the target reading
    // Integrated LUFS is the headline mastering number - bold it so it reads
    // as primary against the momentary/short-term cells.
    lufsI.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                                                    13.0f, juce::Font::bold)));
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
    // ComboBox uses 1-based item IDs; we map item-id N -> array index N-1.
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
    const int idx = jlimit (0, kNumMasteringTargets - 1,
                                    session.mastering().targetPresetIndex.load());
    masteringTargetCombo.setSelectedId (idx + 1, juce::dontSendNotification);
    masteringTargetCombo.onChange = [this]
    {
        const int newIdx = masteringTargetCombo.getSelectedId() - 1;
        session.mastering().targetPresetIndex.store (
            jlimit (0, kNumMasteringTargets - 1, newIdx));
    };
    addAndMakeVisible (masteringTargetCombo);

    // Export
    exportButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff2a3a48));
    exportButton.setColour (juce::TextButton::textColourOffId, juce::Colour (0xffd0e0f0));
    exportButton.onClick = [this] { doExport(); };
    addAndMakeVisible (exportButton);

    // Waveform display (top of view)
    waveform = std::make_unique<WaveformDisplay> (engine.getMasteringPlayer());
    addAndMakeVisible (waveform.get());

   #if DUSKSTUDIO_HAS_NATIVE_UI
    eqProxy.onVisibilityChanged = [this] { scheduleNativePanelSync(); };
    limiterProxy.onVisibilityChanged = [this] { scheduleNativePanelSync(); };
    phaseProxy.onVisibilityChanged = [this] { scheduleNativePanelSync(); };
    addChildComponent (eqProxy);
    addChildComponent (limiterProxy);
    addChildComponent (phaseProxy);
   #endif

    const char* compactPanels[] = { "Digital EQ", "Multiband compressor", "Limiter", "Phase" };
    for (int i = 0; i < 4; ++i) compactPanelCombo.addItem (compactPanels[i], i + 1);
    compactPanelCombo.setSelectedId (1);
    compactPanelCombo.setTooltip ("Select a mastering module in a compact window.");
    compactPanelCombo.onChange = [this] { resized(); };
    addChildComponent (compactPanelCombo);

    // The multiband compressor's slot: its split header, whose status light
    // engages it and whose label opens Multi-Comp 2's Multiband face, as the
    // console's COMP headers do; the preset picker; and an overview of the
    // bands the face holds.
    compPanelWrapper = std::make_unique<juce::Component>();
    addAndMakeVisible (compPanelWrapper.get());

    compHeaderBtn = std::make_unique<SplitModuleButton> ("MULTIBAND COMP");
    compHeaderBtn->setCallbacks (
        [this] { return ! session.mastering().compEnabled.load (std::memory_order_relaxed); },
        [this]
        {
            auto& mp = session.mastering();
            mp.compEnabled.store (! mp.compEnabled.load (std::memory_order_relaxed), std::memory_order_relaxed);
            if (compHeaderBtn != nullptr) compHeaderBtn->refresh();
        },
        [this] { openCompEditorPopup(); },
        {});
    compHeaderBtn->setTooltip ("Click the status light to engage or bypass the multiband compressor; "
                               "click MULTIBAND COMP to open its editor.");
    compPanelWrapper->addAndMakeVisible (compHeaderBtn.get());

#if DUSKSTUDIO_HAS_DUSK_DSP
    // DP-24-style 3-band preset picker in the comp header. A muted caption to
    // its left (matching the Mode / Target / Release caption style elsewhere in
    // this view) reads as a deliberate labelled control rather than a lone combo.
    compPresetCaption.setText ("PRESET", juce::dontSendNotification);
    compPresetCaption.setJustificationType (juce::Justification::centredRight);
    compPresetCaption.setColour (juce::Label::textColourId, juce::Colour (0xff808088));
    compPresetCaption.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
    compPanelWrapper->addAndMakeVisible (compPresetCaption);

    compPresetCombo.addItem ("Select preset...", 1);
    for (int i = 0; i < mbpresets::kNumPresets; ++i)
        compPresetCombo.addItem (mbpresets::kPresets[(size_t) i].name, i + 2);
    compPresetCombo.setSelectedId (1, juce::dontSendNotification);
    compPresetCombo.setTooltip ("Apply a DP-24-style 3-band mastering compressor preset. "
                                  "Maps onto the multiband comp with the high-mid band disabled.");
    compPresetCombo.onChange = [this]
    {
        // The preset goes into the session, which the compressor and its
        // face both read. The picked name stays showing: a starting point,
        // not live state.
        const int id = compPresetCombo.getSelectedId();
        if (id >= 2)
            mbpresets::apply (id - 2, engine.getMasteringChain().getCompressor(), session.mastering().comp);
        if (refreshCompOverview() && compPanelWrapper != nullptr)
            repaint (compPanelWrapper->getBounds());
    };
    compPanelWrapper->addAndMakeVisible (compPresetCombo);
#endif
    refreshCompOverview();

    // The session's source is already in the player: every session switch
    // loads it, whether or not this page exists yet.
    updateLabels();
    phaseSourceGeneration = engine.getMasteringPlayer().getSourceGeneration();
    phaseAnalyzer.prepare (engine.getTimelineSampleRate(), session.mastering().phase.snapshot());
    startTimerHz (30);
}

MasteringView::~MasteringView()
{
    stopTimer();   // before derived members destruct
   #if DUSKSTUDIO_HAS_NATIVE_UI
    // The framework children reach back into this view through their callbacks, so they
    // go down while it is still whole.
    eqWindow.reset();
    limiterWindow.reset();
    phaseWindow.reset();
    if (compEditor != nullptr)
        compEditor->shutdown();
    compEditorDim.reset();
   #endif
}

void MasteringView::visibilityChanged()
{
    // The compressor editor belongs to this page and goes with it.
    if (! isShowing())
        closeCompEditorPopup();
    scheduleNativePanelSync();
}

void MasteringView::parentHierarchyChanged()
{
    // The native children embed into the top-level window's handle, which only exists
    // once this view is in a realised hierarchy.
    scheduleNativePanelSync();
}

void MasteringView::scheduleNativePanelSync()
{
   #if DUSKSTUDIO_HAS_NATIVE_UI
    if (nativeSyncPending)
        return;
    nativeSyncPending = true;
    juce::Component::SafePointer<MasteringView> safeThis (this);
    dusk::callAsync ([safeThis]
    {
        if (auto* self = safeThis.getComponent())
        {
            self->nativeSyncPending = false;
            self->applyNativePanelSync();
        }
    });
   #endif
}

void MasteringView::syncNativePanelVisibility()
{
   #if DUSKSTUDIO_HAS_NATIVE_UI
    const int selected = compactPanelCombo.getSelectedId() - 1;
    const std::array<std::pair<NativePanelProxy*, int>, 3> panels { {
        { &eqProxy, 0 }, { &limiterProxy, 2 }, { &phaseProxy, 3 }
    } };
    for (const auto& panel : panels)
    {
        auto& proxy = *panel.first;
        const bool wanted = ! compactPanelCombo.isVisible() || selected == panel.second;
        const bool hidden = (int) proxy.getProperties().getWithDefault ("dusk_modalHideCount", 0) > 0;
        // A cover may have opened with no visible proxy to take a hide token.
        proxy.setVisible (wanted && ! hidden && (! PluginEditorHider::hasActiveSurface() || proxy.isVisible()));
    }
   #endif
}

void MasteringView::applyNativePanelSync()
{
   #if DUSKSTUDIO_HAS_NATIVE_UI
    syncNativePanelVisibility();
    auto* const topLevel = getTopLevelComponent();
    const auto parentHandle = topLevel != nullptr
                            ? embedscale::nativeParentHandle (*topLevel) : 0;

    struct Panel
    {
        std::unique_ptr<imgui::DuskPanelWindow>& window;
        NativePanelProxy& proxy;
        const char* className;
        const char* logTag;
        const char* displayName;
        std::unique_ptr<imgui::DuskPanelView> (*makeView) (MasteringView&);
    };
    const Panel panels[] = {
        { eqWindow, eqProxy, "dusk-studio-mastering-eq", "mastering-eq", "Mastering EQ",
          [] (MasteringView& self)
          {
              return imgui::makeMasteringEqView (self.session.mastering(),
                                                 &self.engine.getMasteringChain());
          } },
        { limiterWindow, limiterProxy, "dusk-studio-mastering-limiter", "mastering-limiter",
          "Mastering limiter",
          [] (MasteringView& self)
          {
              return imgui::makeMasteringLimiterView (
                  self.session.mastering(), self.engine.getMasteringChain().getLimiter());
          } },
        { phaseWindow, phaseProxy, "dusk-studio-mastering-phase", "mastering-phase", "Mastering Phase",
          [] (MasteringView& self)
          {
              auto* audioEngine = &self.engine;
              return imgui::makeMasteringPhaseView (self.session.mastering().phase, self.phaseAnalyzer,
                  [audioEngine] { return audioEngine->getMasteringMonoMonitor(); },
                  [audioEngine] (bool mono) { audioEngine->setMasteringMonoMonitor (mono); });
          } }
    };

    for (const auto& panel : panels)
    {
        const bool wanted = parentHandle != 0 && isShowing() && panel.proxy.isVisible()
                         && ! panel.proxy.getBounds().isEmpty();

        if (! wanted)
        {
            if (panel.window != nullptr && panel.window->isOpen())
                panel.window->close();
            continue;
        }
        if (panel.window != nullptr && panel.window->isOpen())
        {
            panel.window->setGeometry (nativePanelGeometry (*this, panel.proxy));
            continue;
        }

        panel.window = std::make_unique<imgui::DuskPanelWindow> (
            panel.className, panel.logTag, panel.displayName);
        panel.window->setTakesKeyboardOnOpen (false);

        imgui::DuskPanelWindow::Callbacks callbacks;
        callbacks.shortcut = [] (imgui::ShellShortcut shortcut)
        {
            return dispatchShellShortcut (shortcut);
        };
        juce::Component::SafePointer<MasteringView> safeThis (this);
        NativePanelProxy* const proxy = &panel.proxy;
        // A close takes two ticks of the framework's pump and the window reads as open
        // for both of them, so a proxy that comes back inside that window would be
        // answered with a geometry change on a child about to be destroyed. Re-running
        // the sync once the close has landed is what puts the panel back.
        callbacks.closed = [safeThis]
        {
            if (auto* const self = safeThis.getComponent())
            {
                self->scheduleNativePanelSync();
                // Native teardown can leave OS focus empty while the framework
                // still reports the covering popup as focused.
                const auto& modals = EmbeddedModal::activeModalStack();
                if (! modals.empty())
                    if (auto* body = modals.back()->getBody())
                    {
                        if (auto* peer = body->getPeer(); peer != nullptr && ! peer->isFocused())
                            peer->grabFocus();
                        if (! body->hasKeyboardFocus (true))
                            body->grabKeyboardFocus();
                    }
            }
        };
        callbacks.geometry = [safeThis, proxy]
        {
            auto* const self = safeThis.getComponent();
            if (self == nullptr)
                return imgui::DuskPanelWindow::Geometry {};
            return nativePanelGeometry (*self, *proxy);
        };
        panel.window->setCallbacks (std::move (callbacks));

        panel.window->setView (panel.makeView (*this));

        if (! panel.window->open (parentHandle, nativePanelGeometry (*this, panel.proxy)))
            panel.window.reset();
    }
   #endif
}

int MasteringView::nativePanelsOpenForScenario() const
{
   #if DUSKSTUDIO_HAS_NATIVE_UI
    int open = 0;
    for (const auto* window : { eqWindow.get(), limiterWindow.get(), phaseWindow.get() })
        if (window != nullptr && window->isOpen())
            ++open;
    return open;
   #else
    return -1;
   #endif
}

bool MasteringView::offerPhaseKey (const std::string& description, std::uint32_t character)
{
   #if DUSKSTUDIO_HAS_NATIVE_UI
    return phaseProxy.isShowing() && phaseWindow != nullptr
        && phaseWindow->offerShellKey (description, character);
   #else
    (void) description;
    (void) character;
    return false;
   #endif
}

bool MasteringView::phaseKeyForScenario (const std::string& description)
{
   #if DUSKSTUDIO_HAS_NATIVE_UI && ! defined (__APPLE__)
    return phaseWindow != nullptr && phaseWindow->hasKeyboardForScenario()
        && phaseCapturesKeyboardForScenario() && phaseWindow->inputForScenario (description);
   #else
    (void) description;
    return false;
   #endif
}

bool MasteringView::phaseCapturesKeyboardForScenario() const
{
   #if DUSKSTUDIO_HAS_NATIVE_UI
    return phaseProxy.isShowing() && phaseWindow != nullptr && phaseWindow->capturesKeyboardForScenario();
   #else
    return false;
   #endif
}

bool MasteringView::phaseHasKeyboardForScenario() const
{
   #if DUSKSTUDIO_HAS_NATIVE_UI
    return phaseProxy.isShowing() && phaseWindow != nullptr && phaseWindow->hasKeyboardForScenario();
   #else
    return false;
   #endif
}

bool MasteringView::phaseTypeForScenario (const std::string& text)
{
   #if DUSKSTUDIO_HAS_NATIVE_UI
    return phaseProxy.isShowing() && phaseWindow != nullptr && phaseWindow->typeForScenario (text);
   #else
    (void) text;
    return false;
   #endif
}

void MasteringView::resetPhasePanelForScenario()
{
   #if DUSKSTUDIO_HAS_NATIVE_UI
    phaseWindow.reset();
    scheduleNativePanelSync();
   #endif
}

bool MasteringView::phasePointerForScenario (const std::string& control, float position, bool pressed, int modifiers)
{
   #if DUSKSTUDIO_HAS_NATIVE_UI
    return phaseProxy.isShowing() && phaseWindow != nullptr
        && phaseWindow->pointerControlForScenario (control, position, pressed, modifiers);
   #else
    (void) control;
    (void) position;
    (void) pressed;
    (void) modifiers;
    return false;
   #endif
}

std::vector<MasteringView::NativePanelCapture>
MasteringView::captureNativePanels (const juce::File& dir)
{
    std::vector<NativePanelCapture> captures;
   #if DUSKSTUDIO_HAS_NATIVE_UI
    const auto ask = [&] (std::unique_ptr<imgui::DuskPanelWindow>& window,
                          const NativePanelProxy& proxy, const char* name)
    {
        if (! proxy.isShowing())
            return;
        // A shown panel without a window keeps its entry, so the figure is reported
        // incomplete rather than written without it.
        const auto file = dir.getChildFile (name);
        if (window != nullptr && window->isOpen())
            window->captureNextFrameTo (file.getFullPathName().toStdString());
        captures.push_back ({ proxy.getBounds(), file });
    };
    ask (eqWindow, eqProxy, "_mastering-eq.ppm");
    ask (limiterWindow, limiterProxy, "_mastering-limiter.ppm");
    ask (phaseWindow, phaseProxy, "_mastering-phase.ppm");
   #else
    juce::ignoreUnused (dir);
   #endif
    return captures;
}

bool MasteringView::nativePanelsOpenForCapture() const
{
   #if DUSKSTUDIO_HAS_NATIVE_UI
    const auto open = [] (const std::unique_ptr<imgui::DuskPanelWindow>& window, const NativePanelProxy& proxy)
    {
        return ! proxy.isShowing() || (window != nullptr && window->isOpen());
    };
    return open (eqWindow, eqProxy) && open (limiterWindow, limiterProxy) && open (phaseWindow, phaseProxy);
   #else
    return true;
   #endif
}

// Each overview row is its label and, after a tab, what it reads, in the
// plug-in's own text.
bool MasteringView::refreshCompOverview()
{
   #if DUSKSTUDIO_HAS_DUSK_DSP
    static constexpr const char* kBandNames[kCompBandCount] = { "LOW", "LOW-MID", "HIGH-MID", "HIGH" };
    const auto& comp = session.mastering().comp;
    std::uint64_t signature = 1469598103934665603ull;
    for (const auto id : compStageParams (CompStage::Multiband))
    {
        const float value = comp.get (id);
        std::uint32_t bits = 0;
        std::memcpy (&bits, &value, sizeof bits);
        signature = (signature ^ bits) * 1099511628211ull;
    }
    if (signature == compOverviewSignature && ! compOverview.empty())
        return false;
    compOverviewSignature = signature;

    const auto& stage = engine.getMasteringChain().getCompressor();
    const auto text = [&stage, &comp] (CompParamId id) { return stage.valueText (id, comp.get (id)); };
    compOverview.clear();
    compOverview.push_back ("XOVER\t" + text (CompParamId::MbCrossover1) + "  " + text (CompParamId::MbCrossover2)
                            + "  " + text (CompParamId::MbCrossover3));
    for (int b = 0; b < kCompBandCount; ++b)
    {
        std::string row = std::string (kBandNames[b]) + "\t";
        if (comp.get (compBandParam (b, CompBandField::Enabled)) < 0.5f)
            row += "off";
        else
        {
            row += text (compBandParam (b, CompBandField::Threshold)) + "  "
                 + text (compBandParam (b, CompBandField::Ratio));
            if (comp.get (compBandParam (b, CompBandField::Bypass)) >= 0.5f) row += "  BYP";
            if (comp.get (compBandParam (b, CompBandField::Solo)) >= 0.5f)   row += "  SOLO";
        }
        compOverview.push_back (std::move (row));
    }
    compOverview.push_back ("OUTPUT\t" + text (CompParamId::MbOutput));
    return true;
   #else
    return false;
   #endif
}

juce::Component* MasteringView::compHeaderForScenario() noexcept
{
    return compHeaderBtn.get();
}

#if DUSKSTUDIO_HAS_NATIVE_UI
imgui::DafEditorHost::Geometry MasteringView::compEditorGeometry()
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
    compEditorBounds = { fitted.logical.getX(), fitted.logical.getY(),
                         fitted.logical.getWidth(), fitted.logical.getHeight() };
    const auto& g = fitted.geometry;
    return { g.x, g.y, g.width, g.height, g.scale };
}

void MasteringView::finishCompEditorClose()
{
    compEditorDim.reset();
    compEditorHider.restore();
    compEditorBounds.clear();
    if (auto* target = EmbeddedModal::focusRestoreTarget().getComponent())
        target->grabKeyboardFocus();
}

std::vector<int> MasteringView::compEditorBoundsForScenario() const
{
    return isCompEditorOpen() ? compEditorBounds : std::vector<int> {};
}
#endif

void MasteringView::openCompEditorPopup()
{
   #if ! DUSKSTUDIO_HAS_NATIVE_UI || ! DUSKSTUDIO_HAS_DUSK_DSP
    showDuskAlert (*this, "Multiband compressor",
                   "The compressor editor needs the native UI and the Dusk DSP, which this build was made without.");
   #else
    if (isCompEditorOpen())
    {
        closeCompEditorPopup();
        return;
    }

    auto* topLevel = getTopLevelComponent();
    if (topLevel == nullptr) topLevel = this;
    const auto parentHandle = embedscale::nativeParentHandle (*topLevel);
    if (parentHandle == 0)
    {
        showDuskAlert (*topLevel, "Multiband compressor",
                       "The compressor editor cannot open: the main window is not ready.");
        return;
    }

    // Closes the notepad and any other native editor before this one takes
    // the window.
    if (auto hook = EmbeddedModal::beforeModalShown())
        hook();

    if (compEditor == nullptr)
    {
        // BYPASS is the header's engage switch; no other control of the face
        // engages the compressor, as none on this page does.
        imgui::CompEditorTarget binding;
        binding.comp = &session.mastering().comp;
        binding.engaged = &session.mastering().compEnabled;
        binding.editsEngage = false;
        compEditor = std::make_unique<imgui::CompEditorHost> (
            [this]() -> MultiCompStage& { return engine.getMasteringChain().getCompressor(); }, binding);

        // Raw `this`: the view owns the host and shuts it down before its own
        // teardown, so none of these outlives it.
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

    // The page's native panels paint above any framework child, so they hide
    // for the editor's lifetime exactly as under an embedded modal.
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
    compEditorBounds.clear();
    const auto& why = compEditor->lastOpenFailure();
    showDuskAlert (*topLevel, "Multiband compressor",
                   why.empty() ? "The compressor editor cannot open on this display backend."
                               : why.c_str());
   #endif
}

void MasteringView::closeCompEditorPopup()
{
   #if DUSKSTUDIO_HAS_NATIVE_UI
    if (compEditor != nullptr && compEditor->isOpen())
        compEditor->close();
   #endif
}

bool MasteringView::isCompEditorOpen() const noexcept
{
   #if DUSKSTUDIO_HAS_NATIVE_UI
    return compEditor != nullptr && compEditor->isOpen();
   #else
    return false;
   #endif
}

void MasteringView::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff0d0d0f));

    // Grouping backdrop behind the TP/M/S/I loudness cells (their own darker
    // cell backgrounds pop against this) so they read as one loudness cluster.
    if (! loudnessClusterBounds.isEmpty())
    {
        const auto box = loudnessClusterBounds.toFloat();
        juce::ColourGradient grad (juce::Colour (0xff1d1d25), box.getX(), box.getY(),
                                    juce::Colour (0xff141419), box.getX(), box.getBottom(), false);
        g.setGradientFill (grad);
        g.fillRoundedRectangle (box, 4.0f);
        g.setColour (juce::Colour (0xff44485a));
        g.drawRoundedRectangle (box, 4.0f, 1.2f);
    }

    // The comp wrapper paints nothing itself, so its margin, header row and
    // overview show this fill; the colours are the ones the EQ and limiter
    // panels draw.
    if (compPanelWrapper != nullptr && compPanelWrapper->isVisible())
    {
        const auto panel = compPanelWrapper->getBounds();
        g.setColour (juce::Colour (0xff20202a));
        g.fillRect (panel);
        g.setColour (juce::Colour (0xff3a3a46));
        g.drawRect (panel, 1);

        // The overview, under the preset picker: a dim label, the values on
        // the right.
        const int top = panel.getY() + (compPresetCombo.isVisible() ? compPresetCombo.getBottom()
                                                                    : compHeaderBtn->getBottom()) + 14;
        auto rows = panel.reduced (12, 0).withTop (top).withBottom (panel.getBottom() - 8);
        g.setFont (11.0f);
        for (const auto& line : compOverview)
        {
            if (rows.getHeight() < 16) break;
            auto row = rows.removeFromTop (18);
            const auto tab = line.find ('\t');
            g.setColour (juce::Colour (0xff808088));
            g.drawText (line.substr (0, tab), row, juce::Justification::centredLeft, true);
            g.setColour (juce::Colour (0xffd0d0d6));
            g.drawText (tab == std::string::npos ? std::string() : line.substr (tab + 1),
                        row.withTrimmedLeft (64), juce::Justification::centredRight, true);
        }
    }
}

int MasteringView::unpaintedCompPanelPixelsForScenario()
{
    if (compPanelWrapper == nullptr || ! isShowing())
        return -1;
    // A snapshot starts transparent, or black when the view is opaque, and the
    // page never paints pure black.
    const auto image = createComponentSnapshot (getLocalBounds());
    const auto panel = compPanelWrapper->getBounds();
    int unpainted = 0;
    for (int y = panel.getY(); y < panel.getBottom(); ++y)
        for (int x = panel.getX(); x < panel.getRight(); ++x)
        {
            const auto argb = image.getPixelAt (x, y).getARGB();
            if ((argb >> 24) != 0xff || argb == 0xff000000)
                ++unpainted;
        }
    return unpainted;
}

void MasteringView::resized()
{
    auto area = getLocalBounds().reduced (12);

    // Header (load buttons + filename label)
    auto header = area.removeFromTop (28);
    loadButton.setBounds (header.removeFromLeft (110));
    header.removeFromLeft (4);
    loadLatestMixdown.setBounds (header.removeFromLeft (180));
    header.removeFromLeft (8);
    exportButton.setBounds (header.removeFromLeft (150));
    header.removeFromLeft (12);
    sourceFileLabel.setBounds (header);
    area.removeFromTop (8);

    // Transport row (now also hosts the LUFS readouts so the
    //    loudness target context sits next to the playback cluster
    //    instead of being buried at the bottom of the view). Width-
    //    clamped placement so narrow windows don't render widgets
    // with negative widths.
    auto transportRow = area.removeFromTop (36);

    // Transport cluster on the left.
    const auto placeL = [&transportRow] (juce::Component& c, int w, int gapAfter = 0)
    {
        if (transportRow.getWidth() <= 0) { c.setVisible (false); return; }
        c.setVisible (true);
        c.setBounds (transportRow.removeFromLeft (std::min (w, transportRow.getWidth())));
        if (gapAfter > 0) transportRow.removeFromLeft (std::min (gapAfter, transportRow.getWidth()));
    };
    placeL (rewindButton, 50, 4);
    placeL (playButton,   70, 4);
    placeL (stopButton,   70, 16);
    placeL (clockLabel,  140, 0);

    // Loudness cluster pinned to the right edge, placed right-to-left so it
    // reads  GR | TP M S I | Reset | Target. The four meter cells sit flush as
    // one grouped block (backdrop drawn in paint()).
    const auto placeR = [&transportRow] (juce::Component& c, int w, int gapBefore = 0)
    {
        if (transportRow.getWidth() <= 0) { c.setVisible (false); return; }
        c.setVisible (true);
        c.setBounds (transportRow.removeFromRight (std::min (w, transportRow.getWidth())));
        if (gapBefore > 0) transportRow.removeFromRight (std::min (gapBefore, transportRow.getWidth()));
    };
    placeR (masteringTargetCombo, 240, 4);
    placeR (targetCaption,         40, 12);
    placeR (resetLoudness,         56, 10);
    placeR (lufsI,    122);
    placeR (lufsS,     96);
    placeR (lufsM,     96);
    placeR (truePeak, 108, 12);
    placeR (grLabel,  205);

    // Backdrop rect around the flush TP..I meter block, for visual grouping.
    if (truePeak.isVisible() && lufsI.isVisible())
        loudnessClusterBounds = juce::Rectangle<int> (truePeak.getX() - 4, truePeak.getY() - 2,
                                                        lufsI.getRight() - truePeak.getX() + 8,
                                                        truePeak.getHeight() + 4);
    else
        loudnessClusterBounds = {};
    area.removeFromTop (8);

    // Waveform: a slim band at the top so the plugin row gets the bulk
    //    of the vertical real estate. Fixed height; what's left in `area`
    //    above the bottom strip and the panel row goes to the plugins.
    constexpr int kWaveH = 90;
    if (waveform != nullptr)
        waveform->setBounds (area.removeFromTop (kWaveH));
    area.removeFromTop (6);

    // The compressor's face opens over the page, so its slot holds the header,
    // the presets and the band overview, and the EQ takes the width it gave up.
    constexpr int kPanelGap = 8;
    constexpr int kEqMin = 480, kCompMin = 200, kLimiterMin = 200, kPhaseMin = 200;
    const bool compact = area.getWidth() < kEqMin + kCompMin + kLimiterMin + kPhaseMin + 3 * kPanelGap;
    compactPanelCombo.setVisible (compact);
    if (compact)
    {
        compactPanelCombo.setBounds (area.removeFromTop (24));
        area.removeFromTop (6);
    }
    auto panelsRow = area;
    const int totalW    = std::max (0, panelsRow.getWidth() - 3 * kPanelGap);
    const int spare    = std::max (0, totalW - kEqMin - kCompMin - kLimiterMin - kPhaseMin);
    const int eqW      = kEqMin + (int) std::round (spare * 0.40);
    const int compW    = kCompMin + (int) std::round (spare * 0.06);
    const int limW     = kLimiterMin + (int) std::round (spare * 0.22);

    auto eqPanel = panelsRow, compPanel = panelsRow, limPanel = panelsRow;
    auto phasePanel = panelsRow;
    if (! compact)
    {
        eqPanel = panelsRow.removeFromLeft (eqW);
        panelsRow.removeFromLeft (kPanelGap);
        compPanel = panelsRow.removeFromLeft (compW);
        panelsRow.removeFromLeft (kPanelGap);
        limPanel = panelsRow.removeFromLeft (limW);
        panelsRow.removeFromLeft (kPanelGap);
        phasePanel = panelsRow;
    }
    const int selected = std::clamp (compactPanelCombo.getSelectedId() - 1, 0, 3);

   #if DUSKSTUDIO_HAS_NATIVE_UI
    const auto placeNativePanel = [] (auto& proxy, const auto& bounds)
    {
        // A partial popup's hide tokens describe the geometry at opening.
        if (PluginEditorHider::hasActiveSurface() && proxy.getBounds() != bounds)
            proxy.setVisible (false);
        proxy.setBounds (bounds);
    };
    syncNativePanelVisibility();
    placeNativePanel (eqProxy, eqPanel);
   #endif

    // Multiband Comp panel: the split header on top, the preset picker under
    // it; paint() draws the band overview below.
    if (compPanelWrapper != nullptr)
    {
        compPanelWrapper->setVisible (! compact || selected == 1);
        compPanelWrapper->setBounds (compPanel);
        auto inner = compPanelWrapper->getLocalBounds().reduced (8);
        // Title bar (full width), then a centred preset picker row beneath it.
        auto headerRow = inner.removeFromTop (22);
        if (compHeaderBtn != nullptr) compHeaderBtn->setBounds (headerRow);
#if DUSKSTUDIO_HAS_DUSK_DSP
        inner.removeFromTop (8);
        auto presetRow = inner.removeFromTop (24);
        const int capW    = 52;
        const int dropW   = std::min (190, presetRow.getWidth() - capW - 6);
        if (dropW <= 0)
        {
            // Too narrow for caption + combo - hide rather than lay out negative
            // widths (mirrors the transport row's width-clamped placement above).
            compPresetCaption.setVisible (false);
            compPresetCombo.setVisible (false);
        }
        else
        {
            compPresetCaption.setVisible (true);
            compPresetCombo.setVisible (true);
            auto cluster = presetRow.withSizeKeepingCentre (capW + 6 + dropW, 24);
            compPresetCaption.setBounds (cluster.removeFromLeft (capW));
            cluster.removeFromLeft (6);
            compPresetCombo.setBounds (cluster);
        }
#endif
    }

   #if DUSKSTUDIO_HAS_NATIVE_UI
    placeNativePanel (limiterProxy, limPanel);
    placeNativePanel (phaseProxy, phasePanel);
    scheduleNativePanelSync();
   #endif
}

void MasteringView::updatePhaseAnalysis()
{
    auto& feed = engine.getMasteringChain().getPhaseFeed();
    const auto source = engine.getMasteringPlayer().getSourceGeneration();
    if (source != phaseSourceGeneration)
    {
        phaseSourceGeneration = source;
        feed.discardPending();
        phaseAnalyzer.markDiscontinuity();
        phaseAnalyzer.resetHeld();
        phaseStreamValid = false;
    }
    const double deviceRate = engine.getCurrentSampleRate();
    const bool active = isShowing() && engine.getStage() == AudioEngine::Stage::Mastering
                        && ! exportModal.isOpen() && deviceRate > 0.0;
    if (! active)
    {
        feed.discardPending();
        if (phaseActive) phaseAnalyzer.markDiscontinuity();
        phaseActive = phaseStreamValid = false;
        return;
    }
    if (! phaseActive)
    {
        feed.discardPending();
        phaseAnalyzer.markDiscontinuity();
        phaseStreamValid = false;
        phaseActive = true;
    }
    const auto settings = session.mastering().phase.snapshot();
    const auto configure = [&] (double rate)
    {
        const auto& report = phaseAnalyzer.report();
        if (report.sampleRate != rate || report.settings.bands != settings.bands
            || report.settings.q != settings.q || report.settings.averagingMs != settings.averagingMs)
            phaseAnalyzer.prepare (rate, settings);
    };
    int pending = feed.pendingSamples();
    if (pending == 0) configure (deviceRate);
    while (pending > 0)
    {
        const auto chunk = feed.read (phaseLeft.data(), phaseRight.data(),
                                      std::min (pending, (int) phaseLeft.size()));
        if (chunk.samples == 0) break;
        pending -= chunk.samples;
        configure (chunk.sampleRate);
        if (! phaseStreamValid || phaseFeedGeneration != chunk.generation || phaseNextSequence != chunk.sequence)
            phaseAnalyzer.markDiscontinuity();
        phaseFeedGeneration = chunk.generation;
        phaseNextSequence = chunk.sequence + (std::uint64_t) chunk.samples;
        phaseStreamValid = true;
        phaseAnalyzer.push (phaseLeft.data(), phaseRight.data(), chunk.samples);
    }
}

void MasteringView::timerCallback()
{
    syncNativePanelVisibility();
    updatePhaseAnalysis();
    auto& player = engine.getMasteringPlayer();
    auto& m = session.mastering();

    if (player.getSourceGeneration() != labelSourceGeneration)
        updateLabels();

    // The engage switch and the band values change from the editor's BYPASS
    // and controls, a session load, MIDI or a preset; each repaints only on a
    // change.
    if (compHeaderBtn != nullptr) compHeaderBtn->refresh();
    if (refreshCompOverview() && compPanelWrapper != nullptr && compPanelWrapper->isVisible())
        repaint (compPanelWrapper->getBounds());

    // Clock from the player playhead.
    const double sr = player.getSourceSampleRate();
    const double sec = (sr > 0.0) ? (double) player.getPlayhead() / sr : 0.0;
    const int mins = (int) (sec / 60.0);
    const int secs = (int) sec % 60;
    const int ms   = (int) ((sec - std::floor (sec)) * 1000.0);
    clockLabel.setText (juce::String::formatted ("%02d:%02d.%03d", mins, secs, ms),
                         juce::dontSendNotification);

    grLabel.setText (juce::String::formatted ("Comp GR %5.1f dB    Lim GR %5.1f dB",
                                                 m.meterCompGrDb.load(),
                                                 m.meterLimiterGrDb.load()),
                      juce::dontSendNotification);

    auto fmtLufs = [] (float v)
    {
        if (v <= -99.0f) return juce::String ("  - LUFS");
        return juce::String::formatted ("%6.1f LUFS", v);
    };
    lufsM.setText ("M " + fmtLufs (m.meterMomentaryLufs.load()),  juce::dontSendNotification);
    lufsS.setText ("S " + fmtLufs (m.meterShortTermLufs.load()),  juce::dontSendNotification);

    const float iLufs = m.meterIntegratedLufs.load();
    lufsI.setText ("I " + fmtLufs (iLufs), juce::dontSendNotification);

    const float tp = m.meterTruePeakDb.load();
    truePeak.setText (tp <= -99.0f ? juce::String ("TP - dBTP")
                                   : juce::String::formatted ("TP %5.1f dBTP", tp),
                       juce::dontSendNotification);

    // Streaming-target color coding. Off = neutral; otherwise:
    //   I-LUFS cell: green inside ±0.5 LU, yellow inside ±2 LU, red beyond.
    //   TP cell:     green at-or-below ceiling, red above.
    // No data (sentinel) keeps the neutral colour so a fresh load doesn't
    // flash green spuriously.
    const int targetIdx = jlimit (0, kNumMasteringTargets - 1,
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

// The line names what the player holds. A source the session names but the
// player could not load is reported as a failed load, which is also how a
// reopened session shows a mastering mix that has since gone missing. A picked
// file that would not open is reported the same way until the source changes,
// while the mix that was loaded keeps playing.
void MasteringView::updateLabels (const std::string& rejectedPick)
{
    const auto& player = engine.getMasteringPlayer();
    const auto& source = session.mastering().sourceFile;
    labelSourceGeneration = player.getSourceGeneration();
    std::string text = "No mix loaded";
    if (! rejectedPick.empty())
        text = "Failed to load: " + rejectedPick;
    else if (player.isLoaded())
    {
        const auto file = player.getLoadedFile();
        text = (file.getFileName() + "  (" + file.getParentDirectory().getFileName() + "/)").toStdString();
    }
    else if (source.getFullPathName().isNotEmpty())
        text = "Failed to load: " + source.getFullPathName().toStdString();
    sourceFileLabel.setText (text, juce::dontSendNotification);
}

bool MasteringView::loadSessionSource (AudioEngine& engine, Session& session)
{
    auto& player = engine.getMasteringPlayer();
    const auto source = session.mastering().sourceFile;
    if (source.getFullPathName().isEmpty())
    {
        player.unloadFile();
        return true;
    }
    if (! player.loadFile (source)) return false;
    // Reset the integrated LUFS history so the reading reflects ONLY the
    // currently-loaded mix, not a mix-of-mixes from prior auditions.
    engine.getMasteringChain().resetLoudness();
    return true;
}

bool MasteringView::loadFile (const juce::File& file)
{
    // Loading closes the current mix first, so a file that won't open is
    // turned away before that. Should the load still fail, the session keeps
    // naming the file, as it keeps a missing region's, and the page reports it.
    const auto path = file.getFullPathName().toStdString();
    if (dusk::audio::FileReader::open (std::filesystem::u8path (path)) == nullptr)
    {
        updateLabels (path);
        return false;
    }
    session.mastering().sourceFile = file;
    const bool loaded = loadSessionSource (engine, session);
    followSource();
    return loaded;
}

void MasteringView::followSource()
{
    if (waveform != nullptr) waveform->followPlayer();
    updateLabels();
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

    // Delivery presets. 1 = archive/session-rate WAV, 2 = dithered CD-spec
    // WAV, 3 = MP3. The chosen preset drives the browser's default name and
    // the render's rate/depth.
    juce::PopupMenu m;
    m.addSectionHeader ("Export master as");
    m.addItem (1, "WAV 24-bit " + juce::String (juce::CharPointer_UTF8 ("\xe2\x80\x94"))
                  + " session rate");
    m.addItem (2, "WAV 16-bit 44.1 kHz " + juce::String (juce::CharPointer_UTF8 ("\xe2\x80\x94"))
                  + " dithered (CD)");
   #if DUSKSTUDIO_HAS_LAME
    m.addItem (3, "MP3 320 kbps");
   #endif

    juce::Component::SafePointer<MasteringView> safe (this);
    showContextMenu (m, *this,
        [safe] (int preset)
        {
            if (safe == nullptr || preset == 0) return;
            safe->openExportBrowser (preset);
        },
        exportButton.getScreenBounds().getBottomLeft());
}

void MasteringView::openExportBrowser (int preset)
{
    const auto spec = BounceEngine::masterExportPreset (preset);
    const juce::String extension (spec.extension);

    const auto defaultFile = session.getSessionDirectory().getChildFile ("master." + extension);

    filebrowser::open (*this, {
        /*title*/                  "Export master",
        /*initialFileOrDirectory*/ defaultFile,
        /*filePatternsAllowed*/    "*." + extension,
        /*mode*/                   filebrowser::Mode::Save,
        /*selectDirectories*/      false,
    },
    [this, spec, extension] (juce::File out)
    {
        if (out == juce::File()) return;
        auto target = out;
        if (! target.hasFileExtension (extension))
            target = target.withFileExtension (extension);

        // The render reads the mix while it writes the master, so replacing
        // the mix would destroy it; it is refused before anything asks.
        const auto mix = engine.getMasteringPlayer().getLoadedFile().getFullPathName().toStdString();
        auto* window = getTopLevelComponent();
        if (savecheck::isSameLocation (std::filesystem::u8path (target.getFullPathName().toStdString()),
                                       std::filesystem::u8path (mix)))
        {
            showDuskAlert (window != nullptr ? *window : *this, savecheck::kLoadedMixTitle,
                           savecheck::loadedMixMessage (target.getFileName().toStdString()));
            return;
        }

        juce::Component::SafePointer<MasteringView> safeThis (this);
        auto launch = [safeThis, spec, target]
        {
            auto* self = safeThis.getComponent();
            if (self == nullptr) return;
            auto panel = std::make_unique<BounceDialog> (self->engine, self->session,
                                                           target,
                                                           BounceEngine::Mode::MasteringChain, spec.format,
                                                           spec.mp3Kbps, spec.sampleRate, spec.wavBitDepth);
            panel->setSize (520, 200);
            panel->onRequestClose = [safeThis] { if (safeThis != nullptr) safeThis->exportModal.close(); };
            self->exportModal.show (*self, std::move (panel), {}, false, false);
        };
        if (! target.existsAsFile())
        {
            launch();
            return;
        }
        showDuskConfirm (window != nullptr ? *window : *this, savecheck::kReplaceFileTitle,
                         savecheck::replaceFileMessage (target.getFileName().toStdString()),
                         savecheck::kReplaceFileButton, launch, "Cancel", {}, /*destructive*/ true);
    });
}
} // namespace duskstudio
