#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <array>
#include <memory>
#include <optional>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
#include "EmbeddedModal.h"
#include "DuskComboBox.h"
#include "RepaintGate.h"
#include "../engine/AudioEngine.h"
#include "../engine/audiofile/WaveformPeaks.h"
#include "../session/Session.h"
#include "../foundation/MessageThread.h"
#include "../dsp/PhaseAnalyzer.h"

namespace duskstudio
{
class MasteringPlayer;

namespace imgui { class DuskPanelWindow; }

// Inline waveform above the mastering controls + playhead line
// that follows MasteringPlayer. Click anywhere to seek.
class WaveformDisplay final : public juce::Component, private dusk::Timer
{
public:
    explicit WaveformDisplay (MasteringPlayer& player);
    ~WaveformDisplay() override;

    // Shows whatever the player has loaded. The timer picks up any load or
    // unload; a caller that just changed the source calls this to show it now.
    void followPlayer();
    // The 20 Hz refresh rests while the display is hidden. Its page hides and
    // shows it without a call of its own reaching here, so the page says when.
    void showStateChanged();
    void visibilityChanged() override { showStateChanged(); }
    bool refreshRunningForScenario() const noexcept { return isTimerRunning(); }
    const std::filesystem::path& shownFileForScenario() const { return shownFile; }
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    void timerCallback() override;

    MasteringPlayer&            player;
    dusk::audio::WaveformSource waveformSource { dusk::audio::WaveformSource::Use::OverviewOnly };
    dusk::audio::WaveformSource::Snapshot waveformSnapshot;
    std::uint64_t                shownSourceGeneration = 0;
    std::filesystem::path        shownFile;
    std::int64_t                 lastPlayhead = -1;
    MeterTimerCount              timerCount;
};

// Mastering-stage workspace. Loads stereo WAV (typically the freshest
// mixdown), plays through MasteringChain (digital EQ -> multiband comp ->
// brickwall limiter), shows post-limiter phase, loudness and dynamics meters.
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

    // Loads the session's mastering source into the player, or unloads the
    // player when the session names none; false when the named file would not
    // load. Needs no view: an open page follows the player on its timer and a
    // new one starts from it, so session switches call this directly.
    static bool loadSessionSource (AudioEngine& engine, Session& session);

    bool loadFile (const juce::File& file);
    // Shows what the player holds now rather than on the next timer tick.
    void followSource();
    std::string sourceTextForScenario() const { return sourceFileLabel.getText().toStdString(); }
    std::filesystem::path waveformFileForScenario() const
    {
        return waveform != nullptr ? waveform->shownFileForScenario() : std::filesystem::path();
    }
    bool refreshRunningForScenario() const noexcept { return isTimerRunning(); }
    bool waveformRefreshRunningForScenario() const noexcept
    {
        return waveform != nullptr && waveform->refreshRunningForScenario();
    }
    auto targetPointForScenario() const { return masteringTargetCombo.getBounds().getCentre(); }
    // Another session was loaded or started: the multiband picker reads
    // "Select preset..." again.
    void sessionReplaced();
    // Mono monitoring belongs to PHASE: off, it is released.
    void releaseMonoWithPhaseOff();
    bool compDoubleClickForScenario (const std::string& control);
    // The panel with this log tag ("mastering-eq", "mastering-comp",
    // "mastering-limiter", "mastering-phase"), worked as the pointer works it.
    bool panelPointerForScenario (const std::string& panel, const std::string& control, float position,
                                  bool pressed, int modifiers);
    bool panelCapturesKeyboardForScenario (const std::string& panel) const;
    // Where the panel with this log tag sits, in `relativeTo`'s coordinates,
    // whether or not it shows: x, y, width, height; empty for an unknown tag.
    std::vector<int> panelAreaForScenario (const std::string& panel, const juce::Component& relativeTo) const;
    int compPresetForScenario() const noexcept { return compPresetChoice; }
    void restoreCompPresetForScenario (int choice) { compPresetChoice = choice; }
    // x and y of the control in the last frame of the panel with this log tag,
    // while it drew all of it clear of the others and the panel is shown and takes
    // pointer input (DuskPanelWindow::takesPointerForScenario); empty otherwise. The
    // pointer calls below reach no panel before that either.
    std::optional<std::array<float, 2>> controlForScenario (const std::string& panel, const std::string& control,
                                                            float position) const;
    bool compTypeForScenario (const std::string& text);
    bool compCapturesKeyboardForScenario() const;
    // How many of the four framework children are up, or -1 when this build has
    // no native panels to open.
    int nativePanelsOpenForScenario() const;
    // How many the page shows side by side in this build, or -1 without any.
    static int nativePanelsExpectedForScenario();
    // The narrowest window, in the top-level window's width, that shows every module
    // side by side rather than one at a time.
    int sideBySideWindowWidthForScenario() const;
    // A key the shell received, for whichever native panel captures the keyboard.
    bool offerPanelKey (const std::string& description, std::uint32_t character);
    // A key as the display server would hand it to whichever native panel holds
    // the keyboard; false when none does.
    bool panelKeyForScenario (const std::string& description);
    bool phaseCapturesKeyboardForScenario() const;
    bool phaseHasKeyboardForScenario() const;
    bool phaseTypeForScenario (const std::string& text);
    void resetPhasePanelForScenario();
    bool phasePointerForScenario (const std::string& control, float position, bool pressed, int modifiers = 0);
    const PhaseAnalyzer::Report& phaseReportForScenario() const { return phaseAnalyzer.report(); }
    bool compactModuleShownForScenario() const { return compactPanelCombo.isShowing(); }
    int compactModuleIndexForScenario() const { return compactPanelCombo.getSelectedId() - 1; }
    auto compactModulePointForScenario() const { return compactPanelCombo.getBounds().getCentre(); }
    void restoreCompactModuleForScenario (int index) { compactPanelCombo.setSelectedId (index + 1); resized(); }
    std::string targetTextForScenario() const { return masteringTargetCombo.getText().toStdString(); }
    void restoreTargetForScenario (int index) { masteringTargetCombo.setSelectedId (index + 1); }
    std::string loudnessTextForScenario (bool peak) const
    {
        return (peak ? truePeak : lufsI).getText().toStdString();
    }
    std::uint32_t loudnessColourForScenario (bool peak) const
    {
        const auto& label = peak ? truePeak : lufsI;
        return label.findColour (decltype (lufsI)::backgroundColourId).getARGB();
    }

    // Screenshot harness only. The stage's EQ, compressor, limiter and Phase panels are framework children,
    // which the JUCE snapshot path cannot reach, so each reads its own steady frame back
    // into `dir` and reports where it belongs inside this view for the caller to paste
    // it into the snapshot.
    struct NativePanelCapture
    {
        juce::Rectangle<int> bounds;
        juce::File file;
    };
    std::vector<NativePanelCapture> captureNativePanels (const juce::File& dir);
    // False while a panel the layout shows has no window open yet.
    bool nativePanelsOpenForCapture() const;

private:
    void timerCallback() override;
    // The 30 Hz refresh is for what the page shows, so it rests while the
    // page is hidden and starts again, with a tick at once, when it shows.
    void showStateChanged();
    MeterTimerCount timerCount;
    bool catchUpTick = false;
    void updatePhaseAnalysis();
    void updateLabels (const std::string& rejectedPick = {});
    std::uint64_t labelSourceGeneration = 0;

   #if DUSKSTUDIO_HAS_NATIVE_UI
    // The panel with this log tag while its proxy shows and its window is open.
    imgui::DuskPanelWindow* shownPanel (const std::string& panel) const;
   #endif
    // A press anywhere on the page, or in another panel, or the window losing the
    // keyboard, closes a list left open in a native panel, which would otherwise keep
    // the keyboard from the shell. An X11 server can hand the page a press that lands
    // in a native panel as well, which must leave that panel's list open for the
    // release that picks from it.
    struct PagePress;
    bool onNativePanel (int x, int y) const;
    void closePanelLists (const imgui::DuskPanelWindow* pressed = nullptr);

    // Open or close the native panels to match the stage. Coalesced onto the next
    // message-loop tick, because the visibility change that asks for one arrives from
    // inside an EmbeddedModal teardown, which is no place to build a GL context.
    void scheduleNativePanelSync();
    void syncNativePanelVisibility();
    void applyNativePanelSync();

    void doLoadPrompt();
    void doLoadLatestMixdown();
    void doExport();
    void openExportBrowser (int preset);

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

    juce::TextButton exportButton { "Export master..." };

    std::unique_ptr<WaveformDisplay>   waveform;
    DuskComboBox compactPanelCombo;
    PhaseAnalyzer phaseAnalyzer;
    std::array<float, 4096> phaseLeft {}, phaseRight {};
    std::uint64_t phaseSourceGeneration = 0, phaseFeedGeneration = 0, phaseNextSequence = 0;
    bool phaseActive = false, phaseStreamValid = false;

    // The EQ, the compressor's controls, the limiter and Phase are native framework
    // children over the shell, placed on their proxy's rectangle. Each module has its
    // own engage switch.
   #if DUSKSTUDIO_HAS_NATIVE_UI
    std::unique_ptr<imgui::DuskPanelWindow> eqWindow;
    std::unique_ptr<imgui::DuskPanelWindow> compWindow;
    std::unique_ptr<imgui::DuskPanelWindow> limiterWindow;
    std::unique_ptr<imgui::DuskPanelWindow> phaseWindow;
    NativePanelProxy eqProxy;
    NativePanelProxy compProxy;
    NativePanelProxy limiterProxy;
    NativePanelProxy phaseProxy;
    bool nativeSyncPending = false;
   #endif

    // The multiband preset last picked, 0 for none: the picker keeps showing its
    // name until another session loads, however often the panel's window reopens.
    int compPresetChoice = 0;
    std::unique_ptr<PagePress> pagePress;
    bool windowFocused = false;

    EmbeddedModal exportModal;
};
} // namespace duskstudio
