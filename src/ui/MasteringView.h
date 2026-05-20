#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <memory>
#include "EmbeddedModal.h"
#include "../engine/AudioEngine.h"
#include "../session/Session.h"

namespace duskstudio
{
class MasteringPlayer;

// Inline waveform display that lives above the mastering controls. Shows
// the loaded mixdown's full waveform (via juce::AudioThumbnail) plus a
// vertical playhead line that follows MasteringPlayer's playhead. Click
// anywhere on the waveform to seek.
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
// The Mastering-stage workspace. Loads a stereo WAV (typically the
// mixdown the user just produced from the Mixing stage), plays it through
// AudioEngine's MasteringChain (Tube EQ → bus comp → brickwall limiter),
// and shows post-limiter peak meters + comp/limiter gain reduction.
//
// Export is wired up via the engine's BounceEngine running in Mastering
// mode (see MasteringView::doExport).
class MasteringView final : public juce::Component, private juce::Timer
{
public:
    MasteringView (Session& session, AudioEngine& engine);
    ~MasteringView() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    // Convenience for the parent: load a specific file (e.g. just-finished
    // mixdown). Returns true on success and updates the UI label.
    bool loadFile (const juce::File& file);

private:
    void timerCallback() override;
    void updateLabels();
    void rebuildKnobValues();

    void doLoadPrompt();
    void doLoadLatestMixdown();
    void doExport();

    Session& session;
    AudioEngine& engine;

    // Header row.
    juce::Label       sourceFileLabel;
    juce::TextButton  loadButton           { "Load mix..." };
    juce::TextButton  loadLatestMixdown    { "Load latest mixdown" };

    // Transport row.
    juce::TextButton  playButton  { "Play" };
    juce::TextButton  stopButton  { "Stop" };
    juce::TextButton  rewindButton{ "|<<" };
    juce::Label       clockLabel;
    juce::Label       grLabel;     // limiter GR + comp GR

    // DSP knob groups. Three columns.
    struct KnobGroup
    {
        juce::Label  title;
        juce::ToggleButton enable;
    };
    KnobGroup eqGroup, compGroup, limGroup;

    // EQ
    juce::Slider eqLfBoost, eqHfBoost, eqHfAtten, eqTubeDrive, eqOutput;
    juce::Label  eqLfBoostL, eqHfBoostL, eqHfAttenL, eqTubeDriveL, eqOutputL;

    // Bus comp
    juce::Slider compThresh, compRatio, compAttack, compRelease, compMakeup;
    juce::Label  compThreshL, compRatioL, compAttackL, compReleaseL, compMakeupL;

    // Limiter
    juce::Slider limDrive, limCeiling, limRelease;
    juce::Label  limDriveL, limCeilingL, limReleaseL;

    // Output meter (peak L/R after limiter) + LUFS readout.
    juce::Label  meterL, meterR;
    juce::Label  lufsM, lufsS, lufsI, truePeak;
    juce::TextButton resetLoudness { "Reset I" };

    // Streaming-platform target preset. Drives I-LUFS / TP cell coloring.
    juce::ComboBox masteringTargetCombo;
    juce::Label    targetCaption;

    // Export footer.
    juce::TextButton exportButton { "Export master..." };

    std::unique_ptr<juce::FileChooser> fileChooser;
    std::unique_ptr<WaveformDisplay>   waveform;

    // Embedded editors for the bottom row (EQ | Multiband Comp | Limiter).
    // EQ uses a custom curve display + band controls (MasteringEqEditor).
    // Multiband Comp embeds ONLY the multi-comp donor's MultibandCompressor-
    // Panel — not the full UniversalCompressor editor (which would include
    // the mode selector for Opto/FET/VCA/Bus/etc., irrelevant here).
    // Limiter uses a Waves L4-style custom MasteringLimiterEditor.
    // Each panel hosts its own ON toggle in its header.
    std::unique_ptr<class MasteringEqEditor>      eqEditor;
    std::unique_ptr<juce::Component>              compEditor;
    std::unique_ptr<class MasteringLimiterEditor> limiterEditor;

    // Wrapper component that hosts the embedded plugin editor and adds a
    // section-header (title + ON toggle) above it. Without the wrapper the
    // plugin editor draws to its own bounds with no header, so there's no
    // way to bypass the comp without going to a separate context-menu.
    std::unique_ptr<juce::Component>              compPanelWrapper;
    juce::Label                                   compPanelTitle;
    juce::ToggleButton                            compPanelEnable { "ON" };

    // Embedded modal host for the master export dialog. Same UX as the
    // mixdown / bounce dialogs in MainComponent.
    EmbeddedModal exportModal;
};
} // namespace duskstudio
