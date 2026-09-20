#include "MainComponent.h"

#include "AuxLaneComponent.h"
#include "AuxView.h"
#include "AudioRegionEditor.h"
#include "BusComponent.h"
#include "ChannelStripComponent.h"
#include "ConsoleView.h"
#include "EmbeddedModal.h"
#include "DuskContextMenu.h"
#include "DpImportDialog.h"
#include "MultiImportTargetPicker.h"
#include "DuskAlerts.h"
#include "TapeStrip.h"
#include "MiniTimelineStrip.h"
#include "GuiHost.h"
#include "MasterStripComponent.h"
#include "MasteringView.h"
#include "PlatformWindowing.h"
#include "PianoRollComponent.h"
#include "TransportBar.h"
#include "../engine/scenario/SuiteRunner.h"

#include <array>
#include <cstddef>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>

// The GUI half of the scenario suite: the live window, presented to the cases
// as scenario::GuiHost. Only a MainComponent member reaches the console, the aux
// view, the session and the engine at once, so the implementation lives here
// rather than beside the other scenario code.
namespace duskstudio
{
namespace
{
// The engine's plug-in loaders take the framework's file and string types.
// Naming them through Session's own getter keeps this file free of the
// framework header, the way the headless cases do it.
using HostFile   = std::decay_t<decltype (std::declval<const Session&>().getSessionDirectory())>;
using HostString = std::decay_t<decltype (std::declval<const Session&>()
                                              .getSessionDirectory().getFullPathName())>;

HostFile hostFile (const std::filesystem::path& path)
{
    return HostFile (HostString::fromUTF8 (path.u8string().c_str()));
}
template <typename Owner, typename Key>
bool dispatchKey (Owner& owner, bool (Owner::*handler) (const Key&),
                  const std::string& description, char text)
{
    const auto key = Key::createFromDescription (HostString (description.c_str()));
    return (owner.*handler) (Key (key.getKeyCode(), key.getModifiers(), text));
}

template <typename Peer, typename Source, typename Point, typename Modifiers, typename... Rest>
void dispatchMouseButton (Peer& peer, void (Peer::*handler) (Source, Point, Modifiers, Rest...),
                          float x, float y, bool down, std::int64_t time, int modifiers = 0)
{
    const int flags = (down ? Modifiers::leftButtonModifier : 0)
                    | ((modifiers & 1) != 0 ? Modifiers::shiftModifier : 0)
                    | ((modifiers & 2) != 0 ? Modifiers::commandModifier : 0);
    const auto saved = Modifiers::currentModifiers;
    Modifiers::currentModifiers = Modifiers (flags);
    (peer.*handler) (Source::mouse, Point (x, y) * peer.getComponent().getDesktopScaleFactor(), Modifiers (flags),
                    1.0f, 0.0f, time, {}, 0);
    Modifiers::currentModifiers = saved;
}

template <typename Component, typename Files>
bool dispatchFileDrop (Component& component, void (Component::*handler) (const Files&, int, int),
                       const std::vector<std::filesystem::path>& files, int x, int y)
{
    Files names;
    for (const auto& path : files) names.add (HostString::fromUTF8 (path.u8string().c_str()));
    if (! component.isInterestedInFileDrag (names)) return false;
    component.fileDragEnter (names, x, y);
    (component.*handler) (names, x, y);
    return true;
}

} // namespace

struct MainComponent::ScenarioStripHandle final : scenario::StripHandle
{
    ScenarioStripHandle (MainComponent& ownerIn, int indexIn)
        : owner (ownerIn), index (indexIn) {}

    bool loadNativeClap (const std::filesystem::path& file, const std::string& pluginId,
                         std::string& errorOut) override
    {
       #if DUSKSTUDIO_HAS_NATIVE_CLAP
        return owner.engine.getChannelStrip (index)
            .loadNativeClap (hostFile (file), errorOut, HostString (pluginId.c_str()));
       #else
        (void) file; (void) pluginId;
        errorOut = "built without native CLAP hosting";
        return false;
       #endif
    }

    bool loadNativeLv2 (const std::filesystem::path& file, const std::string& pluginId,
                        std::string& errorOut) override
    {
       #if DUSKSTUDIO_HAS_NATIVE_LV2
        return owner.engine.getChannelStrip (index)
            .loadNativeLv2 (hostFile (file), errorOut, HostString (pluginId.c_str()));
       #else
        (void) file; (void) pluginId;
        errorOut = "built without native LV2 hosting";
        return false;
       #endif
    }

    bool loadNativeVst3 (const std::filesystem::path& file, const std::string& pluginId,
                         std::string& errorOut) override
    {
       #if DUSKSTUDIO_HAS_NATIVE_VST3
        return owner.engine.getChannelStrip (index)
            .loadNativeVst3 (hostFile (file), errorOut, HostString (pluginId.c_str()));
       #else
        (void) file; (void) pluginId;
        errorOut = "built without native VST3 hosting";
        return false;
       #endif
    }

    void unloadNativePlugins() override
    {
        auto& strip = owner.engine.getChannelStrip (index);
        strip.unloadNativeClap();
        strip.unloadNativeLv2();
        strip.unloadNativeVst3();

        auto& track = owner.session.track (index);
        track.nativeClapPath.clear();
        track.nativeClapPluginId.clear();
        track.nativeClapStateBase64.clear();
        track.nativeLv2Path.clear();
        track.nativeLv2PluginId.clear();
        track.nativeLv2StateBase64.clear();
        track.nativeVst3Path.clear();
        track.nativeVst3PluginId.clear();
        track.nativeVst3StateBase64.clear();
    }

    void refreshInsertButton() override
    {
        if (auto* component = strip()) component->refreshInsertButtonForCapture();
    }

    bool openEditor() override
    {
        auto* component = strip();
        return component != nullptr && component->openPluginEditorForScenario();
    }

    void closeEditor() override
    {
        if (auto* component = strip()) component->closePluginEditorForScenario();
    }

    bool hasOpenEditor() const override
    {
        auto* component = strip();
        return component != nullptr && component->hasOpenPluginEditorForScenario();
    }

    bool pluginWindowMissing() const override
    {
        auto* component = strip();
        return component != nullptr && component->pluginWindowMissingForScenario();
    }

    bool readEditorControl (const std::string& name, double& valueOut) const override
    {
        // The native LV2 editor drives the very instance the slot owns, so what
        // the plug-in's UI is bound to is that instance's port value.
        if (! hasOpenEditor()) return false;
       #if DUSKSTUDIO_HAS_NATIVE_LV2
        auto& strip = owner.engine.getChannelStrip (index);
        if (! strip.isNativeLv2Loaded()) return false;
        auto& slot = strip.getNativeLv2Slot();
        for (int i = 0; i < slot.paramCount(); ++i)
            if (const auto* info = slot.paramInfo (i); info != nullptr && info->name == name)
                return slot.getParamValue (info->id, valueOut);
       #else
        (void) name; (void) valueOut;
       #endif
        return false;
    }

    void clickMute() override
    {
        if (auto* component = strip()) component->clickMuteForScenario();
    }

    void clickSolo() override
    {
        if (auto* component = strip()) component->clickSoloForScenario();
    }

    ChannelStripComponent* strip() const
    {
        return owner.consoleView != nullptr ? owner.consoleView->getStripComponent (index)
                                            : nullptr;
    }

    MainComponent& owner;
    const int index;
};

struct MainComponent::ScenarioAuxLaneHandle final : scenario::AuxLaneHandle
{
    ScenarioAuxLaneHandle (MainComponent& ownerIn, int laneIn)
        : owner (ownerIn), lane (laneIn) {}

    bool loadNativeClap (int slot, const std::filesystem::path& file,
                         const std::string& pluginId) override
    {
       #if DUSKSTUDIO_HAS_NATIVE_CLAP
        auto* component = laneComponent();
        return component != nullptr
            && component->loadNativeClapForSlotForScenario (slot, file, pluginId);
       #else
        (void) slot; (void) file; (void) pluginId;
        return false;
       #endif
    }

    // The lane's own remove path: it detaches the editors and suspends audio
    // around the teardown, clears the slot's references and rebuilds, one
    // message-loop tick later.
    void unloadSlot (int slot) override
    {
        if (slot < 0 || slot >= AuxLaneParams::kMaxLanePlugins) return;
        if (auto* component = laneComponent()) component->unloadSlotForScenario (slot);
    }

    void rebuildSlots() override
    {
        if (auto* component = laneComponent()) component->rebuildSlotsForScenario();
    }

    bool attachEditor (int slot) override
    {
        auto* component = laneComponent();
        return component != nullptr && component->attachEditorForSlotForScenario (slot);
    }

    AuxLaneComponent* laneComponent() const
    {
        return owner.auxView != nullptr ? owner.auxView->getLaneComponent (lane) : nullptr;
    }

    MainComponent& owner;
    const int lane;
};

struct MainComponent::ScenarioGuiHost final : scenario::GuiHost
{
    explicit ScenarioGuiHost (MainComponent& ownerIn) : owner (ownerIn) {}

    bool openAudioEditor (int track, int region) override
    {
        if (owner.audioEditor != nullptr || owner.pianoRoll != nullptr) return false;
        owner.openAudioEditor (track, region);
        return owner.audioEditor != nullptr;
    }
    void closeAudioEditor() override { owner.closeAudioEditor(); }
    std::vector<double> audioEditorView() const override
    { return owner.audioEditor != nullptr ? owner.audioEditor->viewForScenario() : std::vector<double> {}; }
    bool clickAudioEditorButton (const std::string& name) override
    {
        if (owner.audioEditor == nullptr) return false;
        for (auto* child : owner.audioEditor->getChildren())
            if (child->isShowing() && child->isEnabled() && child->getName().toStdString() == name)
            {
                const auto point = owner.getTopLevelComponent()->getLocalPoint (child, child->getLocalBounds().getCentre()).toFloat();
                return pointerAt (point.x, point.y, true) && pointerAt (point.x, point.y, false);
            }
        return false;
    }
    bool clickAudioEditorSample (std::int64_t sample) override
    {
        auto* editor = owner.audioEditor.get();
        if (editor == nullptr) return false;
        const auto point = owner.getTopLevelComponent()->getLocalPoint (editor, editor->samplePointForScenario (sample)).toFloat();
        return pointerAt (point.x, point.y, true) && pointerAt (point.x, point.y, false);
    }
    std::vector<int> audioEditorPoint (const std::string& kind, std::int64_t sample) const override
    {
        if (owner.audioEditor == nullptr) return {};
        const auto p = owner.audioEditor->gesturePointForScenario (kind, sample);
        return { p.x, p.y };
    }
    std::vector<std::int64_t> audioEditorSelection() const override
    { return owner.audioEditor != nullptr ? owner.audioEditor->selectionForScenario() : std::vector<std::int64_t> {}; }
    bool audioEditorPointer (int x, int y, bool down, bool shift) override
    {
        auto* editor = owner.audioEditor.get();
        if (editor == nullptr) return false;
        const auto p = owner.getTopLevelComponent()->getLocalPoint (editor,
            editor->getLocalBounds().getTopLeft().translated (x, y)).toFloat();
        return pointerAt (p.x, p.y, down, shift ? 1 : 0);
    }
    bool openPiano (int track, int region) override
    {
        if (owner.pianoRoll != nullptr) return false;
        owner.openPianoRoll (track, region);
        return owner.pianoRoll != nullptr;
    }
    void closePiano() override { owner.closePianoRoll(); }
    bool pressPeerKey (const std::string& description, char text) override
    {
        auto* peer = owner.getPeer();
        if (peer == nullptr) return false;
        using Peer = std::remove_pointer_t<decltype (peer)>;
        return dispatchKey (*peer, &Peer::handleKeyPress, description, text);
    }
    bool pointerAt (float x, float y, bool down, int modifiers = 0)
    {
        auto* peer = owner.getPeer();
        if (peer == nullptr) return false;
        using Peer = std::remove_pointer_t<decltype (peer)>;
        const auto time = std::chrono::duration_cast<std::chrono::milliseconds> (
            std::chrono::system_clock::now().time_since_epoch()).count();
        dispatchMouseButton (*peer, &Peer::handleMouseEvent, x, y, down, time, modifiers);
        return true;
    }
    bool pianoPointer (int x, int y, bool down, int modifiers = 0)
    {
        auto* editor = owner.pianoRoll.get();
        if (editor == nullptr) return false;
        const auto point = owner.getTopLevelComponent()->getLocalPoint (editor,
            editor->getLocalBounds().getTopLeft().translated (x, y)).toFloat();
        return pointerAt (point.x, point.y, down, modifiers);
    }
    bool setTimelineShown (bool shown) override
    {
        const bool original = owner.tapeStripExpanded;
        owner.setTimelineVisible (shown);
        return original;
    }
    bool tapeRulerPointer (float fraction, bool down, bool shift) override
    {
        auto* tape = owner.tapeStrip.get();
        if (tape == nullptr || ! tape->isShowing()) return false;
        const auto point = owner.getTopLevelComponent()->getLocalPoint (tape, tape->rulerPointForScenario (fraction)).toFloat();
        return pointerAt (point.x, point.y, down, shift ? 1 : 0);
    }
    std::int64_t tapeRulerSample (float fraction) const override
    {
        return owner.tapeStrip != nullptr ? owner.tapeStrip->rulerSampleForScenario (fraction) : -1;
    }
    bool clickContextMenuItem (const std::string& text) override
    {
        int x = 0, y = 0;
        if (! contextMenuItemPointForScenario (text, x, y)) return false;
        auto* body = EmbeddedModal::activeModalStack().back()->getBody();
        const auto point = owner.getTopLevelComponent()->getLocalPoint (body,
            body->getLocalBounds().getTopLeft().translated (x, y)).toFloat();
        return pointerAt (point.x, point.y, true) && pointerAt (point.x, point.y, false);
    }
    bool dropFilesOnTrack (int track, const std::vector<std::filesystem::path>& files) override
    {
        auto* tape = owner.tapeStrip.get();
        if (tape == nullptr || ! tape->isShowing()) return false;
        const auto point = tape->dropPointForScenario (track);
        if (! tape->getLocalBounds().contains (point)) return false;
        return dispatchFileDrop (*tape, &TapeStrip::filesDropped, files, point.x, point.y);
    }
    std::vector<std::string> confirmationText() const override { return confirmationTextForScenario(); }
    bool captureMiniMarkers (bool enabled) override
    {
        if (owner.miniTimeline == nullptr) return false;
        owner.miniTimeline->captureMarkerPaintForScenario (enabled);
        return true;
    }
    std::vector<scenario::MiniMarkerPaint> miniMarkerPaint() const override
    {
        return owner.miniTimeline != nullptr ? owner.miniTimeline->markerPaintForScenario()
                                            : std::vector<scenario::MiniMarkerPaint> {};
    }
    bool clickMiniSample (std::int64_t sample) override
    {
        auto* mini = owner.miniTimeline.get();
        if (mini == nullptr || ! mini->isShowing()) return false;
        const auto point = owner.getTopLevelComponent()->getLocalPoint (mini, mini->samplePointForScenario (sample)).toFloat();
        return pointerAt (point.x, point.y, true) && pointerAt (point.x, point.y, false);
    }
    bool clickMiniMarker (int index) override
    {
        auto* mini = owner.miniTimeline.get();
        if (mini == nullptr || ! mini->isShowing()) return false;
        const auto& rows = mini->markerPaintForScenario();
        if (index < 0 || index >= (int) rows.size() || rows[(size_t) index].labelWidth <= 0) return false;
        const auto& row = rows[(size_t) index];
        const auto point = owner.getTopLevelComponent()->getLocalPoint (mini,
            mini->getLocalBounds().getTopLeft().translated (row.labelX + row.labelWidth / 2, row.labelY)).toFloat();
        return pointerAt (point.x, point.y, true) && pointerAt (point.x, point.y, false);
    }
    void refreshMasteringSource() override
    {
        if (owner.masteringView != nullptr) owner.masteringView->refreshSourceForScenario();
    }
    bool clickMasteringButton (const std::string& label) override
    {
        if (owner.masteringView == nullptr) return false;
        for (auto* child : owner.masteringView->getChildren())
            if (auto* button = dynamic_cast<decltype (owner.recordingStageBtn)*> (child);
                button != nullptr && button->isShowing() && button->isEnabled()
                && button->getButtonText().toStdString() == label)
            {
                const auto point = owner.getTopLevelComponent()->getLocalPoint (
                    button, button->getLocalBounds().getCentre()).toFloat();
                return pointerAt (point.x, point.y, true) && pointerAt (point.x, point.y, false);
            }
        return false;
    }
    bool clickFileMenu() override
    {
        if (! owner.menuBar.isShowing()) return false;
        const auto point = owner.getTopLevelComponent()->getLocalPoint (&owner.menuBar,
            owner.menuBar.getLocalBounds().getTopLeft().translated (20, owner.menuBar.getHeight() / 2)).toFloat();
        return pointerAt (point.x, point.y, true) && pointerAt (point.x, point.y, false);
    }
    bool focusFileName() override
    {
        const auto& stack = EmbeddedModal::activeModalStack();
        if (stack.empty() || stack.back()->getBody() == nullptr) return false;
        for (auto* container : stack.back()->getBody()->getChildren())
            for (auto* child : container->getChildren())
                if (auto* editor = dynamic_cast<decltype (owner.statusLabel.getCurrentTextEditor())> (child);
                    editor != nullptr && editor->isShowing() && ! editor->isReadOnly())
                {
                    const auto point = owner.getTopLevelComponent()->getLocalPoint (
                        editor, editor->getLocalBounds().getCentre()).toFloat();
                    return pointerAt (point.x, point.y, true) && pointerAt (point.x, point.y, false);
                }
        return false;
    }
    bool clickModalButton (const std::string& label) override
    {
        const auto& stack = EmbeddedModal::activeModalStack();
        if (stack.empty() || stack.back()->getBody() == nullptr) return false;
        for (auto* child : stack.back()->getBody()->getChildren())
            if (auto* button = dynamic_cast<decltype (owner.recordingStageBtn)*> (child);
                button != nullptr && button->isShowing() && button->isEnabled()
                && button->getButtonText().toStdString() == label)
            {
                const auto point = owner.getTopLevelComponent()->getLocalPoint (
                    button, button->getLocalBounds().getCentre()).toFloat();
                return pointerAt (point.x, point.y, true) && pointerAt (point.x, point.y, false);
            }
        return false;
    }
    std::vector<std::string> multiImportRows() const override
    {
        const auto& stack = EmbeddedModal::activeModalStack();
        if (stack.empty()) return {};
        const auto* picker = dynamic_cast<const MultiImportTargetPicker*> (stack.back()->getBody());
        return picker != nullptr ? picker->rowsForScenario() : std::vector<std::string> {};
    }
    bool clickMultiImportTarget (int row) override
    {
        const auto& stack = EmbeddedModal::activeModalStack();
        if (stack.empty()) return false;
        const auto* picker = dynamic_cast<const MultiImportTargetPicker*> (stack.back()->getBody());
        int x = 0, y = 0;
        if (picker == nullptr || ! picker->targetPointForScenario (row, x, y)) return false;
        const auto point = owner.getTopLevelComponent()->getLocalPoint (picker,
            picker->getLocalBounds().getTopLeft().translated (x, y)).toFloat();
        return pointerAt (point.x, point.y, true) && pointerAt (point.x, point.y, false);
    }
    std::vector<std::string> dpImportSummary() const override
    {
        const auto& stack = EmbeddedModal::activeModalStack();
        if (stack.empty()) return {};
        const auto* dialog = dynamic_cast<const DpImportDialog*> (stack.back()->getBody());
        return dialog != nullptr ? dialog->summaryForScenario() : std::vector<std::string> {};
    }
    bool clickPianoCcToggle() override
    {
        if (owner.pianoRoll == nullptr) return false;
        const auto point = owner.pianoRoll->ccTogglePointForScenario();
        return pianoPointer (point.x, point.y, true) && pianoPointer (point.x, point.y, false);
    }
    bool pianoCcPointer (std::int64_t tick, int value, bool down) override
    {
        if (owner.pianoRoll == nullptr) return false;
        const auto point = owner.pianoRoll->ccPointForScenario (tick, value);
        return pianoPointer (point.x, point.y, down);
    }
    bool pianoNotePointer (std::int64_t tick, int pitch, bool down, int modifiers) override
    {
        if (owner.pianoRoll == nullptr) return false;
        const auto point = owner.pianoRoll->notePointForScenario (tick, pitch);
        return pianoPointer (point.x, point.y, down, modifiers);
    }
    std::vector<int> pianoSelection() const override
    {
        return owner.pianoRoll != nullptr ? owner.pianoRoll->selectionForScenario() : std::vector<int> {};
    }
    int pianoCcController() const override
    {
        return owner.pianoRoll != nullptr ? owner.pianoRoll->ccControllerForScenario() : -1;
    }

    bool clickTimeFormat() override
    {
        return owner.transportBar != nullptr && owner.transportBar->clickTimeFormatForScenario();
    }

    std::string clockText() const override
    {
        return owner.transportBar != nullptr ? owner.transportBar->clockTextForScenario() : std::string {};
    }

    void switchToStage (Stage stage) override
    {
        owner.switchToStage (stage == Stage::Recording ? AudioEngine::Stage::Recording
                             : stage == Stage::Aux ? AudioEngine::Stage::Aux
                             : stage == Stage::Mastering ? AudioEngine::Stage::Mastering : AudioEngine::Stage::Mixing);
    }

    scenario::StripHandle* strip (int index) override
    {
        if (index < 0 || index >= Session::kNumTracks) return nullptr;
        auto& handle = strips[(std::size_t) index];
        if (handle == nullptr)
            handle = std::make_unique<ScenarioStripHandle> (owner, index);
        return handle->strip() != nullptr ? handle.get() : nullptr;
    }

    scenario::AuxLaneHandle* auxLane (int index) override
    {
        if (index < 0 || index >= Session::kNumAuxLanes) return nullptr;
        auto& handle = lanes[(std::size_t) index];
        if (handle == nullptr)
            handle = std::make_unique<ScenarioAuxLaneHandle> (owner, index);
        return handle->laneComponent() != nullptr ? handle.get() : nullptr;
    }

    bool automationView (StripKind kind, int index,
                         std::string& label, bool& faderEnabled) override
    {
        const auto read = [&label, &faderEnabled] (const auto* component)
        {
            if (component == nullptr) return false;
            label = component->autoModeLabelForScenario();
            faderEnabled = component->faderEnabledForScenario();
            return true;
        };
        auto* console = owner.consoleView.get();
        switch (kind)
        {
            case StripKind::Channel:
                return read (console != nullptr ? console->getStripComponent (index) : nullptr);
            case StripKind::Bus:
                return read (console != nullptr ? console->getBusComponent (index) : nullptr);
            case StripKind::Master:
                return read (console != nullptr ? console->getMasterStripComponent() : nullptr);
            case StripKind::Aux:
                return read (owner.auxView != nullptr ? owner.auxView->getLaneComponent (index) : nullptr);
        }
        return false;
    }

    bool canEmbedPluginEditors() const override
    {
        auto* peer = owner.getPeer();
        return peer != nullptr && peer->getNativeHandle() != nullptr
            && platform::hasUsableDisplay();
    }

    bool modalStackEmpty() const override
    {
        return EmbeddedModal::activeModalStack().empty();
    }

    void closeTopModal() override
    {
        auto& stack = EmbeddedModal::activeModalStack();
        if (! stack.empty()) stack.back()->close();
    }

    void autosaveTick() override { owner.writeAutosave(); }

    bool openSession (const std::filesystem::path& sessionJson) override
    {
        return owner.loadSessionFromJson (hostFile (sessionJson));
    }

    bool answerRecovery (Recovery choice) override
    {
        return owner.answerRecoveryPrompt ((int) choice);
    }

    MainComponent& owner;
    std::array<std::unique_ptr<ScenarioStripHandle>, Session::kNumTracks> strips;
    std::array<std::unique_ptr<ScenarioAuxLaneHandle>, Session::kNumAuxLanes> lanes;
};

namespace
{
struct GuiRun
{
    // Held as the interface, not the nested implementation: this is namespace
    // scope, where that name is MainComponent's business alone.
    std::unique_ptr<scenario::GuiHost> host;
    std::unique_ptr<scenario::SuiteRunner> runner;
};

// One GUI suite per process - it ends by quitting the app - so a file-scope
// holder is all the ownership the run needs, and MainComponent stays free of a
// member only the harness would ever use.
std::unique_ptr<GuiRun>& activeGuiRun()
{
    static std::unique_ptr<GuiRun> run;
    return run;
}
} // namespace

void MainComponent::runGuiScenarios (std::string spec)
{
    auto& run = activeGuiRun();
    if (run != nullptr) return;

    run = std::make_unique<GuiRun>();
    run->host = std::make_unique<ScenarioGuiHost> (*this);
    run->runner = std::make_unique<scenario::SuiteRunner> (
        std::move (spec), *run->host, session, engine,
        [] (int exitCode)
        {
            // Set by the app before the window existed: the runner cannot reach
            // the application object from here without the GUI framework.
            if (auto& exitFn = scenario::guiSuiteExit()) exitFn (exitCode);
        });
    run->runner->start();
}
} // namespace duskstudio
