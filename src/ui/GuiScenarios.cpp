#include "MainComponent.h"
#include "BounceDialog.h"
#if DUSKSTUDIO_HAS_NATIVE_UI
 #include "imgui/DuskPanelWindow.h"
#endif

#include "AuxLaneComponent.h"
#include "AuxView.h"
#include "AudioRegionEditor.h"
#include "PianoRollComponent.h"
#include "PluginPickerPanel.h"
#include "TapeStrip.h"
#include "BusComponent.h"
#include "ChannelStripComponent.h"
#include "ConsoleView.h"
#include "EmbeddedModal.h"
#include "GuiHost.h"
#include "MasterStripComponent.h"
#include "MasteringView.h"
#include "PlatformWindowing.h"
#include "NativeEditorEmbedScale.h"
#include "TransportBar.h"
#include "../engine/scenario/SuiteRunner.h"

#include <array>
#include <cstddef>
#include <chrono>
#include <cstdint>
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
                          float x, float y, bool down, std::int64_t time, bool right = false)
{
    const int flags = down ? (right ? Modifiers::rightButtonModifier : Modifiers::leftButtonModifier) : 0;
    (peer.*handler) (Source::mouse, Point (x, y) * peer.getComponent().getDesktopScaleFactor(), Modifiers (flags),
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

    bool captureSources (bool enabled) override
    {
        auto* component = laneComponent();
        return component != nullptr && component->captureSourcesForScenario (enabled);
    }

    std::vector<std::string> sourceRows() const override
    {
        auto* component = laneComponent();
        return component != nullptr ? component->sourceRowsForScenario() : std::vector<std::string>();
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

    bool pressPeerKey (const std::string& description, char text) override
    {
        auto* peer = owner.getPeer();
        if (peer == nullptr) return false;
        using Peer = std::remove_pointer_t<decltype (peer)>;
        return dispatchKey (*peer, &Peer::handleKeyPress, description, text);
    }

    bool clickAt (float x, float y, int count, bool right = false)
    {
        auto* peer = owner.getPeer();
        if (peer == nullptr) return false;
        using Peer = std::remove_pointer_t<decltype (peer)>;
        const auto time = std::chrono::duration_cast<std::chrono::milliseconds> (
            std::chrono::system_clock::now().time_since_epoch()).count();
        for (int click = 0; click < count; ++click)
        {
            dispatchMouseButton (*peer, &Peer::handleMouseEvent, x, y, true, time + click * 40, right);
            dispatchMouseButton (*peer, &Peer::handleMouseEvent, x, y, false, time + click * 40 + 20, right);
        }
        return true;
    }

    bool clickModalAt (float xFraction, float yFraction) override
    {
        const auto& stack = EmbeddedModal::activeModalStack();
        if (stack.empty() || stack.back()->getBody() == nullptr) return false;
        auto* body = stack.back()->getBody();
        const auto local = body->getLocalBounds().getRelativePoint (xFraction, yFraction);
        const auto point = owner.getTopLevelComponent()->getLocalPoint (body, local).toFloat();
        return clickAt (point.x, point.y, 1);
    }

    bool clickFader (int index, bool readout, bool right) override
    {
        auto* strip = owner.consoleView->getStripComponent (index);
        if (strip == nullptr || ! strip->isShowing()) return false;
        const auto point = owner.getTopLevelComponent()->getLocalPoint (strip, strip->faderPointForScenario (readout)).toFloat();
        return clickAt (point.x, point.y, 1, right);
    }

    bool faderEditing (int index) const override
    { return owner.consoleView->getStripComponent (index)->faderEditingForScenario(); }
    double faderValue (int index) const override
    { return owner.consoleView->getStripComponent (index)->faderValueForScenario(); }

    bool virtualKeyboardOpen() const override
    {
       #if DUSKSTUDIO_HAS_NATIVE_UI
        return owner.virtualKeyboardWindow != nullptr && owner.virtualKeyboardWindow->isOpen();
       #else
        return false;
       #endif
    }
    bool inputVirtualKeyboard (const std::string& key) override
    {
       #if DUSKSTUDIO_HAS_NATIVE_UI
        return virtualKeyboardOpen() && owner.virtualKeyboardWindow->inputForScenario (key);
       #else
        return false;
       #endif
    }
    void closeVirtualKeyboard() override { owner.closeVirtualKeyboard(); }

    bool openAudioSettings() override { owner.openAudioSettings(); return audioSettingsOpen(); }
    void closeAudioSettings() override { owner.closeAudioSettings(); }
    bool audioSettingsOpen() const override
    {
       #if DUSKSTUDIO_HAS_NATIVE_UI
        return owner.audioSettingsWindow != nullptr && owner.audioSettingsWindow->isOpen();
       #else
        return false;
       #endif
    }
    bool clickAudioSettingsControl (const std::string& control) override
    {
       #if DUSKSTUDIO_HAS_NATIVE_UI
        return audioSettingsOpen() && owner.audioSettingsWindow->clickControlForScenario (control);
       #else
        (void) control;
        return false;
       #endif
    }
    bool inputAudioSettings (const std::string& input) override
    {
       #if DUSKSTUDIO_HAS_NATIVE_UI
        return audioSettingsOpen() && owner.audioSettingsWindow->inputForScenario (input);
       #else
        (void) input;
        return false;
       #endif
    }
    bool pointerAudioSettings (const std::string& control, float position, bool pressed) override
    {
       #if DUSKSTUDIO_HAS_NATIVE_UI
        return audioSettingsOpen()
            && owner.audioSettingsWindow->pointerControlForScenario (control, position, pressed);
       #else
        (void) control;
        (void) position;
        (void) pressed;
        return false;
       #endif
    }
    double uiScale() const override { return embedscale::globalScale(); }
    void restoreUiScale (float scale) override { owner.restoreUiScaleForScenario (scale); }
    int tapeExpansionState() const override
    {
        const bool displayed = owner.tapeStrip->isVisible() && ! owner.tapeStrip->getBounds().isEmpty();
        return (owner.tapeStripExpanded ? 2 : 0) + (displayed ? 1 : 0);
    }
    int timelineChaseState() const override
    { return (owner.hdrChaseBtn.getToggleState() ? 2 : 0) + (owner.tapeStrip->isChaseEnabled() ? 1 : 0); }
    bool openRegionEditor (int track, int region, bool midi) override
    {
        if (midi) { owner.openPianoRoll (track, region); return owner.pianoRoll != nullptr; }
        owner.openAudioEditor (track, region);
        return owner.audioEditor != nullptr;
    }
    int regionEditorChase() const override
    {
        const auto read = [this] (const auto* editor)
        {
            if (editor == nullptr) return -1;
            for (auto* child : editor->getChildren())
                if (const auto* button = dynamic_cast<const decltype (owner.hdrChaseBtn)*> (child))
                    if (button->getButtonText() == "Chase") return button->getToggleState() ? 1 : 0;
            return -1;
        };
        return owner.pianoRoll != nullptr ? read (owner.pianoRoll.get()) : read (owner.audioEditor.get());
    }
    void closeRegionEditors() override { owner.closePianoRoll(); owner.closeAudioEditor(); }
    bool clickInsert (int track) override
    {
        auto* strip = owner.consoleView->getStripComponent (track);
        if (strip == nullptr || ! strip->isShowing()) return false;
        const auto point = owner.getTopLevelComponent()->getLocalPoint (strip, strip->insertPointForScenario()).toFloat();
        return clickAt (point.x, point.y, 1);
    }
    std::vector<std::string> pickerRows (bool headers) const override
    {
        std::vector<std::string> rows;
        const auto& stack = EmbeddedModal::activeModalStack();
        for (const auto* modal : stack)
            if (const auto* picker = dynamic_cast<PluginPickerPanel*> (modal->getBody()))
                for (const auto& row : picker->rowsForScenario())
                    if (row.header == headers) rows.push_back (row.text);
        return rows;
    }
    bool clickPickerRow (const std::string& text) override
    {
        const auto& stack = EmbeddedModal::activeModalStack();
        if (stack.empty()) return false;
        if (auto* picker = dynamic_cast<PluginPickerPanel*> (stack.back()->getBody()))
            for (const auto& row : picker->rowsForScenario())
                if (! row.header && row.text == text)
                {
                    auto local = picker->getLocalBounds().getCentre();
                    local.setXY (row.x, row.y);
                    const auto point = owner.getTopLevelComponent()->getLocalPoint (picker, local).toFloat();
                    return clickAt (point.x, point.y, 1);
                }
        return false;
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
                    return clickAt (point.x, point.y, 1);
                }
        return false;
    }
    bool midiBindingsOpen() const override { return owner.midiBindingsModal.isOpen(); }

    bool openMidiIo (int index) override
    { return owner.consoleView->getStripComponent (index)->openIoConfigPopupForCapture ((int) Track::Mode::Midi) != nullptr; }
    bool clickMidiSelector (int index, int kind) override
    {
        auto* combo = owner.consoleView->getStripComponent (index)->midiSelectorForScenario (kind);
        if (! combo->isShowing()) return false;
        const auto point = owner.getTopLevelComponent()->getLocalPoint (combo, combo->getLocalBounds().getCentre()).toFloat();
        return clickAt (point.x, point.y, 1);
    }
    std::string midiSelectorText (int index, int kind) const override
    { return owner.consoleView->getStripComponent (index)->midiSelectorForScenario (kind)->getText().toStdString(); }

    bool meterClip (int index) override
    { return owner.consoleView->getStripComponent (index)->meterClipForScenario(); }

    bool groupChipView (int index, std::string& text, int& master, bool& filled) override
    {
        auto* strip = owner.consoleView->getStripComponent (index);
        return strip != nullptr && strip->groupChipViewForScenario (text, master, filled);
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

    bool clickMasteringTarget() override
    {
        auto* view = owner.masteringView.get();
        if (view == nullptr || ! view->isShowing()) return false;
        const auto point = owner.getTopLevelComponent()->getLocalPoint (view, view->targetPointForScenario()).toFloat();
        return clickAt (point.x, point.y, 1);
    }
    std::string masteringTargetText() const override
    {
        return owner.masteringView != nullptr ? owner.masteringView->targetTextForScenario() : std::string();
    }
    std::uint32_t masteringLoudnessColour (bool peak) const override
    {
        return owner.masteringView != nullptr ? owner.masteringView->loudnessColourForScenario (peak) : 0;
    }
    void restoreMasteringTarget (int index) override
    {
        if (owner.masteringView != nullptr) owner.masteringView->restoreTargetForScenario (index);
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
                button->triggerClick();
                return true;
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

    void startMixdown() override { owner.menuItemSelected (1010, 0); }
    bool mixdownRunning() const override
    {
        const auto* panel = dynamic_cast<BounceDialog*> (owner.mixdownModal.getBody());
        return panel != nullptr && panel->isRenderingForScenario();
    }
    std::string statusMessage() const override { return owner.statusLabel.getText().toStdString(); }

    void requestSessionSwitch (const std::filesystem::path& sessionJson) override
    {
        owner.openSessionPath (hostFile (sessionJson));
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
