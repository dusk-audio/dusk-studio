#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <memory>
#include "EmbeddedModal.h"
#include "DuskComboBox.h"
#include "../engine/AudioEngine.h"
#include "../session/Session.h"

namespace duskstudio
{
class MasteringPlayer;

// Inline AudioThumbnail above the mastering controls + playhead line
// that follows MasteringPlayer. Click anywhere to seek.
class WaveformDisplay final : public juce::Component, private juce::Timer
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
    juce::int64                 lastPlayhead = -1;
};

// Mastering-stage workspace. Loads stereo WAV (typically the freshest
// mixdown), plays through MasteringChain (Tube EQ -> bus comp ->
// brickwall limiter), shows post-limiter peak meters + comp/limiter GR.
// Export = engine's BounceEngine in Mastering mode.
class MasteringView final : public juce::Component, private juce::Timer
{
public:
    MasteringView (Session& session, AudioEngine& engine);
    ~MasteringView() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    bool loadFile (const juce::File& file);

private:
    void timerCallback() override;
    void updateLabels();

    void doLoadPrompt();
    void doLoadLatestMixdown();
    void doExport();

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

    std::unique_ptr<juce::FileChooser> fileChooser;
    std::unique_ptr<WaveformDisplay>   waveform;

    // EQ = custom curve + band controls. Comp embeds ONLY the donor's
    // MultibandCompressorPanel (not the full UniversalCompressor editor
    // — mode selector for Opto/FET/VCA/Bus is irrelevant here).
    // Limiter = Waves L4-style. Each panel has its own ON toggle.
    std::unique_ptr<class MasteringEqEditor>      eqEditor;
    std::unique_ptr<juce::Component>              compEditor;
    std::unique_ptr<class MasteringLimiterEditor> limiterEditor;

    // Plugin editor draws to its own bounds with no header — wrapper
    // hosts (title + ON toggle) above it. Without the wrapper there's
    // no way to bypass the comp outside a context menu.
    std::unique_ptr<juce::Component>              compPanelWrapper;
    // Shared console chrome for the donor multiband panel — the same LED-pill
    // header the EQ + limiter editors now use, so all three sections match.
    std::unique_ptr<class CompHeaderButton>       compHeaderBtn;
    // Last compEnabled state pushed to compHeaderBtn — the button only repaints
    // on its own click, so the 20 Hz timer watches this to pick up external
    // changes (e.g. session load).
    bool compHeaderEnabledSeen { false };

    EmbeddedModal exportModal;
};
} // namespace duskstudio
