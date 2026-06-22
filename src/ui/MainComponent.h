#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <memory>
#include "EmbeddedModal.h"
#include "DuskMenuBar.h"
#include "DuskStudioLookAndFeel.h"
#include "ConsoleView.h"
#include "MultiImportTargetPicker.h"
#include "AppConfig.h"
#include "../engine/AudioEngine.h"
#include "../session/Session.h"

namespace duskstudio
{
namespace dp { struct SongScan; }

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

    // Screenshot-capture harness (DUSKSTUDIO_CAPTURE_DIR). Synthesises a
    // small demo session, drives each documented stage / strip / modal,
    // writes PNGs into outDir, then quits the app. Defined in
    // ScreenshotCapture.cpp. No-op effect on normal runs (never called).
    void captureScreenshots (const juce::File& outDir);

    juce::StringArray getMenuBarNames() override;
    juce::PopupMenu   getMenuForIndex (int topLevelMenuIndex,
                                         const juce::String& menuName) override;
    void              menuItemSelected (int menuItemID,
                                          int topLevelMenuIndex) override;

    // MainWindow's close-confirm dialog calls this. Save-to-current if
    // session.json exists, else opens Save As. onComplete fires on the
    // message thread (sync if no chooser, async after dismissal).
    void saveSessionAndThen (std::function<void(bool)> onComplete);

    // Industry-standard dirty-only prompt. If autosave is newer than
    // session.json, shows Save / Don't Save / Cancel; otherwise quits.
    void requestQuit();

    // Sequenced shutdown — destroy-notify storm of a hard quit races
    // the compositor on Linux/Wayland (Mutter) and has been observed
    // to take down the whole session.
    //
    // Order: stop autosave -> stop transport (drain + commit) -> detach
    // audio callback -> drop every plugin editor window -> sync
    // windowing -> hide main window -> sync again -> post quit.
    void beginSafeShutdown();

    // Process-shutdown only. Drops plugin instance ownership without
    // destructing. See AudioEngine::leakAllPluginInstancesForShutdown.
    void leakAllPluginInstancesForShutdown();

    // Open a session from a path (a session.json file or a session directory
    // containing one) — used for command-line / file-manager "open with" and
    // the second-instance handoff. No-op if the path resolves to nothing.
    void openSessionPath (const juce::File& path);

private:
    void openAudioSettings();
    void openBounceDialog();
    void openBounceStemsDialog();
    void cleanOutUnreferencedFiles();
    void launchStartupDialog();
    void switchToStage (AudioEngine::Stage);

    // Show / hide the tape strip (timeline) and reflow. Shared by the Cmd+\
    // toggle and the TransportBar TIMELINE button.
    void setTimelineVisible (bool show);

    // Visibility / view construction / strip mode / toggle buttons.
    // Called by switchToStage AND by session-load so a rebuilt
    // consoleView lands in the right mode even when engine stage
    // didn't change.
    void syncStageUi (AudioEngine::Stage);
    void doMixdown();

    bool saveSessionTo (const juce::File& sessionDir);
    void saveAsPrompt();
    bool loadSessionFromJson (const juce::File& sessionJson);
    // Tail-half called either directly (no autosave) or from the
    // recovery prompt callback.
    bool finishLoadingSessionFrom (const juce::File& sessionJson,
                                    const juce::File& sessionDir);
    void openFromFilePrompt();
    // Runs `proceed` immediately if the session is clean, otherwise shows the
    // Save / Don't Save / Cancel prompt and runs `proceed` only after a
    // successful Save or an explicit Don't Save. Shared by New / Open / Open
    // Recent / New-from-Template so none of them silently discards unsaved work.
    void guardUnsavedThen (const juce::String& title, const juce::String& message,
                            std::function<void()> proceed);
    void newSessionPrompt();
    // The folder-pick + create half of newSessionPrompt — runs only once any
    // unsaved-changes prompt has been resolved.
    void promptNewSessionLocation();
    // True if the live session diverges from the last manual save / autosave.
    // Drives the unsaved-changes prompt on quit and on New Session.
    bool currentSessionDirty();
    // Reset to a clean default session in `dir` (NOT the current session saved
    // under a new name) and open it through the normal load path.
    void createNewSessionAt (const juce::File& dir);

    // FileChooser -> ImportTargetPicker (24 tracks, smart-sort +
    // recommendation) -> FileImporter on commit. Flips track.mode if
    // needed.
    void importPrompt();

    // DP song-folder import: folder picker -> DpImporter::scanSongFolder ->
    // DpImportDialog confirmation -> runDpImport drives FileImporter over the
    // fragments, assigning each to a track at song start.
    void importDpSongPrompt();
    void runDpImport (const dp::SongScan& scan, bool importMixer, bool importTimeline);

    // Shared between File-menu prompts and TapeStrip drag-drop.
    // trackHint >= 0 biases the recommendation when the file matches.
    void runAudioImportFlow (const juce::File& source,
                              juce::int64 timelineStart,
                              int trackHint);
    void runMidiImportFlow  (const juce::File& source,
                              juce::int64 timelineStart,
                              int trackHint);

    // Two batch flows:
    //  enqueueImports: per-file picker chain. Each onCommit pops next
    //    via kickNextImport; onCancel clears the queue. Sequential
    //    commits bump per-file hint to lastCommitted+1.
    //  openMultiImportPicker + enqueueImportsWithTargets: one modal
    //    with a row per file; commit dispatches the batch.
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
        int        trackIndex = -1;   // -1 = needs picker; >=0 pre-assigned
        bool       isMidi     = false;
    };
    std::vector<PendingImport> pendingImportQueue;
    juce::int64 pendingImportTimelineStart = 0;
    int  pendingImportInitialHint   = -1;
    int  pendingImportLastCommitted = -2;

    // 30 s autosave to session.json.autosave (atomic temp+rename). On
    // load, if newer than session.json, prompt to recover. Manual save
    // deletes the autosave.
    void timerCallback() override;
    void writeAutosave();
    juce::File getAutosaveFileFor (const juce::File& sessionDir) const;
    bool autosaveIsNewerThan (const juce::File& sessionJson) const;
    void deleteAutosaveFor   (const juce::File& sessionDir) const;
    static constexpr int kAutosaveIntervalMs = 30000;

    // Full JSON kept for quit-prompt diff + recovery; the heavy compare
    // path uses the hash fields below.
    juce::String lastSavedSessionJson;
    juce::String lastWrittenAutosaveJson;
    // M8 fingerprint: cached hash of the volatile-state-stripped JSON
    // so writeAutosave's dedup is a single 32-bit compare instead of a
    // full juce::String equality scan against a possibly-MB-sized
    // serialisation. juce::DefaultHashFunctions::generateHash returns
    // an int; we store as uint32 to avoid the sign bit. 0 = unseen
    // sentinel (legitimate hash collisions at 0 are vanishingly rare;
    // the worst-case false positive is a missed autosave on the first
    // tick after load, recovered on the next).
    std::uint32_t lastSavedSessionStrippedHash    { 0 };
    std::uint32_t lastWrittenAutosaveStrippedHash { 0 };
    void setLastSavedSessionJson (const juce::String& json);
    void setLastWrittenAutosaveJson (const juce::String& json);

    Session session;
    AudioEngine engine { session, appconfig::resolveWorkerCount() };

    DuskStudioLookAndFeel lookAndFeel;

    // JUCE only shows setTooltip() text when a TooltipWindow exists in
    // the tree. One here means every child gets tooltips without
    // wiring its own.
    juce::TooltipWindow tooltipWindow { this, 600 };

    // Dusk-native menu bar — same MenuBarModel API as juce::MenuBarComponent
    // but popups render via showContextMenu (in-window EmbeddedModal)
    // instead of native PopupMenu. Fixes XWayland popup flash / focus drop.
    DuskMenuBar menuBar;

    juce::TextButton recordingStageBtn { "RECORDING" };
    juce::TextButton mixingStageBtn    { "MIXING" };
    juce::TextButton auxStageBtn       { "AUX" };
    juce::TextButton masteringStageBtn { "MASTERING" };

    // Rebuilt by syncBankButtons to match ConsoleView::numBanks() (1..16).
    // All hidden when console shows every track at once (numBanks==1).
    // Lives here (not in ConsoleView) so the row sits below the stage
    // selector and freed vertical space inside the console goes to
    // channel-strip faders.
    std::vector<std::unique_ptr<juce::TextButton>> bankButtons;
    void syncBankButtons (int numBanks);

    // Pinned to the header row between the rightmost bank tab and the
    // tuner button. Forwards into TapeStrip's helpers; no-op when
    // TapeStrip is collapsed.
    juce::TextButton hdrZoomOutBtn { "-"    };
    juce::TextButton hdrZoomInBtn  { "+"    };
    juce::TextButton hdrZoomFitBtn { "Fit"  };
    // Tape-strip grid snap: on/off toggle + resolution picker. Write
    // session.snapToGrid / snapResolution, which the SnapHelpers gate every
    // region / marker / loop / tempo drag on.
    juce::TextButton hdrSnapBtn    { "Snap" };
    juce::TextButton hdrSnapResBtn { {}     };
    void showSnapResolutionMenu();
    void refreshSnapUi();

    // Every dialog opens via DuskFileBrowser (in-window EmbeddedModal)
    // which owns its own lifetime — no caller-side keep-alive. Standalone
    // juce::FileChooser had X11/Wayland positioning + stacking issues.
    std::unique_ptr<class MasteringView> masteringView;
    std::unique_ptr<class AuxView>       auxView;

    // Explicitly torn down in our dtor BEFORE AudioEngine destructs —
    // the dialog hosts an AudioDeviceSelectorComponent that listens to
    // engine.deviceManager. Letting JUCE's ModalComponentManager clean
    // it up at app exit (which runs AFTER us) would deref a freed
    // AudioDeviceManager → SIGSEGV.
    EmbeddedModal audioSettingsModal;
    EmbeddedModal mixdownModal;
    EmbeddedModal bounceModal;
    EmbeddedModal quitModal;
    // Replaces juce::AlertWindow so styling matches the rest of the app.
    EmbeddedModal recoveryModal;
    EmbeddedModal virtualKeyboardModal;
    EmbeddedModal importTargetModal;
    EmbeddedModal scanModal;
    EmbeddedModal shortcutsModal;
    EmbeddedModal supportersModal;
    void openShortcuts();

    // Scan-on-startup runs asynchronously behind a progress modal. Triggered
    // from the first resized() (the window is sized + on screen by then),
    // guarded so it fires exactly once.
    bool startupScanTriggered = false;
    // Set synchronously in the ctor when a startup dialog will be shown, so the
    // resized()-driven scan defers instead of stacking a second modal over it.
    // dismissStartupDialog() clears it and re-invokes the scan.
    bool startupDialogPending = false;
    // Set when the user picks Quit from the startup dialog: the dialog still
    // dismisses (tearing down the dim backdrop), but dismissStartupDialog must
    // NOT kick the deferred plugin scan into a process that's shutting down.
    bool startupQuitRequested = false;
    void maybeStartStartupPluginScan();

    void toggleVirtualKeyboard();

    // True once the audio callback is removed for shutdown — makes
    // detach idempotent and signals publishPluginStateForSave that the
    // atomic-park sleeps can be skipped.
    bool engineDetached = false;
    juce::Label statusLabel;

    // Helper for the top-bar status. Short label (session-dir name +
    // state suffix) with the full path attached as tooltip. Replaces
    // direct setText("Loaded: " + fullPath) calls that bloated the bar.
    // isAutosave appends "(autosave)" so the user can tell.
    void setStatusForPath (const juce::String& prefix,
                              const juce::File& path,
                              bool isAutosave = false);
    std::unique_ptr<ConsoleView> consoleView;
    std::unique_ptr<class TransportBar>      transportBar;
    std::unique_ptr<class TapeStrip>         tapeStrip;
    std::unique_ptr<class SystemStatusBar>  systemStatusBar;
    bool tapeStripExpanded = false;

    // Embedded modal so it appears centered on the main UI with a dim
    // backdrop. Both torn down together via dismissStartupDialog.
    std::unique_ptr<class StartupDialog> startupDialog;
    std::unique_ptr<class DimOverlay>    startupDim;
    // onDone runs AFTER the async teardown completes, so a caller can open the
    // next UI (e.g. a session-load recovery prompt) without stacking it over
    // the still-present startup dialog.
    void dismissStartupDialog (std::function<void()> onDone = {});

    // Cross-OS cursor overlay — paints Grab / Cut / Draw glyphs at the
    // mouse position via a 60 Hz JUCE timer, bypassing the platform
    // cursor pipeline entirely. See CursorOverlay.h for the design.
    std::unique_ptr<class CursorOverlay> cursorOverlay;

    // Constructed on demand on MidiRegion click; dismissed by clicking
    // backdrop or Esc. The roll is the single visible exception to
    // "no tabs / no hidden panels" per DuskStudio.md. Tracked indices
    // let the tape-strip click handler toggle (same region) vs swap
    // (different region).
    std::unique_ptr<class DimOverlay>          pianoRollDim;
    std::unique_ptr<class PianoRollComponent>  pianoRoll;
    int pianoRollTrackIdx  = -1;
    int pianoRollRegionIdx = -1;
    void openPianoRoll  (int trackIdx, int regionIdx);
    void closePianoRoll();          // immediate teardown (swap / mutual exclusion)
    void closePianoRollAnimated();  // collapse into region rect, then teardown

    // Mutually exclusive with the piano roll (opening one closes the other).
    std::unique_ptr<class DimOverlay>           audioEditorDim;
    std::unique_ptr<class AudioRegionEditor>    audioEditor;
    int audioEditorTrackIdx  = -1;
    int audioEditorRegionIdx = -1;
    void openAudioEditor  (int trackIdx, int regionIdx);
    void closeAudioEditor();
    void closeAudioEditorAnimated();

    // The edit tool (session.editMode) is global, but a modal editor changing
    // it (e.g. picking scissors in the audio editor) should not leak back to
    // the tapestrip. Snapshot the mode when the first modal opens, restore it
    // once the last modal closes. Restore is async so a swap / region-navigate
    // (close-then-open in one call stack) is seen as "still in a modal" and
    // skipped. modalEditModeSaved guards so swaps don't re-snapshot the
    // modal's tool. editMode is no longer persisted (SessionSerializer), so a
    // fresh launch is always the Grab default, not a stale Cut.
    EditMode savedEditMode      = EditMode::Grab;
    bool     modalEditModeSaved = false;
    void snapshotEditModeForModal();
    void scheduleEditModeRestore();
    void restoreEditModeIfModalClosed();

    // One-shot deferred teardown for the editor collapse animation — kept
    // alive while the ComponentAnimator runs, then fires the immediate
    // close. Only one region editor is open at a time, so a single slot
    // suffices.
    std::unique_ptr<juce::Timer> editorTeardownTimer;

    // 30 Hz timer polls Session::tuneLatestHz / Level into the overlay.
    // onDismiss closes the modal AND clears tuneTrackIndex so the
    // audio thread stops running the detector.
    std::unique_ptr<class DimOverlay>    tunerDim;
    std::unique_ptr<class TunerOverlay>  tuner;
    std::unique_ptr<class juce::Timer>   tunerPoller;
    void toggleTuner();
    void closeTuner();

    // Snapshot of the recent-sessions list captured when the File menu
    // opens, so menuItemSelected can resolve an "Open Recent" pick by
    // index without re-reading the file (and risking a different order
    // if a save fired between menu-open and click).
    juce::Array<juce::File> menuRecentSessions;
};
} // namespace duskstudio
