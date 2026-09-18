#include "MainComponent.h"

#include "AuxLaneComponent.h"
#include "AuxView.h"
#include "ChannelStripComponent.h"
#include "ConsoleView.h"
#include "EmbeddedModal.h"
#include "GuiHost.h"
#include "PlatformWindowing.h"
#include "../engine/scenario/SuiteRunner.h"

#include <array>
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

    void switchToStage (Stage stage) override
    {
        owner.switchToStage (stage == Stage::Aux ? AudioEngine::Stage::Aux
                                                 : AudioEngine::Stage::Mixing);
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
