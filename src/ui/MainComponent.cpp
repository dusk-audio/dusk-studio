#include "MainComponent.h"
#include <cstdio>
#include <cstdlib>  // std::getenv (DUSKSTUDIO_USE_OOP_PLUGINS)
#include "AppConfig.h"
#include "AudioSettingsPanel.h"
#include "DuskFileBrowser.h"
#include "AuxView.h"
#include "BounceDialog.h"
#include "ConsoleView.h"
#include "DimOverlay.h"
#include "DuskAlerts.h"
#include "PianoRollComponent.h"
#include "PlatformWindowing.h"
#include "AudioRegionEditor.h"
#include "TunerOverlay.h"
#include "VirtualKeyboardComponent.h"
#include "../session/SessionTemplates.h"
#include "MasteringView.h"
#include "StartupDialog.h"
#include "SystemStatusBar.h"
#include "TapeStrip.h"
#include "TransportBar.h"
#include "../session/MarkerEditActions.h"
#include "../session/RecentSessions.h"
#include "../session/SessionSerializer.h"
#include "../engine/FileImporter.h"
#include "ImportTargetPicker.h"
#include <juce_audio_formats/juce_audio_formats.h>

namespace duskstudio
{
namespace
{
// Shared helpers for the file-import flow (both menu-driven and
// drag-and-drop). Lives at file scope so the TapeStrip drop callback
// wired up in the MainComponent ctor can reference them.
juce::AudioFormatManager& importAudioFormatManager()
{
    // AudioFormatManager is non-copyable; constexpr-init isn't an option.
    // Cheap to construct + register; share a static instance and lazily
    // register on first use via the flag below.
    static juce::AudioFormatManager fm;
    static bool registered = false;
    if (! registered)
    {
        fm.registerBasicFormats();
        registered = true;
    }
    return fm;
}

// Route every import-error alert through the in-window Dusk alert
// modal. The free function pulls the active top-level so existing
// call sites don't need to thread a Component reference through.
void showImportError (const juce::String& title, const juce::String& message)
{
    if (auto* tlw = juce::TopLevelWindow::getActiveTopLevelWindow())
    {
        if (auto* dw = dynamic_cast<juce::DocumentWindow*> (tlw))
            if (auto* content = dw->getContentComponent())
            {
                showDuskAlert (*content, title, message);
                return;
            }
    }
    // Fallback only when no top-level exists (shouldn't happen in
    // normal use; safety net for headless / shutdown paths).
    juce::AlertWindow::showAsync (
        juce::MessageBoxOptions()
            .withIconType (juce::MessageBoxIconType::WarningIcon)
            .withTitle (title)
            .withMessage (message)
            .withButton ("OK"),
        nullptr);
}
} // namespace

// Tracks the top-level window so AuxView's editor hosts (separate X11
// toplevels on Linux/Wayland) can follow the main window across the
// screen. Override targets ComponentMovementWatcher protected virtuals.
namespace
{
// Dusk Studio-styled "save changes before quitting?" panel hosted by
// MainComponent::quitModal via EmbeddedModal. Three actions: Save (writes
// session.json or pops the Save As chooser, then quits on success),
// Don't Save (quits immediately, autosave keeps the changes for next
// launch's recovery prompt), Cancel (just dismisses).
class QuitConfirmDialog final : public juce::Component
{
public:
    QuitConfirmDialog()
    {
        titleLabel.setText ("Save changes before quitting?", juce::dontSendNotification);
        titleLabel.setFont (juce::Font (juce::FontOptions (18.0f, juce::Font::bold)));
        titleLabel.setColour (juce::Label::textColourId, juce::Colour (0xffe8e8e8));
        addAndMakeVisible (titleLabel);

        bodyLabel.setText (
            "Your session has unsaved changes since the last manual save. "
            "If you don't save, the autosave will still be available "
            "the next time you open this session.",
            juce::dontSendNotification);
        bodyLabel.setFont (juce::Font (juce::FontOptions (13.0f)));
        bodyLabel.setColour (juce::Label::textColourId, juce::Colour (0xffc0c0c8));
        bodyLabel.setJustificationType (juce::Justification::topLeft);
        bodyLabel.setMinimumHorizontalScale (1.0f);
        addAndMakeVisible (bodyLabel);

        styleAccent (saveButton);
        styleNeutral (dontSaveButton);
        styleNeutral (cancelButton);

        saveButton    .onClick = [this] { if (onSave)     onSave(); };
        dontSaveButton.onClick = [this] { if (onDontSave) onDontSave(); };
        cancelButton  .onClick = [this] { if (onCancel)   onCancel(); };

        addAndMakeVisible (saveButton);
        addAndMakeVisible (dontSaveButton);
        addAndMakeVisible (cancelButton);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff1a1a20));
        g.setColour (juce::Colour (0xff2a2a32));
        g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (0.5f), 6.0f, 1.0f);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (24);
        titleLabel.setBounds (area.removeFromTop (28));
        area.removeFromTop (10);
        bodyLabel.setBounds (area.removeFromTop (60));

        // Right-aligned button row, primary action on the right (Save).
        auto buttonRow = area.removeFromBottom (36);
        const int btnW = 110;
        saveButton    .setBounds (buttonRow.removeFromRight (btnW));
        buttonRow.removeFromRight (8);
        dontSaveButton.setBounds (buttonRow.removeFromRight (btnW));
        buttonRow.removeFromRight (8);
        cancelButton  .setBounds (buttonRow.removeFromRight (90));
    }

    std::function<void()> onSave;
    std::function<void()> onDontSave;
    std::function<void()> onCancel;

private:
    static void styleAccent (juce::TextButton& b)
    {
        b.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff4a7c9e));
        b.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
    }
    static void styleNeutral (juce::TextButton& b)
    {
        b.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff2a2a30));
        b.setColour (juce::TextButton::textColourOffId, juce::Colour (0xffe0e0e0));
    }

    juce::Label      titleLabel;
    juce::Label      bodyLabel;
    juce::TextButton saveButton    { "Save" };
    juce::TextButton dontSaveButton { "Don't Save" };
    juce::TextButton cancelButton  { "Cancel" };
};

// Autosave recovery prompt. Replaces juce::AlertWindow::showAsync which
// rendered the native question-mark dialog and didn't match the rest
// of the app's modal styling. Same three actions: Recover (load the
// newer autosave), Load (load the saved session, autosave is discarded
// by the next manual save), Cancel (bail out of the load entirely).
class AutosaveRecoveryDialog final : public juce::Component
{
public:
    AutosaveRecoveryDialog (const juce::File& sessionDir,
                              const juce::Time& autosaveTime,
                              const juce::Time& savedTime)
    {
        titleLabel.setText ("Recover from autosave?", juce::dontSendNotification);
        titleLabel.setFont (juce::Font (juce::FontOptions (18.0f, juce::Font::bold)));
        titleLabel.setColour (juce::Label::textColourId, juce::Colour (0xffe8e8e8));
        addAndMakeVisible (titleLabel);

        bodyLabel.setText (
            "An autosave file is newer than the saved session at\n"
            + sessionDir.getFullPathName()
            + "\n\nAutosave:  " + autosaveTime.toString (true, true)
            + "\nSaved:        " + savedTime    .toString (true, true)
            + "\n\nDusk Studio probably exited unexpectedly. "
              "Recover the newer autosave, or load the saved session and "
              "discard it?",
            juce::dontSendNotification);
        bodyLabel.setFont (juce::Font (juce::FontOptions (13.0f)));
        bodyLabel.setColour (juce::Label::textColourId, juce::Colour (0xffc0c0c8));
        bodyLabel.setJustificationType (juce::Justification::topLeft);
        bodyLabel.setMinimumHorizontalScale (1.0f);
        addAndMakeVisible (bodyLabel);

        recoverButton .setColour (juce::TextButton::buttonColourId,  juce::Colour (0xff4a7c9e));
        recoverButton .setColour (juce::TextButton::textColourOffId, juce::Colours::white);
        loadButton    .setColour (juce::TextButton::buttonColourId,  juce::Colour (0xff2a2a30));
        loadButton    .setColour (juce::TextButton::textColourOffId, juce::Colour (0xffe0e0e0));
        cancelButton  .setColour (juce::TextButton::buttonColourId,  juce::Colour (0xff2a2a30));
        cancelButton  .setColour (juce::TextButton::textColourOffId, juce::Colour (0xffe0e0e0));

        recoverButton.onClick = [this] { if (onRecover) onRecover(); };
        loadButton   .onClick = [this] { if (onLoad)    onLoad();    };
        cancelButton .onClick = [this] { if (onCancel)  onCancel();  };

        addAndMakeVisible (recoverButton);
        addAndMakeVisible (loadButton);
        addAndMakeVisible (cancelButton);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff1a1a20));
        g.setColour (juce::Colour (0xff2a2a32));
        g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (0.5f), 6.0f, 1.0f);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (24);
        titleLabel.setBounds (area.removeFromTop (28));
        area.removeFromTop (10);
        bodyLabel.setBounds (area.removeFromTop (140));

        auto buttonRow = area.removeFromBottom (36);
        recoverButton.setBounds (buttonRow.removeFromRight (160));
        buttonRow.removeFromRight (8);
        loadButton   .setBounds (buttonRow.removeFromRight (170));
        buttonRow.removeFromRight (8);
        cancelButton .setBounds (buttonRow.removeFromRight (90));
    }

    std::function<void()> onRecover;
    std::function<void()> onLoad;
    std::function<void()> onCancel;

private:
    juce::Label      titleLabel;
    juce::Label      bodyLabel;
    juce::TextButton recoverButton { "Recover autosave" };
    juce::TextButton loadButton    { "Load saved session" };
    juce::TextButton cancelButton  { "Cancel" };
};
} // namespace

MainComponent::MainComponent()
{
    // Accessibility floor — screen readers announce the root view as
    // "Dusk Studio mixer" instead of "Component". Tab navigation across
    // strips is enabled per-strip via setWantsKeyboardFocus on each
    // ChannelStripComponent in resized().
    setTitle ("Dusk Studio mixer");
    setDescription ("16-channel portastudio-style mixer");

    juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);

   #if DUSKSTUDIO_HAS_OOP_PLUGINS
    // DUSKSTUDIO_USE_OOP_PLUGINS=1 routes new plugin loads through the
    // dusk-studio-plugin-host child process. Read once at startup; flipping
    // mid-session would require reloading every plugin to pick up the
    // new mode. Off by default while the OOP path is still maturing.
    if (auto* env = std::getenv ("DUSKSTUDIO_USE_OOP_PLUGINS");
        env != nullptr && *env != 0 && *env != '0')
    {
        engine.getPluginManager().setOopEnabled (true);
        std::fprintf (stdout,
                      "[Dusk Studio] Out-of-process plugin hosting enabled "
                      "(DUSKSTUDIO_USE_OOP_PLUGINS=1).\n");
    }
   #endif

    // Optional plugin scan on launch. Per-machine setting (AppConfig);
    // default off. Synchronous — blocks the message thread for a few
    // seconds with a full plugin folder. Logs the result so the user
    // sees the validation in stderr without an extra modal.
    if (appconfig::getScanPluginsOnStartup())
    {
        auto& mgr = engine.getPluginManager();
        const int added = mgr.scanInstalledPlugins();
        const int total = mgr.getEffectDescriptions().size()
                         + mgr.getInstrumentDescriptions().size();
        std::fprintf (stderr,
                      "[Dusk Studio] Scan-on-startup: added %d new plugins "
                      "(%d total).\n", added, total);
        std::fflush (stderr);
    }

    // Default to a session under ~/Music/Dusk Studio/Untitled. The user can change
    // this later via a session-management UI; for the recorder MVP this is
    // enough to get WAVs on disk.
    auto musicDir = juce::File::getSpecialLocation (juce::File::userMusicDirectory);
    if (! musicDir.exists()) musicDir = juce::File::getSpecialLocation (juce::File::userHomeDirectory);
    session.setSessionDirectory (musicDir.getChildFile ("Dusk Studio").getChildFile ("Untitled"));

    // Top-of-window menu bar drives File / Settings actions. Replaces the
    // old row of TextButtons (Audio settings... / Save / Save As... / etc).
    menuBar.setModel (this);
    addAndMakeVisible (menuBar);

    // Stage selector - segmented look. The active stage gets a brighter
    // fill so the user always knows where they are.
    auto styleStageButton = [] (juce::TextButton& b, juce::Colour onColour)
    {
        b.setClickingTogglesState (true);
        b.setRadioGroupId (1001);
        b.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff202024));
        b.setColour (juce::TextButton::buttonOnColourId, onColour);
        b.setColour (juce::TextButton::textColourOffId,  juce::Colour (0xff909094));
        b.setColour (juce::TextButton::textColourOnId,   juce::Colours::white);
    };
    // Stage tab accent colours. Saturations chosen so each active tab
    // has comparable visual weight - the prior aux 0xff9080c0 read as
    // washed-out next to the saturated red / blue / purple of the other
    // three, making the AUX active state look like an outline rather
    // than a filled tab.
    // TransportBar must be added BEFORE the stage tabs + bank buttons so
    // those overlay controls naturally render on top in JUCE's child
    // z-order. Constructing + addAndMakeVisible'ing it here means we
    // don't have to call toFront() per-resize for every overlay child.
    transportBar = std::make_unique<TransportBar> (engine);
    transportBar->onTunerToggle = [this] { toggleTuner(); };
    transportBar->onVirtualKeyboardToggle = [this] { toggleVirtualKeyboard(); };
    transportBar->onTapeStripToggle = [this] (bool expanded)
    {
        tapeStripExpanded = expanded;
        if (tapeStrip != nullptr) tapeStrip->setVisible (expanded);
        // Collapse each track strip's EQ + COMP into popup buttons while the
        // SUMMARY view is up - without this the fader and bus assigns get
        // pushed off the bottom of the strip and become unusable.
        if (consoleView != nullptr) consoleView->setStripsCompactMode (expanded);
        resized();
    };
    addAndMakeVisible (transportBar.get());

    styleStageButton (recordingStageBtn, juce::Colour (0xffd03030));   // red, like REC
    styleStageButton (mixingStageBtn,    juce::Colour (0xff5a8ad0));   // mix-desk blue
    styleStageButton (auxStageBtn,       juce::Colour (0xff6e5ad0));   // aux indigo-violet
    styleStageButton (masteringStageBtn, juce::Colour (0xff8a5ad0));   // mastering purple
    recordingStageBtn.setConnectedEdges (juce::Button::ConnectedOnRight);
    mixingStageBtn   .setConnectedEdges (juce::Button::ConnectedOnLeft | juce::Button::ConnectedOnRight);
    auxStageBtn      .setConnectedEdges (juce::Button::ConnectedOnLeft | juce::Button::ConnectedOnRight);
    masteringStageBtn.setConnectedEdges (juce::Button::ConnectedOnLeft);
    recordingStageBtn.onClick = [this] { switchToStage (AudioEngine::Stage::Recording); };
    mixingStageBtn   .onClick = [this] { switchToStage (AudioEngine::Stage::Mixing); };
    auxStageBtn      .onClick = [this] { switchToStage (AudioEngine::Stage::Aux); };
    masteringStageBtn.onClick = [this] { switchToStage (AudioEngine::Stage::Mastering); };
    addAndMakeVisible (recordingStageBtn);
    addAndMakeVisible (mixingStageBtn);
    addAndMakeVisible (auxStageBtn);
    addAndMakeVisible (masteringStageBtn);
    recordingStageBtn.setToggleState (true, juce::dontSendNotification);  // Recording is the workflow default

    // SNAP / zoom-out / zoom-in / fit cluster. Pulled out of TapeStrip
    // to free the ruler band; placed in the header row between the
    // rightmost bank button and the tuner button (transport bar).
    auto styleHdrPill = [] (juce::TextButton& b)
    {
        b.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff262630));
        b.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff3f5870));
        b.setColour (juce::TextButton::textColourOffId,  juce::Colour (0xffd0d0d4));
        b.setColour (juce::TextButton::textColourOnId,   juce::Colours::white);
        b.setMouseClickGrabsKeyboardFocus (false);
    };
    styleHdrPill (hdrSnapToggle);
    styleHdrPill (hdrZoomOutBtn);
    styleHdrPill (hdrZoomInBtn);
    styleHdrPill (hdrZoomFitBtn);
    hdrSnapToggle.setClickingTogglesState (true);
    hdrSnapToggle.setToggleState (session.snapToGrid, juce::dontSendNotification);
    hdrSnapToggle.setTooltip ("Snap region drags to the grid.");
    hdrZoomOutBtn.setTooltip ("Zoom out (-)");
    hdrZoomInBtn .setTooltip ("Zoom in (=)");
    hdrZoomFitBtn.setTooltip ("Zoom to fit (Cmd+0)");
    hdrSnapToggle.onClick = [this]
    {
        session.snapToGrid = hdrSnapToggle.getToggleState();
    };
    hdrZoomOutBtn.onClick = [this]
    {
        if (tapeStrip != nullptr) tapeStrip->zoomByFactor (1.0f / 1.25f);
    };
    hdrZoomInBtn.onClick = [this]
    {
        if (tapeStrip != nullptr) tapeStrip->zoomByFactor (1.25f);
    };
    hdrZoomFitBtn.onClick = [this]
    {
        if (tapeStrip != nullptr) tapeStrip->zoomFit();
    };
    addAndMakeVisible (hdrSnapToggle);
    addAndMakeVisible (hdrZoomOutBtn);
    addAndMakeVisible (hdrZoomInBtn);
    addAndMakeVisible (hdrZoomFitBtn);

    // Bank-button row is rebuilt by syncBankButtons() each layout pass
    // (visible only when the window can't fit all 16 strips at min
    // width). Sits on a row directly below the stage selector so the
    // channel strips inside ConsoleView get the full body height.

    // Save / Save As / Open / Mixdown / Bounce / Audio settings now live in
    // the menu bar - see getMenuForIndex() and menuItemSelected() below.

    statusLabel.setJustificationType (juce::Justification::centredLeft);
    // Dim grey + smaller font so the top bar's session-name readout
    // doesn't compete with the menu bar / system meters for visual
    // priority. Full path attached as tooltip via setStatusForPath.
    statusLabel.setColour (juce::Label::textColourId, juce::Colour (0xff909094));
    statusLabel.setFont (juce::Font (juce::FontOptions (11.5f)));
    setStatusForPath ("Session", session.getSessionDirectory());
    addAndMakeVisible (statusLabel);

    systemStatusBar = std::make_unique<SystemStatusBar> (engine);
    addAndMakeVisible (systemStatusBar.get());

    // Read the user's persisted "tape strip expanded by default"
    // preference from AppConfig (per-machine setting in the General
    // settings panel). Default off so the strips get full vertical
    // room unless the user explicitly opted in.
    tapeStripExpanded = appconfig::getTapeStripExpandedDefault();
    tapeStrip = std::make_unique<TapeStrip> (session, engine);
    tapeStrip->setVisible (tapeStripExpanded);
    tapeStrip->onMidiRegionDoubleClicked  = [this] (int t, int r) { openPianoRoll   (t, r); };
    tapeStrip->onAudioRegionDoubleClicked = [this] (int t, int r) { openAudioEditor (t, r); };
    tapeStrip->onFilesDropped = [this] (juce::Array<juce::File> files,
                                          juce::int64 timelineStart,
                                          int trackHint)
    {
        if (! engine.getTransport().isStopped())
        {
            showImportError ("Import",
                              "Stop playback before importing files.");
            return;
        }
        if (files.size() > 1)
            openMultiImportPicker (std::move (files), timelineStart);
        else
            enqueueImports (std::move (files), timelineStart, trackHint);
    };
    addAndMakeVisible (tapeStrip.get());

    // Sync the transport-bar TAPE toggle with the collapsed default.
    if (transportBar != nullptr)
        transportBar->setTapeStripExpanded (tapeStripExpanded);

    // Intentionally NO auto-load on startup. Standard DAW behavior is to
    // start with a fresh session and require the user to explicitly Load. The
    // previous best-effort auto-load of ~/Music/Dusk Studio/Untitled/session.json
    // was confusing - settings (master fader, mutes, etc.) silently persisted
    // across launches. Use the Load button to restore a saved session.
    // TODO: replace with a startup dialog (New / Open Recent / Browse...) when
    // we add proper session management UX.

    consoleView = std::make_unique<ConsoleView> (session, engine);
    addAndMakeVisible (consoleView.get());
    consoleView->setOnStripFocusRequested ([this] (int t)
    {
        if (tapeStrip != nullptr) tapeStrip->setSelectedTrack (t);
    });

    // Initial stage is Recording (engine default + recordingStageBtn
    // toggled on) — sync strip controls so the first paint shows
    // input / IN / ARM / PRINT instead of aux sends. switchToStage()
    // handles subsequent changes.
    consoleView->setStripsMixingMode (engine.getStage() == AudioEngine::Stage::Mixing);

    // Auto-size to screen: prefer the reference size (1440x1280) but shrink to
    // fit smaller displays. The ConsoleView itself reflows responsively.
    const int kPreferredW = 1440;
    const int kPreferredH = 1280;
    const int kTopBarH    = 32 + 8 + 16;  // settings button + status row + outer padding

    int w = kPreferredW;
    int h = kPreferredH;
    if (auto* primary = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
    {
        const auto userArea = primary->userArea;
        w = juce::jmin (kPreferredW, userArea.getWidth()  - 24);
        h = juce::jmin (kPreferredH, userArea.getHeight() - 24);
    }

    const int minContentW = ConsoleView::minimumContentWidth() + 16;  // + outer padding
    const int minContentH = 480 + kTopBarH;
    w = juce::jmax (w, minContentW);
    h = juce::jmax (h, minContentH);

    setSize (w, h);

    // Dev affordance for screenshot / xdotool flows: set DUSKSTUDIO_START_STAGE=mastering
    // to land in Mastering at startup. No-op in normal use.
    if (auto envStage = std::getenv ("DUSKSTUDIO_START_STAGE"))
    {
        const juce::String s (envStage);
        if (s.equalsIgnoreCase ("mastering"))
            switchToStage (AudioEngine::Stage::Mastering);
        else if (s.equalsIgnoreCase ("recording"))
            switchToStage (AudioEngine::Stage::Recording);
    }

    // Receive keyboard input - needed for Ctrl+Z / Ctrl+Y. Children
    // (text editors, sliders) still grab focus when interacted with;
    // this just ensures clicks on empty canvas land here.
    setWantsKeyboardFocus (true);

    // Defer the startup dialog to the next message-loop tick so the main
    // window paints first - otherwise the dialog can pop up over a blank
    // canvas. SafePointer guards the case where the user closes the app
    // before the message loop catches up to the queued lambda.
    // DUSKSTUDIO_SKIP_STARTUP_DIALOG=1 bypasses the picker for screenshot /
    // xdotool flows.
    // DUSKSTUDIO_LOAD_SESSION=/path/to/session.json bypasses the startup
    // dialog and loads the named session immediately. Useful for
    // benchmarking the load path (the [Dusk Studio/Load] timing line ends
    // up in the parent terminal) and for scripted reproductions of
    // user-reported regressions.
    if (const char* loadPath = std::getenv ("DUSKSTUDIO_LOAD_SESSION");
        loadPath != nullptr && *loadPath)
    {
        juce::Component::SafePointer<MainComponent> safeThis (this);
        juce::String pathStr (loadPath);
        juce::MessageManager::callAsync ([safeThis, pathStr]
        {
            if (safeThis != nullptr)
                safeThis->loadSessionFromJson (juce::File (pathStr));
        });
    }
    else if (std::getenv ("DUSKSTUDIO_SKIP_STARTUP_DIALOG") == nullptr)
    {
        juce::Component::SafePointer<MainComponent> safeThis (this);
        juce::MessageManager::callAsync ([safeThis]
        {
            if (safeThis != nullptr) safeThis->launchStartupDialog();
        });
    }

    // Autosave heartbeat. Writes session.json.autosave next to the canonical
    // session.json every kAutosaveIntervalMs (30 s) so a crash loses at most
    // ~30 s of work. The write is atomic (temp + rename), so even a kill
    // during the timer fire never leaves a half-written autosave.
    startTimer (kAutosaveIntervalMs);
}

MainComponent::~MainComponent()
{
    stopTimer();   // halt autosave before tearing down engine / session

    // Close every plugin editor window (real top-level juce::DocumentWindows
    // with their own native X11 peers) BEFORE the rest of the UI cascade
    // tears down. Without this, ChannelStripComponent destructors run
    // their dropPluginEditor() inside Mutter's own teardown of our main
    // window, which on Linux/Wayland race-crashes the compositor. Belt-
    // and-suspenders for any quit path that doesn't go through
    // requestQuit's onSave / onDontSave handlers above (e.g. SIGTERM,
    // window-manager-initiated kill).
    if (consoleView != nullptr)
        consoleView->dropAllPluginEditors();

    // Force-delete any audio settings dialog we launched. JUCE's
    // ModalComponentManager keeps modal dialogs alive on its own stack and
    // would clean them up at app exit via ScopedJuceInitialiser_GUI's
    // destructor - but that runs AFTER MainComponent destructs (and
    // therefore AFTER our AudioEngine + AudioDeviceManager are gone). The
    // dialog's AudioDeviceSelectorComponent listens to AudioDeviceManager
    // and would crash on listener-removal in that delayed teardown.
    // Deleting it here, while AudioEngine is still alive, is safe.
    // Embedded modal teardown: closes the panel + dim while AudioEngine
    // is still alive, so AudioDeviceSelectorComponent's listener removal
    // happens before the engine and its DeviceManager go away.
    audioSettingsModal.close();
    mixdownModal      .close();
    bounceModal       .close();

    // Intentionally NO auto-save here. Standard DAW behavior is to require
    // an explicit Save before exit. The previous auto-save on destruct
    // paired with auto-load on construct caused settings (master fader
    // position, mutes, etc.) to silently persist across launches. Use the
    // Save button when you want to keep state.

    juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
}

void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff0d0d0f));
}

bool MainComponent::keyPressed (const juce::KeyPress& key)
{
    auto& um = engine.getUndoManager();
    const auto mods    = key.getModifiers();
    const int  code    = key.getKeyCode();
    const bool cmd     = mods.isCommandDown();   // Ctrl on Linux/Windows, Cmd on macOS
    const bool shift   = mods.isShiftDown();
    const bool noMods  = ! cmd && ! shift && ! mods.isAltDown();

    // ── Edit-mode shortcuts (Ardour-style). 'G' picks Grab Mode so the
    // user can flip back to move/select after a Range or Cut detour. No
    // modifiers so it never collides with the Cmd+letter clipboard ops.
    if (code == 'G' && noMods)
    {
        session.editMode = EditMode::Grab;
        if (audioEditor != nullptr) audioEditor->syncEditModeToolbar();
        if (pianoRoll   != nullptr) pianoRoll->syncEditModeToolbar();
        if (tapeStrip   != nullptr) tapeStrip->repaint();
        return true;
    }

    // ── Take cycling. Alt+T = forward (next take), Alt+Shift+T = backward.
    // Routes through TapeStrip's selection state; no-op when no region
    // is selected or the selection has no take history. T (plain) is
    // already claimed by split-at-playhead, hence the Alt modifier.
    if (code == 'T' && mods.isAltDown() && ! cmd)
    {
        if (tapeStrip != nullptr)
        {
            const bool ok = shift ? tapeStrip->cycleSelectedTakeBackward()
                                   : tapeStrip->cycleSelectedTakeForward();
            if (ok) return true;
        }
    }

    // ── TapeStrip zoom: '=' / '+' zoom in, '-' zoom out, '0' fit.
    // Skipped when a modal editor (audio / piano roll) has focus —
    // those have their own zoom keypress paths and grab focus first.
    if (tapeStrip != nullptr && audioEditor == nullptr && pianoRoll == nullptr)
    {
        const auto ch = key.getTextCharacter();
        if (noMods && (ch == '=' || ch == '+'))
        {
            tapeStrip->zoomByFactor (1.15f);
            return true;
        }
        if (noMods && ch == '-')
        {
            tapeStrip->zoomByFactor (1.0f / 1.15f);
            return true;
        }
        if (noMods && ch == '0')
        {
            tapeStrip->zoomFit();
            return true;
        }
    }

    // ── Edit: Ctrl/Cmd+Z, Ctrl/Cmd+Shift+Z, Ctrl/Cmd+Y ──
    if (code == 'Z' && cmd && ! shift) { um.undo(); return true; }
    if ((code == 'Z' && cmd && shift) || (code == 'Y' && cmd))
    {
        um.redo();
        return true;
    }

    // ── Region clipboard: Ctrl/Cmd+C, Ctrl/Cmd+X, Ctrl/Cmd+V; Delete ──
    // Each routes through TapeStrip, which owns the selection state. They
    // no-op when nothing's selected (or for paste, when the clipboard's
    // empty), letting the keypress fall through to default handling.
    if (tapeStrip != nullptr)
    {
        if (code == 'C' && cmd && ! shift)
        {
            if (tapeStrip->copySelectedRegion()) return true;
        }
        if (code == 'X' && cmd && ! shift)
        {
            if (tapeStrip->cutSelectedRegion()) return true;
        }
        if (code == 'V' && cmd && ! shift)
        {
            if (tapeStrip->pasteAtPlayhead()) return true;
        }
        if ((key == juce::KeyPress::deleteKey
             || key == juce::KeyPress::backspaceKey)
            && noMods)
        {
            if (tapeStrip->deleteSelectedRegion()) return true;
        }
        // 'T' (no modifiers) splits the selected region at the
        // playhead - razor-tool equivalent without needing a tool
        // mode. No-op when no region is selected or the playhead is
        // outside it. Mnemonic: "Trim / spliT". 'B' (Reaper-style
        // razor) is taken by Bounce; Cmd+T (Logic-style) was rejected
        // for consistency with Dusk Studio's other no-mod transport hotkeys.
        if (code == 'T' && noMods)
        {
            if (tapeStrip->splitSelectedAtPlayhead()) return true;
        }
        // Cmd/Ctrl+D duplicates the selected region. The piano roll's
        // own Cmd+D handler runs first when the modal is open (its
        // keyPressed has priority via JUCE focus), so this only fires
        // for tape-strip selection. Falls through harmlessly when
        // nothing is selected.
        if (code == 'D' && cmd && ! shift)
        {
            if (tapeStrip->duplicateSelectedRegion()) return true;
        }
        // Cmd/Ctrl + Left/Right nudges the selected region by 1 beat;
        // Shift adds for 1 bar. Computed from the live tempo so the
        // nudge matches the user's musical grid. Cmd was already used
        // for clipboard / save / open so it composes cleanly with
        // arrow keys (no existing binding).
        if ((key == juce::KeyPress::leftKey || key == juce::KeyPress::rightKey)
            && cmd)
        {
            const double sr   = engine.getCurrentSampleRate();
            const float  bpm  = session.tempoBpm.load (std::memory_order_relaxed);
            if (sr > 0.0 && bpm > 0.0f)
            {
                const int beatsPerBar = juce::jmax (1,
                    session.beatsPerBar.load (std::memory_order_relaxed));
                const double beatSamples = sr * 60.0 / (double) bpm;
                const double stepSamples = shift ? beatSamples * (double) beatsPerBar
                                                  : beatSamples;
                const juce::int64 delta = (juce::int64) std::round (stepSamples);
                const juce::int64 signedDelta = key == juce::KeyPress::leftKey ? -delta : delta;
                if (tapeStrip->nudgeSelectedRegion (signedDelta)) return true;
            }
        }
    }

    // ── File: Ctrl/Cmd+S (save), Ctrl/Cmd+Shift+S (save as), Ctrl/Cmd+O (open) ──
    // Buttons are gone (replaced by the menu bar) - dispatch to menu IDs
    // directly so the keyboard path keeps working.
    if (code == 'S' && cmd && ! shift) { menuItemSelected (1003, 0); return true; }   // Save
    if (code == 'S' && cmd &&   shift) { menuItemSelected (1004, 0); return true; }   // Save as
    if (code == 'O' && cmd)            { menuItemSelected (1002, 0); return true; }   // Open

    // ── Bounce: Ctrl/Cmd+B (Logic-style; intuitive "B for Bounce") ──
    if (code == 'B' && cmd) { menuItemSelected (1011, 0); return true; }              // Bounce

    // ── Transport: Space toggles play/stop, R toggles record ──
    // Pro Tools / Reaper / Logic / Bitwig all use Space as the universal
    // play-stop toggle. R for record matches Pro Tools / Cubase. Both
    // require no modifiers so a focused button or text editor still owns
    // the key.
    if (key == juce::KeyPress::spaceKey && noMods)
    {
        auto& transport = engine.getTransport();
        if (transport.isStopped()) engine.play();
        else                       { engine.stop(); if (transportBar != nullptr) transportBar->notifyRecordStopped(); }
        return true;
    }
    if (code == 'R' && noMods)
    {
        auto& transport = engine.getTransport();
        if (transport.isRecording()) { engine.stop(); if (transportBar != nullptr) transportBar->notifyRecordStopped(); }
        else                         engine.record();
        return true;
    }

    // ── Virtual MIDI Keyboard: K toggles the embedded VKB modal so the
    // user's typing keyboard becomes a MIDI input source. The modal pushes
    // events into the synthetic "Virtual Keyboard (Dusk Studio)" device — to
    // hear notes, a track must select that device on its MIDI input
    // dropdown and have an instrument plugin loaded.
    if (code == 'K' && noMods)
    {
        toggleVirtualKeyboard();
        return true;
    }

    // ── Navigation: Home → playhead to 0 (universal). End is intentionally
    // skipped because "end of timeline" isn't a fixed sample on a portastudio
    // - the timeline grows with the longest region.
    if (key == juce::KeyPress::homeKey)
    {
        engine.getTransport().setPlayhead (0);
        return true;
    }

    // ── '.' (period) → stop transport and rewind to 0. Pro Tools / Cubase
    // convention. Mirrors the Stop button on the transport bar with the
    // added rewind that the bare Stop doesn't provide.
    if (key.getTextCharacter() == '.' && noMods)
    {
        auto& tr = engine.getTransport();
        if (! tr.isStopped())
        {
            engine.stop();
            if (transportBar != nullptr) transportBar->notifyRecordStopped();
        }
        tr.setPlayhead (0);
        return true;
    }

    // ── Markers: 'M' (no modifiers) drops a marker at the current playhead.
    // Common DAW shortcut. Skips when typing - the noMods guard means this
    // only fires when no text editor has focus.
    if (code == 'M' && noMods)
    {
        const auto playhead = engine.getTransport().getPlayhead();
        um.beginNewTransaction ("Add marker");
        um.perform (new AddMarkerAction (session, playhead));
        if (tapeStrip != nullptr) tapeStrip->repaint();
        return true;
    }

    // ── Loop / punch: bracket keys set the current playhead as in/out;
    // L and P toggle the corresponding mode on/off. Shift+bracket switches
    // to punch boundaries; the unshifted form sets loop boundaries.
    auto& transport = engine.getTransport();
    if (code == '[' && ! cmd && ! mods.isAltDown())
    {
        const auto playhead = transport.getPlayhead();
        if (shift)
        {
            const auto end = transport.getPunchOut();
            transport.setPunchRange (playhead,
                                       end > playhead ? end : playhead);
        }
        else
        {
            const auto end = transport.getLoopEnd();
            transport.setLoopRange (playhead,
                                      end > playhead ? end : playhead);
        }
        if (tapeStrip != nullptr) tapeStrip->repaint();
        return true;
    }
    if (code == ']' && ! cmd && ! mods.isAltDown())
    {
        const auto playhead = transport.getPlayhead();
        if (shift)
        {
            const auto start = transport.getPunchIn();
            transport.setPunchRange (start < playhead ? start : playhead,
                                       playhead);
        }
        else
        {
            const auto start = transport.getLoopStart();
            transport.setLoopRange (start < playhead ? start : playhead,
                                      playhead);
        }
        if (tapeStrip != nullptr) tapeStrip->repaint();
        return true;
    }
    if (code == 'L' && noMods)
    {
        transport.setLoopEnabled (! transport.isLoopEnabled());
        if (tapeStrip != nullptr) tapeStrip->repaint();
        return true;
    }
    if (code == 'P' && noMods)
    {
        transport.setPunchEnabled (! transport.isPunchEnabled());
        if (tapeStrip != nullptr) tapeStrip->repaint();
        return true;
    }

    // ── Metronome click toggle: 'C' (no modifiers). Matches the C/I
    // (count-in) abbreviation already used on the transport bar's button.
    // The TransportBar's clickToggle button polls the same atom on its
    // 30 Hz timer and re-syncs its visual state.
    if (code == 'C' && noMods)
    {
        auto& enabled = session.metronomeEnabled;
        enabled.store (! enabled.load (std::memory_order_relaxed),
                        std::memory_order_relaxed);
        return true;
    }

    // ── Track arm / solo / mute on the selected track. Selection state
    // lives on TapeStrip (the most-recently-clicked region's track); when
    // nothing's selected, the shortcuts no-op rather than guessing. The
    // ChannelStrip's existing 30 Hz timer picks up the atom changes and
    // refreshes its toggles.
    if (tapeStrip != nullptr)
    {
        const int sel = tapeStrip->getSelectedTrack();
        if (sel >= 0 && sel < Session::kNumTracks)
        {
            if (code == 'A' && noMods)
            {
                // Route through setTrackArmed so armedTrackCount stays
                // in sync. Bypassing it (atom.store directly) left the
                // counter stale and silently broke the Rec button's
                // anyTrackArmed() fast-path.
                const bool now = session.track (sel).recordArmed
                                       .load (std::memory_order_relaxed);
                session.setTrackArmed (sel, ! now);
                return true;
            }
            if (code == 'S' && noMods)
            {
                // Route through setTrackSoloed so soloTrackCount stays
                // in sync (same reason as ARM above).
                const bool now = session.track (sel).strip.solo
                                       .load (std::memory_order_relaxed);
                session.setTrackSoloed (sel, ! now);
                return true;
            }
            // 'X' = mute toggle. M is already taken by drop-marker; X is
            // mnemonic for "kill / cross out" and matches Reaper's mute
            // keybinding.
            if (code == 'X' && noMods)
            {
                auto& m = session.track (sel).strip.mute;
                m.store (! m.load (std::memory_order_relaxed),
                          std::memory_order_relaxed);
                return true;
            }
        }
    }

    // ── Window: F11 toggles fullscreen. Walks up to the parent
    // ResizableWindow because that's the layer that owns the OS window
    // state, not MainComponent itself.
    if (key == juce::KeyPress::F11Key)
    {
        if (auto* window = findParentComponentOfClass<juce::ResizableWindow>())
        {
            window->setFullScreen (! window->isFullScreen());
            return true;
        }
    }

    return false;
}

void MainComponent::setStatusForPath (const juce::String& prefix,
                                          const juce::File& path,
                                          bool isAutosave)
{
    // Prefer the session-dir name (which the user picked) over the
    // session.json filename (always literal "session.json"). Falls
    // back to the file's own name when path == a non-session file
    // (e.g. error states pointing at a missing dir).
    auto display = path.getParentDirectory().getFileName();
    if (display.isEmpty()) display = path.getFileName();
    juce::String text = prefix + ": " + display;
    if (isAutosave) text += "  (autosave)";
    statusLabel.setText (text, juce::dontSendNotification);
    statusLabel.setTooltip (path.getFullPathName());
}

void MainComponent::syncBankButtons (int desiredCount)
{
    // Grow / shrink the bank-button vector to match. Shrinking destroys
    // the trailing buttons (removes them from the parent automatically
    // via ~Component). Each fresh button is wired with the Dusk bank
    // style + click handler that hops the console to that bank index.
    while ((int) bankButtons.size() > desiredCount)
        bankButtons.pop_back();

    while ((int) bankButtons.size() < desiredCount)
    {
        const int idx = (int) bankButtons.size();
        auto btn = std::make_unique<juce::TextButton>();
        btn->setClickingTogglesState (true);
        btn->setRadioGroupId (1002);
        btn->setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff202024));
        btn->setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffd0a060));
        btn->setColour (juce::TextButton::textColourOffId,  juce::Colours::lightgrey);
        btn->setColour (juce::TextButton::textColourOnId,   juce::Colour (0xff121214));
        btn->onClick = [this, idx]
        {
            if (consoleView != nullptr) consoleView->setBank (idx);
        };
        addAndMakeVisible (btn.get());
        bankButtons.push_back (std::move (btn));
    }
}

void MainComponent::resized()
{
    auto area = getLocalBounds().reduced (8);

    // Slim menu-bar row at the very top. The menu bar grows to fit its
    // top-level menu names (~80 px); status text + system meters share
    // the rest of the row to its right.
    auto top = area.removeFromTop (22);
    menuBar.setBounds (top.removeFromLeft (200));
    top.removeFromLeft (8);
    if (systemStatusBar != nullptr)
        systemStatusBar->setBounds (top.removeFromRight (300));
    top.removeFromRight (8);
    statusLabel.setBounds (top);
    area.removeFromTop (4);

    const auto curStage = engine.getStage();
    const bool inMastering = (curStage == AudioEngine::Stage::Mastering);
    const bool inAux       = (curStage == AudioEngine::Stage::Aux);
    const bool inFullscreenView = inMastering || inAux;

    // ── Combined transport / stage / bank row ──
    // Everything that used to live on three separate rows (stage selector,
    // bank buttons, transport bar) collapses into ONE row. The transport
    // bar paints the row's chrome + transport buttons + clock + right-edge
    // toggles; we overlay the stage selector centred on top of its hint
    // area, and bank buttons just to the left of the stage block. The
    // hint label is hidden so it doesn't render under the overlays.
    constexpr int kRowH = 44;
    juce::Rectangle<int> rowBounds;
    if (! inFullscreenView && transportBar != nullptr)
    {
        rowBounds = area.removeFromTop (kRowH);
        transportBar->setBounds (rowBounds);
        transportBar->setHintVisible (false);
    }
    else
    {
        // In mastering / aux the transport bar is hidden; the stage selector
        // still needs a row to live on. Reserve the same height so the stage
        // row doesn't jump positions when the user switches stages.
        rowBounds = area.removeFromTop (kRowH);
    }

    constexpr int kStageBtnH = 28;
    const int stageY = rowBounds.getY() + (rowBounds.getHeight() - kStageBtnH) / 2;

    // Banking decision: ConsoleView reports how many channel strips fit
    // alongside the always-anchored bus + master column at min width.
    // When all 16 fit we render NO bank buttons (numBanks == 1). When
    // they don't fit, we render one button per bank with the actual
    // 1-based range as the label ("1-13", "14-16", etc.) — bank size
    // tracks the window width, not a fixed stride of 8.
    //
    // CRITICAL: use the WIDTH the console is ABOUT to receive (area's
    // current width), not consoleView->getWidth() — the latter reflects
    // the PREVIOUS resize pass, so a fast snap-to-half-screen would
    // render labels one frame stale (showed "1-6" with only 3 strips
    // visible).
    const int consoleTargetWidth = area.getWidth();
    const int numBanks = (consoleView != nullptr && ! inFullscreenView)
                            ? ConsoleView::numBanksForWidth (consoleTargetWidth) : 1;
    const bool needsBanking = numBanks > 1;
    const bool transportCompact = rowBounds.getWidth() < TransportBar::kCompactTransportWidth;

    // Stage tabs shrink in compact mode so they don't crowd against the
    // (now centered) transport cluster at the OS minimum window width.
    const int stageW = transportCompact ? 92 : 110;
    const int stageBlockW = stageW * 4;
    const int  kBankBtnW   = transportCompact ? 70 : 100;
    constexpr int kBankBtnGap = 6;
    constexpr int kBankBtnH  = 26;

    // Center the stages + banks group within the row. Transport sits on
    // the LEFT (TransportBar own widgets); BPM cluster sits on the RIGHT.
    // The centered group reads: STAGES (4 tabs) [+ BANK 1..N buttons
    // when 16 strips need banking].
    constexpr int kStageToBankGap = 12;
    const int bankClusterW = needsBanking
                                ? (kStageToBankGap
                                     + numBanks * kBankBtnW
                                     + (numBanks - 1) * kBankBtnGap)
                                : 0;
    const int groupW       = stageBlockW + bankClusterW;
    const int stageX       = rowBounds.getX() + (rowBounds.getWidth() - groupW) / 2;

    recordingStageBtn.setBounds (stageX,                stageY, stageW, kStageBtnH);
    mixingStageBtn   .setBounds (stageX + stageW,       stageY, stageW, kStageBtnH);
    auxStageBtn      .setBounds (stageX + 2 * stageW,   stageY, stageW, kStageBtnH);
    masteringStageBtn.setBounds (stageX + 3 * stageW,   stageY, stageW, kStageBtnH);
    // Z-order is correct by construction: transportBar is added BEFORE
    // the stage tabs + bank buttons in the ctor, so the overlays sit on
    // top of the transport bar's painted background naturally.

    // Rebuild bank-button row to match the current numBanks. Buttons
    // sit inline immediately right of the centered stage block.
    syncBankButtons (needsBanking ? numBanks : 0);
    int lastBankRight = stageX + stageBlockW;   // fallback when no banks
    if (needsBanking && consoleView != nullptr)
    {
        const int bankY = rowBounds.getY() + (rowBounds.getHeight() - kBankBtnH) / 2;
        int bankX = stageX + stageBlockW + kStageToBankGap;
        const int activeBank = consoleView->getBank();
        for (int i = 0; i < (int) bankButtons.size(); ++i)
        {
            auto* btn = bankButtons[(size_t) i].get();
            const auto range = ConsoleView::rangeForBankAtWidth (i, consoleTargetWidth);
            btn->setButtonText (transportCompact
                                  ? (juce::String (range.first) + "-" + juce::String (range.second))
                                  : (juce::String ("BANK ") + juce::String (i + 1) + "  ("
                                       + juce::String (range.first) + "-"
                                       + juce::String (range.second) + ")"));
            btn->setBounds (bankX, bankY, kBankBtnW, kBankBtnH);
            btn->setToggleState (i == activeBank, juce::dontSendNotification);
            bankX += kBankBtnW + kBankBtnGap;
        }
        lastBankRight = bankX - kBankBtnGap;
    }

    // SNAP + zoom cluster pinned in the gap between the rightmost bank
    // tab and the tuner button (transport bar right edge - kBtnDia(36)
    // - right padding). Hidden when there's no TapeStrip context (e.g.
    // user in AUX/Mastering fullscreen views — strip controls don't
    // apply there).
    constexpr int kHdrSnapW    = 50;
    constexpr int kHdrZoomBtnW = 30;
    constexpr int kHdrFitW     = 36;
    constexpr int kHdrBtnGap   = 4;
    constexpr int kHdrBtnH     = 24;
    constexpr int kHdrClusterW = kHdrSnapW + kHdrZoomBtnW * 2 + kHdrFitW + kHdrBtnGap * 3;
    // Only show when TapeStrip is actually expanded — when it's
    // collapsed or the user is in a fullscreen stage (AUX/Mastering),
    // SNAP+zoom have no on-screen subject so they don't belong in
    // the header.
    const bool hdrClusterVisible = ! inFullscreenView
                                  && tapeStrip != nullptr
                                  && tapeStripExpanded;
    hdrSnapToggle.setVisible (hdrClusterVisible);
    hdrZoomOutBtn.setVisible (hdrClusterVisible);
    hdrZoomInBtn .setVisible (hdrClusterVisible);
    hdrZoomFitBtn.setVisible (hdrClusterVisible);
    if (hdrClusterVisible)
    {
        // Real tuner X (parent-local on transportBar = parent-local on
        // our row, since transportBar.setBounds(rowBounds) puts them
        // in the same coord space). The previous heuristic
        // (rowBounds.getRight() - 56) was ~320 px off — tuner sits
        // BEFORE the right-anchored BPM / tap / time-sig / tape cluster.
        const int tunerLeftInBar = transportBar != nullptr ? transportBar->getTunerLeftX() : rowBounds.getWidth() - 56;
        const int tunerLeft = rowBounds.getX() + tunerLeftInBar;
        const int leftEdge  = lastBankRight + 16;
        const int rightEdge = tunerLeft - 16;
        const int gapW      = juce::jmax (0, rightEdge - leftEdge);
        const int clusterX  = leftEdge + juce::jmax (0, (gapW - kHdrClusterW) / 2);
        const int clusterY  = rowBounds.getY() + (rowBounds.getHeight() - kHdrBtnH) / 2;
        int x = clusterX;
        hdrSnapToggle.setBounds (x, clusterY, kHdrSnapW,   kHdrBtnH); x += kHdrSnapW   + kHdrBtnGap;
        hdrZoomOutBtn.setBounds (x, clusterY, kHdrZoomBtnW, kHdrBtnH); x += kHdrZoomBtnW + kHdrBtnGap;
        hdrZoomInBtn .setBounds (x, clusterY, kHdrZoomBtnW, kHdrBtnH); x += kHdrZoomBtnW + kHdrBtnGap;
        hdrZoomFitBtn.setBounds (x, clusterY, kHdrFitW,    kHdrBtnH);

        // Sync SNAP visual state with the session atom (e.g. session
        // load, MIDI-bound toggle).
        if (hdrSnapToggle.getToggleState() != (bool) session.snapToGrid)
            hdrSnapToggle.setToggleState (session.snapToGrid, juce::dontSendNotification);
    }

    area.removeFromTop (4);

    if (! inFullscreenView)
    {
        if (tapeStrip != nullptr && tapeStripExpanded)
        {
            tapeStrip->setBounds (area.removeFromTop (tapeStrip->naturalHeight()));
            area.removeFromTop (4);
        }

        if (consoleView != nullptr) consoleView->setBounds (area);
    }
    else if (inMastering)
    {
        if (masteringView != nullptr) masteringView->setBounds (area);
    }
    else if (inAux)
    {
        if (auxView != nullptr) auxView->setBounds (area);
    }

    // Re-centre the startup modal + its dim backdrop when the main window
    // resizes while the dialog is up. DimOverlay::parentSizeChanged handles
    // its own resize, but we still drive the centred dialog bounds.
    if (startupDialog != nullptr)
    {
        startupDialog->setBounds (getLocalBounds()
                                       .withSizeKeepingCentre (startupDialog->getWidth(),
                                                                  startupDialog->getHeight()));
    }
}

void MainComponent::openAudioSettings()
{
    // Embedded modal - identical UX to a window (Esc to dismiss, click
    // outside to close, body holds focus) but rendered over the main
    // canvas so we don't fragment across OS windows. The Modal helper
    // handles the dim overlay + body lifetime.
    if (audioSettingsModal.isOpen()) return;
    auto panel = std::make_unique<AudioSettingsPanel> (engine.getDeviceManager(),
                                                          engine, session);
    // Tall enough that the new grouped layout (Audio + Control Surface +
    // MIDI Bindings + MIDI Sync + General + Advanced, each with a
    // section header + separator) fits without any group being clipped.
    // The Audio block hosts JUCE's AudioDeviceSelectorComponent in a
    // fixed 280 px slot — see AudioSettingsPanel::resized.
    panel->setSize (820, 920);
    audioSettingsModal.show (*this, std::move (panel));
}

void MainComponent::switchToStage (AudioEngine::Stage s)
{
    if (engine.getStage() == s) return;

    std::fprintf (stderr, "[MainComponent] switchToStage(%d) enter\n", (int) s);
    std::fflush (stderr);
    engine.setStage (s);
    // Mirror into session so save/load round-trips the user's last
    // visited stage — new sessions still default to Recording via the
    // Session::uiStage default + AudioEngine::stage default.
    session.uiStage.store ((int) s, std::memory_order_relaxed);

    syncStageUi (s);
    resized();
}

void MainComponent::syncStageUi (AudioEngine::Stage s)
{
    // Mixing/Recording share the console + tape strip; Mastering swaps to
    // MasteringView; Aux swaps to AuxView. Both heavy views are constructed
    // lazily so users who never visit them pay no startup cost.
    const bool wantMastering = (s == AudioEngine::Stage::Mastering);
    const bool wantAux       = (s == AudioEngine::Stage::Aux);
    const bool wantMixing    = (s == AudioEngine::Stage::Mixing);
    const bool wantFullscreenView = wantMastering || wantAux;

    if (consoleView   != nullptr) consoleView  ->setVisible (! wantFullscreenView);
    if (transportBar  != nullptr) transportBar ->setVisible (! wantFullscreenView);
    if (tapeStrip     != nullptr) tapeStrip    ->setVisible (! wantFullscreenView && tapeStripExpanded);

    // Channel strips swap their tracking-only block (input/IN/ARM/PRINT) for
    // a row of 4 AUX send knobs while in Mixing.
    if (consoleView != nullptr) consoleView->setStripsMixingMode (wantMixing);

    if (wantMastering)
    {
        if (masteringView == nullptr)
        {
            masteringView = std::make_unique<MasteringView> (session, engine);
            addAndMakeVisible (masteringView.get());
        }
        masteringView->setVisible (true);
    }
    else if (masteringView != nullptr)
    {
        masteringView->setVisible (false);
    }

    if (wantAux)
    {
        if (auxView == nullptr)
        {
            std::fprintf (stderr, "[MainComponent] syncStageUi(Aux): constructing AuxView\n");
            std::fflush (stderr);
            auxView = std::make_unique<AuxView> (session, engine);
            addAndMakeVisible (auxView.get());
            std::fprintf (stderr, "[MainComponent] syncStageUi(Aux): AuxView constructed + addAndMakeVisible'd\n");
            std::fflush (stderr);
        }
        auxView->setVisible (true);
        std::fprintf (stderr, "[MainComponent] syncStageUi(Aux): auxView setVisible(true), bounds=%d,%d %dx%d\n",
                      auxView->getX(), auxView->getY(), auxView->getWidth(), auxView->getHeight());
        std::fflush (stderr);
    }
    else if (auxView != nullptr)
    {
        auxView->setVisible (false);
    }

    // Sync the segmented buttons (radio group means only one is on, but
    // explicitly setting the right one keeps a programmatic switch - like
    // doMixdown's auto-handoff - visually consistent).
    recordingStageBtn.setToggleState (s == AudioEngine::Stage::Recording, juce::dontSendNotification);
    mixingStageBtn   .setToggleState (s == AudioEngine::Stage::Mixing,    juce::dontSendNotification);
    auxStageBtn      .setToggleState (s == AudioEngine::Stage::Aux,       juce::dontSendNotification);
    masteringStageBtn.setToggleState (s == AudioEngine::Stage::Mastering, juce::dontSendNotification);
}

void MainComponent::doMixdown()
{
    // One-shot bounce of the master mix to <sessionDir>/mixdown.wav, then
    // auto-switch to Mastering with that file loaded. No file dialog -
    // overwrites mixdown.wav each time. The "Bounce..." button retains
    // the explicit dialog flow for ad-hoc renders.
    auto target = session.getSessionDirectory().getChildFile ("mixdown.wav");

    auto panel = std::make_unique<BounceDialog> (engine, session,
                                                   engine.getDeviceManager(), target);
    panel->setSize (520, 200);

    // Hand off to Mastering once the bounce finishes successfully. The
    // dialog fires this on its message-thread "Close" path, well after the
    // worker has restored engine state, so the stage flip is safe.
    juce::Component::SafePointer<MainComponent> safeThis (this);
    panel->onSuccessfulFinish = [safeThis] (juce::File rendered)
    {
        if (safeThis == nullptr) return;
        safeThis->mixdownModal.close();
        safeThis->switchToStage (AudioEngine::Stage::Mastering);
        if (safeThis->masteringView != nullptr)
            safeThis->masteringView->loadFile (rendered);
    };

    mixdownModal.show (*this, std::move (panel));
}

void MainComponent::launchStartupDialog()
{
    auto recents = RecentSessions::load();

    startupDialog = std::make_unique<StartupDialog> (recents);
    startupDialog->setSize (560, juce::jlimit (220, 520,
                                                 140 + (recents.isEmpty() ? 60 : recents.size() * 30)));

    startupDialog->onOpenRecent = [this] (juce::File dir)
    {
        loadSessionFromJson (dir.getChildFile ("session.json"));
    };
    startupDialog->onNewSession = [this] { newSessionPrompt(); };
    startupDialog->onOpenFile   = [this] { openFromFilePrompt(); };
    startupDialog->onSkip       = [] {};  // nothing - the bootstrap default dir stays
    // closeDialog calls onDismiss after each action; the host (us) tears
    // down the embedded dialog + its dim backdrop together.
    startupDialog->onDismiss    = [this] { dismissStartupDialog(); };

    // Dim backdrop covers the rest of the UI; clicking the dim is a
    // shortcut for "Continue blank" — the same as the dialog's own Skip
    // button — so it doesn't trap the user when they want to dismiss.
    startupDim = std::make_unique<DimOverlay>();
    startupDim->setBounds (getLocalBounds());
    startupDim->onClick = [this] { dismissStartupDialog(); };
    addAndMakeVisible (startupDim.get());

    // Centered on the main window. The dialog is plain dark — no native
    // title bar — to match the embedded-modal aesthetic shared with the
    // TapeMachine gear modal and the SUMMARY EQ/COMP popups.
    const auto bounds = getLocalBounds()
                            .withSizeKeepingCentre (startupDialog->getWidth(),
                                                       startupDialog->getHeight());
    startupDialog->setBounds (bounds);
    addAndMakeVisible (startupDialog.get());
}

void MainComponent::dismissStartupDialog()
{
    // Defer the actual delete by one message-loop tick — closeDialog is
    // typically called from inside one of the dialog's own button click
    // handlers, and tearing down the dialog from inside its own callback
    // chain is fragile. callAsync runs on the message thread after the
    // click handler returns, when nothing's still on the dialog's stack.
    juce::Component::SafePointer<MainComponent> safeThis (this);
    juce::MessageManager::callAsync ([safeThis]
    {
        if (safeThis == nullptr) return;
        safeThis->startupDialog.reset();
        safeThis->startupDim.reset();
    });
}

void MainComponent::newSessionPrompt()
{
    // Single-dialog "Save As" UX: filename text field + folder browser in
    // one step. The typed name becomes the session folder; the navigated
    // directory becomes its parent.
    auto startDir = juce::File::getSpecialLocation (juce::File::userMusicDirectory)
                        .getChildFile ("Dusk Studio");
    if (! startDir.exists()) startDir.createDirectory();

    filebrowser::open (*this, {
        /*title*/                  "Name your new session",
        /*initialFileOrDirectory*/ startDir.getChildFile ("MySong"),
        /*filePatternsAllowed*/    juce::String(),
        /*mode*/                   filebrowser::Mode::Save,
        /*warnAboutOverwriting*/   true,
        /*selectDirectories*/      false,
    },
    [this] (juce::File chosen)
    {
        if (chosen == juce::File()) return;
        // Treat the chosen path as the new session folder. saveSessionTo
        // creates the dir + audio subdir and persists an initial session.json
        // so subsequent saves don't re-prompt.
        saveSessionTo (chosen);
    });
}

bool MainComponent::saveSessionTo (const juce::File& dir)
{
    if (dir == juce::File()) return false;

    // setSessionDirectory creates the dir + audio subdir if missing - safe
    // to call even when the user picked an existing session folder.
    session.setSessionDirectory (dir);

    // Plugin state I/O races the renderer on plugins that don't honour
    // JUCE's "must not overlap" contract (u-he Diva is the smoking-gun
    // example), corrupting the plugin's internal state and leading to
    // an abort inside ~VST3PluginInstance later. To capture fresh
    // plugin state safely we briefly remove the audio callback for the
    // duration of the save, then re-attach. The user hears a short
    // (~10-50 ms × N loaded plugins) dropout on Ctrl+S, which beats
    // crashing.
    //
    // engineDetached==true means a higher layer (the quit-save path)
    // has already detached + does NOT want re-attach. We honour that
    // and skip the re-attach below.
    const bool reattachAudioAfter = ! engineDetached;
    if (! engineDetached)
    {
        engine.getDeviceManager().removeAudioCallback (&engine);
        engineDetached = true;   // tell publishPluginStateForSave to skip park sleeps
    }

    engine.publishPluginStateForSave (/*audioCallbackDetached*/ true);
    engine.publishTransportStateForSave();

    const auto target = dir.getChildFile ("session.json");
    const juce::String json = SessionSerializer::serialize (session);
    const bool saveOk = SessionSerializer::writeAtomic (target, json);

    if (reattachAudioAfter)
    {
        // Re-attach so audio resumes after the save returns. PluginSlot
        // already called prepareToPlay on each plugin during state
        // capture (resume side of the suspend bracket), so each plugin
        // is ready for the next callback.
        engine.getDeviceManager().addAudioCallback (&engine);
        engineDetached = false;
    }

    if (saveOk)
    {
        RecentSessions::add (dir);
        // A successful manual save makes the autosave stale - drop it so the
        // recovery prompt doesn't fire on the next clean load.
        deleteAutosaveFor (dir);
        // Remember the JSON so the next autosave tick can recognise an
        // idle (no further edits) state and skip the write.
        setLastSavedSessionJson    (json);
        setLastWrittenAutosaveJson (juce::String());
        setStatusForPath ("Saved", target);
        return true;
    }
    setStatusForPath ("Save failed", target);
    // Status-label-only feedback is too easy to miss on a critical
    // operation. Pop a modal so the user knows the session WASN'T
    // saved and can act before losing more work.
    showDuskAlert (*this, "Save failed",
                      "Dusk Studio could not write the session file:\n\n    "
                      + target.getFullPathName() + "\n\n"
                      "Common causes: disk full, missing write "
                      "permission, or the parent folder was moved "
                      "since the session was opened. The session "
                      "is unchanged in memory; try Save As to a "
                      "different location.");
    return false;
}

juce::File MainComponent::getAutosaveFileFor (const juce::File& sessionDir) const
{
    return sessionDir.getChildFile ("session.json.autosave");
}

// Hosted plugins (per-channel inserts, aux-lane slots, master tape) re-
// emit slightly different APVTS state on each getStateInformation call -
// internal counters / smoother phases that aren't user-meaningful. The
// autosave dirty-check must ignore that drift, otherwise it fires every
// 30 s on a freshly-saved session and resurfaces the recovery dialog on
// the next launch. Strip the two base64 state values to "" before any
// content compare.
static juce::String stripVolatileStateForDirtyCompare (juce::String s)
{
    static const char* const markers[] = { "\"plugin_state\":", "\"tape_state\":" };
    for (const char* marker : markers)
    {
        const juce::String m (marker);
        int pos = 0;
        while ((pos = s.indexOf (pos, m)) != -1)
        {
            const int afterColon = pos + m.length();
            const int q1 = s.indexOfChar (afterColon, '"');
            if (q1 < 0) break;
            const int q2 = s.indexOfChar (q1 + 1, '"');
            if (q2 < 0) break;
            s = s.replaceSection (q1, q2 - q1 + 1, "\"\"");
            pos = q1 + 2;
        }
    }
    return s;
}

bool MainComponent::autosaveIsNewerThan (const juce::File& sessionJson) const
{
    const auto autosave = getAutosaveFileFor (sessionJson.getParentDirectory());
    if (! autosave.existsAsFile()) return false;
    if (! sessionJson.existsAsFile()) return true;   // autosave is the only state we have
    // Content-based comparison only. mtime is unreliable on common
    // filesystems (ext4 typically has 1 s mtime resolution; FAT-class
    // filesystems are 2 s), so a manual save followed by an autosave
    // within the same second can look like "autosave not newer" via
    // mtime even when the in-memory state genuinely diverged. Manual
    // save deletes the autosave (see saveSession), so the autosave
    // file existing AND its content differing from session.json is
    // the authoritative signal that a recovery point is available.
    // Volatile plugin/tape APVTS drift is stripped so an idle session
    // doesn't see spurious diffs.
    const auto autosaveStripped    = stripVolatileStateForDirtyCompare (autosave.loadFileAsString());
    const auto sessionJsonStripped = stripVolatileStateForDirtyCompare (sessionJson.loadFileAsString());
    return autosaveStripped != sessionJsonStripped;
}

void MainComponent::deleteAutosaveFor (const juce::File& sessionDir) const
{
    const auto autosave = getAutosaveFileFor (sessionDir);
    if (autosave.existsAsFile()) autosave.deleteFile();
}

void MainComponent::writeAutosave()
{
    const auto dir = session.getSessionDirectory();
    if (dir == juce::File()) return;

    // Ensure the directory exists before serialising. setSessionDirectory's
    // ctor path normally handles this, but a session that was constructed
    // via the default ~/Music/Dusk Studio/Untitled fallback may never have been
    // touched on disk yet.
    dir.createDirectory();

    // Same publish bookend as the manual save - the serializer reads
    // only from Session, not from the live engine. Audio is running on
    // the autosave path (timer fires from the live message loop), so
    // the publish keeps its atomic-park sleeps to defend against the
    // audio-thread re-entry race.
    engine.publishPluginStateForSave (/*audioCallbackDetached*/ false);
    engine.publishTransportStateForSave();

    // Skip the write entirely when the snapshot matches what's already on
    // disk - either the last manual save (no real edits since Ctrl+S) or
    // the previous autosave tick (timer firing on idle state). Without
    // this skip the autosave file's mtime would creep past session.json
    // every 30 s, producing a false-positive recovery prompt on next load.
    const juce::String json = SessionSerializer::serialize (session);
    // Strip volatile plugin / tape APVTS state before comparing - donor
    // getStateInformation drifts between calls even with no user change,
    // which would otherwise fire an autosave write every tick on idle
    // sessions. lastSaved/lastWritten stripped versions are cached on
    // assignment so only the current snapshot is stripped per tick.
    const auto stripped = stripVolatileStateForDirtyCompare (json);
    if (stripped == lastSavedSessionJsonStripped)    return;
    if (stripped == lastWrittenAutosaveJsonStripped) return;

    const auto target = getAutosaveFileFor (dir);
    if (! SessionSerializer::writeAtomic (target, json))
    {
        DBG ("MainComponent: autosave write failed at " << target.getFullPathName());
        return;
    }
    setLastWrittenAutosaveJson (json);
}

void MainComponent::setLastSavedSessionJson (const juce::String& json)
{
    lastSavedSessionJson         = json;
    lastSavedSessionJsonStripped = stripVolatileStateForDirtyCompare (json);
}

void MainComponent::setLastWrittenAutosaveJson (const juce::String& json)
{
    lastWrittenAutosaveJson         = json;
    lastWrittenAutosaveJsonStripped = stripVolatileStateForDirtyCompare (json);
}

void MainComponent::timerCallback()
{
    // The audio thread is independent of the message thread, so a slow JSON
    // serialise here doesn't glitch playback. Still, we want autosave cheap -
    // SessionSerializer::save on a 16-track session should land well under
    // 30 ms.
    writeAutosave();
}

void MainComponent::requestQuit()
{
    // Industry-standard dirty-only prompt: nothing changed since the last
    // manual save (autosave file isn't newer than session.json) → quit
    // immediately. Otherwise show the Dusk Studio-styled Save / Don't Save /
    // Cancel modal.
    const auto dir = session.getSessionDirectory();
    const auto sessionJson = dir.getChildFile ("session.json");
    const bool dirty = (dir != juce::File()) && autosaveIsNewerThan (sessionJson);

    if (! dirty)
    {
        // No unsaved changes - quit through the same staged-shutdown
        // path the dirty flow uses, so the engine is quiesced and
        // window peers tear down in a deterministic order. Bypassing
        // beginSafeShutdown here historically left the audio callback
        // attached during MainWindow destruction, which raced plugin
        // editor / native-peer teardown.
        beginSafeShutdown();
        return;
    }

    if (quitModal.isOpen()) return;  // user double-clicked the X

    auto dialog = std::make_unique<QuitConfirmDialog>();
    dialog->setSize (440, 200);

    // Each handler defers its body via callAsync so the button-click
    // call stack fully unwinds before we destruct the dialog. Closing
    // an EmbeddedModal that owns the dialog from inside the dialog's
    // own button callback was a use-after-free against the std::function
    // backing the button's onClick, and the resulting message-thread
    // corruption was the trigger for the GNOME compositor crash on
    // Save / Don't Save. EmbeddedModal::close defends against this on
    // its end too (it move-captures the body into a callAsync), but
    // deferring at the call site is the canonical idiom.
    //
    // Save / Don't Save also detach the audio callback up front. This
    // does two things on the quit path:
    //   • the save below can call publishPluginStateForSave with the
    //     "audio detached" fast path (no atomic-park sleeps), which is
    //     the difference between a snappy save and several hundred
    //     milliseconds of message-thread blocking on a session with
    //     multiple heavy plugins;
    //   • plugin getStateInformation runs with no concurrent
    //     processBlock, which side-steps the data race in plugins that
    //     don't honour JUCE's "must not overlap" contract on Linux.
    juce::Component::SafePointer<MainComponent> safeThis (this);

    dialog->onCancel = [safeThis]
    {
        juce::MessageManager::callAsync ([safeThis]
        {
            if (auto* self = safeThis.getComponent())
                self->quitModal.close();
        });
    };
    dialog->onDontSave = [safeThis]
    {
        // Quit and let the autosave linger. Next launch's session-load
        // will offer to recover from it.
        juce::MessageManager::callAsync ([safeThis]
        {
            if (auto* self = safeThis.getComponent())
            {
                self->quitModal.close();
                self->beginSafeShutdown();
            }
        });
    };
    dialog->onSave = [safeThis]
    {
        // saveSessionAndThen handles both the sync (existing dir) and
        // async (Save As file chooser) paths. Close the modal first so
        // the chooser, if it opens, isn't fighting our overlay for
        // input. On save success, quit; on failure (chooser cancel,
        // disk error) the user is left in the app and can retry.
        juce::MessageManager::callAsync ([safeThis]
        {
            auto* self = safeThis.getComponent();
            if (self == nullptr) return;
            self->quitModal.close();

            // Quiesce engine BEFORE saveSessionAndThen so the save's
            // publishPluginStateForSave skips the atomic-park sleeps
            // and plugins see no concurrent processBlock during state
            // I/O. stopTimer prevents the autosave timer from re-
            // entering a save mid-shutdown.
            self->stopTimer();
            self->engine.getDeviceManager().removeAudioCallback (&self->engine);
            self->engineDetached = true;

            self->saveSessionAndThen ([safeThis] (bool ok)
            {
                if (! ok) return;
                if (auto* s = safeThis.getComponent())
                    s->beginSafeShutdown();
            });
        });
    };

    // Focus-locked: save-before-quit MUST go through Save / Don't Save /
    // Cancel. Esc and click-outside would let the user dismiss with no
    // decision, leaving the dirty state ambiguous and the X-button quit
    // request silently swallowed.
    quitModal.show (*this, std::move (dialog), /*onDismiss*/ {},
                       /*dismissOnClickOutside*/ false,
                       /*dismissOnEscape*/        false);
}

void MainComponent::leakAllPluginInstancesForShutdown()
{
    engine.leakAllPluginInstancesForShutdown();
}

void MainComponent::beginSafeShutdown()
{
    // Quiesce the engine + tear down native window peers in an order
    // the host windowing system can keep up with. Several earlier
    // shutdown variants were observed to crash Mutter on Linux/
    // Wayland; this one walks a deliberately conservative sequence
    // and prints a stderr marker before each phase so a future crash
    // gives us a precise line number ("which phase did we die in?").
    //
    // Idempotent on the autosave timer + audio callback removal so
    // the quit-save path can detach those up front (to make the
    // intervening save fast) and still call beginSafeShutdown() to
    // finish the teardown.
    auto markPhase = [] (const char* msg)
    {
        std::fprintf (stderr, "[Dusk Studio/shutdown] %s\n", msg);
        std::fflush (stderr);
    };

    markPhase ("phase 1: stop autosave timer");
    stopTimer();

    markPhase ("phase 2: stop transport (commits in-flight recording)");
    auto& transport = engine.getTransport();
    if (transport.isRecording() || transport.isPlaying())
        engine.stop();

    if (! engineDetached)
    {
        markPhase ("phase 3: detach audio callback");
        engine.getDeviceManager().removeAudioCallback (&engine);
        engineDetached = true;
    }
    else
    {
        markPhase ("phase 3: audio callback already detached (skipping)");
    }

    markPhase ("phase 3b: release plugin resources (setActive(false) on each)");
    // Quiesce every plugin BEFORE editor windows + engine destructors
    // start running. Diva's terminate() (called inside its destructor)
    // tries to talk back to the host's VST3 context; that's only safe
    // when the plugin already considers itself inactive. Without this,
    // ~VST3PluginInstance has been observed to abort with
    // __cxa_pure_virtual on session shutdown.
    engine.releaseAllPluginResources();

    markPhase ("phase 4: drop plugin editor windows");
    if (consoleView != nullptr)
        consoleView->dropAllPluginEditors();
    // AUX plugin editors are inline children of their AuxLaneComponent —
    // they tear down with the normal ~MainWindow → ~AuxView cascade. The
    // mutter focus race that required an explicit pre-shutdown pass only
    // applied to the old floating-window editors.

    markPhase ("phase 5: flush window operations");
    duskstudio::platform::flushWindowOperations();

    // Walk every juce::TopLevelWindow so any future window class
    // (mastering popout, file dialog left open) inherits the
    // protection without per-site plumbing.
    markPhase ("phase 5b: clear keyboard focus from every top-level window");
    for (int i = juce::TopLevelWindow::getNumTopLevelWindows(); --i >= 0;)
        if (auto* w = juce::TopLevelWindow::getTopLevelWindow (i))
            duskstudio::platform::prepareForTopLevelDestruction (*w);

    // Yield to mutter's compositor loop so it ticks a focus-out /
    // focus-in cycle and updates its internal focus_window tracker
    // before phase 6's unmap arrives. Without this gap, mutter sees
    // the unmap before processing the EWMH activate above and trips
    // meta_window_unmanage.
    juce::Component::SafePointer<MainComponent> safeThis (this);
    juce::MessageManager::callAsync ([safeThis]
    {
        auto* self = safeThis.getComponent();
        if (self == nullptr) return;

        auto mark = [] (const char* msg)
        {
            std::fprintf (stderr, "[Dusk Studio/shutdown] %s\n", msg);
            std::fflush (stderr);
        };

        mark ("phase 6: hide main window");
        if (auto* tlw = self->getTopLevelComponent())
            tlw->setVisible (false);
        duskstudio::platform::flushWindowOperations();

        duskstudio::platform::clearXInputFocus();

        mark ("phase 7: defer systemRequestedQuit to next message-loop tick");
        juce::MessageManager::callAsync ([]
        {
            std::fprintf (stderr,
                          "[Dusk Studio/shutdown] phase 7b: posting systemRequestedQuit\n");
            std::fflush (stderr);
            if (auto* app = juce::JUCEApplicationBase::getInstance())
                app->systemRequestedQuit();
        });
    });

    markPhase ("phase 8: beginSafeShutdown returning to message loop (yield to mutter)");
}

void MainComponent::saveSessionAndThen (std::function<void(bool)> onComplete)
{
    const auto dir = session.getSessionDirectory();
    if (dir.getChildFile ("session.json").existsAsFile())
    {
        // Sync save into the existing dir.
        const bool ok = saveSessionTo (dir);
        if (onComplete) onComplete (ok);
        return;
    }

    // No prior save - open a Save As dialog where the user types the session
    // name and picks a parent directory in one step. The chooser is in
    // saveMode | canSelectFiles so the OS shows a filename text field; we
    // then treat the resulting "file" path as the session folder we'll
    // create (saveSessionTo already creates the directory if missing).
    auto startDir = session.getSessionDirectory().getParentDirectory();
    if (! startDir.isDirectory())
        startDir = juce::File::getSpecialLocation (juce::File::userMusicDirectory)
                       .getChildFile ("Dusk Studio");
    if (! startDir.exists()) startDir.createDirectory();

    juce::String defaultName = session.getSessionDirectory().getFileName();
    if (defaultName.isEmpty() || defaultName == "Untitled") defaultName = "MySong";

    filebrowser::open (*this, {
        /*title*/                  "Save session as...",
        /*initialFileOrDirectory*/ startDir.getChildFile (defaultName),
        /*filePatternsAllowed*/    juce::String(),
        /*mode*/                   filebrowser::Mode::Save,
        /*warnAboutOverwriting*/   true,
        /*selectDirectories*/      false,
    },
    [this, onComplete = std::move (onComplete)] (juce::File chosen)
    {
        if (chosen == juce::File())
        {
            if (onComplete) onComplete (false);
            return;
        }
        const bool ok = saveSessionTo (chosen);
        if (onComplete) onComplete (ok);
    });
}

void MainComponent::saveAsPrompt()
{
    // Single-dialog Save As: filename text field + folder browser in one
    // step. The typed name becomes the session folder; the navigated
    // directory becomes its parent. Replaces the old two-step modal-then-
    // chooser flow which only let the user browse, never type.
    auto startDir = session.getSessionDirectory().getParentDirectory();
    if (! startDir.isDirectory())
        startDir = juce::File::getSpecialLocation (juce::File::userMusicDirectory)
                       .getChildFile ("Dusk Studio");
    if (! startDir.exists()) startDir.createDirectory();

    juce::String defaultName = session.getSessionDirectory().getFileName();
    if (defaultName.isEmpty() || defaultName == "Untitled") defaultName = "MySong";

    filebrowser::open (*this, {
        /*title*/                  "Save session as...",
        /*initialFileOrDirectory*/ startDir.getChildFile (defaultName),
        /*filePatternsAllowed*/    juce::String(),
        /*mode*/                   filebrowser::Mode::Save,
        /*warnAboutOverwriting*/   true,
        /*selectDirectories*/      false,
    },
    [this] (juce::File chosen)
    {
        if (chosen == juce::File()) return;
        saveSessionTo (chosen);
    });
}

bool MainComponent::loadSessionFromJson (const juce::File& sessionJson)
{
    if (! sessionJson.existsAsFile())
    {
        setStatusForPath ("No session at", sessionJson);
        return false;
    }

    const auto dir = sessionJson.getParentDirectory();

    // Recovery prompt path. The dialog is async so we hand it a continuation
    // that runs the actual load once the user has picked. The synchronous
    // bool return reflects only "we accepted the load attempt"; the load
    // itself completes after the dialog dismisses.
    if (autosaveIsNewerThan (sessionJson))
    {
        const auto autosave = getAutosaveFileFor (dir);
        juce::Component::SafePointer<MainComponent> safe (this);

        auto body = std::make_unique<AutosaveRecoveryDialog> (
            dir,
            autosave  .getLastModificationTime(),
            sessionJson.getLastModificationTime());
        body->setSize (560, 280);

        auto* raw = body.get();
        raw->onRecover = [safe, sessionJson, dir, autosave]
        {
            if (auto* self = safe.getComponent())
            {
                self->recoveryModal.close();
                self->finishLoadingSessionFrom (autosave, dir);
            }
        };
        raw->onLoad = [safe, sessionJson, dir]
        {
            if (auto* self = safe.getComponent())
            {
                self->recoveryModal.close();
                self->finishLoadingSessionFrom (sessionJson, dir);
            }
        };
        raw->onCancel = [safe]
        {
            if (auto* self = safe.getComponent())
                self->recoveryModal.close();
        };

        recoveryModal.show (*this, std::move (body),
                              [safe] { if (auto* self = safe.getComponent()) self->recoveryModal.close(); });
        return true;
    }

    return finishLoadingSessionFrom (sessionJson, dir);
}

bool MainComponent::finishLoadingSessionFrom (const juce::File& sourceJson,
                                                 const juce::File& dir)
{
    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    session.setSessionDirectory (dir);

    if (! SessionSerializer::load (session, sourceJson))
    {
        setStatusForPath ("Load failed", sourceJson);
        return false;
    }
    const auto tAfterParse = juce::Time::getMillisecondCounterHiRes();

    // Autosave's job is done - clean up so the next load has a clean slate.
    // (Even when the user chose "load saved session" we drop the autosave, on
    // the assumption they've made a deliberate choice to discard it.)
    deleteAutosaveFor (dir);

    // Note: lastSavedSessionJson is seeded later in this function, after
    // consumePluginStateAfterLoad has filled in plugin/transport state.
    // Seeding it here would miss those fields and the first autosave tick
    // would still fire (snapshot mismatch). The seed lives at the tail.

    // After deserialisation, the Track::pluginDescriptionXml /
    // pluginStateBase64 fields are populated; ask the engine to
    // re-instantiate each track's plugin from those.
    // Drop the prior session's undo stack BEFORE consuming plugin
    // state. Without this, hitting Cmd+Z right after a session load
    // would replay edits from the OLD session against the NEW one's
    // region indices - either a no-op or a use-after-free on the
    // referenced AudioRegion. Belt-and-suspenders: also resets redo.
    engine.getUndoManager().clearUndoHistory();

    engine.consumePluginStateAfterLoad();
    // Surface any plugin restore failures as a single summary dialog so
    // the user doesn't think a saved-with-Diva mix is intact when Diva
    // failed to instantiate. Deferred via callAsync so the load path
    // can finish drawing the freshly-loaded session before the modal
    // pops on top.
    {
        const auto& failures = engine.getLastPluginLoadFailures();
        if (! failures.empty())
        {
            juce::String body =
                "These plugins from the saved session could not be loaded "
                "and were left empty:\n\n";
            for (const auto& f : failures)
                body += "    " + f.location + "  -  " + f.pluginName + "\n";
            body += "\nCheck that the plugins are still installed for the "
                    "right format (VST3 / LV2) and that this binary can "
                    "find them, then reload the session.";
            juce::Component::SafePointer<MainComponent> safeThis (this);
            juce::MessageManager::callAsync (
                [body = std::move (body), safeThis]
                {
                    if (auto* self = safeThis.getComponent())
                        showDuskAlert (*self, "Missing plugins", body);
                });
        }
    }
    const auto tAfterPlugins = juce::Time::getMillisecondCounterHiRes();
    engine.consumeTransportStateAfterLoad();

    // Open MIDI output ports for any tracks the loaded session had
    // routed. Done here (not in the engine constructor) so startup
    // never blocks on snd_seq_connect_to for ports nobody uses.
    engine.openConfiguredMidiOutputs();
    const auto tAfterMidiOuts = juce::Time::getMillisecondCounterHiRes();

    // Reconstruct the console so all controls reflect the freshly
    // loaded values (atomic state is in `session`, but UI widgets
    // captured initial values in their constructors).
    consoleView.reset();
    consoleView = std::make_unique<ConsoleView> (session, engine);
    addAndMakeVisible (consoleView.get());
    consoleView->setOnStripFocusRequested ([this] (int t)
    {
        if (tapeStrip != nullptr) tapeStrip->setSelectedTrack (t);
    });
    if (consoleView != nullptr && transportBar != nullptr)
        consoleView->setStripsCompactMode (tapeStripExpanded);

    // Restore the saved UI stage. switchToStage() early-returns when the
    // engine is already on the requested stage, but the freshly-rebuilt
    // consoleView (above) still needs the per-stage sync (strip mixing
    // mode, mastering/aux view construction, toggle button state). Call
    // syncStageUi unconditionally so the on-screen stage always matches
    // session.uiStage after load.
    {
        const int loaded = juce::jlimit (0, 3, session.uiStage.load (std::memory_order_relaxed));
        const auto wantStage = (AudioEngine::Stage) loaded;
        if (engine.getStage() != wantStage)
            engine.setStage (wantStage);
        syncStageUi (wantStage);
    }
    resized();
    const auto tAfterConsole = juce::Time::getMillisecondCounterHiRes();

    RecentSessions::add (dir);
    setStatusForPath ("Loaded", sourceJson,
                         /*isAutosave*/ sourceJson.getFileName().endsWithIgnoreCase (".autosave"));

    // Seed the saved-state snapshot from the live in-memory session now
    // that plugin + transport state have been consumed. The next autosave
    // tick compares against this snapshot and skips the write while the
    // session remains untouched.
    setLastSavedSessionJson    (SessionSerializer::serialize (session));
    setLastWrittenAutosaveJson (juce::String());

    std::fprintf (stderr,
                  "[Dusk Studio/Load] %s: parse=%dms plugins=%dms midiOuts=%dms console=%dms total=%dms\n",
                  sourceJson.getFileName().toRawUTF8(),
                  (int) (tAfterParse    - t0),
                  (int) (tAfterPlugins  - tAfterParse),
                  (int) (tAfterMidiOuts - tAfterPlugins),
                  (int) (tAfterConsole  - tAfterMidiOuts),
                  (int) (tAfterConsole  - t0));
    return true;
}

void MainComponent::openFromFilePrompt()
{
    auto startDir = session.getSessionDirectory();
    if (! startDir.isDirectory())
        startDir = juce::File::getSpecialLocation (juce::File::userMusicDirectory);

    filebrowser::open (*this, {
        /*title*/                  "Open session.json",
        /*initialFileOrDirectory*/ startDir.getChildFile ("session.json"),
        /*filePatternsAllowed*/    "*.json",
        /*mode*/                   filebrowser::Mode::Open,
        /*warnAboutOverwriting*/   false,
        /*selectDirectories*/      false,
    },
    [this] (juce::File chosen)
    {
        if (chosen == juce::File()) return;
        loadSessionFromJson (chosen);
    });
}

void MainComponent::openBounceDialog()
{
    auto defaultDir = session.getSessionDirectory();
    if (! defaultDir.isDirectory())
        defaultDir = juce::File::getSpecialLocation (juce::File::userMusicDirectory);
    const auto defaultFile = defaultDir.getChildFile ("bounce.wav");

    filebrowser::open (*this, {
        /*title*/                  "Bounce master mix to WAV",
        /*initialFileOrDirectory*/ defaultFile,
        /*filePatternsAllowed*/    "*.wav",
        /*mode*/                   filebrowser::Mode::Save,
        /*warnAboutOverwriting*/   true,
        /*selectDirectories*/      false,
    },
    [this] (juce::File out)
    {
        if (out == juce::File()) return;  // user cancelled
        auto outFile = out;
        if (! outFile.hasFileExtension ("wav"))
            outFile = outFile.withFileExtension ("wav");

        auto panel = std::make_unique<BounceDialog> (engine, session,
                                                       engine.getDeviceManager(), outFile);
        panel->setSize (520, 200);
        bounceModal.show (*this, std::move (panel));
    });
}

// True when the track's name is empty or still the default "N" string.
// Used by the import flow to decide whether to auto-rename the track to
// the imported file's basename (preserves user-renamed tracks).
static bool trackHasDefaultName (const Track& t, int idx)
{
    const auto trimmed = t.name.trim();
    return trimmed.isEmpty() || trimmed == juce::String (idx + 1);
}

static void maybeRenameTrackFromFile (Track& t, int idx, const juce::File& f)
{
    if (trackHasDefaultName (t, idx))
        t.name = f.getFileNameWithoutExtension();
}

void MainComponent::runAudioImportFlow (const juce::File& source,
                                            juce::int64 timelineStart,
                                            int trackHint)
{
    std::unique_ptr<juce::AudioFormatReader> reader (
        importAudioFormatManager().createReaderFor (source));
    if (reader == nullptr)
    {
        showImportError ("Import audio",
                          "Unsupported or unreadable audio file: " + source.getFileName());
        return;
    }

    ImportTargetPicker::FileSummary summary;
    summary.file          = source;
    summary.sampleRate    = reader->sampleRate;
    summary.numChannels   = juce::jmin (2, (int) reader->numChannels);
    summary.lengthSamples = (juce::int64) reader->lengthInSamples;
    summary.isMidi        = false;
    reader.reset();   // close before the picker calls FileImporter

    auto picker = std::make_unique<ImportTargetPicker> (
        session,
        std::move (summary),
        timelineStart,
        engine.getCurrentSampleRate(),
        session.tempoBpm.load (std::memory_order_relaxed),
        session.beatsPerBar.load (std::memory_order_relaxed),
        session.timeDisplayMode.load (std::memory_order_relaxed),
        trackHint,
        [safeThis = juce::Component::SafePointer<MainComponent> (this),
         source, timelineStart] (int trackIndex)
        {
            auto* self = safeThis.getComponent();
            if (self == nullptr) return;
            self->importTargetModal.close();

            // Re-check transport state — the user could have hit Play
            // between opening the picker and confirming a target. The
            // success path mutates Track::regions in place, which is
            // only safe with playback halted. Abort the WHOLE chain on
            // mid-batch play so the user sees one error rather than
            // silently dropping every remaining queued file.
            if (! self->engine.getTransport().isStopped())
            {
                showImportError ("Import audio",
                                         "Stop playback before importing files.");
                self->cancelImportChain();
                return;
            }

            auto& track = self->session.track (trackIndex);
            const auto mode = (Track::Mode) track.mode.load (std::memory_order_relaxed);

            duskstudio::fileimport::AudioImportRequest req;
            req.source            = source;
            req.audioDir          = self->session.getAudioDirectory();
            req.trackIndex        = trackIndex;
            req.sessionSampleRate = self->engine.getCurrentSampleRate();
            req.targetChannels    = (mode == Track::Mode::Stereo) ? 2 : 1;
            req.timelineStart     = timelineStart;

            auto res = duskstudio::fileimport::importAudio (req);
            if (! res.ok)
            {
                showImportError ("Import audio failed", res.errorMessage);
                return;
            }

            // Transport is stopped (re-checked above), so PlaybackEngine
            // isn't iterating Track::regions on the audio thread - mutating
            // in place is safe; the next play() pulls the new layout via
            // preparePlayback.
            track.regions.push_back (std::move (res.region));
            duskstudio::maybeRenameTrackFromFile (track, trackIndex, source);
            if (self->tapeStrip != nullptr) self->tapeStrip->repaint();
            self->pendingImportLastCommitted = trackIndex;
            self->kickNextImport();
        },
        [safeThis = juce::Component::SafePointer<MainComponent> (this)]
        {
            if (auto* self = safeThis.getComponent())
                self->cancelImportChain();
        });

    importTargetModal.show (*this, std::move (picker));
}

void MainComponent::runMidiImportFlow (const juce::File& source,
                                          juce::int64 timelineStart,
                                          int trackHint)
{
    juce::MidiFile peek;
    {
        juce::FileInputStream in (source);
        if (! in.openedOk() || ! peek.readFrom (in))
        {
            showImportError ("Import MIDI", "Could not read MIDI file.");
            return;
        }
    }
    ImportTargetPicker::FileSummary summary;
    summary.file        = source;
    summary.isMidi      = true;
    summary.numChannels = -1;

    int noteCount = 0;
    juce::int64 maxTick = 0;
    const int ppq = (int) peek.getTimeFormat();
    for (int t = 0; t < peek.getNumTracks(); ++t)
    {
        if (const auto* trk = peek.getTrack (t))
        {
            for (int i = 0; i < trk->getNumEvents(); ++i)
            {
                const auto& m = trk->getEventPointer (i)->message;
                if (m.isNoteOn() && m.getVelocity() > 0) ++noteCount;
                maxTick = juce::jmax (maxTick,
                                        (juce::int64) std::llround (m.getTimeStamp()));
            }
        }
    }
    summary.numMidiNotes = noteCount;
    summary.lengthTicks  = (ppq > 0 && ppq != kMidiTicksPerQuarter)
                              ? (juce::int64) std::llround ((double) maxTick
                                   * (double) kMidiTicksPerQuarter / (double) ppq)
                              : maxTick;

    auto picker = std::make_unique<ImportTargetPicker> (
        session,
        std::move (summary),
        timelineStart,
        engine.getCurrentSampleRate(),
        session.tempoBpm.load (std::memory_order_relaxed),
        session.beatsPerBar.load (std::memory_order_relaxed),
        session.timeDisplayMode.load (std::memory_order_relaxed),
        trackHint,
        [safeThis = juce::Component::SafePointer<MainComponent> (this),
         source, timelineStart] (int trackIndex)
        {
            auto* self = safeThis.getComponent();
            if (self == nullptr) return;
            self->importTargetModal.close();

            // Mirror the audio-import recheck: the user could have hit
            // Play between opening the picker and confirming a target.
            // MidiRegions is AtomicSnapshot-mutated (RT-safe by design)
            // so this isn't a strict data-race guard, but importing a
            // region mid-playback produces confusing UX (notes appear
            // partway through the take). Bail consistently with the
            // audio path - cancel the whole chain so a multi-file batch
            // doesn't silently drop everything after the first play-edge.
            if (! self->engine.getTransport().isStopped())
            {
                showImportError ("Import MIDI",
                                         "Stop playback before importing files.");
                self->cancelImportChain();
                return;
            }

            auto& track = self->session.track (trackIndex);

            duskstudio::fileimport::MidiImportRequest req;
            req.source            = source;
            req.sessionSampleRate = self->engine.getCurrentSampleRate();
            req.sessionBpm        = self->session.tempoBpm.load (std::memory_order_relaxed);
            req.timelineStart     = timelineStart;

            auto res = duskstudio::fileimport::importMidi (req);
            if (! res.ok)
            {
                showImportError ("Import MIDI failed", res.errorMessage);
                return;
            }

            track.midiRegions.mutate ([&] (std::vector<MidiRegion>& v)
            {
                v.push_back (std::move (res.region));
            });
            duskstudio::maybeRenameTrackFromFile (track, trackIndex, source);
            if (self->tapeStrip != nullptr) self->tapeStrip->repaint();
            self->pendingImportLastCommitted = trackIndex;
            self->kickNextImport();
        },
        [safeThis = juce::Component::SafePointer<MainComponent> (this)]
        {
            if (auto* self = safeThis.getComponent())
                self->cancelImportChain();
        });

    importTargetModal.show (*this, std::move (picker));
}

void MainComponent::importAudioPrompt()
{
    if (! engine.getTransport().isStopped())
    {
        showImportError ("Import audio", "Stop playback before importing files.");
        return;
    }

    const auto startDir = juce::File::getSpecialLocation (juce::File::userMusicDirectory);
    filebrowser::openMulti (*this, {
        /*title*/                  "Import audio file(s)",
        /*initialFileOrDirectory*/ startDir,
        /*filePatternsAllowed*/    "*.wav;*.aiff;*.aif;*.flac",
        /*mode*/                   filebrowser::Mode::Open,
        /*warnAboutOverwriting*/   false,
        /*selectDirectories*/      false,
    },
    [safeThis = juce::Component::SafePointer<MainComponent> (this)]
    (juce::Array<juce::File> chosen)
    {
        auto* self = safeThis.getComponent();
        if (self == nullptr || chosen.isEmpty()) return;
        const auto t = self->engine.getTransport().getPlayhead();
        if (chosen.size() > 1)
            self->openMultiImportPicker (chosen, t);
        else
            self->enqueueImports (chosen, t, -1);
    });
}

void MainComponent::importMidiPrompt()
{
    if (! engine.getTransport().isStopped())
    {
        showImportError ("Import MIDI", "Stop playback before importing files.");
        return;
    }

    const auto startDir = juce::File::getSpecialLocation (juce::File::userMusicDirectory);
    filebrowser::openMulti (*this, {
        /*title*/                  "Import MIDI file(s)",
        /*initialFileOrDirectory*/ startDir,
        /*filePatternsAllowed*/    "*.mid;*.midi",
        /*mode*/                   filebrowser::Mode::Open,
        /*warnAboutOverwriting*/   false,
        /*selectDirectories*/      false,
    },
    [safeThis = juce::Component::SafePointer<MainComponent> (this)]
    (juce::Array<juce::File> chosen)
    {
        auto* self = safeThis.getComponent();
        if (self == nullptr || chosen.isEmpty()) return;
        const auto t = self->engine.getTransport().getPlayhead();
        if (chosen.size() > 1)
            self->openMultiImportPicker (chosen, t);
        else
            self->enqueueImports (chosen, t, -1);
    });
}

void MainComponent::enqueueImports (juce::Array<juce::File> files,
                                       juce::int64 timelineStart,
                                       int trackHint)
{
    pendingImportQueue.clear();
    pendingImportLastCommitted = -2;
    pendingImportInitialHint   = trackHint;
    pendingImportTimelineStart = timelineStart;
    for (const auto& f : files)
    {
        PendingImport p;
        p.file       = f;
        p.trackIndex = -1;   // needs picker
        const auto ext = f.getFileExtension().toLowerCase();
        p.isMidi     = (ext == ".mid" || ext == ".midi");
        pendingImportQueue.push_back (std::move (p));
    }
    kickNextImport();
}

void MainComponent::enqueueImportsWithTargets (
    std::vector<MultiImportTargetPicker::Assignment> assignments,
    juce::int64 timelineStart)
{
    pendingImportQueue.clear();
    pendingImportLastCommitted = -2;
    pendingImportInitialHint   = -1;
    pendingImportTimelineStart = timelineStart;
    for (const auto& a : assignments)
    {
        PendingImport p;
        p.file       = a.file;
        p.trackIndex = a.trackIndex;   // pre-assigned, picker skipped
        p.isMidi     = a.isMidi;
        pendingImportQueue.push_back (std::move (p));
    }
    kickNextImport();
}

void MainComponent::kickNextImport()
{
    if (pendingImportQueue.empty()) return;
    auto entry = pendingImportQueue.front();
    pendingImportQueue.erase (pendingImportQueue.begin());

    // Pre-assigned (multi-picker path): skip the per-file modal and
    // dispatch straight to the importer.
    if (entry.trackIndex >= 0)
    {
        MultiImportTargetPicker::Assignment a;
        a.file       = entry.file;
        a.trackIndex = entry.trackIndex;
        a.isMidi     = entry.isMidi;
        commitImportNoModal (a, pendingImportTimelineStart);
        return;
    }

    // Sequential hint: after the first commit, push subsequent files to
    // adjacent tracks so a drop on track 2 fills 2/3/4 unless the user
    // overrides each pick in the modal.
    const int hint = pendingImportLastCommitted >= 0
                       ? juce::jmin (pendingImportLastCommitted + 1,
                                       Session::kNumTracks - 1)
                       : pendingImportInitialHint;

    if (entry.isMidi)
        runMidiImportFlow (entry.file, pendingImportTimelineStart, hint);
    else
        runAudioImportFlow (entry.file, pendingImportTimelineStart, hint);
}

void MainComponent::cancelImportChain()
{
    pendingImportQueue.clear();
    pendingImportLastCommitted = -2;
    importTargetModal.close();
}

void MainComponent::commitImportNoModal (
    const MultiImportTargetPicker::Assignment& a, juce::int64 timelineStart)
{
    // Mirror the per-file picker's onCommit body but without re-opening
    // a modal. Mid-batch transport state changes abort the whole queue,
    // same as the single-file path.
    if (! engine.getTransport().isStopped())
    {
        showImportError (a.isMidi ? "Import MIDI" : "Import audio",
                          "Stop playback before importing files.");
        cancelImportChain();
        return;
    }

    auto& track = session.track (a.trackIndex);

    if (a.isMidi)
    {
        duskstudio::fileimport::MidiImportRequest req;
        req.source            = a.file;
        req.sessionSampleRate = engine.getCurrentSampleRate();
        req.sessionBpm        = session.tempoBpm.load (std::memory_order_relaxed);
        req.timelineStart     = timelineStart;

        auto res = duskstudio::fileimport::importMidi (req);
        if (! res.ok)
        {
            showImportError ("Import MIDI failed", res.errorMessage);
            kickNextImport();
            return;
        }
        track.midiRegions.mutate ([&] (std::vector<MidiRegion>& v)
        {
            v.push_back (std::move (res.region));
        });
    }
    else
    {
        const auto mode = (Track::Mode) track.mode.load (std::memory_order_relaxed);

        duskstudio::fileimport::AudioImportRequest req;
        req.source            = a.file;
        req.audioDir          = session.getAudioDirectory();
        req.trackIndex        = a.trackIndex;
        req.sessionSampleRate = engine.getCurrentSampleRate();
        req.targetChannels    = (mode == Track::Mode::Stereo) ? 2 : 1;
        req.timelineStart     = timelineStart;

        auto res = duskstudio::fileimport::importAudio (req);
        if (! res.ok)
        {
            showImportError ("Import audio failed", res.errorMessage);
            kickNextImport();
            return;
        }
        track.regions.push_back (std::move (res.region));
    }

    duskstudio::maybeRenameTrackFromFile (track, a.trackIndex, a.file);
    if (tapeStrip != nullptr) tapeStrip->repaint();
    pendingImportLastCommitted = a.trackIndex;
    kickNextImport();
}

void MainComponent::openMultiImportPicker (juce::Array<juce::File> files,
                                              juce::int64 timelineStart)
{
    // Peek each file to build a FileSummary the picker can render. Audio
    // peek opens an AudioFormatReader; MIDI peek runs MidiFile::readFrom +
    // counts notes. Both happen synchronously on the message thread - the
    // user clicked Import and is waiting for the modal to appear.
    std::vector<ImportTargetPicker::FileSummary> summaries;
    summaries.reserve ((size_t) files.size());
    for (const auto& f : files)
    {
        ImportTargetPicker::FileSummary s;
        s.file = f;
        const auto ext = f.getFileExtension().toLowerCase();
        s.isMidi = (ext == ".mid" || ext == ".midi");

        if (s.isMidi)
        {
            juce::MidiFile peek;
            juce::FileInputStream in (f);
            if (in.openedOk() && peek.readFrom (in))
            {
                int noteCount = 0;
                juce::int64 maxTick = 0;
                const int ppq = (int) peek.getTimeFormat();
                for (int t = 0; t < peek.getNumTracks(); ++t)
                    if (const auto* trk = peek.getTrack (t))
                        for (int i = 0; i < trk->getNumEvents(); ++i)
                        {
                            const auto& m = trk->getEventPointer (i)->message;
                            if (m.isNoteOn() && m.getVelocity() > 0) ++noteCount;
                            maxTick = juce::jmax (maxTick,
                                (juce::int64) std::llround (m.getTimeStamp()));
                        }
                s.numMidiNotes = noteCount;
                s.lengthTicks  = (ppq > 0 && ppq != kMidiTicksPerQuarter)
                                    ? (juce::int64) std::llround ((double) maxTick
                                          * (double) kMidiTicksPerQuarter / (double) ppq)
                                    : maxTick;
            }
            s.numChannels = -1;
        }
        else
        {
            std::unique_ptr<juce::AudioFormatReader> reader (
                importAudioFormatManager().createReaderFor (f));
            if (reader == nullptr)
                continue;
            s.sampleRate    = reader->sampleRate;
            s.numChannels   = juce::jmin (2, (int) reader->numChannels);
            s.lengthSamples = (juce::int64) reader->lengthInSamples;
        }
        summaries.push_back (std::move (s));
    }

    // Every file failed to open (no reader, malformed MIDI). Surface the
    // error and bail rather than opening a multi-import modal with zero
    // rows and a permanently-disabled Import button.
    if (summaries.empty())
    {
        showImportError ("Import",
                          "None of the selected files could be opened. "
                          "Check that they are valid audio or MIDI files.");
        return;
    }

    auto picker = std::make_unique<MultiImportTargetPicker> (
        session, std::move (summaries), timelineStart,
        [safeThis = juce::Component::SafePointer<MainComponent> (this),
         timelineStart]
        (std::vector<MultiImportTargetPicker::Assignment> assignments)
        {
            auto* self = safeThis.getComponent();
            if (self == nullptr) return;
            self->importTargetModal.close();
            self->enqueueImportsWithTargets (std::move (assignments), timelineStart);
        },
        [safeThis = juce::Component::SafePointer<MainComponent> (this)]
        {
            if (auto* self = safeThis.getComponent())
                self->cancelImportChain();
        });

    // Modal stays open on click-outside - the user must explicitly
    // Cancel or Import. Esc still routes to the body's key handling
    // (no onDismiss installed -> Esc is currently a no-op too).
    importTargetModal.show (*this, std::move (picker), {}, /*dismissOnClickOutside*/ false);
}

// ── MenuBarModel ─────────────────────────────────────────────────────────
//
// Two top-level menus drive every header action that used to be a separate
// TextButton. Item IDs are namespaced per-menu so menuItemSelected can
// dispatch with a single switch, no need to also branch on the top-level
// menu index.
namespace
{
enum MenuItemId
{
    kMenuFileNew      = 1001,
    kMenuFileOpen     = 1002,
    kMenuFileSave     = 1003,
    kMenuFileSaveAs   = 1004,
    kMenuFileImportAudio = 1006,
    kMenuFileImportMidi  = 1007,
    kMenuFileMixdown  = 1010,
    kMenuFileBounce   = 1011,
    kMenuFileCleanOut = 1012,
    kMenuFileOptimizeAutomation = 1013,
    kMenuFileQuit     = 1099,
    // Reserved range for template entries (one per SessionTemplate enum
    // value, indexed off this base). Stays well above the file-action IDs
    // so future additions don't collide.
    kMenuFileTemplateBase = 1200,
    kMenuSettingsAudio = 2001,
    kMenuSettingsAbout = 2002,
};
}

juce::StringArray MainComponent::getMenuBarNames()
{
    return { "File", "Settings" };
}

juce::PopupMenu MainComponent::getMenuForIndex (int topLevelMenuIndex,
                                                  const juce::String& /*name*/)
{
    juce::PopupMenu menu;
    if (topLevelMenuIndex == 0)   // File
    {
        menu.addItem (kMenuFileNew,    "New session...");

        // New From Template submenu - drops opinionated track names /
        // colours / modes onto the live session so the user is one
        // arm-click from recording. Iterates the SessionTemplate enum so
        // a new template appears here automatically.
        juce::PopupMenu templates;
        for (int i = 0; i < (int) SessionTemplate::kCount; ++i)
            templates.addItem (kMenuFileTemplateBase + i,
                                nameForTemplate ((SessionTemplate) i));
        menu.addSubMenu ("New from template", templates);

        menu.addItem (kMenuFileOpen,   "Open...");
        menu.addItem (kMenuFileSave,   "Save");
        menu.addItem (kMenuFileSaveAs, "Save as...");
        menu.addSeparator();
        menu.addItem (kMenuFileImportAudio, "Import audio...");
        menu.addItem (kMenuFileImportMidi,  "Import MIDI...");
        menu.addSeparator();
        menu.addItem (kMenuFileMixdown, "Mixdown");
        menu.addItem (kMenuFileBounce,  "Bounce...");
        menu.addSeparator();
        menu.addItem (kMenuFileCleanOut, "Clean out unreferenced files...");
        menu.addItem (kMenuFileOptimizeAutomation, "Optimize automation...");
        menu.addSeparator();
        menu.addItem (kMenuFileQuit, "Quit");
    }
    else if (topLevelMenuIndex == 1)   // Settings
    {
        menu.addItem (kMenuSettingsAudio, "Settings...");
        menu.addSeparator();
        menu.addItem (kMenuSettingsAbout, "About Dusk Studio");
    }
    return menu;
}

void MainComponent::menuItemSelected (int menuItemID, int /*topLevelMenuIndex*/)
{
    switch (menuItemID)
    {
        case kMenuFileNew:    newSessionPrompt();       break;
        case kMenuFileOpen:   openFromFilePrompt();     break;
        case kMenuFileSave:
        {
            // Mirror the Save button's smart behavior: redirect to Save As
            // if the session has never been saved (no session.json yet) so
            // the user picks a real destination instead of clobbering the
            // bootstrap "Untitled" dir.
            const auto dir = session.getSessionDirectory();
            if (! dir.getChildFile ("session.json").existsAsFile())
                saveAsPrompt();
            else
                saveSessionTo (dir);
            break;
        }
        case kMenuFileSaveAs: saveAsPrompt();           break;
        case kMenuFileImportAudio: importAudioPrompt(); break;
        case kMenuFileImportMidi:  importMidiPrompt();  break;
        case kMenuFileMixdown: doMixdown();             break;
        case kMenuFileBounce:  openBounceDialog();      break;
        case kMenuFileCleanOut: cleanOutUnreferencedFiles(); break;
        case kMenuFileOptimizeAutomation:
        {
            // Safety contract from Session.h: handleWritePassComplete is
            // NOT lock-free against audio-thread lane reads, so we only
            // call it when no strip is in Read/Touch and the transport
            // is stopped (no audio routing through evaluateLane). The
            // dialog enforces this explicitly so a user who hasn't read
            // the spec can't trigger a race.
            const bool playing = engine.getTransport().isPlaying();
            bool anyActive = false;
            auto isActive = [] (int amode) noexcept
            {
                return amode == (int) AutomationMode::Read
                    || amode == (int) AutomationMode::Write
                    || amode == (int) AutomationMode::Touch;
            };
            for (int t = 0; t < Session::kNumTracks && ! anyActive; ++t)
                if (isActive (session.track (t).automationMode.load (std::memory_order_relaxed)))
                    anyActive = true;
            for (int a = 0; a < Session::kNumAuxLanes && ! anyActive; ++a)
                if (isActive (session.auxLane (a).params.automationMode.load (std::memory_order_relaxed)))
                    anyActive = true;
            if (! anyActive
                && isActive (session.master().automationMode.load (std::memory_order_relaxed)))
                anyActive = true;

            if (playing || anyActive)
            {
                showDuskAlert (*this, "Optimize automation",
                                  playing
                                    ? juce::String ("Stop playback before optimising automation. "
                                                      "The optimiser rewrites every lane's point "
                                                      "data; running it while the audio thread "
                                                      "may be reading the lanes is unsafe.")
                                    : juce::String ("Set every strip's automation mode to Off "
                                                      "before optimising. The optimiser rewrites "
                                                      "lane data; doing it while a strip is in "
                                                      "Read or Touch can race the audio thread."));
                break;
            }

            // Count points before / after so the user sees what the
            // optimiser actually did.
            auto countPoints = [this]() noexcept
            {
                std::size_t total = 0;
                for (int t = 0; t < Session::kNumTracks; ++t)
                    for (int p = 0; p < kNumAutomationParams; ++p)
                        total += session.track (t).automationLanes[(size_t) p].points.size();
                for (int a = 0; a < Session::kNumAuxLanes; ++a)
                    for (int p = 0; p < kNumAutomationParams; ++p)
                        total += session.auxLane (a).params.automationLanes[(size_t) p].points.size();
                for (int p = 0; p < kNumAutomationParams; ++p)
                    total += session.master().automationLanes[(size_t) p].points.size();
                return total;
            };
            const auto before = countPoints();
            handleWritePassComplete (session);
            const auto after = countPoints();

            showDuskAlert (*this, "Optimize automation",
                              juce::String ("Thinned ")
                                + juce::String ((juce::int64) before)
                                + " automation points down to "
                                + juce::String ((juce::int64) after) + ".");
            break;
        }
        case kMenuFileQuit:
            if (auto* app = juce::JUCEApplicationBase::getInstance())
                app->systemRequestedQuit();
            break;
        case kMenuSettingsAudio: openAudioSettings();   break;
        case kMenuSettingsAbout:
        {
            // Pull the version string from the JUCE_APPLICATION_VERSION_STRING
            // compile define (wired through CMakeLists from PROJECT_VERSION).
            // Always matches what's in Info.plist so a bug report's reported
            // version can be cross-checked.
            const auto body =
                juce::String ("Dusk Studio ") + JUCE_APPLICATION_VERSION_STRING + "\n\n"
                "Portastudio-style DAW.\n"
                "Built " __DATE__ " " __TIME__;
            showDuskAlert (*this, "About Dusk Studio", body);
            break;
        }
        default:
            // Template menu items live in [kMenuFileTemplateBase, +kCount).
            if (menuItemID >= kMenuFileTemplateBase
                && menuItemID < kMenuFileTemplateBase + (int) SessionTemplate::kCount)
            {
                applyTemplate (session,
                                (SessionTemplate) (menuItemID - kMenuFileTemplateBase));
                // Rebuild the console view so the new track names / colours
                // / modes propagate into existing strip components - the
                // simplest way to pick up name + colour + mode in one shot.
                consoleView.reset();
                consoleView = std::make_unique<ConsoleView> (session, engine);
                addAndMakeVisible (consoleView.get());
                consoleView->setOnStripFocusRequested ([this] (int t)
                {
                    if (tapeStrip != nullptr) tapeStrip->setSelectedTrack (t);
                });
                consoleView->setStripsMixingMode (
                    engine.getStage() == AudioEngine::Stage::Mixing);
                if (tapeStrip != nullptr) tapeStrip->repaint();
                resized();
            }
            break;
    }
}

void MainComponent::cleanOutUnreferencedFiles()
{
    // Build the set of WAVs the session is currently using - both the
    // live region's `file` and every previousTakes entry. Anything in
    // the audio dir not in this set is fair game for deletion.
    auto audioDir = session.getAudioDirectory();
    if (! audioDir.isDirectory())
    {
        showDuskAlert (*this, "Clean out",
                          "This session has no audio directory yet, "
                          "so there's nothing to clean.");
        return;
    }

    juce::StringArray referenced;   // full paths
    for (int t = 0; t < Session::kNumTracks; ++t)
    {
        for (const auto& r : session.track (t).regions)
        {
            referenced.addIfNotAlreadyThere (r.file.getFullPathName());
            for (const auto& take : r.previousTakes)
                referenced.addIfNotAlreadyThere (take.file.getFullPathName());
        }
    }

    // Walk the audio directory for .wav files. Anything outside the
    // referenced set is a candidate. Subdirectories are intentionally
    // skipped so external WAVs the user dropped in by hand don't get
    // touched.
    juce::Array<juce::File> candidates;
    juce::int64 totalBytes = 0;
    for (const auto& f : audioDir.findChildFiles (juce::File::findFiles, false, "*.wav"))
    {
        if (! referenced.contains (f.getFullPathName()))
        {
            candidates.add (f);
            totalBytes += f.getSize();
        }
    }

    if (candidates.isEmpty())
    {
        showDuskAlert (*this, "Clean out",
                          "No unreferenced files found. The audio "
                          "directory is already clean.");
        return;
    }

    const auto sizeMB = (double) totalBytes / (1024.0 * 1024.0);
    const auto msg = "Found " + juce::String (candidates.size())
                   + " unreferenced .wav file(s) totalling "
                   + juce::String (sizeMB, 1) + " MB.\n\n"
                   + "These were created by past record passes that no "
                     "longer have any region or take pointing at them. "
                     "Deleting cannot be undone.";

    juce::Component::SafePointer<MainComponent> safeThis (this);
    showDuskConfirm (*this, "Clean out unreferenced files", msg,
                       /*primary*/   "Delete",
                       /*onPrimary*/ [safeThis, candidates]
                       {
                           int deleted = 0;
                           for (const auto& f : candidates)
                               if (f.deleteFile()) ++deleted;
                           if (auto* self = safeThis.getComponent())
                               self->statusLabel.setText (
                                   "Deleted " + juce::String (deleted)
                                       + " unreferenced file(s).",
                                   juce::dontSendNotification);
                       },
                       /*secondary*/   "Cancel",
                       /*onSecondary*/ {},
                       /*destructive*/ true);
}

void MainComponent::openPianoRoll (int trackIdx, int regionIdx)
{
    // Mutually exclusive with the audio editor. Opening the piano roll
    // tears down any open audio editor first.
    if (audioEditor != nullptr) closeAudioEditor();

    // Toggle vs swap. Clicking the SAME region while the roll is already
    // open dismisses it (a second click on a region is naturally read as
    // "I'm done"); clicking a DIFFERENT region tears the current roll
    // down and re-opens on the new target so the user doesn't have to
    // dismiss-then-click.
    if (pianoRoll != nullptr)
    {
        const bool sameRegion = (pianoRollTrackIdx == trackIdx
                                  && pianoRollRegionIdx == regionIdx);
        closePianoRoll();
        if (sameRegion) return;
    }

    pianoRoll = std::make_unique<PianoRollComponent> (session, engine, trackIdx, regionIdx);
    pianoRollTrackIdx  = trackIdx;
    pianoRollRegionIdx = regionIdx;
    pianoRollDim = std::make_unique<DimOverlay> (0.80f);

    // Sized as a centred panel that leaves a small inset on each side so
    // the dimmed backdrop is still visible (helps users see they're in a
    // modal). The inset shrinks on small windows so the roll always has
    // a workable surface even on a 1280-wide screen.
    const auto bounds = getLocalBounds();
    const int inset = juce::jmax (24, juce::jmin (bounds.getWidth(), bounds.getHeight()) / 16);
    const auto rollBounds = bounds.reduced (inset);

    pianoRollDim->setBounds (bounds);
    pianoRollDim->onClick = [this] { closePianoRoll(); };
    addAndMakeVisible (pianoRollDim.get());

    pianoRoll->setBounds (rollBounds);
    pianoRoll->onCloseRequested = [this] { closePianoRoll(); };
    pianoRoll->onNavigateToRegion = [this] (int t, int newIdx)
    {
        // Close + reopen on the new region. Same-track only; the
        // editor already validated the bounds before calling.
        closePianoRoll();
        openPianoRoll (t, newIdx);
    };
    addAndMakeVisible (pianoRoll.get());
    pianoRoll->grabKeyboardFocus();
}

void MainComponent::closePianoRoll()
{
    if (pianoRoll != nullptr) removeChildComponent (pianoRoll.get());
    if (pianoRollDim != nullptr) removeChildComponent (pianoRollDim.get());
    pianoRoll.reset();
    pianoRollDim.reset();
    pianoRollTrackIdx  = -1;
    pianoRollRegionIdx = -1;
    if (tapeStrip != nullptr) tapeStrip->repaint();
}

void MainComponent::openAudioEditor (int trackIdx, int regionIdx)
{
    // Mutual exclusion with the piano roll - opening the audio editor
    // tears down any open piano roll first.
    if (pianoRoll != nullptr) closePianoRoll();

    // Toggle vs swap, mirroring openPianoRoll's semantics. Same target
    // region on a re-double-click closes; a different region swaps.
    if (audioEditor != nullptr)
    {
        const bool sameRegion = (audioEditorTrackIdx == trackIdx
                                  && audioEditorRegionIdx == regionIdx);
        closeAudioEditor();
        if (sameRegion) return;
    }

    audioEditor = std::make_unique<AudioRegionEditor> (session, engine, trackIdx, regionIdx);
    audioEditorTrackIdx  = trackIdx;
    audioEditorRegionIdx = regionIdx;
    audioEditorDim = std::make_unique<DimOverlay> (0.80f);

    const auto bounds = getLocalBounds();
    const int inset = juce::jmax (24, juce::jmin (bounds.getWidth(), bounds.getHeight()) / 16);
    const auto editorBounds = bounds.reduced (inset);

    audioEditorDim->setBounds (bounds);
    audioEditorDim->onClick = [this] { closeAudioEditor(); };
    addAndMakeVisible (audioEditorDim.get());

    audioEditor->setBounds (editorBounds);
    audioEditor->onCloseRequested = [this] { closeAudioEditor(); };
    audioEditor->onNavigateToRegion = [this] (int t, int newIdx)
    {
        closeAudioEditor();
        openAudioEditor (t, newIdx);
    };
    addAndMakeVisible (audioEditor.get());
    audioEditor->grabKeyboardFocus();
}

void MainComponent::closeAudioEditor()
{
    if (audioEditor    != nullptr) removeChildComponent (audioEditor.get());
    if (audioEditorDim != nullptr) removeChildComponent (audioEditorDim.get());
    audioEditor.reset();
    audioEditorDim.reset();
    audioEditorTrackIdx  = -1;
    audioEditorRegionIdx = -1;
    if (tapeStrip != nullptr) tapeStrip->repaint();
}

namespace
{
// Tiny Timer subclass used by the tuner overlay to poll the engine's
// pitch atoms at 30 Hz on the message thread. Public-internal because
// the unique_ptr in MainComponent.h holds it as juce::Timer*.
class TunerPoller final : public juce::Timer
{
public:
    TunerPoller (Session& s, TunerOverlay& o) : session (s), overlay (o)
    {
        startTimerHz (30);
    }
    void timerCallback() override
    {
        const float hz = session.tuneLatestHz   .load (std::memory_order_relaxed);
        const float lv = session.tuneLatestLevel.load (std::memory_order_relaxed);
        overlay.setDetected (hz, lv);
    }
private:
    Session& session;
    TunerOverlay& overlay;
};
} // namespace

void MainComponent::toggleTuner()
{
    if (tuner != nullptr) { closeTuner(); return; }

    // Pick the track to tune. Prefers the user's most-recent selection
    // (the same selectedTrack the keyboard shortcuts use); falls back
    // to track 0 when nothing's selected so the button always does
    // something rather than silently failing.
    int trackIdx = 0;
    if (tapeStrip != nullptr)
    {
        const int sel = tapeStrip->getSelectedTrack();
        if (sel >= 0 && sel < Session::kNumTracks) trackIdx = sel;
    }
    session.tuneTrackIndex.store (trackIdx, std::memory_order_relaxed);
    session.tuneLatestHz   .store (0.0f,    std::memory_order_relaxed);
    session.tuneLatestLevel.store (0.0f,    std::memory_order_relaxed);

    tuner = std::make_unique<TunerOverlay>();
    tuner->onDismiss = [this] { closeTuner(); };
    tunerDim = std::make_unique<DimOverlay>();
    tunerDim->setBounds (getLocalBounds());
    tunerDim->onClick = [this] { closeTuner(); };
    addAndMakeVisible (tunerDim.get());

    tuner->setBounds (getLocalBounds());
    addAndMakeVisible (tuner.get());

    tunerPoller = std::make_unique<TunerPoller> (session, *tuner);
}

void MainComponent::closeTuner()
{
    tunerPoller.reset();
    if (tuner    != nullptr) removeChildComponent (tuner.get());
    if (tunerDim != nullptr) removeChildComponent (tunerDim.get());
    tuner.reset();
    tunerDim.reset();
    session.tuneTrackIndex.store (-1, std::memory_order_relaxed);
}

void MainComponent::toggleVirtualKeyboard()
{
    if (virtualKeyboardModal.isOpen())
    {
        // Closing the VKB: also reset any in-flight step-record
        // chord state on the open piano roll. Stale held-counters
        // would otherwise survive across VKB open/close cycles.
        if (pianoRoll != nullptr)
            pianoRoll->resetStepRecordState();
        virtualKeyboardModal.close();
        return;
    }

    auto body = std::make_unique<VirtualKeyboardComponent> (engine);
    body->setSize (720, 220);

    // Step-record wiring: when the piano roll is open at the time
    // each VKB note fires, the note also lands as a MidiNote at
    // the playhead. We capture by SafePointer so closing either
    // modal can't dangle. The roll's stepRecordNoteOn/Off handle
    // the chord-aware playhead-advance logic.
    juce::Component::SafePointer<MainComponent> safeThis (this);
    body->onNoteOn = [safeThis] (int note, int vel, int /*chan*/)
    {
        if (auto* self = safeThis.getComponent())
            if (self->pianoRoll != nullptr)
                self->pianoRoll->stepRecordNoteOn (note, vel);
    };
    body->onNoteOff = [safeThis] (int note, int /*chan*/)
    {
        if (auto* self = safeThis.getComponent())
            if (self->pianoRoll != nullptr)
                self->pianoRoll->stepRecordNoteOff (note);
    };

    virtualKeyboardModal.show (*this, std::move (body));
}
} // namespace duskstudio
