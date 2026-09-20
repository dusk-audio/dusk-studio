#include "MainComponent.h"

#include "AuxLaneComponent.h"
#include "AudioRegionEditor.h"
#include "DimOverlay.h"
#include "AuxView.h"
#include "BusComponent.h"
#include "ChannelStripComponent.h"
#include "ConsoleView.h"
#include "EmbeddedModal.h"
#include "GuiHost.h"
#include "MasterStripComponent.h"
#include "MasteringView.h"
#include "PianoRollComponent.h"
#include "PlatformWindowing.h"
#include "TransportBar.h"
#include "TapeStrip.h"
#include "../engine/scenario/SuiteRunner.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
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
                          float x, float y, bool down, std::int64_t time)
{
    (peer.*handler) (Source::mouse, Point (x, y), Modifiers (down ? Modifiers::leftButtonModifier : 0),
                    1.0f, 0.0f, time, {}, 0);
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

    bool clickArm() override
    {
        auto* component = strip();
        return component != nullptr && component->clickArmForScenario();
    }

    bool armLit() const override
    {
        auto* component = strip();
        return component != nullptr && component->armLitForScenario();
    }

    bool inputSettingsOpen() const override
    {
        auto* component = strip();
        return component != nullptr && component->inputSettingsOpenForScenario();
    }

    void loadBuiltin (const std::string& id) override
    {
        if (auto* component = strip()) component->loadBuiltinForScenario (id);
    }

    void clickMonitor() override
    {
        if (auto* component = strip()) component->clickMonitorForScenario();
    }

    void restoreTrackMode (int mode) override
    {
        if (auto* component = strip())
        {
            component->openIoConfigPopupForCapture (mode);
            component->closeIoConfigPopupForCapture();
        }
    }

    bool instrumentControlsMatch (int input, bool monitor) const override
    {
        auto* component = strip();
        return component != nullptr && component->instrumentControlsMatchForScenario (input, monitor);
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

    bool pressKey (const std::string& description, char text) override
    {
        return dispatchKey (owner, &MainComponent::keyPressed, description, text);
    }

    std::function<void()> preserveKeyboardFocus() override
    {
        const int focus = owner.consoleView->getFocusedStrip();
        const int page = owner.consoleView->getBank();
        const int selection = owner.tapeStrip->getSelectedTrack();
        return [this, focus, page, selection]
        {
            owner.consoleView->restoreFocusForScenario (focus);
            owner.consoleView->setBank (page);
            owner.tapeStrip->setSelectedTrack (selection);
        };
    }

    bool clickTimeFormat() override
    {
        return owner.transportBar != nullptr && owner.transportBar->clickTimeFormatForScenario();
    }

    bool timelineViewMatches (bool expanded) const override
    {
        return owner.tapeStrip != nullptr && owner.tapeStripExpanded == expanded
            && owner.tapeStrip->isShowing() == expanded;
    }

    bool stripCompact (int index) const override
    {
        auto* strip = owner.consoleView != nullptr ? owner.consoleView->getStripComponent (index) : nullptr;
        return strip != nullptr && strip->isCompactMode();
    }

    std::string clockText() const override
    {
        return owner.transportBar != nullptr ? owner.transportBar->clockTextForScenario() : std::string {};
    }

    void switchToStage (Stage stage) override
    {
        switch (stage)
        {
            case Stage::Recording: owner.switchToStage (AudioEngine::Stage::Recording); break;
            case Stage::Mixing: owner.switchToStage (AudioEngine::Stage::Mixing); break;
            case Stage::Aux: owner.switchToStage (AudioEngine::Stage::Aux); break;
            case Stage::Mastering: owner.switchToStage (AudioEngine::Stage::Mastering); break;
        }
    }

    bool clickStage (Stage stage) override
    {
        auto* button = &owner.recordingStageBtn;
        switch (stage)
        {
            case Stage::Recording: break;
            case Stage::Mixing: button = &owner.mixingStageBtn; break;
            case Stage::Aux: button = &owner.auxStageBtn; break;
            case Stage::Mastering: button = &owner.masteringStageBtn; break;
        }
        if (! button->isShowing()) return false;
        button->triggerClick();
        return true;
    }

    bool stageViewMatches (Stage stage) const override
    {
        const auto showing = [] (const auto& component)
        { return component != nullptr && component->isShowing(); };
        const bool console = stage == Stage::Recording || stage == Stage::Mixing;
        return showing (owner.consoleView) == console
            && showing (owner.transportBar) == console
            && showing (owner.auxView) == (stage == Stage::Aux)
            && showing (owner.masteringView) == (stage == Stage::Mastering)
            && owner.recordingStageBtn.getToggleState() == (stage == Stage::Recording)
            && owner.mixingStageBtn.getToggleState() == (stage == Stage::Mixing)
            && owner.auxStageBtn.getToggleState() == (stage == Stage::Aux)
            && owner.masteringStageBtn.getToggleState() == (stage == Stage::Mastering);
    }

    bool stripStageControlsMatch (int index, bool mixing) const override
    {
        auto* console = owner.consoleView.get();
        if (console == nullptr || index < 0 || index >= Session::kNumTracks) return false;
        const int page = console->getBank();
        const int activeBank = owner.session.activeBank.load();
        const int surfaceBank = owner.session.mcu.bank.load();
        console->setBank (index / std::max (1, console->bankStride()));
        const auto* component = console->getStripComponent (index);
        const bool matches = component != nullptr && component->stageControlsMatchForScenario (mixing);
        console->setBank (page);
        owner.session.activeBank.store (activeBank);
        owner.session.mcu.bank.store (surfaceBank);
        return matches;
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

    std::string modalText() const override
    {
        const auto& stack = EmbeddedModal::activeModalStack();
        if (stack.empty()) return {};
        const auto* body = stack.back()->getBody();
        return body == nullptr ? std::string {}
                               : body->getTitle().toStdString() + "\n" + body->getDescription().toStdString();
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
                button->triggerClick();
                return true;
            }
        return false;
    }

    void closeTopModal() override
    {
        auto& stack = EmbeddedModal::activeModalStack();
        if (! stack.empty()) stack.back()->close();
    }

    void autosaveTick() override { owner.writeAutosave(); }
    void openAbout() override { owner.menuItemSelected (2002, 2); }
    void startMixdown() override { owner.menuItemSelected (1010, 0); }

    bool clickAt (float x, float y, int count)
    {
        auto* peer = owner.getPeer();
        if (peer == nullptr) return false;
        using Peer = std::remove_pointer_t<decltype (peer)>;
        const auto time = std::chrono::duration_cast<std::chrono::milliseconds> (
            std::chrono::system_clock::now().time_since_epoch()).count();
        for (int click = 0; click < count; ++click)
        {
            dispatchMouseButton (*peer, &Peer::handleMouseEvent, x, y, true, time + click * 40);
            dispatchMouseButton (*peer, &Peer::handleMouseEvent, x, y, false, time + click * 40 + 20);
        }
        return true;
    }

    bool doubleClickAudioRegion (int track, int region) override
    {
        if (owner.tapeStrip == nullptr || ! owner.tapeStrip->isShowing()) return false;
        const auto bounds = owner.tapeStrip->audioRegionScreenRect (track, region);
        if (bounds.isEmpty()) return false;
        const auto point = owner.getTopLevelComponent()->getLocalPoint (owner.tapeStrip.get(), bounds.getCentre()).toFloat();
        return clickAt (point.x, point.y, 2);
    }

    bool audioEditorOpen() const override { return owner.audioEditor != nullptr; }
    void closeAudioEditor() override { owner.closeAudioEditor(); }

    bool pressAudioEditorKey (const std::string& description) override
    {
        return owner.audioEditor != nullptr && owner.audioEditor->isShowing()
            && dispatchKey (*owner.audioEditor, &AudioRegionEditor::keyPressed, description, 0);
    }

    bool clickOutsideAudioEditor() override
    {
        if (owner.audioEditorDim == nullptr) return false;
        const auto point = owner.getTopLevelComponent()->getLocalPoint (
            owner.audioEditorDim.get(), owner.audioEditorDim->getLocalBounds().getTopLeft()).toFloat();
        return clickAt (point.x + 2.0f, point.y + 2.0f, 1);
    }
    void openPianoRoll (int track, int region) override { owner.openPianoRoll (track, region); }
    void closePianoRoll() override { owner.closePianoRoll(); }

    bool pressPianoRollKey (const std::string& description) override
    {
        return owner.pianoRoll != nullptr && owner.pianoRoll->isShowing()
            && dispatchKey (*owner.pianoRoll, &PianoRollComponent::keyPressed, description, 0);
    }

    bool loadMasteringFile (const std::filesystem::path& path) override
    {
        return owner.masteringView != nullptr && owner.masteringView->loadFile (hostFile (path));
    }

    bool clickMasteringButton (const std::string& label) override
    {
        if (owner.masteringView == nullptr) return false;
        for (auto* child : owner.masteringView->getChildren())
            if (auto* button = dynamic_cast<decltype (owner.recordingStageBtn)*> (child);
                button != nullptr && button->isShowing() && button->isEnabled()
                && button->getButtonText().toStdString() == label)
            {
                button->triggerClick();
                return true;
            }
        return false;
    }

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
