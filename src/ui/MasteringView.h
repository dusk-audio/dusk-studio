#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <memory>
#include <vector>
#include "EmbeddedModal.h"
#include "DuskComboBox.h"
#include "../engine/AudioEngine.h"
#include "../session/Session.h"
#include "../foundation/MessageThread.h"

namespace duskstudio
{
class MasteringPlayer;

namespace imgui { class DuskPanelWindow; }

#if DUSKSTUDIO_HAS_NATIVE_UI
// A JUCE stand-in for one of the mastering stage's native panels. Its bounds are where
// the framework child goes, and its visibility is the one thing the covering-surface
// machinery already toggles: the tag is what makes an EmbeddedModal take a native
// surface down and put it back, and a framework child is exactly that. DGL refuses to
// hide a window while it is embedded, so "hidden" here means closed and reopened.
class NativePanelProxy final : public juce::Component
{
public:
    NativePanelProxy()
    {
        getProperties().set (kPluginEditorTag, true);
        setInterceptsMouseClicks (false, false);
    }

    std::function<void()> onVisibilityChanged;

private:
    void visibilityChanged() override
    {
        if (onVisibilityChanged)
            onVisibilityChanged();
    }
};
#endif

// Inline AudioThumbnail above the mastering controls + playhead line
// that follows MasteringPlayer. Click anywhere to seek.
class WaveformDisplay final : public juce::Component, private dusk::Timer
{
public:
    explicit WaveformDisplay (MasteringPlayer& player);
    ~WaveformDisplay() override;

    void setSource (const juce::File& file);  // empty file clears
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    void timerCallback() override;

    MasteringPlayer&            player;
    juce::AudioFormatManager    formatManager;
    juce::AudioThumbnailCache   thumbnailCache { 4 };
    juce::AudioThumbnail        thumbnail;
    std::int64_t                 lastPlayhead = -1;
};

// Mastering-stage workspace. Loads stereo WAV (typically the freshest
// mixdown), plays through MasteringChain (Tube EQ -> bus comp ->
// brickwall limiter), shows post-limiter peak meters + comp/limiter GR.
// Export = engine's BounceEngine in Mastering mode.
class MasteringView final : public juce::Component, private dusk::Timer
{
public:
    MasteringView (Session& session, AudioEngine& engine);
    ~MasteringView() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void visibilityChanged() override;
    void parentHierarchyChanged() override;

    bool loadFile (const juce::File& file);

    // Screenshot harness only. The stage's EQ and limiter panels are framework children,
    // which the JUCE snapshot path cannot reach, so each reads its own steady frame back
    // into `dir` and reports where it belongs inside this view for the caller to paste
    // it into the snapshot.
    struct NativePanelCapture
    {
        juce::Rectangle<int> bounds;
        juce::File file;
    };
    std::vector<NativePanelCapture> captureNativePanels (const juce::File& dir);

private:
    void timerCallback() override;
    void updateLabels();

    // Open or close the two native panels to match the stage. Coalesced onto the next
    // message-loop tick, because the visibility change that asks for one arrives from
    // inside an EmbeddedModal teardown, which is no place to build a GL context.
    void scheduleNativePanelSync();
    void applyNativePanelSync();

    void doLoadPrompt();
    void doLoadLatestMixdown();
    void doExport();
    void openExportBrowser (int preset);

    // Apply a DP-24-style 3-band preset to the mastering multiband compressor
    // (no-op without donor DSP). See dsp/MultibandCompPresets.h.
    void applyMultibandPreset (int presetIndex);

    Session& session;
    AudioEngine& engine;

    juce::Label       sourceFileLabel;
    juce::TextButton  loadButton           { "Load mix..." };
    juce::TextButton  loadLatestMixdown    { "Load latest mixdown" };

    juce::TextButton  playButton  { "Play" };
    juce::TextButton  stopButton  { "Stop" };
    juce::TextButton  rewindButton{ "|<<" };
    juce::Label       clockLabel;
    juce::Label       grLabel;

    juce::Label  lufsM, lufsS, lufsI, truePeak;
    // Backdrop rect around the TP/M/S/I cells, recomputed in resized().
    juce::Rectangle<int> loudnessClusterBounds;
    juce::TextButton resetLoudness { "Reset I" };

    DuskComboBox masteringTargetCombo;
    juce::Label    targetCaption;

    // Multiband-comp preset picker in the comp panel header (donor DSP only).
    juce::Label  compPresetCaption;
    DuskComboBox compPresetCombo;

    juce::TextButton exportButton { "Export master..." };

    std::unique_ptr<WaveformDisplay>   waveform;

    // EQ = curve + band controls, Limiter = Waves L4-style; both are native framework
    // children over the shell, placed on their proxy's rectangle. Comp embeds ONLY the
    // donor's MultibandCompressorPanel (not the full UniversalCompressor editor - the
    // mode selector for Opto/FET/VCA/Bus is irrelevant here). Each panel has its own ON
    // toggle.
    std::unique_ptr<juce::Component> compEditor;
   #if DUSKSTUDIO_HAS_NATIVE_UI
    std::unique_ptr<imgui::DuskPanelWindow> eqWindow;
    std::unique_ptr<imgui::DuskPanelWindow> limiterWindow;
    NativePanelProxy eqProxy;
    NativePanelProxy limiterProxy;
    bool nativeSyncPending = false;
   #endif

    // Plugin editor draws to its own bounds with no header - wrapper
    // hosts (title + ON toggle) above it. Without the wrapper there's
    // no way to bypass the comp outside a context menu.
    std::unique_ptr<juce::Component>              compPanelWrapper;
    // Shared console chrome for the donor multiband panel - the same LED-pill
    // header the EQ + limiter editors now use, so all three sections match.
    std::unique_ptr<class CompHeaderButton>       compHeaderBtn;
    // Last compEnabled state pushed to compHeaderBtn - the button only repaints
    // on its own click, so the 20 Hz timer watches this to pick up external
    // changes (e.g. session load).
    bool compHeaderEnabledSeen { false };

    EmbeddedModal exportModal;
};
} // namespace duskstudio
