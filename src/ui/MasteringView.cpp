#include "MasteringView.h"
#include "BounceDialog.h"
#include "DuskAlerts.h"
#include "DuskContextMenu.h"
#include "DuskFileBrowser.h"
#include "EmbeddedModal.h"
#include "SaveTargetChecks.h"
#include "../engine/BounceEngine.h"
#include "../engine/MasteringPlayer.h"
#include "../engine/audiofile/FileReader.h"
#if DUSKSTUDIO_HAS_NATIVE_UI
 #include "NativeEditorEmbedScale.h"
 #include "imgui/DuskPanelWindow.h"
 #include "imgui/MasteringCompView.h"
 #include "imgui/MasteringEqView.h"
 #include "imgui/MasteringLimiterView.h"
 #include "imgui/MasteringPhaseView.h"
#endif
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

namespace duskstudio
{
namespace
{
template <typename T>
inline T jlimit (T lo, T hi, T value) noexcept { return std::clamp (value, lo, std::max (lo, hi)); }

// Each minimum is what that module's panel needs to show all of its controls (the
// EQ's and the compressor's are their views' own); a page too narrow for all four
// side by side shows one at a time, chosen above them.
constexpr int kPageInset = 12;
constexpr int kPanelGap = 8;
constexpr int kEqMin = 300, kCompMin = 400, kLimiterMin = 230, kPhaseMin = 200;
constexpr int kSideBySideMin = kEqMin + kCompMin + kLimiterMin + kPhaseMin + 3 * kPanelGap;

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
}

WaveformDisplay::~WaveformDisplay() { stopTimer(); }

void WaveformDisplay::showStateChanged()
{
    if (isTimerRunning() || ! shownInLayout (*this)) return;
    startTimerHz (20);
    timerCount.set (true);
    timerCallback();
}

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
    if (! shownInLayout (*this))
    {
        stopTimer();
        timerCount.set (false);
        return;
    }
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

struct MasteringView::PagePress final : public juce::MouseListener
{
    explicit PagePress (MasteringView& owner) : view (owner) {}
    void mouseDown (const juce::MouseEvent& e) override
    {
        const auto r = e.getEventRelativeTo (&view);
        if (! view.onNativePanel (r.x, r.y))
            view.closePanelLists();
    }
    MasteringView& view;
};

MasteringView::MasteringView (Session& s, AudioEngine& e)
    : session (s), engine (e)
{
    pagePress = std::make_unique<PagePress> (*this);
    addMouseListener (pagePress.get(), true);

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
    compProxy.onVisibilityChanged = [this] { scheduleNativePanelSync(); };
    limiterProxy.onVisibilityChanged = [this] { scheduleNativePanelSync(); };
    phaseProxy.onVisibilityChanged = [this] { scheduleNativePanelSync(); };
    addChildComponent (eqProxy);
    addChildComponent (compProxy);
    addChildComponent (limiterProxy);
    addChildComponent (phaseProxy);
   #endif

    const char* compactPanels[] = { "Digital EQ", "Multiband compressor", "Limiter", "Phase" };
    for (int i = 0; i < 4; ++i) compactPanelCombo.addItem (compactPanels[i], i + 1);
    compactPanelCombo.setSelectedId (1);
    compactPanelCombo.setTooltip ("Select a mastering module in a compact window.");
    compactPanelCombo.onChange = [this] { resized(); };
    addChildComponent (compactPanelCombo);

    // The session's source is already in the player: every session switch
    // loads it, whether or not this page exists yet.
    updateLabels();
    phaseSourceGeneration = engine.getMasteringPlayer().getSourceGeneration();
    phaseAnalyzer.prepare (engine.getTimelineSampleRate(), session.mastering().phase.snapshot());
}

MasteringView::~MasteringView()
{
    stopTimer();   // before derived members destruct
    removeMouseListener (pagePress.get());
   #if DUSKSTUDIO_HAS_NATIVE_UI
    // The framework children reach back into this view through their callbacks, so they
    // go down while it is still whole.
    eqWindow.reset();
    compWindow.reset();
    limiterWindow.reset();
    phaseWindow.reset();
   #endif
}

void MasteringView::visibilityChanged()
{
    scheduleNativePanelSync();
    showStateChanged();
}

void MasteringView::parentHierarchyChanged()
{
    // The native children embed into the top-level window's handle, which only exists
    // once this view is in a realised hierarchy.
    scheduleNativePanelSync();
    showStateChanged();
}

void MasteringView::showStateChanged()
{
    if (waveform != nullptr)
        waveform->showStateChanged();
    if (isTimerRunning() || ! shownInLayout (*this)) return;
    // The first tick comes at once, off this call, which runs while the page
    // is still being shown.
    catchUpTick = true;
    startTimer (1);
    timerCount.set (true);
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
    const std::array<std::pair<NativePanelProxy*, int>, 4> panels { {
        { &eqProxy, 0 }, { &compProxy, 1 }, { &limiterProxy, 2 }, { &phaseProxy, 3 }
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
#if DUSKSTUDIO_HAS_DUSK_DSP
        { compWindow, compProxy, "dusk-studio-mastering-comp", "mastering-comp", "Mastering multiband compressor",
          [] (MasteringView& self)
          {
              return imgui::makeMasteringCompView (self.session.mastering(),
                                                   self.engine.getMasteringChain().getCompressor(),
                                                   self.compPresetChoice);
          } },
#endif
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
        callbacks.pressed = [safeThis, window = panel.window.get()]
        {
            if (auto* const self = safeThis.getComponent())
            {
                self->closePanelLists (window);
                // Closing the list puts back the panels it hid, which must not
                // happen inside this panel's frame.
                dusk::callAsync ([] { DuskComboBox::dismissOpenListForOutsidePress(); });
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
    for (const auto* window : { eqWindow.get(), compWindow.get(), limiterWindow.get(), phaseWindow.get() })
        if (window != nullptr && window->isOpen())
            ++open;
    return open;
   #else
    return -1;
   #endif
}

int MasteringView::nativePanelsExpectedForScenario()
{
   #if DUSKSTUDIO_HAS_NATIVE_UI && DUSKSTUDIO_HAS_DUSK_DSP
    return 4;
   #elif DUSKSTUDIO_HAS_NATIVE_UI
    return 3;
   #else
    return -1;
   #endif
}

int MasteringView::sideBySideWindowWidthForScenario() const
{
    const auto* topLevel = getTopLevelComponent();
    const int around = topLevel != nullptr ? topLevel->getWidth() - getWidth() : 0;
    return around + 2 * kPageInset + kSideBySideMin;
}

bool MasteringView::onNativePanel (int x, int y) const
{
   #if DUSKSTUDIO_HAS_NATIVE_UI
    const std::array<std::pair<const imgui::DuskPanelWindow*, const NativePanelProxy*>, 4> panels { {
        { eqWindow.get(), &eqProxy }, { compWindow.get(), &compProxy },
        { limiterWindow.get(), &limiterProxy }, { phaseWindow.get(), &phaseProxy }
    } };
    for (const auto& panel : panels)
        if (panel.first != nullptr && panel.first->isOpen() && panel.second->isVisible()
            && panel.second->getBounds().contains (x, y))
            return true;
   #else
    (void) x;
    (void) y;
   #endif
    return false;
}

void MasteringView::closePanelLists (const imgui::DuskPanelWindow* pressed)
{
   #if DUSKSTUDIO_HAS_NATIVE_UI
    for (auto* window : { eqWindow.get(), compWindow.get(), limiterWindow.get(), phaseWindow.get() })
        if (window != nullptr && window != pressed)
            window->closeLists();
   #else
    (void) pressed;
   #endif
}

void MasteringView::sessionReplaced()
{
    compPresetChoice = 0;
   #if DUSKSTUDIO_HAS_NATIVE_UI
    for (auto* window : { eqWindow.get(), compWindow.get(), limiterWindow.get(), phaseWindow.get() })
        if (window != nullptr)
            window->sessionReplaced();
   #endif
}

bool MasteringView::offerPanelKey (const std::string& description, std::uint32_t character)
{
   #if DUSKSTUDIO_HAS_NATIVE_UI
    for (const auto* tag : { "mastering-eq", "mastering-comp", "mastering-limiter", "mastering-phase" })
        if (auto* window = shownPanel (tag); window != nullptr && window->offerShellKey (description, character))
            return true;
    return false;
   #else
    (void) description;
    (void) character;
    return false;
   #endif
}

bool MasteringView::panelKeyForScenario (const std::string& description)
{
   #if DUSKSTUDIO_HAS_NATIVE_UI && ! defined (__APPLE__)
    for (const auto* tag : { "mastering-eq", "mastering-comp", "mastering-limiter", "mastering-phase" })
        if (auto* window = shownPanel (tag);
            window != nullptr && window->hasKeyboardForScenario() && window->capturesKeyboardForScenario())
            return window->inputForScenario (description);
    return false;
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

bool MasteringView::compDoubleClickForScenario (const std::string& control)
{
   #if DUSKSTUDIO_HAS_NATIVE_UI
    return compProxy.isShowing() && compWindow != nullptr && compWindow->takesPointerForScenario()
        && compWindow->doubleClickControlForScenario (control);
   #else
    (void) control;
    return false;
   #endif
}

#if DUSKSTUDIO_HAS_NATIVE_UI
imgui::DuskPanelWindow* MasteringView::shownPanel (const std::string& panel) const
{
    const std::array<std::pair<imgui::DuskPanelWindow*, const NativePanelProxy*>, 4> panels { {
        { eqWindow.get(), &eqProxy }, { compWindow.get(), &compProxy },
        { limiterWindow.get(), &limiterProxy }, { phaseWindow.get(), &phaseProxy }
    } };
    static constexpr const char* tags[] = { "mastering-eq", "mastering-comp", "mastering-limiter", "mastering-phase" };
    for (std::size_t i = 0; i < panels.size(); ++i)
        if (panel == tags[i])
            return panels[i].second->isShowing() && panels[i].first != nullptr && panels[i].first->isOpen()
                 ? panels[i].first : nullptr;
    return nullptr;
}
#endif

bool MasteringView::panelPointerForScenario (const std::string& panel, const std::string& control, float position,
                                             bool pressed, int modifiers)
{
   #if DUSKSTUDIO_HAS_NATIVE_UI
    auto* window = shownPanel (panel);
    return window != nullptr && window->takesPointerForScenario()
        && window->pointerControlForScenario (control, position, pressed, modifiers);
   #else
    (void) panel;
    (void) control;
    (void) position;
    (void) pressed;
    (void) modifiers;
    return false;
   #endif
}

std::vector<int> MasteringView::panelAreaForScenario (const std::string& panel, const juce::Component& relativeTo) const
{
   #if DUSKSTUDIO_HAS_NATIVE_UI
    const std::array<std::pair<const char*, const NativePanelProxy*>, 4> proxies { {
        { "mastering-eq", &eqProxy }, { "mastering-comp", &compProxy },
        { "mastering-limiter", &limiterProxy }, { "mastering-phase", &phaseProxy }
    } };
    for (const auto& entry : proxies)
        if (panel == entry.first)
        {
            const auto area = relativeTo.getLocalArea (this, entry.second->getBounds());
            return { area.getX(), area.getY(), area.getWidth(), area.getHeight() };
        }
   #else
    (void) panel;
    (void) relativeTo;
   #endif
    return {};
}

bool MasteringView::panelCapturesKeyboardForScenario (const std::string& panel) const
{
   #if DUSKSTUDIO_HAS_NATIVE_UI
    const auto* window = shownPanel (panel);
    return window != nullptr && window->capturesKeyboardForScenario();
   #else
    (void) panel;
    return false;
   #endif
}

std::optional<std::array<float, 2>> MasteringView::controlForScenario (const std::string& panel,
                                                                       const std::string& control,
                                                                       float position) const
{
   #if DUSKSTUDIO_HAS_NATIVE_UI
    const auto* window = shownPanel (panel);
    if (window == nullptr || ! window->takesPointerForScenario()) return std::nullopt;
    const auto point = window->controlAtForScenario (control, position);
    if (! point) return std::nullopt;
    return std::array<float, 2> { point->x, point->y };
   #else
    (void) panel;
    (void) control;
    (void) position;
    return std::nullopt;
   #endif
}

bool MasteringView::compTypeForScenario (const std::string& text)
{
   #if DUSKSTUDIO_HAS_NATIVE_UI
    return compProxy.isShowing() && compWindow != nullptr && compWindow->typeForScenario (text);
   #else
    (void) text;
    return false;
   #endif
}

bool MasteringView::compCapturesKeyboardForScenario() const
{
   #if DUSKSTUDIO_HAS_NATIVE_UI
    return compProxy.isShowing() && compWindow != nullptr && compWindow->capturesKeyboardForScenario();
   #else
    return false;
   #endif
}

bool MasteringView::phasePointerForScenario (const std::string& control, float position, bool pressed, int modifiers)
{
   #if DUSKSTUDIO_HAS_NATIVE_UI
    return phaseProxy.isShowing() && phaseWindow != nullptr && phaseWindow->takesPointerForScenario()
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
    ask (compWindow, compProxy, "_mastering-comp.ppm");
    ask (limiterWindow, limiterProxy, "_mastering-limiter.ppm");
    ask (phaseWindow, phaseProxy, "_mastering-phase.ppm");
   #else
    (void) dir;
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
    return open (eqWindow, eqProxy) && open (compWindow, compProxy) && open (limiterWindow, limiterProxy)
        && open (phaseWindow, phaseProxy);
   #else
    return true;
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
}

void MasteringView::resized()
{
    auto area = getLocalBounds().reduced (kPageInset);

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

    const bool compact = area.getWidth() < kSideBySideMin;
    compactPanelCombo.setVisible (compact);
    if (compact)
    {
        compactPanelCombo.setBounds (area.removeFromTop (24));
        area.removeFromTop (6);
    }
    auto panelsRow = area;
    const int totalW    = std::max (0, panelsRow.getWidth() - 3 * kPanelGap);
    const int spare    = std::max (0, totalW - kEqMin - kCompMin - kLimiterMin - kPhaseMin);
    const int eqW      = kEqMin + (int) std::round (spare * 0.32);
    const int compW    = kCompMin + (int) std::round (spare * 0.30);
    const int limW     = kLimiterMin + (int) std::round (spare * 0.14);

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
    placeNativePanel (compProxy, compPanel);
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
                        && ! exportModal.isOpen() && deviceRate > 0.0
                        && session.mastering().phase.enabled.load (std::memory_order_relaxed);
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
    // A minimised window still counts as shown, as restoring it tells no component.
    if (! shownInLayout (*this))
    {
        stopTimer();
        timerCount.set (false);
        return;
    }
    if (std::exchange (catchUpTick, false))
        startTimerHz (30);
    // The window losing the keyboard is the user leaving it, where a native child's
    // own focus is not: a window manager moves that on a press inside the child.
    auto* const peer = getPeer();
    const bool focused = peer != nullptr && peer->isFocused();
    if (windowFocused && ! focused)
        closePanelLists();
    windowFocused = focused;
    // The switch also changes with a session load, which knows nothing of the
    // monitor fold.
    if (! session.mastering().phase.enabled.load (std::memory_order_relaxed) && engine.getMasteringMonoMonitor())
        engine.setMasteringMonoMonitor (false);
    updatePhaseAnalysis();
    auto& player = engine.getMasteringPlayer();
    auto& m = session.mastering();

    if (player.getSourceGeneration() != labelSourceGeneration)
        updateLabels();

    // Clock from the player playhead.
    const double sr = player.getSourceSampleRate();
    const double sec = (sr > 0.0) ? (double) player.getPlayhead() / sr : 0.0;
    const int mins = (int) (sec / 60.0);
    const int secs = (int) sec % 60;
    const int ms   = (int) ((sec - std::floor (sec)) * 1000.0);
    char text[64];
    std::snprintf (text, sizeof text, "%02d:%02d.%03d", mins, secs, ms);
    clockLabel.setText (text, juce::dontSendNotification);

    std::snprintf (text, sizeof text, "Comp GR %5.1f dB    Lim GR %5.1f dB",
                   (double) m.meterCompGrDb.load(), (double) m.meterLimiterGrDb.load());
    grLabel.setText (text, juce::dontSendNotification);

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
