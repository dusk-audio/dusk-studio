#include "../GuiScenarioSupport.h"
#include "../../../foundation/Base64.h"
#include "../../../foundation/Fs.h"
#include "../../../foundation/Json.h"
#include "../../../dsp/AuxLaneStrip.h"
#include "../../../dsp/ChannelStrip.h"
#include "../../../engine/scenario/cases/OopStubHarness.h"
#include "../../../session/RegionEditActions.h"
#include "../../../session/SessionSerializer.h"

#if DUSKSTUDIO_HAS_NATIVE_VST3
 #include "../../../engine/vst3/Vst3HostContext.h"
 #if defined(__GNUC__)
  #pragma GCC diagnostic push
  #pragma GCC diagnostic ignored "-Wshadow"
 #endif
 #include <pluginterfaces/vst/ivsteditcontroller.h>
 #if defined(__GNUC__)
  #pragma GCC diagnostic pop
 #endif
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

#if ! defined(_WIN32)
 #include <signal.h>
 #include <sys/types.h>
#endif

// What counts as unsaved. A change made only inside a third-party plug-in - its
// own editor, a preset it loads, a MIDI-bound parameter - does: Quit and the
// session switch ask about it, a successful Save answers it, and opening a
// session, including one whose native restore waits for the device, starts
// clean. So does a recovered autosave whose save failed. A session whose text
// is not ASCII compares the same as any other.
namespace duskstudio::scenario::gui
{
namespace
{
namespace fs = std::filesystem;

// A track name that is not ASCII, as UTF-8.
constexpr const char* kNonAsciiName = "B\xc3\xa4sse \xe2\x99\xaa";

// A blank session of the case's own, opened from its folder. The session the
// suite was on comes back however the case ends, and its load unloads every
// plug-in the case loaded.
std::optional<ScenarioResult> openCaseSession (GuiHost& host, ScenarioContext& ctx,
                                               const std::string& name, fs::path& canonical,
                                               const std::function<void (Session&)>& seed = {})
{
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    if (! engine.getTransport().isStopped() || ! host.modalStackEmpty()
        || ! engine.isAudioCallbackRegistered() || engine.getCurrentSampleRate() <= 0.0)
        return ScenarioResult::skip ("requires stopped live audio and no modal");
    const auto originalDir = currentSessionDirectory (session);
    const auto restore = ctx.tempDir() / "restore.json";
    engine.publishPluginStateForSave (true);
    if (! SessionSerializer::save (session, restore))
        return ScenarioResult::fail ("could not preserve the initial session");
    ctx.cleanup ([&host, &engine, &session, originalDir, restore]
    {
        engine.stop();
        drainModals (host);
        engine.getUndoManager().clearUndoHistory();
        reopenSavedSession (host, restore);
        applySessionDirectory (session, originalDir);
    });
    const auto folder = ctx.tempDir() / name;
    fs::create_directories (folder);
    canonical = folder / "session.json";
    auto blank = std::make_unique<Session>();
    if (seed) seed (*blank);
    if (! SessionSerializer::save (*blank, canonical))
        return ScenarioResult::fail ("could not create the case's session");
    blank.reset();
    reopenSavedSession (host, canonical);
    return std::nullopt;
}

void pushSaveSteps (GuiHost& host, ScenarioContext& ctx, std::vector<Step>& steps)
{
    pushFileMenuSteps (host, ctx, steps, "Save");
    steps.push_back ({ 0, [&host, &ctx]
    {
        ctx.expect (host.modalStackEmpty() && host.statusMessage().rfind ("Saved:", 0) == 0,
                    "Save did not complete: " + host.modalText() + " " + host.statusMessage());
    } });
}

// Quit has to ask, with the quit prompt; Cancel takes it down again.
void pushQuitAsksSteps (GuiHost& host, ScenarioContext& ctx, std::vector<Step>& steps,
                        std::string what)
{
    steps.push_back ({ 0, [&host, &ctx, what] { ctx.expect (host.requestQuit(), what); } });
    steps.push_back ({ 200, [&host, &ctx]
    {
        if (host.modalStackEmpty()) return;
        ctx.expect (host.modalText().find ("Save changes before quitting?") != std::string::npos,
                    "Quit raised the wrong prompt: " + host.modalText());
        ctx.expect (host.clickModalButton ("Cancel"), "the quit prompt could not be cancelled");
    } });
    steps.push_back ({ 200, [&host, &ctx]
    {
        ctx.expect (host.modalStackEmpty(), "Cancel left the quit prompt open");
    } });
}

nlohmann::ordered_json savedJson (const fs::path& canonical)
{
    auto parsed = nlohmann::ordered_json::parse (dusk::fs::loadFileAsString (canonical), nullptr, false);
    return parsed.is_object() ? parsed : nlohmann::ordered_json::object();
}

struct HostedFixture
{
    PluginDescriptor descriptor;
    int gainIndex = -1;
    // Switched on, the state outgrows what a sandboxed capture can carry.
    int largeIndex = -1;
    // Switched on, the child exits from its next processed block.
    int terminateIndex = -1;
};

// The crash-state VST3 as the JUCE-backed slot loads it, with the index of its
// State Gain control.
std::optional<HostedFixture> inspectHostedFixture (ScenarioContext& ctx, std::string& error)
{
    auto& engine = ctx.engine();
    auto& manager = engine.getPluginManager();
    const auto fixture = ctx.fixture ("crash_state.vst3");
    if (! fixture)
    {
        error = "the state fixture is unavailable";
        return std::nullopt;
    }
    using File = std::decay_t<decltype (ctx.session().getSessionDirectory())>;
    using Text = std::decay_t<decltype (ctx.session().getSessionDirectory().getFullPathName())>;
    Text loadError;
    auto probe = manager.createPluginInstance (File (fixture->u8string().c_str()),
        engine.getCurrentSampleRate(), ScenarioContext::kBlockSize, loadError);
    if (! probe)
    {
        error = "could not inspect the state fixture: " + loadError.toStdString();
        return std::nullopt;
    }
    HostedFixture result { manager.descriptorForInstance (*probe), -1, -1, -1 };
    result.descriptor.location = fixture->u8string();
    const auto& parameters = probe->getParameters();
    for (int index = 0; index < parameters.size(); ++index)
    {
        if (parameters[index]->getName (100) == "State Gain") result.gainIndex = index;
        if (parameters[index]->getName (100) == "Large State") result.largeIndex = index;
        if (parameters[index]->getName (100) == "Terminate Child") result.terminateIndex = index;
    }
    if (result.gainIndex < 0 || result.largeIndex < 0 || result.terminateIndex < 0)
    {
        error = "the fixture lacks its State Gain, Large State or Terminate Child control";
        return std::nullopt;
    }
    return result;
}

struct HostedLoad
{
    bool done = false, loaded = false;
    std::string error;
};

// Loads the fixture into a track's JUCE-backed slot, in this process or in its
// sandbox; the suite's hosting mode comes back with the case.
std::shared_ptr<HostedLoad> loadHosted (ScenarioContext& ctx, const HostedFixture& fixture,
                                        int track, bool sandboxed)
{
    auto& manager = ctx.engine().getPluginManager();
    auto& slot = ctx.engine().getChannelStrip (track).getPluginSlot();
    const bool wasOop = manager.isOopEnabled();
    ctx.cleanup ([&slot, &manager, wasOop]
    {
        slot.unload();
        manager.setOopEnabled (wasOop);
    });
    manager.setOopEnabled (sandboxed);
    auto load = std::make_shared<HostedLoad>();
    slot.loadFromDescriptorAsync (fixture.descriptor, [load] (bool loaded, auto error)
    {
        load->loaded = loaded;
        load->error = error.toStdString();
        load->done = true;
    });
    return load;
}

// What the plug-in's own editor does on a knob move in this process; a
// sandboxed plug-in's editor is out of reach, so its edit arrives the way a
// MIDI binding sends one.
void editHosted (PluginSlot& slot, int index, float value, bool sandboxed)
{
    if (sandboxed)
        slot.setParamNormalised (index, value);
    else if (auto* instance = slot.getInstance())
        instance->getParameters()[index]->setValueNotifyingHost (value);
}

#if DUSKSTUDIO_HAS_NATIVE_CLAP
constexpr const char* kParamTouchId = "studio.dusk.test.param-touch";
constexpr clap_id kAlpha = 40;
constexpr clap_id kBeta = 41;

// The fixture's studio.dusk.test.param-touch-host, as the fixture lays it out.
struct ParamTouchControl
{
    bool (CLAP_ABI *markDirty) (const clap_plugin_t*);
    bool (CLAP_ABI *requestFlush) (const clap_plugin_t*);
    bool (CLAP_ABI *rescan) (const clap_plugin_t*, std::uint32_t flags);
    bool (CLAP_ABI *clear) (const clap_plugin_t*, clap_id id, std::uint32_t flags);
    bool (CLAP_ABI *editFromGui) (const clap_plugin_t*, clap_id id, double value);
    void (CLAP_ABI *reenter) (const clap_plugin_t*, void (*fn) (void*), void* context);
    void (CLAP_ABI *markDirtyOnDestroy) (const clap_plugin_t*);
    const void* (CLAP_ABI *probe) (const clap_plugin_t*);
    void (CLAP_ABI *markDirtyOnSave) (const clap_plugin_t*);
};

const clap_plugin_t* paramTouchPlugin (clap::NativeClapSlot& slot)
{
    const auto* instance = slot.getInstance();
    return instance != nullptr ? instance->getPlugin() : nullptr;
}

const ParamTouchControl* paramTouchControl (clap::NativeClapSlot& slot)
{
    const auto* plugin = paramTouchPlugin (slot);
    return plugin == nullptr ? nullptr : static_cast<const ParamTouchControl*> (
        plugin->get_extension (plugin, "studio.dusk.test.param-touch-host"));
}

std::vector<std::uint8_t> paramTouchState (double alpha, double beta)
{
    std::vector<std::uint8_t> bytes { 'P', 'T', 1, 0 };
    bytes.resize (20);
    const double values[] { alpha, beta };
    std::memcpy (bytes.data() + 4, values, sizeof (values));
    return bytes;
}

// { alpha, beta } from a saved param-touch state; negative when it is not one.
std::array<double, 2> savedParamTouch (const nlohmann::ordered_json& node)
{
    if (! node.is_object() || ! node.contains ("native_clap_state") || ! node["native_clap_state"].is_string())
        return { -1.0, -1.0 };
    const auto text = node["native_clap_state"].get<std::string>();
    const auto bytes = dusk::base64::decode (text.data(), text.size());
    if (bytes.size() != 20 || bytes[0] != 'P' || bytes[1] != 'T') return { -1.0, -1.0 };
    std::array<double, 2> values {};
    std::memcpy (values.data(), bytes.data() + 4, sizeof (values));
    return values;
}

bool loadParamTouch (ScenarioContext& ctx, const fs::path& fixture, int track, int lane)
{
    auto& engine = ctx.engine();
    using File = std::decay_t<decltype (ctx.session().getSessionDirectory())>;
    const File file (fixture.u8string().c_str());
    std::string trackError, laneError;
    engine.suspendProcessing();
    const bool trackLoaded = track < 0 || engine.getChannelStrip (track).loadNativeClap (file, trackError, kParamTouchId);
    const bool laneLoaded = lane < 0 || engine.getAuxLaneStrip (lane).loadNativeClap (0, file, laneError, kParamTouchId);
    engine.resumeProcessing();
    if (track >= 0) engine.getChannelStrip (track).insertMode.store (ChannelStrip::kInsertPlugin);
    if (lane >= 0) engine.getAuxLaneStrip (lane).insertMode[0].store (AuxLaneStrip::kInsertPlugin);
    return ctx.expect (trackLoaded && laneLoaded,
                       "could not load the param-touch CLAP: " + trackError + laneError);
}

constexpr int kClapTrack = 3;
constexpr int kClapLane = 1;

std::optional<ScenarioResult> runClapEdits (GuiHost& host, ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    const auto fixture = ctx.fixture ("param_touch.clap");
    if (! fixture) return ScenarioResult::fail ("the param-touch CLAP fixture is unavailable");
    fs::path canonical;
    if (auto early = openCaseSession (host, ctx, "CLAP plug-in edits", canonical)) return early;
    if (! loadParamTouch (ctx, *fixture, kClapTrack, kClapLane)) return ctx.verdict();

    auto& trackSlot = engine.getChannelStrip (kClapTrack).getNativeClapSlot();
    auto& laneSlot = engine.getAuxLaneStrip (kClapLane).getNativeClapSlot (0);
    const auto* control = paramTouchControl (trackSlot);
    if (control == nullptr || control->markDirtyOnSave == nullptr)
        return ScenarioResult::fail ("the fixture's host control is missing");
    auto revision = std::make_shared<std::uint64_t> (0);
    auto savedState = std::make_shared<std::string>();

    auto steps = std::make_shared<std::vector<Step>>();
    pushSaveSteps (host, ctx, *steps);
    steps->push_back ({ 0, [&host, &ctx, &trackSlot, control, revision]
    {
        if (host.requestQuit())
        {
            ctx.expect (false, "a freshly saved CLAP session asked to save before quitting");
            drainModals (host);
        }
        *revision = trackSlot.stateRevision();
        ctx.expect (control->editFromGui (paramTouchPlugin (trackSlot), kBeta, 0.5),
                    "the host offers the plug-in no request_flush");
    } });
    steps->push_back ({ 0, [] {}, [&trackSlot, revision] { return trackSlot.stateRevision() > *revision; },
                        "the edit made in the plug-in's GUI never reached the host" });
    pushQuitAsksSteps (host, ctx, *steps, "an edit made only in a CLAP's own GUI did not ask to save before quitting");
    pushSaveSteps (host, ctx, *steps);
    steps->push_back ({ 0, [&host, &ctx, &laneSlot, canonical]
    {
        ctx.expect (! host.sessionDirty(), "Save left a CLAP's GUI edit unsaved");
        auto saved = savedJson (canonical);
        ctx.expect (std::abs (savedParamTouch (saved["tracks"][kClapTrack])[1] - 0.5) < 1.0e-9,
                    "Save did not write the CLAP's GUI edit");
        laneSlot.queueParamBinding (1, 0.75f);
        ctx.expect (host.sessionDirty(), "a MIDI-bound move on an aux CLAP counted only after its drain tick");
    } });
    pushSaveSteps (host, ctx, *steps);
    steps->push_back ({ 0, [&host, &ctx, &trackSlot, control, canonical]
    {
        ctx.expect (! host.sessionDirty(), "Save left a MIDI-bound CLAP move unsaved");
        auto saved = savedJson (canonical);
        ctx.expect (std::abs (savedParamTouch (saved["aux_lanes"][kClapLane]["plugin_slots"][0])[1] - 0.75) < 1.0e-6,
                    "Save did not write the aux CLAP's MIDI-bound move");
        control->markDirtyOnSave (paramTouchPlugin (trackSlot));
    } });
    pushSaveSteps (host, ctx, *steps);
    steps->push_back ({ 0, [&host, &ctx]
    {
        ctx.expect (host.sessionDirty(),
                    "a change the plug-in reported while Save read its state counted as saved");
    } });
    pushSaveSteps (host, ctx, *steps);
    steps->push_back ({ 0, [&host, &ctx, &engine, &session, &trackSlot, control, revision, savedState]
    {
        ctx.expect (! host.sessionDirty(), "a Save after the late change left the session dirty");
        engine.play();
        *savedState = session.track (kClapTrack).nativeClapStateBase64.toStdString();
        *revision = trackSlot.stateRevision();
        control->editFromGui (paramTouchPlugin (trackSlot), kAlpha, 0.8);
    } });
    steps->push_back ({ 0, [&host, &ctx, &engine, &session, savedState]
    {
        ctx.expect (host.sessionDirty(), "a CLAP GUI edit during playback did not count as unsaved");
        ctx.expect (engine.getTransport().isPlaying() && ! engine.isProcessingSuspended(),
                    "asking about unsaved changes stopped playback or held the audio thread");
        ctx.expect (session.track (kClapTrack).nativeClapStateBase64.toStdString() == *savedState,
                    "asking about unsaved changes captured plug-in state during playback");
        engine.stop();
    }, [&trackSlot, revision] { return trackSlot.stateRevision() > *revision; },
       "the GUI edit made during playback never reached the host" });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

constexpr int kOpenTrack = 2;
constexpr int kOpenHostedTrack = 8;
constexpr int kDeferredTrack = 5;
constexpr int kDeferredLane = 3;

// A session opened from disk starts clean, its restored plug-ins included, and
// so does one saved while a native restore still waited for the device to
// prepare; an edit after either still counts.
std::optional<ScenarioResult> runOpenStartsClean (GuiHost& host, ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    const auto fixture = ctx.fixture ("param_touch.clap");
    if (! fixture) return ScenarioResult::fail ("the param-touch CLAP fixture is unavailable");
    std::string hostedError;
    const auto hosted = inspectHostedFixture (ctx, hostedError);
    if (! hosted) return ScenarioResult::fail (hostedError);
    fs::path canonical;
    if (auto early = openCaseSession (host, ctx, "Plug-in state reopens", canonical)) return early;
    if (! loadParamTouch (ctx, *fixture, kOpenTrack, -1)) return ctx.verdict();
    engine.getChannelStrip (kOpenTrack).getNativeClapSlot().setParamValue (kAlpha, 0.3);
    const auto hostedLoad = loadHosted (ctx, *hosted, kOpenHostedTrack, false);
    const auto deferredSession = ctx.tempDir() / "Deferred restore" / "session.json";
    fs::create_directories (deferredSession.parent_path());
    {
        auto blank = std::make_unique<Session>();
        blank->track (9).name = std::string (kNonAsciiName);
        if (! SessionSerializer::save (*blank, deferredSession))
            return ScenarioResult::fail ("could not create the deferred-restore session");
    }
    const double rate = engine.getCurrentSampleRate();
    const int blockSize = engine.getCurrentBlockSize();
    auto revision = std::make_shared<std::uint64_t> (0);

    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 0, [&ctx, &engine, hosted, hostedLoad]
    {
        auto& slot = engine.getChannelStrip (kOpenHostedTrack).getPluginSlot();
        ctx.expect (hostedLoad->loaded && slot.isLoaded(), "the state fixture did not load: " + hostedLoad->error);
        engine.getChannelStrip (kOpenHostedTrack).insertMode.store (ChannelStrip::kInsertPlugin);
        editHosted (slot, hosted->gainIndex, 0.4f, false);
    }, [hostedLoad] { return hostedLoad->done; }, "the state fixture did not finish loading" });
    pushSaveSteps (host, ctx, *steps);
    steps->push_back ({ 0, [&host, &ctx, &engine, canonical]
    {
        reopenSavedSession (host, canonical);
        auto& slot = engine.getChannelStrip (kOpenTrack).getNativeClapSlot();
        double alpha = -1.0;
        ctx.expect (slot.isLoaded() && slot.getParamValue (kAlpha, alpha) && std::abs (alpha - 0.3) < 1.0e-9,
                    "reopening did not restore the CLAP and its state");
        ctx.expect (engine.getChannelStrip (kOpenHostedTrack).getPluginSlot().isLoaded(),
                    "reopening did not restore the hosted plug-in");
        ctx.expect (! host.sessionDirty(), "a session just opened with plug-in state read as unsaved");
    } });
    steps->push_back ({ 500, [&host, &ctx, &engine, revision]
    {
        ctx.expect (! host.sessionDirty(), "audio running through the restored plug-ins made the session unsaved");
        auto& slot = engine.getChannelStrip (kOpenTrack).getNativeClapSlot();
        *revision = slot.stateRevision();
        const auto* control = paramTouchControl (slot);
        ctx.expect (control != nullptr && control->editFromGui (paramTouchPlugin (slot), kBeta, 0.6),
                    "the restored plug-in offers no GUI edit");
    } });
    steps->push_back ({ 0, [] {}, [&engine, revision]
    {
        return engine.getChannelStrip (kOpenTrack).getNativeClapSlot().stateRevision() > *revision;
    }, "the restored plug-in's GUI edit never reached the host" });
    pushQuitAsksSteps (host, ctx, *steps, "a GUI edit to a restored CLAP did not ask to save before quitting");
    steps->push_back ({ 0, [&host, &ctx, &engine, &session, fixture, deferredSession]
    {
        reopenSavedSession (host, deferredSession);
        const auto state = paramTouchState (0.4, 0.0);
        const auto encoded = dusk::base64::encode (state.data(), state.size());
        using File = std::decay_t<decltype (session.getSessionDirectory())>;
        const File file (fixture->u8string().c_str());
        auto& track = session.track (kDeferredTrack);
        auto& lane = session.auxLane (kDeferredLane);
        // The track's restore names no plug-in id, as an older session does, so
        // accepting it also rewrites the identity the saved baseline holds.
        track.nativeClapPath = file.getFullPathName();
        track.nativeClapPluginId.clear();
        track.nativeClapStateBase64 = encoded.c_str();
        lane.nativeClapPath[0] = file.getFullPathName();
        lane.nativeClapPluginId[0] = kParamTouchId;
        lane.nativeClapStateBase64[0] = encoded.c_str();
        auto& strip = engine.getChannelStrip (kDeferredTrack);
        auto& aux = engine.getAuxLaneStrip (kDeferredLane);
        strip.setPendingNativeClap (file, state);
        aux.setPendingNativeClap (0, file, state, kParamTouchId);
        strip.insertMode.store (ChannelStrip::kInsertPlugin);
        aux.insertMode[0].store (AuxLaneStrip::kInsertPlugin);
        ctx.expect (! strip.isNativeClapLoaded() && ! aux.isNativeClapLoaded (0),
                    "the deferred restores were loaded before the device prepared");
    } });
    pushSaveSteps (host, ctx, *steps);
    steps->push_back ({ 0, [&host, &ctx, &engine, rate, blockSize]
    {
        ctx.expect (! host.sessionDirty(), "a session saved with restores still pending read as unsaved");
        engine.prepareForSelfTest (rate, blockSize);
        auto& strip = engine.getChannelStrip (kDeferredTrack);
        auto& aux = engine.getAuxLaneStrip (kDeferredLane);
        double alpha = -1.0;
        ctx.expect (strip.isNativeClapLoaded() && aux.isNativeClapLoaded (0)
                        && strip.getNativeClapSlot().getParamValue (kAlpha, alpha) && std::abs (alpha - 0.4) < 1.0e-9,
                    "the device prepare did not complete the deferred restores");
        ctx.expect (! host.sessionDirty(), "deferred restores completing after Save made the session unsaved");
        aux.getNativeClapSlot (0).setParamValue (kAlpha, 0.9);
        ctx.expect (host.sessionDirty(), "an edit to a deferred-restored aux CLAP did not count as unsaved");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}
#endif

#if DUSKSTUDIO_HAS_NATIVE_VST3
constexpr int kVst3Track = 4;
constexpr std::uint32_t kProbeGain = 1;

std::optional<ScenarioResult> runVst3Edits (GuiHost& host, ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    const auto fixture = ctx.fixture ("state_probe.vst3");
    if (! fixture) return ScenarioResult::fail ("the state-probe VST3 fixture is unavailable");
    fs::path canonical;
    if (auto early = openCaseSession (host, ctx, "VST3 plug-in edits", canonical)) return early;
    {
        using File = std::decay_t<decltype (session.getSessionDirectory())>;
        auto& strip = engine.getChannelStrip (kVst3Track);
        std::string error;
        engine.suspendProcessing();
        const bool loaded = strip.loadNativeVst3 (File (fixture->u8string().c_str()), error);
        engine.resumeProcessing();
        strip.insertMode.store (ChannelStrip::kInsertPlugin);
        if (! ctx.expect (loaded, "could not load the state-probe VST3: " + error)) return ctx.verdict();
    }
    auto& slot = engine.getChannelStrip (kVst3Track).getNativeVst3Slot();
    auto savedState = std::make_shared<std::string>();

    auto steps = std::make_shared<std::vector<Step>>();
    pushSaveSteps (host, ctx, *steps);
    steps->push_back ({ 0, [&host, &ctx, &session, &slot, savedState]
    {
        if (host.requestQuit())
        {
            ctx.expect (false, "a freshly saved VST3 session asked to save before quitting");
            drainModals (host);
        }
        *savedState = session.track (kVst3Track).nativeVst3StateBase64.toStdString();
        auto* handler = slot.getInstance() != nullptr
            ? static_cast<Steinberg::Vst::IComponentHandler*> (slot.getInstance()->getHost().componentHandler())
            : nullptr;
        if (! ctx.expect (handler != nullptr, "the VST3 has no component handler")) return;
        handler->beginEdit (kProbeGain);
        handler->performEdit (kProbeGain, 0.6);
        handler->endEdit (kProbeGain);
    } });
    pushQuitAsksSteps (host, ctx, *steps, "an edit made only in a VST3's own editor did not ask to save before quitting");
    pushSaveSteps (host, ctx, *steps);
    steps->push_back ({ 0, [&host, &ctx, &session, &slot, savedState]
    {
        ctx.expect (! host.sessionDirty(), "Save left a VST3 editor edit unsaved");
        ctx.expect (session.track (kVst3Track).nativeVst3StateBase64.toStdString() != *savedState,
                    "Save did not write the VST3 editor edit");
        *savedState = session.track (kVst3Track).nativeVst3StateBase64.toStdString();
        slot.setBypassed (true);
        slot.setParamValue (kProbeGain, 0.2);
        ctx.expect (host.sessionDirty(), "a parameter write to a bypassed VST3 did not count as unsaved");
    } });
    pushSaveSteps (host, ctx, *steps);
    steps->push_back ({ 0, [&host, &ctx, &session, &slot, savedState]
    {
        slot.setBypassed (false);
        ctx.expect (! host.sessionDirty(), "Save left a bypassed VST3's write unsaved");
        ctx.expect (session.track (kVst3Track).nativeVst3StateBase64.toStdString() != *savedState,
                    "Save did not deliver the bypassed VST3's queued write into its state");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}
#endif

#if DUSKSTUDIO_HAS_NATIVE_LV2
constexpr int kLv2Lane = 2;
constexpr std::uint32_t kProbeGainPort = 4;
constexpr const char* kStateProbeUri = "urn:duskstudio:test:state-probe";

std::optional<ScenarioResult> runLv2Edits (GuiHost& host, ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    const auto fixture = ctx.fixture ("state_probe.lv2");
    if (! fixture) return ScenarioResult::fail ("the state-probe LV2 fixture is unavailable");
    fs::path canonical;
    if (auto early = openCaseSession (host, ctx, "LV2 plug-in edits", canonical)) return early;
    const auto other = ctx.tempDir() / "Other session" / "session.json";
    fs::create_directories (other.parent_path());
    {
        auto blank = std::make_unique<Session>();
        if (! SessionSerializer::save (*blank, other))
            return ScenarioResult::fail ("could not create the other session");
    }
    {
        using File = std::decay_t<decltype (session.getSessionDirectory())>;
        auto& aux = engine.getAuxLaneStrip (kLv2Lane);
        std::string error;
        engine.suspendProcessing();
        const bool loaded = aux.loadNativeLv2 (0, File (fixture->u8string().c_str()), error, kStateProbeUri);
        engine.resumeProcessing();
        aux.insertMode[0].store (AuxLaneStrip::kInsertPlugin);
        if (! ctx.expect (loaded, "could not load the state-probe LV2: " + error)) return ctx.verdict();
    }
    auto& slot = engine.getAuxLaneStrip (kLv2Lane).getNativeLv2Slot (0);
    auto savedState = std::make_shared<std::string>();
    const auto folder = canonical.parent_path();

    auto steps = std::make_shared<std::vector<Step>>();
    pushSaveSteps (host, ctx, *steps);
    steps->push_back ({ 0, [&host, &ctx, &session, &slot, savedState, other]
    {
        ctx.expect (! host.sessionDirty(), "a freshly saved LV2 session read as unsaved");
        *savedState = session.auxLane (kLv2Lane).nativeLv2StateBase64[0].toStdString();
        if (! ctx.expect (slot.getInstance() != nullptr, "the LV2 has no instance")) return;
        slot.getInstance()->setControlPortValueFromUi (kProbeGainPort, 0.5f);
        host.requestSessionSwitch (other);
    } });
    steps->push_back ({ 0, [&host, &ctx]
    {
        ctx.expect (host.modalText().find ("Save changes before opening another session?") != std::string::npos,
                    "an edit made only in an LV2's own UI did not ask before switching sessions: '"
                        + host.modalText() + "'");
        if (! host.modalStackEmpty())
            ctx.expect (host.clickModalButton ("Cancel"), "the switch prompt could not be cancelled");
    }, [&host, &session, folder]
    {
        return ! host.modalStackEmpty() || currentSessionDirectory (session) != folder;
    }, "the session switch neither asked nor switched" });
    steps->push_back ({ 200, [&host, &ctx, &session, folder]
    {
        ctx.expect (host.modalStackEmpty() && currentSessionDirectory (session) == folder,
                    "Cancel did not keep the LV2 session open");
    } });
    pushSaveSteps (host, ctx, *steps);
    steps->push_back ({ 0, [&host, &ctx, &session, savedState]
    {
        ctx.expect (! host.sessionDirty(), "Save left an LV2 UI edit unsaved");
        ctx.expect (session.auxLane (kLv2Lane).nativeLv2StateBase64[0].toStdString() != *savedState,
                    "Save did not write the LV2 UI edit");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}
#endif

constexpr int kHostedTrack = 6;
constexpr int kEmptyTrack = 7;

// A plug-in hosted through the JUCE-backed slot, in this process or in its
// sandbox child. Its state is stripped from the JSON comparison, so nothing
// but the slot's own revision can tell an edit from a clean session.
std::optional<ScenarioResult> runHostedEdits (GuiHost& host, ScenarioContext& ctx, bool sandboxed)
{
#if ! DUSKSTUDIO_HAS_OOP_PLUGINS
    if (sandboxed) return ScenarioResult::skip ("sandboxed plug-ins are unavailable");
#endif
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    std::string error;
    const auto hosted = inspectHostedFixture (ctx, error);
    if (! hosted) return ScenarioResult::fail (error);
    fs::path canonical;
    if (auto early = openCaseSession (host, ctx, sandboxed ? "Sandboxed plug-in edits" : "Hosted plug-in edits",
                                      canonical))
        return early;
    auto& slot = engine.getChannelStrip (kHostedTrack).getPluginSlot();
    const auto load = loadHosted (ctx, *hosted, kHostedTrack, sandboxed);
    const int gainIndex = hosted->gainIndex;
    const auto edit = [&slot, gainIndex, sandboxed] (float value) { editHosted (slot, gainIndex, value, sandboxed); };

    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 0, [&ctx, &engine, &slot, load, sandboxed]
    {
        ctx.expect (load->loaded && slot.isRemote() == sandboxed, "the state fixture did not load: " + load->error);
        engine.getChannelStrip (kHostedTrack).insertMode.store (ChannelStrip::kInsertPlugin);
    }, [load] { return load->done; }, "the state fixture did not finish loading" });
    pushSaveSteps (host, ctx, *steps);
    steps->push_back ({ 0, [&host, &ctx, edit]
    {
        if (host.requestQuit())
        {
            ctx.expect (false, "a freshly saved hosted plug-in asked to save before quitting");
            drainModals (host);
        }
        edit (0.25f);
    } });
    pushQuitAsksSteps (host, ctx, *steps, "an edit made only inside a hosted plug-in did not ask to save before quitting");
    pushSaveSteps (host, ctx, *steps);
    steps->push_back ({ 0, [&host, &ctx]
    {
        ctx.expect (! host.sessionDirty(), "Save left a hosted plug-in's edit unsaved");
    } });
    if (! sandboxed)
    {
        // Undo puts back the plug-in the clone replaced, with the state it had
        // then, unsaved edit included; the JSON names the same plug-in either way.
        steps->push_back ({ 0, [&host, &ctx, &engine, &session, &slot, edit]
        {
            edit (0.75f);
            auto& undo = engine.getUndoManager();
            undo.clearUndoHistory();
            undo.beginNewTransaction();
            ctx.expect (undo.perform (new CloneTrackAction (session, engine, kEmptyTrack, kHostedTrack)),
                        "the clone over the hosted plug-in refused to run");
            ctx.expect (! slot.isLoaded(), "cloning an empty track left the hosted plug-in loaded");
            ctx.expect (undo.undo() && slot.isLoaded(), "undoing the clone did not bring the hosted plug-in back");
            ctx.expect (host.sessionDirty(), "undoing a clone over an edited hosted plug-in read as saved");
            undo.clearUndoHistory();
        } });
    }
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

// Recover writes the recovered state to session.json straight away. When that
// write fails, the recovered work exists only in memory and in the recovery
// file, so the session stays unsaved and keeps the file, however its controls
// move afterwards, until a save succeeds.
std::optional<ScenarioResult> runRecoverySaveFailure (GuiHost& host, ScenarioContext& ctx)
{
    auto& session = ctx.session();
    fs::path launch;
    if (auto early = openCaseSession (host, ctx, "Recovery launch", launch)) return early;
    const auto folder = ctx.tempDir() / "Recovery save fails";
    fs::create_directories (folder);
    const auto canonical = folder / "session.json";
    const auto autosave = folder / "session.json.autosave";
    // A folder where the save writes its temporary copy fails the write while
    // the session folder itself stays writable, as a locked file does.
    const auto blocker = folder / "session.json.tmp";
    {
        auto saved = std::make_unique<Session>();
        saved->track (0).name = std::string (kNonAsciiName);
        if (! SessionSerializer::save (*saved, canonical))
            return ScenarioResult::fail ("could not create the saved session");
        saved->track (0).strip.faderDb.store (-6.0f);
        if (! SessionSerializer::save (*saved, autosave))
            return ScenarioResult::fail ("could not create the recovery file");
    }
    fs::create_directories (blocker);
    if (! dusk::fs::writeStringToFile (blocker / "keep", "blocks the save"))
        return ScenarioResult::fail ("could not block the recovery save");
    ctx.cleanup ([blocker]
    {
        std::error_code error;
        fs::remove_all (blocker, error);
    });

    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 0, [&host, &ctx, canonical]
    {
        ctx.expect (host.openSession (canonical) && ! host.modalStackEmpty(),
                    "the newer recovery file raised no recovery prompt");
        ctx.expect (host.answerRecovery (GuiHost::Recovery::Recover), "the recovery prompt could not be answered");
    } });
    steps->push_back ({ 0, [&host, &ctx, &session, autosave]
    {
        ctx.expect (host.modalText().find ("Save failed") != std::string::npos,
                    "the failed recovery save raised the wrong alert: " + host.modalText());
        drainModals (host);
        ctx.expect (nearly (session.track (0).strip.faderDb.load(), -6.0f), "Recover did not load the recovery file");
        ctx.expect (fs::exists (autosave), "the failed recovery save removed the recovery file");
        ctx.expect (host.sessionDirty(), "a recovered session whose save failed read as saved");
        session.track (0).strip.faderDb.store (-9.0f);
        host.autosaveTick();
        session.track (0).strip.faderDb.store (-6.0f);
        host.autosaveTick();
        ctx.expect (fs::exists (autosave),
                    "reverting an edit after the failed recovery save deleted the only copy of the recovered work");
        ctx.expect (nearly (savedFaderOf (autosave), -6.0f),
                    "after the failed recovery save, the recovery file kept a reverted edit instead of the recovered work");
        ctx.expect (host.sessionDirty(), "reverting an edit made the unsaved recovered session read as saved");
    }, [&host] { return ! host.modalStackEmpty(); }, "the failed recovery save raised no alert" });
    pushQuitAsksSteps (host, ctx, *steps, "quitting after a failed recovery save did not ask to save");
    steps->push_back ({ 0, [blocker]
    {
        std::error_code error;
        fs::remove_all (blocker, error);
    } });
    pushSaveSteps (host, ctx, *steps);
    steps->push_back ({ 0, [&host, &ctx, canonical, autosave]
    {
        ctx.expect (! host.sessionDirty(), "a Save after the failed recovery save left the session unsaved");
        ctx.expect (! fs::exists (autosave), "the Save left the recovery file behind");
        ctx.expect (nearly (savedFaderOf (canonical), -6.0f), "the Save did not write the recovered work");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

// An idle session named in text that is not ASCII must not rewrite its recovery
// file on every heartbeat, and must compare equal to its saved self.
std::optional<ScenarioResult> runNonAsciiAutosaveIdle (GuiHost& host, ScenarioContext& ctx)
{
    auto& session = ctx.session();
    fs::path canonical;
    if (auto early = openCaseSession (host, ctx, "Non-ASCII session", canonical,
                                      [] (Session& seeded) { seeded.track (0).name = std::string (kNonAsciiName); }))
        return early;
    const auto autosave = canonical.parent_path() / "session.json.autosave";
    const float savedFader = savedFaderOf (canonical);
    session.track (0).strip.faderDb.store (savedFader - 3.0f);
    host.autosaveTick();
    if (! ctx.expect (fs::exists (autosave), "an edit wrote no recovery file")) return ctx.verdict();
    ctx.expect (dusk::fs::writeStringToFile (autosave, "untouched"), "could not mark the recovery file");
    host.autosaveTick();
    ctx.expect (dusk::fs::loadFileAsString (autosave) == "untouched",
                "an idle heartbeat rewrote the recovery file of a session with a non-ASCII name");
    ctx.expect (host.sessionDirty(), "the edit did not count as unsaved");
    session.track (0).strip.faderDb.store (savedFader);
    ctx.expect (! host.sessionDirty(), "reverting the edit left the non-ASCII session unsaved");
    std::error_code error;
    fs::remove (autosave, error);
    return ctx.verdict();
}

constexpr int kFailingTrack = 0;

// The open confirmation prompt's title and text, empty when none is up.
std::string confirmation (GuiHost& host)
{
    std::string text;
    for (const auto& part : host.confirmationText()) text += part + "\n";
    return text;
}
#if DUSKSTUDIO_HAS_NATIVE_CLAP
constexpr int kFreshClapTrack = 3;
#endif

// A sandboxed plug-in whose state outgrows the capture fails every Save. Cancel
// saves nothing. Save anyway saves the session with that slot's last saved
// state and every other slot captured fresh, completes the Save a prompt was
// waiting for, and leaves the failed slot unsaved.
std::optional<ScenarioResult> runCaptureFailureSaveAnyway (GuiHost& host, ScenarioContext& ctx)
{
#if ! DUSKSTUDIO_HAS_OOP_PLUGINS
    (void) host;
    (void) ctx;
    return ScenarioResult::skip ("sandboxed plug-ins are unavailable");
#else
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    std::string error;
    const auto hosted = inspectHostedFixture (ctx, error);
    if (! hosted) return ScenarioResult::fail (error);
    fs::path canonical;
    if (auto early = openCaseSession (host, ctx, "Save anyway", canonical)) return early;
    const auto folder = canonical.parent_path();
    const auto other = ctx.tempDir() / "After save anyway" / "session.json";
    fs::create_directories (other.parent_path());
    {
        auto blank = std::make_unique<Session>();
        if (! SessionSerializer::save (*blank, other))
            return ScenarioResult::fail ("could not create the next session");
    }
    auto& slot = engine.getChannelStrip (kFailingTrack).getPluginSlot();
    const auto load = loadHosted (ctx, *hosted, kFailingTrack, true);
  #if DUSKSTUDIO_HAS_NATIVE_CLAP
    const auto clapFixture = ctx.fixture ("param_touch.clap");
    if (! clapFixture) return ScenarioResult::fail ("the param-touch CLAP fixture is unavailable");
    if (! loadParamTouch (ctx, *clapFixture, kFreshClapTrack, -1)) return ctx.verdict();
  #endif
    struct Saved { std::string state, file; };
    auto saved = std::make_shared<Saved>();
    const int largeIndex = hosted->largeIndex;
    const int gainIndex = hosted->gainIndex;

    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 0, [&ctx, &engine, &slot, load]
    {
        ctx.expect (load->loaded && slot.isRemote(), "the state fixture did not load sandboxed: " + load->error);
        engine.getChannelStrip (kFailingTrack).insertMode.store (ChannelStrip::kInsertPlugin);
    }, [load] { return load->done; }, "the state fixture did not finish loading" });
    pushSaveSteps (host, ctx, *steps);
    steps->push_back ({ 0, [&ctx, &engine, &session, &slot, saved, largeIndex, gainIndex, canonical]
    {
        saved->state = session.track (kFailingTrack).pluginStateBase64.toStdString();
        ctx.expect (! saved->state.empty(), "the sandboxed plug-in saved no state");
        slot.setParamNormalised (gainIndex, 0.25f);
        slot.setParamNormalised (largeIndex, 1.0f);
      #if DUSKSTUDIO_HAS_NATIVE_CLAP
        engine.getChannelStrip (kFreshClapTrack).getNativeClapSlot().setParamValue (kAlpha, 0.6);
      #else
        (void) engine;
      #endif
        saved->file = dusk::fs::loadFileAsString (canonical);
    } });
    pushFileMenuSteps (host, ctx, *steps, "Save");
    steps->push_back ({ 0, [&host, &ctx]
    {
        const auto text = confirmation (host);
        ctx.expect (text.rfind ("Save failed\n", 0) == 0
                        && text.find ("could not capture plug-in state") != std::string::npos
                        && text.find ("Track 1 - Dusk Crash State Fixture") != std::string::npos
                        && text.find ("changes made in it since are lost") != std::string::npos,
                    "the capture failure did not offer its choice: '" + text + "' over '" + host.modalText() + "'");
        ctx.expect (host.clickModalButton ("Cancel"), "the capture failure offered no Cancel");
    }, [&host] { return ! host.modalStackEmpty(); }, "the failing Save raised nothing" });
    steps->push_back ({ 200, [&host, &ctx, saved, canonical]
    {
        ctx.expect (host.modalStackEmpty(), "Cancel left the capture-failure prompt open");
        ctx.expect (dusk::fs::loadFileAsString (canonical) == saved->file, "Cancel wrote the session");
        ctx.expect (host.statusMessage().rfind ("Save failed", 0) == 0, "Cancel did not leave Save failed in the status line");
        ctx.expect (host.sessionDirty(), "Cancel left the session reading as saved");
    } });
    pushFileMenuSteps (host, ctx, *steps, "Save");
    // Shrunk again, the state would now capture: Save anyway must still keep
    // what the prompt listed rather than read it afresh.
    steps->push_back ({ 0, [&host, &ctx, &slot, largeIndex]
    {
        slot.setParamNormalised (largeIndex, 0.0f);
        ctx.expect (host.clickModalButton ("Save anyway"), "the capture failure offered no Save anyway: " + host.modalText());
    }, [&host] { return ! host.modalStackEmpty(); }, "the second failing Save raised nothing" });
    steps->push_back ({ 0, [&host, &ctx, saved, canonical]
    {
        auto json = savedJson (canonical);
        const auto& row = json["tracks"][static_cast<std::size_t> (kFailingTrack)];
        ctx.expect (row.contains ("plugin_state") && row["plugin_state"] == saved->state,
                    "Save anyway did not keep the failed slot's last saved state");
      #if DUSKSTUDIO_HAS_NATIVE_CLAP
        ctx.expect (std::abs (savedParamTouch (json["tracks"][static_cast<std::size_t> (kFreshClapTrack)])[0] - 0.6) < 1.0e-9,
                    "Save anyway did not capture the other plug-ins fresh");
      #endif
        ctx.expect (host.sessionDirty(), "Save anyway counted the failed slot's lost changes as saved");
    }, [&host] { return host.modalStackEmpty() && host.statusMessage().rfind ("Saved:", 0) == 0; },
       "Save anyway did not save" });
    steps->push_back ({ 0, [&host, &slot, largeIndex, other]
    {
        slot.setParamNormalised (largeIndex, 1.0f);
        host.requestSessionSwitch (other);
    } });
    steps->push_back ({ 0, [&host, &ctx]
    {
        ctx.expect (host.modalText().find ("Save changes before opening another session?") != std::string::npos,
                    "the failed slot's unsaved changes did not ask before switching: " + host.modalText());
        ctx.expect (host.clickModalButton ("Save"), "the switch prompt offered no Save");
    }, [&host, &session, folder]
    {
        return ! host.modalStackEmpty() || currentSessionDirectory (session) != folder;
    }, "the session switch neither asked nor switched" });
    steps->push_back ({ 0, [&host, &ctx]
    {
        ctx.expect (host.clickModalButton ("Save anyway"), "the prompt's Save offered no Save anyway: " + host.modalText());
    }, [&host]
    {
        return confirmation (host).find ("could not capture plug-in state") != std::string::npos
            || host.modalText().find ("could not capture plug-in state") != std::string::npos;
    }, "the prompt's Save did not reach the capture failure" });
    steps->push_back ({ 0, [&ctx, saved, canonical]
    {
        auto json = savedJson (canonical);
        ctx.expect (json["tracks"][static_cast<std::size_t> (kFailingTrack)]["plugin_state"] == saved->state,
                    "the prompt's Save anyway did not keep the failed slot's last saved state");
    }, [&session, other] { return currentSessionDirectory (session) == other.parent_path(); },
       "Save anyway did not let the session switch go on" });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
#endif
}

// A Save anyway prompt still up when another session opens belongs to the
// session it was raised for: answering it then saves nothing and raises nothing.
std::optional<ScenarioResult> runStaleSaveAnyway (GuiHost& host, ScenarioContext& ctx)
{
#if ! DUSKSTUDIO_HAS_OOP_PLUGINS
    (void) host;
    (void) ctx;
    return ScenarioResult::skip ("sandboxed plug-ins are unavailable");
#else
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    std::string error;
    const auto hosted = inspectHostedFixture (ctx, error);
    if (! hosted) return ScenarioResult::fail (error);
    fs::path canonical;
    if (auto early = openCaseSession (host, ctx, "Stale prompt", canonical)) return early;
    const auto other = ctx.tempDir() / "Opened over the prompt" / "session.json";
    fs::create_directories (other.parent_path());
    {
        auto blank = std::make_unique<Session>();
        if (! SessionSerializer::save (*blank, other))
            return ScenarioResult::fail ("could not create the other session");
    }
    auto& slot = engine.getChannelStrip (kFailingTrack).getPluginSlot();
    const auto load = loadHosted (ctx, *hosted, kFailingTrack, true);
    const int largeIndex = hosted->largeIndex;
    struct Files { std::string first, other; };
    auto files = std::make_shared<Files>();

    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 0, [&ctx, &engine, &slot, load]
    {
        ctx.expect (load->loaded && slot.isRemote(), "the state fixture did not load sandboxed: " + load->error);
        engine.getChannelStrip (kFailingTrack).insertMode.store (ChannelStrip::kInsertPlugin);
    }, [load] { return load->done; }, "the state fixture did not finish loading" });
    pushSaveSteps (host, ctx, *steps);
    steps->push_back ({ 0, [&slot, files, largeIndex, canonical, other]
    {
        slot.setParamNormalised (largeIndex, 1.0f);
        files->first = dusk::fs::loadFileAsString (canonical);
        files->other = dusk::fs::loadFileAsString (other);
    } });
    pushFileMenuSteps (host, ctx, *steps, "Save");
    steps->push_back ({ 0, [&host, &ctx, &session, other]
    {
        ctx.expect (host.openSession (other) && currentSessionDirectory (session) == other.parent_path(),
                    "the other session did not open over the prompt");
        ctx.expect (confirmation (host).find ("could not capture plug-in state") != std::string::npos,
                    "opening the other session took the capture-failure prompt down");
        ctx.expect (host.clickModalButton ("Save anyway"), "the stale prompt offered no Save anyway");
    }, [&host] { return confirmation (host).find ("could not capture plug-in state") != std::string::npos; },
       "the failing Save raised no capture-failure prompt" });
    steps->push_back ({ 300, [&host, &ctx, &session, files, canonical, other]
    {
        ctx.expect (host.modalStackEmpty(), "answering the stale prompt raised '" + host.modalText() + "'");
        ctx.expect (currentSessionDirectory (session) == other.parent_path(),
                    "answering the stale prompt moved the open session");
        ctx.expect (dusk::fs::loadFileAsString (canonical) == files->first,
                    "answering the stale prompt wrote the session it was raised for");
        ctx.expect (dusk::fs::loadFileAsString (other) == files->other,
                    "answering the stale prompt wrote the session opened over it");
        ctx.expect (! host.sessionDirty(), "answering the stale prompt marked the other session unsaved");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
#endif
}

// A sandboxed plug-in cut off from the audio path by an oversized buffer keeps
// its child, idle but alive, so a Save reads its state as usual, edits queued
// for it while it was cut off included.
std::optional<ScenarioResult> runCutOffSandboxSaves (GuiHost& host, ScenarioContext& ctx)
{
#if ! DUSKSTUDIO_HAS_OOP_PLUGINS
    (void) host;
    (void) ctx;
    return ScenarioResult::skip ("sandboxed plug-ins are unavailable");
#else
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    std::string error;
    const auto hosted = inspectHostedFixture (ctx, error);
    if (! hosted) return ScenarioResult::fail (error);
    fs::path canonical;
    if (auto early = openCaseSession (host, ctx, "Cut-off sandbox", canonical)) return early;
    auto& slot = engine.getChannelStrip (kFailingTrack).getPluginSlot();
    const auto load = loadHosted (ctx, *hosted, kFailingTrack, true);
    const double rate = engine.getCurrentSampleRate();
    const int blockSize = engine.getCurrentBlockSize();
    ctx.cleanup ([&engine, rate, blockSize]
    {
        if (engine.getCurrentBlockSize() != blockSize) engine.prepareForSelfTest (rate, blockSize);
    });
    const int gainIndex = hosted->gainIndex;
    auto state = std::make_shared<std::string>();

    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 0, [&ctx, &engine, &slot, load]
    {
        ctx.expect (load->loaded && slot.isRemote(), "the state fixture did not load sandboxed: " + load->error);
        engine.getChannelStrip (kFailingTrack).insertMode.store (ChannelStrip::kInsertPlugin);
    }, [load] { return load->done; }, "the state fixture did not finish loading" });
    pushSaveSteps (host, ctx, *steps);
    steps->push_back ({ 0, [&host, &ctx, &engine, &slot, rate]
    {
        // A buffer larger than the sandbox carries leaves the slot silent until reload.
        engine.prepareForSelfTest (rate, 2 * duskstudio::ipc::kMaxBlock);
        ctx.expect (! slot.isRemote() && slot.getLoadedName().isNotEmpty(),
                    "the oversized buffer did not cut the sandboxed plug-in off while keeping it");
        ctx.expect (! host.sessionDirty(), "cutting the plug-in off counted as an unsaved change");
    } });
    pushSaveSteps (host, ctx, *steps);
    steps->push_back ({ 0, [&host, &ctx, &session, &slot, gainIndex, state]
    {
        ctx.expect (! host.sessionDirty(), "a Save with nothing changed left the cut-off plug-in unsaved");
        *state = session.track (kFailingTrack).pluginStateBase64.toStdString();
        slot.setParamNormalised (gainIndex, 0.25f);
        ctx.expect (host.sessionDirty(), "an edit to a sandboxed plug-in cut off from the audio read as saved");
    } });
    // Well past the 30 Hz drain, so an edit the drain could drop is gone by the Save.
    pushFileMenuSteps (host, ctx, *steps, "Save");
    steps->push_back ({ 0, [&host, &ctx, state, canonical]
    {
        const auto text = confirmation (host);
        ctx.expect (text.empty() && host.statusMessage().rfind ("Saved:", 0) == 0,
                    "Save did not read a sandboxed plug-in that was only cut off from the audio: '" + text + "'");
        if (! text.empty()) ctx.expect (host.clickModalButton ("Cancel"), "the capture failure offered no Cancel");
        auto json = savedJson (canonical);
        ctx.expect (json["tracks"][static_cast<std::size_t> (kFailingTrack)]["plugin_state"] != *state,
                    "Save wrote the cut-off plug-in's state without the edit made while it was cut off");
        ctx.expect (! host.sessionDirty(), "the cut-off plug-in read as unsaved after Save read its state");
    }, [&host]
    {
        return ! host.modalStackEmpty() || host.statusMessage().rfind ("Saved:", 0) == 0;
    }, "the Save neither asked nor finished" });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
#endif
}

#if DUSKSTUDIO_HAS_OOP_PLUGINS
// Drives one block straight through the slot, with the engine's own callback
// held off, so a child armed to exit reaches its next process call.
void processOneBlock (AudioEngine& engine, PluginSlot& slot)
{
    std::array<float, ScenarioContext::kBlockSize> left {}, right {};
    oopstub::SlotMidiBuffer midi;
    const int count = std::clamp (engine.getCurrentBlockSize(), 1, ScenarioContext::kBlockSize);
    engine.suspendProcessing();
    slot.processStereoBlock (left.data(), right.data(), count, midi);
    engine.resumeProcessing();
}
#endif

// A crashed sandboxed child can only hand back the state last read from it.
// After an edit, Save asks, before and after Re-enable plugin (crashed) drops
// the dead child, and Save anyway keeps the earlier state and stays unsaved.
std::optional<ScenarioResult> runCrashedSandboxAsks (GuiHost& host, ScenarioContext& ctx)
{
#if ! DUSKSTUDIO_HAS_OOP_PLUGINS
    (void) host;
    (void) ctx;
    return ScenarioResult::skip ("sandboxed plug-ins are unavailable");
#else
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    std::string error;
    const auto hosted = inspectHostedFixture (ctx, error);
    if (! hosted) return ScenarioResult::fail (error);
    fs::path canonical;
    if (auto early = openCaseSession (host, ctx, "Crashed sandbox", canonical)) return early;
    auto& slot = engine.getChannelStrip (kFailingTrack).getPluginSlot();
    const auto load = loadHosted (ctx, *hosted, kFailingTrack, true);
    const int gainIndex = hosted->gainIndex;
    const int terminateIndex = hosted->terminateIndex;
    auto state = std::make_shared<std::string>();
    const auto pushAsksSteps = [&host, &ctx] (std::vector<Step>& list, std::string when)
    {
        list.push_back ({ 0, [&host, &ctx, when]
        {
            const auto text = confirmation (host);
            ctx.expect (text.find ("Track 1 - Dusk Crash State Fixture") != std::string::npos
                            && text.find ("stopped processing") != std::string::npos,
                        "Save " + when + " did not ask about the crashed plug-in's lost edit: '" + text
                            + "' status '" + host.statusMessage() + "'");
        }, [&host]
        {
            return ! host.modalStackEmpty() || host.statusMessage().rfind ("Saved:", 0) == 0;
        }, "the Save neither asked nor finished" });
    };

    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 0, [&ctx, &engine, &slot, load]
    {
        ctx.expect (load->loaded && slot.isRemote(), "the state fixture did not load sandboxed: " + load->error);
        engine.getChannelStrip (kFailingTrack).insertMode.store (ChannelStrip::kInsertPlugin);
    }, [load] { return load->done; }, "the state fixture did not finish loading" });
    pushSaveSteps (host, ctx, *steps);
    steps->push_back ({ 0, [&session, &slot, state, gainIndex, terminateIndex]
    {
        *state = session.track (kFailingTrack).pluginStateBase64.toStdString();
        slot.setParamNormalised (gainIndex, 0.25f);
        slot.setParamNormalised (terminateIndex, 1.0f);
    } });
    steps->push_back ({ 0, [&host, &ctx]
    {
        ctx.expect (host.sessionDirty(), "the crashed plug-in's edit read as saved");
    }, [&engine, &slot]
    {
        if (! slot.wasCrashed()) processOneBlock (engine, slot);
        return slot.wasCrashed();
    }, "the armed child never exited" });
    pushFileMenuSteps (host, ctx, *steps, "Save");
    pushAsksSteps (*steps, "with the child crashed");
    steps->push_back ({ 0, [&host, &ctx]
    {
        if (! confirmation (host).empty())
            ctx.expect (host.clickModalButton ("Cancel"), "the capture failure offered no Cancel");
    } });
    steps->push_back ({ 200, [&host, &ctx, &slot]
    {
        ctx.expect (host.modalStackEmpty(), "Cancel left the capture-failure prompt open");
        slot.clearAutoBypass();
        ctx.expect (! slot.wasCrashed(), "Re-enable plugin (crashed) did not clear the crash");
        ctx.expect (host.sessionDirty(), "Re-enable made the crashed plug-in's lost edit read as saved");
    } });
    pushFileMenuSteps (host, ctx, *steps, "Save");
    pushAsksSteps (*steps, "after Re-enable");
    steps->push_back ({ 0, [&host, &ctx]
    {
        if (! confirmation (host).empty())
            ctx.expect (host.clickModalButton ("Save anyway"), "the capture failure offered no Save anyway");
    } });
    steps->push_back ({ 0, [&host, &ctx, state, canonical]
    {
        auto json = savedJson (canonical);
        ctx.expect (json["tracks"][static_cast<std::size_t> (kFailingTrack)]["plugin_state"] == *state,
                    "Save anyway did not keep the crashed plug-in's last read state");
        ctx.expect (host.sessionDirty(), "Save anyway counted the crashed plug-in's lost edit as saved");
    }, [&host] { return host.modalStackEmpty() && host.statusMessage().rfind ("Saved:", 0) == 0; },
       "Save anyway did not save" });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
#endif
}

// A sandboxed plug-in loaded over another starts from what it was loaded with,
// not from the state its predecessor last had read. Unedited, its child can
// die before its first Save and the Save still has nothing to ask about.
std::optional<ScenarioResult> runFreshSandboxCrashSaves (GuiHost& host, ScenarioContext& ctx)
{
#if ! DUSKSTUDIO_HAS_OOP_PLUGINS || defined(_WIN32)
    (void) host;
    (void) ctx;
    return ScenarioResult::skip ("needs sandboxed plug-ins and a child that can be killed by pid");
#else
    auto& engine = ctx.engine();
    std::string error;
    const auto hosted = inspectHostedFixture (ctx, error);
    if (! hosted) return ScenarioResult::fail (error);
    fs::path canonical;
    if (auto early = openCaseSession (host, ctx, "Fresh sandbox crash", canonical)) return early;
    auto& slot = engine.getChannelStrip (kFailingTrack).getPluginSlot();
    const auto first = loadHosted (ctx, *hosted, kFailingTrack, true);
    const int gainIndex = hosted->gainIndex;
    auto second = std::make_shared<HostedLoad>();

    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 0, [&ctx, &engine, &slot, first]
    {
        ctx.expect (first->loaded && slot.isRemote(), "the state fixture did not load sandboxed: " + first->error);
        engine.getChannelStrip (kFailingTrack).insertMode.store (ChannelStrip::kInsertPlugin);
    }, [first] { return first->done; }, "the state fixture did not finish loading" });
    pushSaveSteps (host, ctx, *steps);
    steps->push_back ({ 0, [&slot, hosted, gainIndex, second]
    {
        slot.setParamNormalised (gainIndex, 0.25f);
        slot.loadFromDescriptorAsync (hosted->descriptor, [second] (bool loaded, auto loadError)
        {
            second->loaded = loaded;
            second->error = loadError.toStdString();
            second->done = true;
        });
    } });
    steps->push_back ({ 0, [&ctx, &slot, second]
    {
        const int pid = slot.getRemoteChildPid();
        if (! ctx.expect (second->loaded && slot.isRemote() && pid > 0,
                          "the replacement did not load sandboxed: " + second->error))
            return;
        ::kill (static_cast<pid_t> (pid), SIGKILL);
    }, [second] { return second->done; }, "the replacement did not finish loading" });
    // Once the reaper has seen it go, the dead child is a crash, not a read that fails.
    steps->push_back ({ 0, [] {}, [&slot] { return slot.wasCrashed(); }, "the killed child was never reaped" });
    pushFileMenuSteps (host, ctx, *steps, "Save");
    steps->push_back ({ 0, [&host, &ctx]
    {
        const auto text = confirmation (host);
        ctx.expect (text.empty() && host.statusMessage().rfind ("Saved:", 0) == 0,
                    "Save asked about a replacement plug-in nobody edited: '" + text + "'");
        if (! text.empty()) ctx.expect (host.clickModalButton ("Cancel"), "the capture failure offered no Cancel");
    }, [&host]
    {
        return ! host.modalStackEmpty() || host.statusMessage().rfind ("Saved:", 0) == 0;
    }, "the Save neither asked nor finished" });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
#endif
}

// Save anyway keeps exactly the slots its prompt listed. One that fails only
// during that Save was never shown, so the prompt comes back listing it too.
std::optional<ScenarioResult> runSaveAnywayAsksAgain (GuiHost& host, ScenarioContext& ctx)
{
#if ! DUSKSTUDIO_HAS_OOP_PLUGINS
    (void) host;
    (void) ctx;
    return ScenarioResult::skip ("sandboxed plug-ins are unavailable");
#else
    static constexpr int kSecondTrack = 1;
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    std::string error;
    const auto hosted = inspectHostedFixture (ctx, error);
    if (! hosted) return ScenarioResult::fail (error);
    fs::path canonical;
    if (auto early = openCaseSession (host, ctx, "Save anyway again", canonical)) return early;
    auto& first = engine.getChannelStrip (kFailingTrack).getPluginSlot();
    auto& second = engine.getChannelStrip (kSecondTrack).getPluginSlot();
    const auto firstLoad = loadHosted (ctx, *hosted, kFailingTrack, true);
    const auto secondLoad = loadHosted (ctx, *hosted, kSecondTrack, true);
    const int largeIndex = hosted->largeIndex;
    struct Saved { std::string first, second; };
    auto saved = std::make_shared<Saved>();

    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 0, [&ctx, &engine, &first, &second, firstLoad, secondLoad]
    {
        ctx.expect (firstLoad->loaded && secondLoad->loaded && first.isRemote() && second.isRemote(),
                    "the state fixtures did not load sandboxed: " + firstLoad->error + secondLoad->error);
        engine.getChannelStrip (kFailingTrack).insertMode.store (ChannelStrip::kInsertPlugin);
        engine.getChannelStrip (kSecondTrack).insertMode.store (ChannelStrip::kInsertPlugin);
    }, [firstLoad, secondLoad] { return firstLoad->done && secondLoad->done; },
       "the state fixtures did not finish loading" });
    pushSaveSteps (host, ctx, *steps);
    steps->push_back ({ 0, [&session, &first, saved, largeIndex]
    {
        saved->first = session.track (kFailingTrack).pluginStateBase64.toStdString();
        saved->second = session.track (kSecondTrack).pluginStateBase64.toStdString();
        first.setParamNormalised (largeIndex, 1.0f);
    } });
    pushFileMenuSteps (host, ctx, *steps, "Save");
    steps->push_back ({ 0, [&host, &ctx, &second, largeIndex]
    {
        const auto text = confirmation (host);
        ctx.expect (text.find ("Track 1 - Dusk Crash State Fixture") != std::string::npos
                        && text.find ("Track 2 -") == std::string::npos,
                    "the first prompt did not list the first slot alone: '" + text + "'");
        second.setParamNormalised (largeIndex, 1.0f);
        ctx.expect (host.clickModalButton ("Save anyway"), "the capture failure offered no Save anyway");
    }, [&host] { return confirmation (host).find ("could not capture plug-in state") != std::string::npos; },
       "the failing Save raised no capture-failure prompt" });
    steps->push_back ({ 0, [&host, &ctx]
    {
        const auto text = confirmation (host);
        ctx.expect (text.find ("Track 1 - Dusk Crash State Fixture") != std::string::npos
                        && text.find ("Track 2 - Dusk Crash State Fixture") != std::string::npos,
                    "the prompt that came back did not list both slots: '" + text + "'");
        ctx.expect (host.clickModalButton ("Save anyway"), "the prompt that came back offered no Save anyway");
    }, [&host] { return confirmation (host).find ("Track 2 - Dusk Crash State Fixture") != std::string::npos; },
       "Save anyway saved past a slot its prompt never listed" });
    steps->push_back ({ 0, [&host, &ctx, saved, canonical]
    {
        auto json = savedJson (canonical);
        ctx.expect (json["tracks"][static_cast<std::size_t> (kFailingTrack)]["plugin_state"] == saved->first
                        && json["tracks"][static_cast<std::size_t> (kSecondTrack)]["plugin_state"] == saved->second,
                    "Save anyway did not keep both listed slots' last saved state");
        ctx.expect (host.sessionDirty(), "Save anyway counted the listed slots' lost changes as saved");
    }, [&host] { return host.modalStackEmpty() && host.statusMessage().rfind ("Saved:", 0) == 0; },
       "the second Save anyway did not save" });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
#endif
}

const ScenarioRegistrar saveAnywayAsksAgain { Scenario {
    "gui.save_anyway_asks_again", { "gui", "session", "save", "ipc", "messages" }, Needs::Engine | Needs::Gui,
    { "crash_state.vst3" }, {}, 60000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runSaveAnywayAsksAgain (host, ctx); }
} };
const ScenarioRegistrar staleSaveAnyway { Scenario {
    "gui.stale_save_anyway_saves_nothing", { "gui", "session", "save", "ipc", "messages" }, Needs::Engine | Needs::Gui,
    { "crash_state.vst3" }, {}, 60000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runStaleSaveAnyway (host, ctx); }
} };
const ScenarioRegistrar cutOffSandbox { Scenario {
    "gui.cut_off_sandbox_saves_its_state", { "gui", "session", "save", "ipc", "plugin" }, Needs::Engine | Needs::Gui,
    { "crash_state.vst3" }, {}, 60000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runCutOffSandboxSaves (host, ctx); }
} };
const ScenarioRegistrar crashedSandboxAsks { Scenario {
    "gui.crashed_sandbox_save_asks", { "gui", "session", "save", "ipc", "plugin" }, Needs::Engine | Needs::Gui,
    { "crash_state.vst3" }, {}, 60000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runCrashedSandboxAsks (host, ctx); }
} };
const ScenarioRegistrar freshSandboxCrashSaves { Scenario {
    "gui.fresh_sandbox_crash_saves_clean", { "gui", "session", "save", "ipc", "plugin" }, Needs::Engine | Needs::Gui,
    { "crash_state.vst3" }, {}, 60000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runFreshSandboxCrashSaves (host, ctx); }
} };
const ScenarioRegistrar captureFailureSaveAnyway { Scenario {
    "gui.capture_failure_save_anyway", { "gui", "session", "save", "ipc", "messages" }, Needs::Engine | Needs::Gui,
    { "crash_state.vst3" }, {}, 90000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runCaptureFailureSaveAnyway (host, ctx); }
} };
const ScenarioRegistrar recoverySaveFailure { Scenario {
    "gui.recovery_save_failure_stays_unsaved", { "gui", "session", "autosave", "quit" }, Needs::Engine | Needs::Gui,
    {}, {}, 30000, [] (GuiHost& host, ScenarioContext& ctx) { return runRecoverySaveFailure (host, ctx); }
} };
const ScenarioRegistrar nonAsciiAutosaveIdle { Scenario {
    "gui.autosave_idle_non_ascii", { "gui", "session", "autosave" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000, [] (GuiHost& host, ScenarioContext& ctx) { return runNonAsciiAutosaveIdle (host, ctx); }
} };

#if DUSKSTUDIO_HAS_NATIVE_CLAP
#define DUSK_GUI_CLAP_CASE(fn) [] (GuiHost& host, ScenarioContext& ctx) { return fn (host, ctx); }
#else
#define DUSK_GUI_CLAP_CASE(fn) [] (GuiHost&, ScenarioContext&) -> std::optional<ScenarioResult> \
    { return ScenarioResult::skip ("native CLAP is unavailable"); }
#endif

const ScenarioRegistrar clapEdits { Scenario {
    "gui.plugin_edits_prompt_clap", { "gui", "session", "plugin", "quit" }, Needs::Engine | Needs::Gui,
    { "param_touch.clap" }, {}, 60000, DUSK_GUI_CLAP_CASE (runClapEdits)
} };
const ScenarioRegistrar openStartsClean { Scenario {
    "gui.plugin_state_open_starts_clean", { "gui", "session", "plugin", "quit" }, Needs::Engine | Needs::Gui,
    { "param_touch.clap", "crash_state.vst3" }, {}, 60000, DUSK_GUI_CLAP_CASE (runOpenStartsClean)
} };
#undef DUSK_GUI_CLAP_CASE

const ScenarioRegistrar vst3Edits { Scenario {
    "gui.plugin_edits_prompt_vst3", { "gui", "session", "plugin", "quit" }, Needs::Engine | Needs::Gui,
    { "state_probe.vst3" }, {}, 30000,
#if DUSKSTUDIO_HAS_NATIVE_VST3
    [] (GuiHost& host, ScenarioContext& ctx) { return runVst3Edits (host, ctx); }
#else
    [] (GuiHost&, ScenarioContext&) -> std::optional<ScenarioResult> { return ScenarioResult::skip ("native VST3 is unavailable"); }
#endif
} };
const ScenarioRegistrar lv2Edits { Scenario {
    "gui.plugin_edits_prompt_lv2", { "gui", "session", "plugin", "quit" }, Needs::Engine | Needs::Gui,
    { "state_probe.lv2" }, {}, 30000,
#if DUSKSTUDIO_HAS_NATIVE_LV2
    [] (GuiHost& host, ScenarioContext& ctx) { return runLv2Edits (host, ctx); }
#else
    [] (GuiHost&, ScenarioContext&) -> std::optional<ScenarioResult> { return ScenarioResult::skip ("native LV2 is unavailable"); }
#endif
} };
const ScenarioRegistrar hostedEdits { Scenario {
    "gui.plugin_edits_prompt_hosted", { "gui", "session", "plugin", "quit" }, Needs::Engine | Needs::Gui,
    { "crash_state.vst3" }, {}, 60000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runHostedEdits (host, ctx, false); }
} };
const ScenarioRegistrar sandboxedEdits { Scenario {
    "gui.plugin_edits_prompt_sandboxed", { "gui", "session", "plugin", "quit", "ipc" }, Needs::Engine | Needs::Gui,
    { "crash_state.vst3" }, {}, 60000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runHostedEdits (host, ctx, true); }
} };
} // namespace
} // namespace duskstudio::scenario::gui
