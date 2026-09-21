#include "MainComponent.h"
#include "BounceDialog.h"
#if DUSKSTUDIO_HAS_NATIVE_UI
 #include "imgui/DuskPanelWindow.h"
#endif

#include "AuxLaneComponent.h"
#include "AudioRegionEditor.h"
#include "DimOverlay.h"
#include "AuxView.h"
#include "PianoRollComponent.h"
#include "PluginPickerPanel.h"
#include "TapeStrip.h"
#include "BusComponent.h"
#include "ChannelStripComponent.h"
#include "ConsoleView.h"
#include "EmbeddedModal.h"
#include "DuskContextMenu.h"
#include "DpImportDialog.h"
#include "MultiImportTargetPicker.h"
#include "DuskAlerts.h"
#include "MiniTimelineStrip.h"
#include "GuiHost.h"
#include "MasterStripComponent.h"
#include "MasteringView.h"
#include "PlatformWindowing.h"
#include "NativeEditorEmbedScale.h"
#include "SystemStatusBar.h"
#include "TransportBar.h"
#include "../engine/scenario/ScenarioContext.h"
#include "../engine/scenario/SuiteRunner.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
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

const char* stageName (AudioEngine::Stage stage)
{
    switch (stage)
    {
        case AudioEngine::Stage::Recording: return "Recording";
        case AudioEngine::Stage::Mixing:    return "Mixing";
        case AudioEngine::Stage::Aux:       return "Aux";
        case AudioEngine::Stage::Mastering: return "Mastering";
    }
    return "unknown";
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
    const int flags = (down ? ((modifiers & 4) != 0 ? Modifiers::rightButtonModifier : Modifiers::leftButtonModifier) : 0)
                    | ((modifiers & 1) != 0 ? Modifiers::shiftModifier : 0)
                    | ((modifiers & 2) != 0 ? Modifiers::commandModifier : 0);
    const auto saved = Modifiers::currentModifiers;
    Modifiers::currentModifiers = Modifiers (flags);
    (peer.*handler) (Source::mouse, Point (x, y) * peer.getComponent().getDesktopScaleFactor(), Modifiers (flags),
                    1.0f, 0.0f, time, {}, 0);
    Modifiers::currentModifiers = saved;
}

template <typename Peer, typename Source, typename Point, typename Modifiers, typename... Rest,
          typename Time, typename Wheel>
void dispatchMouseWheel (Peer& peer, void (Peer::*mouse) (Source, Point, Modifiers, Rest...),
                         void (Peer::*wheel) (Source, Point, Time, const Wheel&, int),
                         float x, float y, float delta, bool command, bool shift, std::int64_t time)
{
    const auto point = Point (x, y) * peer.getComponent().getDesktopScaleFactor();
    const int flags = (command ? Modifiers::commandModifier : 0) | (shift ? Modifiers::shiftModifier : 0);
    const auto saved = Modifiers::currentModifiers;
    Modifiers::currentModifiers = Modifiers (flags);
    (peer.*mouse) (Source::mouse, point, Modifiers (flags), 1.0f, 0.0f, time, {}, 0);
    (peer.*wheel) (Source::mouse, point, static_cast<Time> (time), Wheel { 0.0f, delta, false, false, false }, 0);
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

template <typename Peer, typename Source, typename Point, typename Time, typename Wheel>
void dispatchWheel (Peer& peer, void (Peer::*handler) (Source, Point, Time, const Wheel&, int),
                    float x, float y, std::int64_t time, float delta)
{
    Wheel wheel {};
    wheel.deltaY = delta;
    (peer.*handler) (Source::mouse, Point (x, y) * peer.getComponent().getDesktopScaleFactor(), time, wheel, 0);
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

    std::string insertLabel() const override
    {
        auto* component = strip();
        return component != nullptr ? component->insertLabelForScenario() : std::string();
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

    bool openInputSettings (int mode) override
    {
        auto* component = strip();
        return component != nullptr && component->openIoConfigPopupForCapture (mode) != nullptr;
    }

    void loadBuiltin (const std::string& id) override
    {
        if (auto* component = strip()) component->loadBuiltinForScenario (id);
    }

    bool midiActivityVisible() const override
    { auto* component = strip(); return component != nullptr && component->midiActivityVisibleForScenario(); }
    bool midiActivityLit() const override
    { auto* component = strip(); return component != nullptr && component->midiActivityLitForScenario(); }

    void clickMonitor() override
    {
        if (auto* component = strip()) component->clickMonitorForScenario();
    }
    void clickAutomationMode() override
    {
        if (auto* component = strip()) component->clickAutoModeForScenario();
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

    void refreshSlot (int slot) override
    {
        if (auto* component = laneComponent()) component->refreshSlotForScenario (slot);
    }

    std::string slotLabel (int slot) const override
    {
        auto* component = laneComponent();
        return component != nullptr ? component->slotLabelForScenario (slot) : std::string();
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
    explicit ScenarioGuiHost (MainComponent& ownerIn) : owner (ownerIn)
    {
        const auto check = [this] (bool passed, const char* error)
        {
            if (! passed) startupErrors.emplace_back (error);
        };
        check (owner.session.getSessionDirectory().getFileName().toStdString() == "Untitled",
               "first launch did not create an Untitled session");
        check (timelineViewMatches (false), "the first-launch tape strip was not collapsed");
        check (stageViewMatches (Stage::Recording), "first launch did not show Recording");
        check (owner.engine.getTransport().isStopped(), "first launch started the transport");
        for (int index = 0; index < Session::kNumTracks; ++index)
        {
            const auto& track = owner.session.track (index);
            check (track.regions.empty() && track.midiRegions.current().empty(),
                   "first launch contained an audio or MIDI region");
            check (! track.recordArmed.load(), "first launch armed a track");
            check (strip (index) != nullptr, "first launch omitted a channel strip");
            launch.trackMode[(std::size_t) index] = track.mode.load();
        }

        launch.uiScale = uiScale();
        launch.tapeExpanded = owner.tapeStripExpanded;
        launch.sessionDir = scenario::currentSessionDirectory (owner.session);
        launch.masterMute = owner.session.master().mute.load();
        launch.masteringSource = owner.session.mastering().sourceFile.getFullPathName().toStdString();
        if (owner.tapeStrip != nullptr)
        {
            launch.tapeChase = owner.tapeStrip->isChaseEnabled();
            launch.tapeShowAll = owner.tapeStrip->showsAllTracksForScenario();
            launch.tapeSelectedTrack = owner.tapeStrip->getSelectedTrack();
        }
        if (owner.consoleView != nullptr)
        {
            launch.consoleBank = owner.consoleView->getBank();
            launch.consoleFocus = owner.consoleView->getFocusedStrip();
        }
        if (auto* peer = owner.getPeer())
        {
            const auto bounds = peer->getBounds();
            launch.window = { bounds.getX(), bounds.getY(), bounds.getWidth(), bounds.getHeight() };
        }
    }

    const std::vector<std::string>& firstLaunchErrors() const override { return startupErrors; }

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

    bool builtinPointer (int track, const std::string& control, float position, bool pressed) override
    {
        auto* strip = owner.consoleView->getStripComponent (track);
        return strip != nullptr && strip->builtinPointerForScenario (control, position, pressed);
    }
    void closeBuiltin (int track) override
    {
        if (auto* strip = owner.consoleView->getStripComponent (track)) strip->closeBuiltinForScenario();
    }
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
        std::vector<decltype (owner.audioEditor->getChildComponent (0))> pending;
        pending.push_back (owner.audioEditor.get());
        while (! pending.empty())
        {
            auto* child = pending.back();
            pending.pop_back();
            for (auto* nested : child->getChildren()) pending.push_back (nested);
            if (child->isShowing() && child->isEnabled() && child->getName().toStdString() == name)
            {
                const auto point = owner.getTopLevelComponent()->getLocalPoint (child, child->getLocalBounds().getCentre()).toFloat();
                return pointerAt (point.x, point.y, true) && pointerAt (point.x, point.y, false);
            }
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
    bool audioEditorPointer (int x, int y, bool down, int modifiers) override
    {
        auto* editor = owner.audioEditor.get();
        if (editor == nullptr) return false;
        const auto p = owner.getTopLevelComponent()->getLocalPoint (editor,
            editor->getLocalBounds().getTopLeft().translated (x, y)).toFloat();
        return pointerAt (p.x, p.y, down, modifiers);
    }
    std::vector<int> audioAutomationPoint (std::int64_t sample, float value) const override
    {
        if (owner.audioEditor == nullptr) return {};
        const auto p = owner.audioEditor->automationPointForScenario (sample, value);
        return { p.x, p.y };
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
    std::vector<double> tapeView() const override
    {
        if (owner.tapeStrip == nullptr) return {};
        const auto v = owner.tapeStrip->viewForScenario();
        return { v[0], v[1], v[2], owner.tapeStripExpanded ? 1.0 : 0.0, v[3], v[4], v[5] };
    }
    void restoreTapeView (const std::vector<double>& view) override
    {
        if (owner.tapeStrip == nullptr || view.size() != 7) return;
        // Through the header button so the strip and the lit toggle agree.
        if (owner.hdrChaseBtn.getToggleState() != (view[6] > 0.5)) owner.hdrChaseBtn.triggerClick();
        owner.tapeStrip->restoreViewForScenario (view);
    }
    bool tapeWheel (float fraction, float delta, bool command, bool shift) override
    {
        auto* tape = owner.tapeStrip.get();
        auto* peer = owner.getPeer();
        if (tape == nullptr || peer == nullptr || ! tape->isShowing()) return false;
        const auto point = owner.getTopLevelComponent()->getLocalPoint (tape, tape->rulerPointForScenario (fraction)).toFloat();
        using Peer = std::remove_pointer_t<decltype (peer)>;
        const auto time = std::chrono::duration_cast<std::chrono::milliseconds> (
            std::chrono::system_clock::now().time_since_epoch()).count();
        dispatchMouseWheel (*peer, &Peer::handleMouseEvent, &Peer::handleMouseWheel,
                            point.x, point.y, delta, command, shift, time);
        return true;
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
    bool clickFileBrowserControl (bool path) override
    {
        const auto& stack = EmbeddedModal::activeModalStack();
        if (stack.empty() || stack.back()->getBody() == nullptr) return false;
        for (auto* container : stack.back()->getBody()->getChildren())
            for (auto* child : container->getChildren())
                if (child->isShowing() && (path ? child->getName() == "path" : child->getTitle() == "Files"))
                {
                    const auto point = owner.getTopLevelComponent()->getLocalPoint (
                        child, child->getLocalBounds().getCentre().withX (20)).toFloat();
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
    bool clickRecord() override
    { return owner.transportBar != nullptr && owner.transportBar->clickRecordForScenario(); }

    std::string dspReadout() const override
    {
        return owner.systemStatusBar != nullptr ? owner.systemStatusBar->dspReadoutForScenario()
                                                : std::string();
    }

    bool doubleClickDspReadout() override
    {
        auto* bar = owner.systemStatusBar.get();
        if (bar == nullptr || ! bar->isShowing()) return false;
        const auto point = owner.getTopLevelComponent()
                               ->getLocalPoint (bar, bar->dspSegmentBounds().getCentre()).toFloat();
        return clickAt (point.x, point.y, 2);
    }

    int consolePageCount() const override { return owner.consoleView->numBanks(); }
    bool consolePageMatches (int index) const override
    {
        auto* view = owner.consoleView.get();
        if (view == nullptr || ! view->isShowing() || view->getBank() != index) return false;
        const auto range = view->rangeForBank (index);
        for (int track = 0; track < Session::kNumTracks; ++track)
        {
            auto* strip = view->getStripComponent (track);
            if (strip == nullptr || strip->isShowing() != (track + 1 >= range.first && track + 1 <= range.second))
                return false;
        }
        return true;
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

    bool clickContextMenuItem (const std::string& text) override
    {
        int x = 0, y = 0;
        if (! contextMenuItemPointForScenario (text, x, y)) return false;
        auto* body = EmbeddedModal::activeModalStack().back()->getBody();
        const auto local = body->getLocalBounds().getTopLeft().translated (x, y);
        const auto point = owner.getTopLevelComponent()->getLocalPoint (body, local).toFloat();
        return clickAt (point.x, point.y, 1);
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
    {
        auto* strip = owner.consoleView->getStripComponent (index);
        return strip != nullptr && strip->faderEditingForScenario();
    }
    double faderValue (int index) const override
    {
        auto* strip = owner.consoleView->getStripComponent (index);
        return strip != nullptr ? strip->faderValueForScenario() : 0.0;
    }

    bool virtualKeyboardOpen() const override
    {
       #if DUSKSTUDIO_HAS_NATIVE_UI
        return owner.virtualKeyboardWindow != nullptr && owner.virtualKeyboardWindow->isOpen();
       #else
        return false;
       #endif
    }
    bool tunerOpen() const override { return owner.tuner != nullptr; }

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
    bool focusPiano() override
    {
        auto* editor = owner.pianoRoll.get();
        if (editor == nullptr || ! editor->isShowing()) return false;
        const auto local = editor->getLocalBounds().getTopLeft().translated (3, 3);
        const auto point = owner.getTopLevelComponent()->getLocalPoint (editor, local).toFloat();
        return clickAt (point.x, point.y, 1);
    }
    std::array<int, 4> pianoOptions() const override
    {
        return owner.pianoRoll != nullptr ? owner.pianoRoll->optionsForScenario() : std::array<int, 4> {};
    }
    std::array<double, 4> pianoViewport() const override
    {
        return owner.pianoRoll != nullptr ? owner.pianoRoll->viewportForScenario() : std::array<double, 4> {};
    }
    bool scrollPiano (float delta, bool command, bool shift) override
    {
        auto* editor = owner.pianoRoll.get();
        auto* peer = owner.getPeer();
        if (editor == nullptr || ! editor->isShowing() || peer == nullptr) return false;
        const auto point = owner.getTopLevelComponent()->getLocalPoint (editor, editor->gridPointForScenario()).toFloat();
        using Peer = std::remove_pointer_t<decltype (peer)>;
        const auto time = std::chrono::duration_cast<std::chrono::milliseconds> (
            std::chrono::system_clock::now().time_since_epoch()).count();
        dispatchMouseWheel (*peer, &Peer::handleMouseEvent, &Peer::handleMouseWheel,
                            point.x, point.y, delta, command, shift, time);
        return true;
    }
    bool clickPianoFit() override
    {
        auto* editor = owner.pianoRoll.get();
        if (editor == nullptr || ! editor->isShowing()) return false;
        const auto point = owner.getTopLevelComponent()->getLocalPoint (editor, editor->fitPointForScenario()).toFloat();
        return clickAt (point.x, point.y, 1);
    }
    void closeRegionEditors() override { owner.closePianoRoll(); owner.closeAudioEditor(); }
    bool setStripCompact (int track, bool compact) override
    {
        auto* strip = owner.consoleView->getStripComponent (track);
        if (strip == nullptr) return false;
        const bool original = strip->isCompactMode();
        strip->setCompactMode (compact);
        return original;
    }
    bool clickStripModule (int track, int module, bool label, bool right) override
    {
        auto* strip = owner.consoleView->getStripComponent (track);
        auto* button = strip != nullptr ? strip->moduleButtonForScenario (module) : nullptr;
        if (button == nullptr || ! button->isShowing()) return false;
        const auto point = owner.getTopLevelComponent()->getLocalPoint (button,
            button->getLocalBounds().getRelativePoint (label ? 0.6f : 0.1f, 0.5f)).toFloat();
        return clickAt (point.x, point.y, 1, right);
    }
    bool stripModuleEditorOpen (int track, int module) const override
    {
        auto* strip = owner.consoleView->getStripComponent (track);
        return strip != nullptr && strip->moduleEditorOpenForScenario (module);
    }
    void closeStripModuleEditors (int track) override
    {
        if (auto* strip = owner.consoleView->getStripComponent (track))
            strip->closeModuleEditorsForScenario();
    }
    bool clickInsert (int track, bool right) override
    {
        auto* strip = owner.consoleView->getStripComponent (track);
        if (strip == nullptr || ! strip->isShowing()) return false;
        const auto point = owner.getTopLevelComponent()->getLocalPoint (strip, strip->insertPointForScenario()).toFloat();
        return clickAt (point.x, point.y, 1, right);
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
    {
        auto* strip = owner.consoleView->getStripComponent (index);
        return strip != nullptr
            && strip->openIoConfigPopupForCapture ((int) Track::Mode::Midi) != nullptr;
    }
    bool clickMidiSelector (int index, int kind) override
    {
        auto* strip = owner.consoleView->getStripComponent (index);
        if (strip == nullptr) return false;
        auto* combo = strip->midiSelectorForScenario (kind);
        if (combo == nullptr || ! combo->isShowing()) return false;
        const auto point = owner.getTopLevelComponent()->getLocalPoint (combo, combo->getLocalBounds().getCentre()).toFloat();
        return clickAt (point.x, point.y, 1);
    }
    std::string midiSelectorText (int index, int kind) const override
    {
        auto* strip = owner.consoleView->getStripComponent (index);
        if (strip == nullptr) return {};
        auto* combo = strip->midiSelectorForScenario (kind);
        return combo != nullptr ? combo->getText().toStdString() : std::string {};
    }

    bool meterClip (int index) override
    {
        auto* strip = owner.consoleView->getStripComponent (index);
        return strip != nullptr && strip->meterClipForScenario();
    }

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

    std::string modalText() const override
    {
        const auto& stack = EmbeddedModal::activeModalStack();
        if (stack.empty()) return {};
        const auto* body = stack.back()->getBody();
        return body == nullptr ? std::string {}
                               : body->getTitle().toStdString() + "\n" + body->getDescription().toStdString();
    }


    void closeTopModal() override
    {
        auto& stack = EmbeddedModal::activeModalStack();
        if (! stack.empty()) stack.back()->close();
    }

    void autosaveTick() override { owner.writeAutosave(); }
    bool doubleClickTempo() override
    {
        auto* bar = owner.transportBar.get();
        if (bar == nullptr || ! bar->isShowing()) return false;
        const auto point = owner.getTopLevelComponent()->getLocalPoint (bar, bar->bpmPointForScenario()).toFloat();
        return clickAt (point.x, point.y, 2);
    }

    bool rightClickPunch() override
    {
        auto* bar = owner.transportBar.get();
        if (bar == nullptr || ! bar->isShowing()) return false;
        const auto point = owner.getTopLevelComponent()->getLocalPoint (bar, bar->punchPointForScenario()).toFloat();
        return clickAt (point.x, point.y, 1, true);
    }

    bool focusModalTextInput() override
    {
        const auto& stack = EmbeddedModal::activeModalStack();
        if (stack.empty() || stack.back()->getBody() == nullptr) return false;
        for (auto* child : stack.back()->getBody()->getChildren())
            if (auto* handler = child->getAccessibilityHandler(); handler != nullptr && child->isShowing()
                && handler->getRole() == decltype (handler->getRole())::editableText)
            {
                const auto point = owner.getTopLevelComponent()->getLocalPoint (child, child->getLocalBounds().getCentre()).toFloat();
                return clickAt (point.x, point.y, 1);
            }
        return false;
    }
    void openAbout() override { owner.menuItemSelected (2002, 2); }
    bool shortcutsOpen() const override { return owner.shortcutsModal.isOpen(); }
    void startMixdown() override { owner.menuItemSelected (1010, 0); }

    bool fullScreen() const override
    { return owner.getPeer() != nullptr && owner.getPeer()->isFullScreen(); }

    using Component = std::remove_pointer_t<decltype (std::declval<MainComponent&>().getChildComponent (0))>;
    Component* findTitledControl (Component& root, const std::string& title)
    {
        if (root.getTitle().toStdString() == title) return &root;
        for (auto* child : root.getChildren())
            if (auto* found = findTitledControl (*child, title)) return found;
        return nullptr;
    }

    bool accessibleControl (const std::string& title, std::string& value, std::string& help) override
    {
        auto* control = findTitledControl (owner, title);
        auto* handler = control != nullptr ? control->getAccessibilityHandler() : nullptr;
        if (handler == nullptr || handler->getTitle().toStdString() != title) return false;
        help = handler->getHelp().toStdString();
        auto* interface = handler->getValueInterface();
        value = interface != nullptr ? interface->getCurrentValueAsString().toStdString() : std::string {};
        return true;
    }

    bool setAccessibleValue (const std::string& title, const std::string& value) override
    {
        auto* control = findTitledControl (owner, title);
        auto* handler = control != nullptr ? control->getAccessibilityHandler() : nullptr;
        auto* interface = handler != nullptr ? handler->getValueInterface() : nullptr;
        if (interface == nullptr || interface->isReadOnly()) return false;
        interface->setValueAsString (HostString (value.c_str()));
        return true;
    }

    int activeAuxLane() const override
    { return owner.auxView != nullptr ? owner.auxView->getActiveLane() : -1; }

    bool clickAuxSelector (int index) override
    {
        auto* button = owner.auxView != nullptr ? owner.auxView->selectorForScenario (index) : nullptr;
        if (button == nullptr || ! button->isShowing() || ! button->isEnabled()) return false;
        button->triggerClick();
        return true;
    }

    bool auxLaneLayoutMatches (int index) const override
    {
        auto* view = owner.auxView.get();
        if (view == nullptr || ! view->isShowing() || view->getActiveLane() != index) return false;
        for (int lane = 0; lane < Session::kNumAuxLanes; ++lane)
        {
            auto* body = view->getLaneComponent (lane);
            auto* button = view->selectorForScenario (lane);
            if (body == nullptr || button == nullptr || ! button->isShowing()
                || body->isShowing() != (lane == index) || button->getToggleState() != (lane == index)) return false;
            if (lane == index && (body->getWidth() < view->getWidth() - 24
                                  || body->getHeight() <= 0 || body->getY() < button->getBottom())) return false;
        }
        return true;
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
            dispatchMouseButton (*peer, &Peer::handleMouseEvent, x, y, true, time + click * 40, right ? 4 : 0);
            dispatchMouseButton (*peer, &Peer::handleMouseEvent, x, y, false, time + click * 40 + 20, right ? 4 : 0);
        }
        return true;
    }

    bool clickAudioRegion (int track, int region) override
    {
        if (owner.tapeStrip == nullptr || ! owner.tapeStrip->isShowing()) return false;
        const auto bounds = owner.tapeStrip->audioRegionScreenRect (track, region);
        if (bounds.isEmpty()) return false;
        const auto point = owner.getTopLevelComponent()->getLocalPoint (owner.tapeStrip.get(), bounds.getCentre()).toFloat();
        return clickAt (point.x, point.y, 1);
    }

    bool dragAt (float startX, float startY, float endX, float endY)
    {
        auto* peer = owner.getPeer();
        if (peer == nullptr) return false;
        using Peer = std::remove_pointer_t<decltype (peer)>;
        const auto time = std::chrono::duration_cast<std::chrono::milliseconds> (
            std::chrono::system_clock::now().time_since_epoch()).count();
        dispatchMouseButton (*peer, &Peer::handleMouseEvent, startX, startY, true, time);
        dispatchMouseButton (*peer, &Peer::handleMouseEvent, endX, endY, true, time + 40);
        dispatchMouseButton (*peer, &Peer::handleMouseEvent, endX, endY, false, time + 80);
        return true;
    }

    int pianoVelocityHeight() const override
    {
        return owner.pianoRoll != nullptr ? owner.pianoRoll->velocityBoundsForScenario().getHeight() : 0;
    }

    bool dragPianoVelocity (std::int64_t tick, float fraction) override
    {
        auto* piano = owner.pianoRoll.get();
        if (piano == nullptr || ! piano->isShowing()) return false;
        const auto bounds = piano->velocityBoundsForScenario();
        const auto local = piano->notePointForScenario (tick, 60).withY (bounds.getCentreY());
        const auto start = owner.getTopLevelComponent()->getLocalPoint (piano, local).toFloat();
        const auto end = owner.getTopLevelComponent()->getLocalPoint (piano,
            local.withY (bounds.getBottom() - static_cast<int> (fraction * static_cast<float> (bounds.getHeight())))).toFloat();
        return dragAt (start.x, start.y, end.x, end.y);
    }

    bool resizePianoVelocity (int pixels) override
    {
        auto* piano = owner.pianoRoll.get();
        if (piano == nullptr || ! piano->isShowing()) return false;
        const auto bounds = piano->velocityBoundsForScenario();
        const auto start = owner.getTopLevelComponent()->getLocalPoint (piano,
            bounds.getTopLeft().translated (20, -2)).toFloat();
        return dragAt (start.x, start.y, start.x, start.y - static_cast<float> (pixels));
    }

    bool togglePianoCc() override
    {
        if (owner.pianoRoll == nullptr) return false;
        owner.pianoRoll->toggleCcForScenario();
        return true;
    }

    int pianoCcHeight() const override
    {
        return owner.pianoRoll != nullptr ? owner.pianoRoll->ccBoundsForScenario().getHeight() : 0;
    }

    bool dragPianoCc (std::int64_t tick, float fraction) override
    {
        auto* piano = owner.pianoRoll.get();
        if (piano == nullptr || ! piano->isShowing()) return false;
        const auto bounds = piano->ccBoundsForScenario();
        if (bounds.isEmpty()) return false;
        const auto local = piano->notePointForScenario (tick, 60).withY (bounds.getCentreY());
        const auto start = owner.getTopLevelComponent()->getLocalPoint (piano, local).toFloat();
        const auto end = owner.getTopLevelComponent()->getLocalPoint (piano,
            local.withY (bounds.getBottom() - static_cast<int> (fraction * static_cast<float> (bounds.getHeight())))).toFloat();
        return dragAt (start.x, start.y, end.x, end.y);
    }

    bool resizePianoCc (int pixels) override
    {
        auto* piano = owner.pianoRoll.get();
        if (piano == nullptr || ! piano->isShowing()) return false;
        const auto bounds = piano->ccBoundsForScenario();
        if (bounds.isEmpty()) return false;
        const auto start = owner.getTopLevelComponent()->getLocalPoint (piano,
            bounds.getTopLeft().translated (20, -2)).toFloat();
        return dragAt (start.x, start.y, start.x, start.y - static_cast<float> (pixels));
    }

    bool wheelPianoVelocity (float delta) override
    {
        auto* piano = owner.pianoRoll.get();
        auto* peer = owner.getPeer();
        if (piano == nullptr || ! piano->isShowing() || peer == nullptr) return false;
        using Peer = std::remove_pointer_t<decltype (peer)>;
        const auto point = owner.getTopLevelComponent()->getLocalPoint (piano,
            piano->velocityBoundsForScenario().getCentre()).toFloat();
        const auto time = std::chrono::duration_cast<std::chrono::milliseconds> (
            std::chrono::system_clock::now().time_since_epoch()).count();
        dispatchWheel (*peer, &Peer::handleMouseWheel, point.x, point.y, time, delta);
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
    int audioEditorRegion() const override { return owner.audioEditorRegionIdx; }
    bool clickAudioEditorWaveform() override
    {
        auto* editor = owner.audioEditor.get();
        if (editor == nullptr || ! editor->isShowing()) return false;
        const auto point = owner.getTopLevelComponent()->getLocalPoint (
            editor, editor->getLocalBounds().getRelativePoint (0.5f, 0.75f)).toFloat();
        return clickAt (point.x, point.y, 1);
    }

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
    int pianoRollRegion() const override { return owner.pianoRollRegionIdx; }
    bool pianoRollOpen() const override { return owner.pianoRoll != nullptr; }
    bool clickPianoGrid (std::int64_t tick, int pitch) override
    {
        auto* piano = owner.pianoRoll.get();
        if (piano == nullptr || ! piano->isShowing()) return false;
        const auto local = piano->notePointForScenario (tick, pitch);
        if (! piano->getLocalBounds().contains (local)) return false;
        const auto point = owner.getTopLevelComponent()->getLocalPoint (piano, local).toFloat();
        return clickAt (point.x, point.y, 1);
    }
    bool doubleClickMidiRegion (int track, int region) override
    {
        if (owner.tapeStrip == nullptr || ! owner.tapeStrip->isShowing()) return false;
        const auto bounds = owner.tapeStrip->midiRegionScreenRect (track, region);
        if (bounds.isEmpty()) return false;
        const auto point = owner.getTopLevelComponent()->getLocalPoint (owner.tapeStrip.get(), bounds.getCentre()).toFloat();
        return clickAt (point.x, point.y, 2);
    }

    bool pressPianoRollKey (const std::string& description) override
    {
        return owner.pianoRoll != nullptr && owner.pianoRoll->isShowing()
            && dispatchKey (*owner.pianoRoll, &PianoRollComponent::keyPressed, description, 0);
    }

    bool loadMasteringFile (const std::filesystem::path& path) override
    {
        return owner.masteringView != nullptr && owner.masteringView->loadFile (hostFile (path));
    }

    bool clickMasteringWaveform (float fraction) override
    {
        if (owner.masteringView == nullptr) return false;
        for (auto* child : owner.masteringView->getChildren())
            if (auto* waveform = dynamic_cast<WaveformDisplay*> (child); waveform != nullptr && waveform->isShowing())
            {
                const auto local = waveform->getLocalBounds().getRelativePoint (fraction, 0.5f);
                const auto point = owner.getTopLevelComponent()->getLocalPoint (waveform, local).toFloat();
                return clickAt (point.x, point.y, 1);
            }
        return false;
    }

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

    std::vector<std::string> launchStateDiff() const override
    {
        std::vector<std::string> lines;

        const auto& stack = EmbeddedModal::activeModalStack();
        if (! stack.empty())
        {
            auto line = std::to_string (stack.size())
                      + (stack.size() == 1 ? " modal open" : " modals open");
            const auto text = modalText();
            const auto title = text.substr (0, text.find ('\n'));
            if (! title.empty()) line += ": " + title;
            lines.push_back (std::move (line));
        }

        if (! stageViewMatches (Stage::Recording))
            lines.push_back (std::string ("stage is ") + stageName (owner.engine.getStage()));

        if (owner.tapeStripExpanded != launch.tapeExpanded)
            lines.push_back (owner.tapeStripExpanded ? "tape strip expanded" : "tape strip collapsed");

        if (owner.tapeStrip != nullptr)
        {
            if (owner.tapeStrip->isChaseEnabled() != launch.tapeChase)
                lines.push_back (owner.tapeStrip->isChaseEnabled() ? "tape chase on" : "tape chase off");
            if (owner.tapeStrip->showsAllTracksForScenario() != launch.tapeShowAll)
                lines.push_back (owner.tapeStrip->showsAllTracksForScenario() ? "tape shows every track"
                                                                              : "tape hides empty tracks");
            if (owner.tapeStrip->getSelectedTrack() != launch.tapeSelectedTrack)
                lines.push_back ("tape selects track " + std::to_string (owner.tapeStrip->getSelectedTrack()));
        }

        const auto& transport = owner.engine.getTransport();
        if (! transport.isStopped())
            lines.push_back (transport.isRecording() ? "transport is recording" : "transport is playing");

        for (int index = 0; index < Session::kNumTracks; ++index)
        {
            const auto& track = owner.session.track (index);
            const auto who = "track " + std::to_string (index);
            if (track.recordArmed.load()) lines.push_back (who + " armed");

            const auto audio = track.regions.size();
            const auto midi = track.midiRegions.current().size();
            if (audio != 0 || midi != 0)
                lines.push_back (who + " holds " + std::to_string (audio) + " audio and "
                                 + std::to_string (midi) + " midi regions");

            const int mode = track.mode.load();
            if (mode != launch.trackMode[(std::size_t) index])
                lines.push_back (who + " mode is " + std::to_string (mode)
                                 + ", launched " + std::to_string (launch.trackMode[(std::size_t) index]));
            if (track.midiInputIndex.load() >= 0)
                lines.push_back (who + " selects MIDI input " + std::to_string (track.midiInputIndex.load()));

            auto& channel = owner.engine.getChannelStrip (index);
            if (channel.getPluginSlot().isLoaded()) lines.push_back (who + " has a hosted plug-in");
            if (channel.isNativeClapLoaded() || channel.isNativeLv2Loaded() || channel.isNativeVst3Loaded())
                lines.push_back (who + " has a native plug-in");
        }

        if (owner.consoleView != nullptr)
        {
            if (owner.consoleView->getBank() != launch.consoleBank)
                lines.push_back ("console bank is " + std::to_string (owner.consoleView->getBank())
                                 + ", launched " + std::to_string (launch.consoleBank));
            if (owner.consoleView->getFocusedStrip() != launch.consoleFocus)
                lines.push_back ("console focuses strip " + std::to_string (owner.consoleView->getFocusedStrip()));
        }

        if (pianoRollOpen())      lines.push_back ("piano roll open");
        if (audioEditorOpen())    lines.push_back ("audio editor open");
        if (audioSettingsOpen())  lines.push_back ("audio settings open");
        if (virtualKeyboardOpen()) lines.push_back ("virtual keyboard open");
        if (tunerOpen())          lines.push_back ("tuner open");
        if (owner.session.master().mute.load() != launch.masterMute)
            lines.push_back (owner.session.master().mute.load() ? "master muted" : "master unmuted");

        const auto masteringSource = owner.session.mastering().sourceFile.getFullPathName().toStdString();
        if (masteringSource != launch.masteringSource)
            lines.push_back ("mastering source is "
                             + (masteringSource.empty() ? std::string ("cleared") : masteringSource));

        if (owner.engine.isProcessingSuspended()) lines.push_back ("engine processing suspended");

        if (std::abs (uiScale() - launch.uiScale) > 0.0001)
            lines.push_back ("ui scale is " + std::to_string (uiScale())
                             + ", launched " + std::to_string (launch.uiScale));

        if (auto* peer = owner.getPeer())
        {
            if (peer->isFullScreen()) lines.push_back ("window is full screen");
            const auto bounds = peer->getBounds();
            const std::array<int, 4> now { bounds.getX(), bounds.getY(), bounds.getWidth(), bounds.getHeight() };
            if (now != launch.window)
                lines.push_back ("window is " + std::to_string (now[2]) + "x" + std::to_string (now[3])
                                 + " at " + std::to_string (now[0]) + "," + std::to_string (now[1])
                                 + ", launched " + std::to_string (launch.window[2]) + "x"
                                 + std::to_string (launch.window[3]) + " at "
                                 + std::to_string (launch.window[0]) + "," + std::to_string (launch.window[1]));
        }

        const auto sessionDir = scenario::currentSessionDirectory (owner.session);
        if (sessionDir != launch.sessionDir)
            lines.push_back ("session directory is " + sessionDir.string());

        return lines;
    }

    void resetForScenario() override
    {
        auto& stack = EmbeddedModal::activeModalStack();
        for (int guard = 0; guard < 32 && ! stack.empty(); ++guard)
            stack.back()->close();

        owner.closePianoRoll();
        owner.closeAudioEditor();
        owner.closeAudioSettings();
        owner.closeVirtualKeyboard();
        owner.closeTuner();
        for (int track = 0; track < Session::kNumTracks; ++track)
            if (auto* component = owner.consoleView != nullptr
                                      ? owner.consoleView->getStripComponent (track) : nullptr)
            {
                component->closeModuleEditorsForScenario();
                component->closeBuiltinForScenario();
            }

        owner.stopTransportForSessionSwitch();
        for (int track = 0; track < Session::kNumTracks; ++track)
            owner.session.setTrackArmed (track, false);

        owner.restoreUiScaleForScenario ((float) launch.uiScale);
        switchToStage (Stage::Recording);
        owner.setTimelineVisible (launch.tapeExpanded);
        if (owner.consoleView != nullptr)
        {
            owner.consoleView->setBank (launch.consoleBank);
            owner.consoleView->restoreFocusForScenario (launch.consoleFocus);
        }
        // Through the header button so the strip and the lit toggle agree.
        if (owner.hdrChaseBtn.getToggleState() != launch.tapeChase) owner.hdrChaseBtn.triggerClick();
        if (owner.tapeStrip != nullptr)
        {
            owner.tapeStrip->setChaseEnabled (launch.tapeChase);
            owner.tapeStrip->setShowAllTracksForScenario (launch.tapeShowAll);
            owner.tapeStrip->setSelectedTrack (launch.tapeSelectedTrack);
            owner.tapeStrip->clearSelectionsForScenario();
        }

        owner.engine.getUndoManager().clearUndoHistory();
        owner.focusMainCanvas();
        PianoRollComponent::clearClipboardForScenario();

        if (scenario::currentSessionDirectory (owner.session) != launch.sessionDir)
            scenario::applySessionDirectory (owner.session, launch.sessionDir);

        // Leaving full screen can recreate the peer, so it is fetched again
        // before the launch bounds go back.
        if (auto* peer = owner.getPeer(); peer != nullptr && peer->isFullScreen())
            owner.toggleFullScreen();
        if (auto* peer = owner.getPeer())
        {
            auto bounds = peer->getBounds();
            bounds.setBounds (launch.window[0], launch.window[1], launch.window[2], launch.window[3]);
            if (peer->getBounds() != bounds) peer->setBounds (bounds, false);
        }
    }

    // What the window held before the first scenario ran. Everything the suite
    // runner puts back, and everything it measures drift against.
    struct LaunchState
    {
        double uiScale = 1.0;
        bool tapeExpanded = false;
        bool tapeChase = false;
        bool tapeShowAll = false;
        bool masterMute = false;
        int tapeSelectedTrack = -1;
        int consoleBank = 0;
        int consoleFocus = -1;
        std::array<int, 4> window { 0, 0, 0, 0 };
        std::array<int, Session::kNumTracks> trackMode {};
        std::string masteringSource;
        std::filesystem::path sessionDir;
    };

    MainComponent& owner;
    LaunchState launch;
    std::vector<std::string> startupErrors;
    std::array<std::unique_ptr<ScenarioStripHandle>, Session::kNumTracks> strips;
    std::array<std::unique_ptr<ScenarioAuxLaneHandle>, Session::kNumAuxLanes> lanes;
};

namespace scenario
{
std::vector<std::string> guiLaunchStateDiff (GuiHost& host) { return host.launchStateDiff(); }
void guiResetForScenario (GuiHost& host) { host.resetForScenario(); }
} // namespace scenario

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
