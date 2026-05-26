#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <memory>
#include "EmbeddedModal.h"
#include "DuskMenuBar.h"
#include "DuskStudioLookAndFeel.h"
#include "ConsoleView.h"
#include "MultiImportTargetPicker.h"
#include "../engine/AudioEngine.h"
#include "../session/Session.h"

namespace duskstudio
{
class MainComponent final : public juce::Component,
                             public juce::MenuBarModel,
                             private juce::Timer
{
public:
    MainComponent();
    ~MainComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;

    // MenuBarModel overrides - drive the File / Settings menus at the top
    // of the window. Replaces the previous row of large TextButtons.
    juce::StringArray getMenuBarNames() override;
    juce::PopupMenu   getMenuForIndex (int topLevelMenuIndex,
                                         const juce::String& menuName) override;
    void              menuItemSelected (int menuItemID,
                                          int topLevelMenuIndex) override;

    // Public entry-point used by MainWindow's close-confirm dialog. Saves
    // to the current sessionDir if a session.json already exists there;
    // otherwise opens the Save As file chooser. The optional `onComplete`
    // callback runs on the message thread once the save is committed -
    // synchronously when no chooser is needed, asynchronously after the
    // chooser dismisses. The bool argument is true on success.
    void saveSessionAndThen (std::function<void(bool)> onComplete);

    // App-quit gate. Called from the window's close button. If there are
    // unsaved changes since the last manual save (autosave file is newer
    // than session.json), shows a Dusk Studio-styled modal with Save / Don't
    // Save / Cancel; otherwise quits immediately. Industry-standard
    // dirty-only prompt - matches Logic / Pro Tools / Bitwig.
    void requestQuit();

    // Staged shutdown that quiesces the engine and tears down native
    // window peers in an order the host windowing system can keep up
    // with. Without this sequencing the destroy-notify storm of a
    // hard quit can race the compositor / window manager and on
    // Linux/Wayland (Mutter) has been observed to take down the
    // whole desktop session.
    //
    // Order: stop autosave timer, stop transport (drains in-flight
    // record buffers + commits regions), detach the audio callback
    // (audio thread stops calling processBlock on plugins about to
    // be torn down), drop every plugin editor window, sync the
    // windowing system, hide the main window, sync again, then
    // post the quit message.
    void beginSafeShutdown();

    // Process-shutdown only: relinquishes ownership of every loaded
    // plugin instance without destroying it. Called from
    // DuskStudioApp::shutdown() right before mainWindow.reset() so the
    // engine teardown skips plugin destruction (some Linux plugins
    // abort the process from their destructor - see
    // AudioEngine::leakAllPluginInstancesForShutdown for the why).
    void leakAllPluginInstancesForShutdown();

private:
    void openAudioSettings();
    void openBounceDialog();
    void openBounceStemsDialog();
    void cleanOutUnreferencedFiles();
    void launchStartupDialog();
    void switchToStage (AudioEngine::Stage);
    // Side-effects-only of switchToStage (visibility, view construction,
    // strip mode, toggle buttons). Called by switchToStage AND by the
    // session-load path so a rebuilt consoleView lands in the correct
    // mode even when the engine stage didn't change.
    void syncStageUi (AudioEngine::Stage);
    void doMixdown();

    // Session-management helpers shared by the header buttons and the
    // startup dialog. All run on the message thread.
    bool saveSessionTo (const juce::File& sessionDir);   // writes session.json, returns true on success
    void saveAsPrompt();                                 // 2-step: name + parent dir
    bool loadSessionFromJson (const juce::File& sessionJson);
    // Synchronous tail-half of loadSessionFromJson. Called either directly
    // (no autosave present) or from the autosave recovery prompt's callback
    // once the user has picked which file to load from.
    bool finishLoadingSessionFrom (const juce::File& sessionJson,
                                    const juce::File& sessionDir);
    void openFromFilePrompt();                           // file picker for session.json
    void newSessionPrompt();                             // dir picker + setSessionDirectory + immediate save

    // File » Import Audio... / Import MIDI... entry points. Each opens a
    // juce::FileChooser, then a target-picker modal (ImportTargetPicker)
    // listing all 16 tracks with smart-sort ranking + recommendation.
    // On commit, runs FileImporter on the message thread and appends the
    // resulting region to the chosen track (flipping track.mode first if
    // a mismatched-mode track was picked).
    void importAudioPrompt();
    void importMidiPrompt();

    // Shared import-flow plumbing reused by both the File-menu prompts
    // and the TapeStrip drag-and-drop callback. Each opens the
    // target-picker modal with the given source + timeline position;
    // trackHint (>=0) biases the picker's recommendation to that row
    // when the dropped file is compatible.
    void runAudioImportFlow (const juce::File& source,
                              juce::int64 timelineStart,
                              int trackHint);
    void runMidiImportFlow  (const juce::File& source,
                              juce::int64 timelineStart,
                              int trackHint);

    // Multi-file batch import. Two entry points:
    //   * enqueueImports - per-file picker chain (single-file flow). Each
    //     runFlow's onCommit pops the next file via kickNextImport;
    //     onCancel clears the queue so a single Cancel aborts the rest.
    //     Sequential commits bump the per-file hint to lastCommitted+1.
    //   * openMultiImportPicker / enqueueImportsWithTargets - shows ONE
    //     modal with a row per file and a track dropdown each. User picks
    //     every target explicitly; commit dispatches the whole batch
    //     without further modals.
    void enqueueImports (juce::Array<juce::File> files,
                          juce::int64 timelineStart,
                          int trackHint);
    void openMultiImportPicker (juce::Array<juce::File> files,
                                  juce::int64 timelineStart);
    void enqueueImportsWithTargets (std::vector<MultiImportTargetPicker::Assignment> assignments,
                                       juce::int64 timelineStart);
    void kickNextImport();
    void commitImportNoModal (const MultiImportTargetPicker::Assignment& a,
                                juce::int64 timelineStart);
    void cancelImportChain();

    struct PendingImport
    {
        juce::File file;
        int        trackIndex = -1;   // -1 = needs picker; >=0 = pre-assigned
        bool       isMidi     = false;
    };
    std::vector<PendingImport> pendingImportQueue;
    juce::int64 pendingImportTimelineStart = 0;
    int  pendingImportInitialHint   = -1;
    int  pendingImportLastCommitted = -2;

    // Autosave: a juce::Timer fires every 30s and writes a session.json.autosave
    // sibling using the same atomic temp+rename pattern as the manual save. On
    // session load (loadSessionFromJson) we check whether the autosave is newer
    // than session.json - if so, prompt the user to recover. Manual saves
    // delete the autosave so the prompt doesn't fire on the next clean load.
    void timerCallback() override;
    void writeAutosave();
    juce::File getAutosaveFileFor (const juce::File& sessionDir) const;
    // Returns true if session.json.autosave exists and is newer than session.json.
    // Caller drives the user-facing prompt.
    bool autosaveIsNewerThan (const juce::File& sessionJson) const;
    void deleteAutosaveFor   (const juce::File& sessionDir) const;
    static constexpr int kAutosaveIntervalMs = 30000;

    // Last serialised state snapshots. writeAutosave compares the current
    // snapshot against both to skip redundant writes - so the autosave
    // file's mtime doesn't creep past session.json's just because the
    // timer keeps firing on an unchanged session. Stored verbatim (not
    // hashed) so no extra JUCE module is needed; session JSON is at most
    // a few hundred KB.
    juce::String lastSavedSessionJson;
    juce::String lastWrittenAutosaveJson;
    // Stripped (volatile-plugin-state nulled) caches updated whenever the
    // parents are assigned. Avoid re-running stripVolatileState 3× per
    // 30 Hz autosave tick.
    juce::String lastSavedSessionJsonStripped;
    juce::String lastWrittenAutosaveJsonStripped;
    void setLastSavedSessionJson (const juce::String& json);
    void setLastWrittenAutosaveJson (const juce::String& json);

    Session session;
    AudioEngine engine { session };

    DuskStudioLookAndFeel lookAndFeel;

    // App-wide tooltip dispatcher. JUCE only displays setTooltip()
    // strings when a TooltipWindow exists somewhere in the component
    // tree; without one, every setTooltip() call is silent. Owning
    // it here means every child (transport, editors, status bar)
    // gets tooltips without each one wiring its own.
    juce::TooltipWindow tooltipWindow { this, 600 };

    // Menu bar at the very top. Replaces the prior row of TextButtons
    // (Audio settings... / Save / Save As... / Open... / Mixdown / Bounce...)
    // - the menu bar is much slimmer and reads as a normal app menu.
    // Dusk-native menu bar: same model API as juce::MenuBarComponent
    // (MainComponent already inherits MenuBarModel) but the popup is
    // rendered via showContextMenu (in-window EmbeddedModal) instead
    // of juce::PopupMenu's native popup. Fixes XWayland popup flash /
    // focus drop the same way the other Dusk-native menus do.
    DuskMenuBar menuBar;

    // Stage selector - segmented control. Drives both engine.setStage() and
    // which view the body shows.
    juce::TextButton recordingStageBtn { "RECORDING" };
    juce::TextButton mixingStageBtn    { "MIXING" };
    juce::TextButton auxStageBtn       { "AUX" };
    juce::TextButton masteringStageBtn { "MASTERING" };

    // Bank A / B buttons. Used when the window is too narrow to show all
    // 16 channel strips at once - we show 8 at a time and the user toggles
    // between bank A (1-8) and bank B (9-16). Lives here in MainComponent
    // (rather than inside ConsoleView) so the row sits directly below the
    // stage selector and the freed vertical space inside the console all
    // goes to the channel strips' fader sections.
    // Dynamic bank-button row. Size is rebuilt by syncBankButtons() to
    // match ConsoleView::numBanks() (1..16). All hidden when the console
    // shows every track at once (numBanks==1). Labels and click bindings
    // are regenerated by syncBankButtons whenever the bank stride
    // changes (window resize, stage swap).
    std::vector<std::unique_ptr<juce::TextButton>> bankButtons;
    void syncBankButtons (int numBanks);

    // SNAP + zoom cluster pulled out of TapeStrip and pinned to the
    // header row between the rightmost bank tab and the tuner button
    // on the transport bar. Forwards clicks into TapeStrip's public
    // zoom/snap helpers; no-op when TapeStrip is collapsed.
    juce::TextButton hdrSnapToggle { "SNAP" };
    juce::TextButton hdrZoomOutBtn { "-"    };
    juce::TextButton hdrZoomInBtn  { "+"    };
    juce::TextButton hdrZoomFitBtn { "Fit"  };

    // File-dialog chooser pointers removed: every dialog now opens via
    // DuskFileBrowser (in-window EmbeddedModal panel) which owns its
    // own lifetime — no caller-side keep-alive needed across the
    // async callback. juce::FileChooser standalone windows had X11/
    // Wayland positioning + stacking issues that the in-window panel
    // sidesteps entirely.
    std::unique_ptr<class MasteringView> masteringView;
    std::unique_ptr<class AuxView>       auxView;

    // Track the audio settings DialogWindow so we can explicitly delete it
    // in our destructor BEFORE AudioEngine destructs. Required because the
    // dialog hosts an AudioDeviceSelectorComponent that's a change-listener
    // on engine.deviceManager - if we let JUCE's ModalComponentManager clean
    // it up at app exit (via ScopedJuceInitialiser_GUI's destructor, which
    // runs AFTER us), the listener removal would dereference a freed
    // AudioDeviceManager → SIGSEGV.
    EmbeddedModal audioSettingsModal;
    EmbeddedModal mixdownModal;
    EmbeddedModal bounceModal;
    EmbeddedModal quitModal;
    // Autosave-recovery prompt shown during loadSessionFromJson when an
    // autosave file is newer than session.json. Replaces the native
    // juce::AlertWindow that didn't match the rest of the app's modal
    // styling.
    EmbeddedModal recoveryModal;
    EmbeddedModal virtualKeyboardModal;
    EmbeddedModal importTargetModal;
    void toggleVirtualKeyboard();

    // True once the audio callback has been removed in preparation for
    // shutdown. Used by saveSessionTo / beginSafeShutdown to make the
    // detach call idempotent and to signal publishPluginStateForSave
    // that the atomic-park sleeps can be skipped (no audio thread to
    // race). Reset is unnecessary - the only path that sets this also
    // ends in systemRequestedQuit.
    bool engineDetached = false;
    juce::Label statusLabel;

    // Helper for the top-bar status line. Shows a short label
    // (session-dir name + state suffix like "(autosave)") with the
    // full path attached as a tooltip. Replaces every direct
    // statusLabel.setText("Loaded: " + fullPath) call so the top bar
    // doesn't get dominated by long path strings.
    //   prefix    - "Loaded", "Saved", "Load failed", etc.
    //   path      - the sourced file (used for name + tooltip).
    //   isAutosave - appends "(autosave)" so the user can tell which
    //                file the load came from.
    void setStatusForPath (const juce::String& prefix,
                              const juce::File& path,
                              bool isAutosave = false);
    std::unique_ptr<ConsoleView> consoleView;
    std::unique_ptr<class TransportBar>      transportBar;
    std::unique_ptr<class TapeStrip>         tapeStrip;
    std::unique_ptr<class SystemStatusBar>  systemStatusBar;
    bool tapeStripExpanded = false;  // collapsed by default; user expands when arranging

    // Startup dialog now lives as an embedded modal (no separate window) so
    // it appears centered on the main UI with a dim backdrop. Both pieces
    // are owned here and torn down together via dismissStartupDialog.
    std::unique_ptr<class StartupDialog> startupDialog;
    std::unique_ptr<class DimOverlay>    startupDim;
    void dismissStartupDialog();

    // Piano-roll overlay - constructed on demand when the user clicks a
    // MidiRegion in the tape strip; dismissed by clicking the dim backdrop
    // or pressing Esc. The roll is the single visible exception to "no
    // tabs / no hidden panels" per Dusk Studio.md (the spec calls it out).
    // Tracks which region is currently open so the tape-strip click handler
    // can toggle (same region) vs swap (different region).
    std::unique_ptr<class DimOverlay>          pianoRollDim;
    std::unique_ptr<class PianoRollComponent>  pianoRoll;
    int pianoRollTrackIdx  = -1;
    int pianoRollRegionIdx = -1;
    void openPianoRoll  (int trackIdx, int regionIdx);
    void closePianoRoll();

    // Audio-region editor - sister to the piano roll, opens on double-click
    // of an audio region in the tape strip. Same DimOverlay + centred-panel
    // pattern. Mutually exclusive with the piano roll (opening one closes
    // the other).
    std::unique_ptr<class DimOverlay>           audioEditorDim;
    std::unique_ptr<class AudioRegionEditor>    audioEditor;
    int audioEditorTrackIdx  = -1;
    int audioEditorRegionIdx = -1;
    void openAudioEditor  (int trackIdx, int regionIdx);
    void closeAudioEditor();

    // Tuner overlay - same modal pattern as the piano roll. While open,
    // a 30 Hz timer polls Session::tuneLatestHz / tuneLatestLevel into
    // the overlay's setDetected(). The overlay's onDismiss closes the
    // modal AND clears Session::tuneTrackIndex so the audio thread stops
    // running the detector when nobody's looking.
    std::unique_ptr<class DimOverlay>    tunerDim;
    std::unique_ptr<class TunerOverlay>  tuner;
    std::unique_ptr<class juce::Timer>   tunerPoller;
    void toggleTuner();
    void closeTuner();
};
} // namespace duskstudio
