#include "../GuiScenarioSupport.h"
#include "../../WheelScroll.h"
#include "../../../foundation/Fs.h"
#include "../../../engine/builtin/BuiltinRegistry.h"
#include "../../../engine/PluginSlot.h"
#include "../../../dsp/ChannelStrip.h"
#include "../../../session/SessionSerializer.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace duskstudio::scenario::gui
{
namespace
{
std::optional<ScenarioResult> runBuiltinMidiLearn (GuiHost& host, ScenarioContext& ctx, const std::string& unitId,
                                                   const std::string& control, float position, bool ownEditor = false)
{
   #if ! DUSKSTUDIO_HAS_NATIVE_UI
    return ScenarioResult::skip ("requires native UI");
   #endif
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    auto& strip = engine.getChannelStrip (0);
    if (! engine.getTransport().isStopped() || ! host.modalStackEmpty() || session.midiLearnPending.load() >= 0)
        return ScenarioResult::skip ("requires stopped transport, no modal and no pending learn");
    if (engine.getMidiInputDevices().empty()) return ScenarioResult::skip ("requires an enumerated MIDI input for injection");
    if (strip.getPluginSlot().isLoaded() || strip.isBuiltinLoaded() || strip.isNativeClapLoaded()
        || strip.isNativeLv2Loaded() || strip.isNativeVst3Loaded() || strip.isNativeAuLoaded()
        || strip.isNativeMultisampleLoaded() || strip.builtinReloadFailed())
        return ScenarioResult::skip ("requires an empty first insert");
    const bool hardwareInputs = engine.setHardwareMidiInputsEnabledForScenario (false);
    ctx.cleanup ([&engine, hardwareInputs] { engine.setHardwareMidiInputsEnabledForScenario (hardwareInputs); });
    const auto bindings = session.midiBindings.current();
    const auto capture = session.midiLearnCapture.load();
    const auto mode = strip.insertMode.load();
    const auto stage = engine.getStage();
    ctx.cleanup ([&host, &engine, &session, &strip, bindings, capture, mode, stage]
    {
        drainModals (host);
        host.closeBuiltin (0);
        engine.suspendProcessing();
        strip.unloadBuiltin();
        strip.insertMode.store (mode);
        engine.resumeProcessing();
        session.midiBindings.publish (std::make_unique<std::vector<MidiBinding>> (bindings));
        session.midiLearnPending.store (-1);
        session.midiLearnCapture.store (capture);
        if (auto* handle = host.strip (0)) handle->refreshInsertButton();
        host.switchToStage (stage == AudioEngine::Stage::Recording ? GuiHost::Stage::Recording
                            : stage == AudioEngine::Stage::Mixing ? GuiHost::Stage::Mixing
                            : stage == AudioEngine::Stage::Aux ? GuiHost::Stage::Aux : GuiHost::Stage::Mastering);
    });
    session.midiBindings.publish (std::make_unique<std::vector<MidiBinding>>());
    host.switchToStage (GuiHost::Stage::Mixing);
    std::string error;
    engine.suspendProcessing();
    const bool loaded = strip.loadBuiltin (unitId, error);
    if (loaded) strip.insertMode.store (ChannelStrip::kInsertPlugin);
    engine.resumeProcessing();
    if (! loaded) return ScenarioResult::fail ("could not load " + unitId + ": " + error);
    if (ownEditor && ! strip.getBuiltinSlot().hasPluginEditor())
        return ScenarioResult::fail (unitId + " brought no editor of its own");
    int paramIndex = -1;
    for (int i = 0; i < strip.getBuiltinSlot().paramCount(); ++i)
        if (const auto* info = strip.getBuiltinSlot().paramInfo (i);
            info != nullptr && info->id != nullptr && control == info->id) paramIndex = i;
    if (paramIndex < 0) return ScenarioResult::fail (unitId + " has no " + control + " parameter");
    const float before = strip.getBuiltinSlot().getParamValue (paramIndex);
    if (auto* handle = host.strip (0)) handle->refreshInsertButton();
    ctx.expect (strip.insertLastTouchedParamIndex() == -1, "fresh unit already has a touched parameter");
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 250, [&host, &ctx]
    { ctx.expect (host.clickInsert (0, false), "insert button unavailable"); } });
    steps->push_back ({ 600, [&host, &ctx, control, position]
    { ctx.expect (host.builtinPointer (0, control, position, true), "native " + control + " pointer down failed"); } });
    steps->push_back ({ 150, [&host, &ctx, control, position]
    { ctx.expect (host.builtinPointer (0, control, position, false), "native " + control + " pointer up failed"); } });
    steps->push_back ({ 150, [&host, &ctx, &strip, control, paramIndex, before]
    {
        ctx.expect (std::abs (strip.getBuiltinSlot().getParamValue (paramIndex) - before) > 0.05f,
                    control + " control did not change parameter");
        ctx.expect (strip.insertLastTouchedParamIndex() == paramIndex, control + " was not tracked as last touched");
        host.closeBuiltin (0);
    } });
    steps->push_back ({ 200, [&host, &ctx]
    { ctx.expect (host.clickInsert (0, true), "insert context menu unavailable"); } });
    steps->push_back ({ 150, [&host, &ctx]
    { ctx.expect (host.clickContextMenuItem ("MIDI Learn last-touched parameter"), "last-touched Learn action unavailable"); } });
    steps->push_back ({ 150, [&host, &ctx]
    { ctx.expect (host.clickContextMenuItem ("MIDI Learn (this track)..."), "track Learn action unavailable"); } });
    steps->push_back ({ 150, [&engine, &session, &ctx]
    {
        const auto pending = session.midiLearnPending.load();
        ctx.expect (pending >= 0 && unpackLearnTargetKind (pending) == MidiBindingTarget::TrackPluginParam
                    && unpackLearnTargetIndex (pending) == 0, "Learn did not target the first insert");
        dusk::MidiBuffer midi;
        const std::uint8_t cc[] = { 0xb2, 74, 96 };
        midi.addEvent (cc, 3, 0);
        engine.stageTestMidiInjection (0, std::move (midi));
    } });
    runSteps (ctx, steps, [&ctx, &session, control, paramIndex]
    {
        ctx.waitUntil ([&session] { return session.midiLearnPending.load() < 0; }, 3000,
            [&ctx, &session, control, paramIndex]
            {
                const auto& bindingsNow = session.midiBindings.current();
                ctx.expect (std::any_of (bindingsNow.begin(), bindingsNow.end(), [paramIndex] (const MidiBinding& binding)
                {
                    return binding.channel == 3 && binding.dataNumber == 74 && binding.trigger == MidiBindingTrigger::CC
                        && binding.target == MidiBindingTarget::TrackPluginParam && binding.targetIndex == 0
                        && binding.paramIndex == paramIndex;
                }), "captured CC did not bind the last-touched " + control + " parameter");
                ctx.complete (ctx.verdict());
            }, "MIDI Learn did not consume the injected CC");
    });
    return std::nullopt;
}

const ScenarioRegistrar builtinMidiLearn { Scenario {
    "gui.builtin_midi_learn", { "gui", "midi" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runBuiltinMidiLearn (host, ctx, "dusk.builtin.utility", "gain_db", 0.25f); }
} };

// DuskVerb 2 brings an editor of its own, so the touch comes through the
// callbacks that editor was handed rather than the knob panel's.
const ScenarioRegistrar duskverbMidiLearn { Scenario {
    "gui.duskverb_midi_learn", { "gui", "midi" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) -> std::optional<ScenarioResult>
    {
        if (! host.canEmbedPluginEditors())
            return ScenarioResult::skip ("requires a window the unit's editor can embed into");
        return runBuiltinMidiLearn (host, ctx, "dusk.builtin.reverb", "mix", 0.8f, true);
    }
} };

// Sunset is an instrument, and its editor is the plug-in's own too.
const ScenarioRegistrar sunsetMidiLearn { Scenario {
    "gui.sunset_midi_learn", { "gui", "midi" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) -> std::optional<ScenarioResult>
    {
        if (! host.canEmbedPluginEditors())
            return ScenarioResult::skip ("requires a window the unit's editor can embed into");
        return runBuiltinMidiLearn (host, ctx, "dusk.builtin.synth", "filterCutoff", 0.25f, true);
    }
} };

// ------------------------------------------- MIDI Learn on a native host

// The manual promises "MIDI Learn last-touched parameter" for every native
// host, and the touch has to come from the plug-in side: a CLAP announces it on
// its output event queue, a VST3 through IComponentHandler::performEdit, an LV2
// through its UI's write function. Each fixture raises the real event, so what
// runs here is the host's own resolve path - the slot's last-touched tracker,
// the strip's precedence between hosts, and the transport bar's timer turning
// the next control moved into a binding that names the parameter.
struct NativeLearnSpec
{
    Format format;
    const char* fixture;
    const char* pluginId;
    // Deliberately not the plug-in's first parameter: a host that reports index
    // 0 for every touch would otherwise pass.
    const char* paramName;
};

int nativeParamIndex (ChannelStrip& strip, Format format, const std::string& name)
{
    const auto find = [&name] (const auto& slot)
    {
        for (int i = 0; i < slot.paramCount(); ++i)
            if (const auto* info = slot.paramInfo (i); info != nullptr && info->name == name)
                return i;
        return -1;
    };
    switch (format)
    {
        case Format::Clap:
           #if DUSKSTUDIO_HAS_NATIVE_CLAP
            return find (strip.getNativeClapSlot());
           #else
            break;
           #endif
        case Format::Lv2:
           #if DUSKSTUDIO_HAS_NATIVE_LV2
            return find (strip.getNativeLv2Slot());
           #else
            break;
           #endif
        case Format::Vst3:
           #if DUSKSTUDIO_HAS_NATIVE_VST3
            return find (strip.getNativeVst3Slot());
           #else
            break;
           #endif
    }
    (void) strip;
    return -1;
}

bool loadNativeSpec (StripHandle& handle, const NativeLearnSpec& spec,
                     const std::filesystem::path& file, std::string& errorOut)
{
    switch (spec.format)
    {
        case Format::Clap: return handle.loadNativeClap (file, spec.pluginId, errorOut);
        case Format::Lv2:  return handle.loadNativeLv2  (file, spec.pluginId, errorOut);
        case Format::Vst3: return handle.loadNativeVst3 (file, spec.pluginId, errorOut);
    }
    return false;
}

std::optional<ScenarioResult> runNativeMidiLearn (GuiHost& host, ScenarioContext& ctx,
                                                  const NativeLearnSpec& spec)
{
    // Static so the lambdas below can name them without a capture; MSVC
    // rejects an uncaptured block-scope constant even when it is not odr-used.
    static constexpr std::uint8_t kLearnStatus = 0xb4;   // CC, channel 5
    static constexpr std::uint8_t kLearnCc     = 22;
    static constexpr std::uint8_t kOtherCc     = 23;

    auto& engine = ctx.engine();
    auto& session = ctx.session();
    auto& strip = engine.getChannelStrip (kStripIndex);

    if (! engine.getTransport().isStopped() || ! host.modalStackEmpty()
        || session.midiLearnPending.load() >= 0)
        return ScenarioResult::skip ("requires stopped transport, no modal and no pending learn");
    if (engine.getMidiInputDevices().empty())
        return ScenarioResult::skip ("requires an enumerated MIDI input for injection");

    const auto fixture = ctx.fixture (spec.fixture);
    if (! fixture)
        return ScenarioResult::skip (std::string ("missing fixture: ") + spec.fixture);

    auto* handle = readyStrip (host, ctx);
    if (handle == nullptr)
        return ScenarioResult::skip ("the console has no strip to drive");

    const bool hardwareInputs = engine.setHardwareMidiInputsEnabledForScenario (false);
    ctx.cleanup ([&engine, hardwareInputs] { engine.setHardwareMidiInputsEnabledForScenario (hardwareInputs); });
    const auto bindings = session.midiBindings.current();
    const auto capture  = session.midiLearnCapture.load();
    const auto mode     = session.track (kStripIndex).mode.load();
    ctx.cleanup ([&host, &session, handle, bindings, capture, mode]
    {
        drainModals (host);
        handle->closeEditor();
        handle->unloadNativePlugins();
        handle->refreshInsertButton();
        session.track (kStripIndex).mode.store (mode);
        session.midiBindings.publish (std::make_unique<std::vector<MidiBinding>> (bindings));
        session.midiLearnPending.store (-1);
        session.midiLearnCapture.store (capture);
    });

    session.midiBindings.publish (std::make_unique<std::vector<MidiBinding>>());
    // A MIDI track runs its insert every block whatever the input routing is,
    // which is what the CLAP fixture needs to get its announcement out.
    session.track (kStripIndex).mode.store ((int) Track::Mode::Midi);

    std::string error;
    if (! loadNativeSpec (*handle, spec, *fixture, error))
        return ScenarioResult::fail ("the fixture did not load: " + error);
    handle->refreshInsertButton();
    ctx.expect (strip.insertLastTouchedParamIndex() == -1,
                "a freshly loaded slot already reports a touched parameter");

    auto expected = std::make_shared<int> (-1);
    auto steps = std::make_shared<std::vector<Step>>();

    steps->push_back ({ 250, [&ctx, &host, &strip, handle, spec, expected]
    {
        *expected = nativeParamIndex (strip, spec.format, spec.paramName);
        if (! ctx.expect (*expected >= 0,
                          std::string ("the fixture exposes no parameter named ") + spec.paramName))
            return;

        if (spec.format == Format::Lv2)
        {
            // The LV2 touch is the plug-in's own UI announcing a property the
            // moment it comes up, so opening the editor is the gesture.
            if (! handle->openEditor())
            {
                dismissAlert (host);
                ctx.complete (ScenarioResult::skip (
                    "the LV2 fixture UI could not be embedded on this display"));
                return;
            }
            return;
        }

       #if DUSKSTUDIO_HAS_NATIVE_CLAP
        if (spec.format == Format::Clap)
        {
            auto& slot = strip.getNativeClapSlot();
            if (const auto* info = slot.paramInfo (*expected)) slot.setParamValue (info->id, 0.8);
        }
       #endif
       #if DUSKSTUDIO_HAS_NATIVE_VST3
        if (spec.format == Format::Vst3)
        {
            auto& slot = strip.getNativeVst3Slot();
            if (const auto* info = slot.paramInfo (*expected)) slot.setParamValue (info->id, 0.8);
        }
       #endif
    } });

    steps->push_back ({ 400, [&ctx, &host, &strip, handle, spec, expected]
    {
        ctx.expect (strip.insertLastTouchedParamIndex() == *expected,
                    std::string ("the host did not track ") + spec.paramName
                        + " as the last touched parameter");
        if (spec.format == Format::Lv2) handle->closeEditor();
        drainModals (host);
    } });

    steps->push_back ({ 250, [&host, &ctx]
    { ctx.expect (host.clickInsert (kStripIndex, true), "insert context menu unavailable"); } });
    steps->push_back ({ 150, [&host, &ctx]
    { ctx.expect (host.clickContextMenuItem ("MIDI Learn last-touched parameter"),
                  "last-touched Learn action unavailable"); } });
    steps->push_back ({ 150, [&host, &ctx]
    { ctx.expect (host.clickContextMenuItem ("MIDI Learn (this track)..."),
                  "track Learn action unavailable"); } });
    steps->push_back ({ 150, [&engine, &session, &ctx]
    {
        const auto pending = session.midiLearnPending.load();
        ctx.expect (pending >= 0 && unpackLearnTargetKind (pending) == MidiBindingTarget::TrackPluginParam
                    && unpackLearnTargetIndex (pending) == kStripIndex,
                    "Learn did not target this track's insert");
        dusk::MidiBuffer midi;
        const std::uint8_t cc[] = { kLearnStatus, kLearnCc, 96 };
        midi.addEvent (cc, 3, 0);
        engine.stageTestMidiInjection (0, std::move (midi));
    } });

    runSteps (ctx, steps, [&ctx, &engine, &session, expected]
    {
        ctx.waitUntil ([&session] { return session.midiLearnPending.load() < 0; }, 5000,
            [&ctx, &engine, &session, expected]
            {
                const auto& bound = session.midiBindings.current();
                const auto matches = [expected] (const MidiBinding& b)
                {
                    return b.channel == 5 && b.dataNumber == kLearnCc
                        && b.trigger == MidiBindingTrigger::CC
                        && b.target == MidiBindingTarget::TrackPluginParam
                        && b.targetIndex == kStripIndex && b.paramIndex == *expected;
                };
                if (! ctx.expect (std::any_of (bound.begin(), bound.end(), matches),
                                  "the captured CC did not bind the last-touched parameter"))
                {
                    ctx.complete (ctx.verdict());
                    return;
                }

                // Learn is one-shot: the next controller moved drives whatever
                // it is already bound to and writes nothing new.
                dusk::MidiBuffer midi;
                const std::uint8_t cc[] = { kLearnStatus, kOtherCc, 40 };
                midi.addEvent (cc, 3, 0);
                engine.stageTestMidiInjection (0, std::move (midi));
                ctx.later (500, [&ctx, &session]
                {
                    const auto& after = session.midiBindings.current();
                    ctx.expect (after.size() == 1
                                && ! std::any_of (after.begin(), after.end(),
                                                  [] (const MidiBinding& b)
                                                  { return b.dataNumber == kOtherCc; }),
                                "a second control re-armed Learn instead of being ignored");
                    ctx.complete (ctx.verdict());
                });
            }, "MIDI Learn did not consume the injected CC");
    });
    return std::nullopt;
}

const ScenarioRegistrar clapMidiLearn { Scenario {
    "gui.clap_midi_learn", { "gui", "midi", "plugins" }, Needs::Engine | Needs::Gui,
    { "param_touch.clap" }, {}, 30000,
    [] (GuiHost& host, ScenarioContext& ctx) -> std::optional<ScenarioResult>
    {
       #if ! DUSKSTUDIO_HAS_NATIVE_CLAP
        (void) host; (void) ctx;
        return ScenarioResult::skip ("built without native CLAP hosting");
       #else
        return runNativeMidiLearn (host, ctx, { Format::Clap, "param_touch.clap",
                                                "studio.dusk.test.param-touch", "Beta" });
       #endif
    }
} };

const ScenarioRegistrar lv2MidiLearn { Scenario {
    "gui.lv2_midi_learn", { "gui", "midi", "plugins", "lv2" }, Needs::Engine | Needs::Gui,
    { "file_state.lv2" }, {}, 30000,
    [] (GuiHost& host, ScenarioContext& ctx) -> std::optional<ScenarioResult>
    {
       #if ! DUSKSTUDIO_HAS_NATIVE_LV2
        (void) host; (void) ctx;
        return ScenarioResult::skip ("built without native LV2 hosting");
       #else
        // A patch property, which is how a JUCE-built LV2 exposes every one of
        // its parameters - the case the manual calls out by name.
        return runNativeMidiLearn (host, ctx, { Format::Lv2, "file_state.lv2",
                                                "urn:duskstudio:test:file-state", "mu" });
       #endif
    }
} };

const ScenarioRegistrar vst3MidiLearn { Scenario {
    "gui.vst3_midi_learn", { "gui", "midi", "plugins", "vst3" }, Needs::Engine | Needs::Gui,
    { "relayout.vst3" }, {}, 30000,
    [] (GuiHost& host, ScenarioContext& ctx) -> std::optional<ScenarioResult>
    {
       #if ! DUSKSTUDIO_HAS_NATIVE_VST3
        (void) host; (void) ctx;
        return ScenarioResult::skip ("built without native VST3 hosting");
       #else
        return runNativeMidiLearn (host, ctx, { Format::Vst3, "relayout.vst3", "",
                                                "Touch Report" });
       #endif
    }
} };

std::optional<ScenarioResult> runSoundfontConversion (GuiHost& host, ScenarioContext& ctx,
                                                    bool replaceWithHardware = false)
{
   #if ! DUSKSTUDIO_HAS_MULTISAMPLE
    (void) host;
    (void) ctx;
    return ScenarioResult::skip ("built without multisample instruments");
   #else
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    auto& track = session.track (0);
    auto& strip = engine.getChannelStrip (0);
    if (! engine.getTransport().isStopped()) return ScenarioResult::skip ("requires stopped transport");
    if (track.mode.load() == (int) Track::Mode::Midi || track.midiInputIndex.load() >= 0)
        return ScenarioResult::skip ("requires an audio track with no MIDI input");
    if (strip.getPluginSlot().isLoaded()) return ScenarioResult::skip ("requires an empty standard plugin slot");
    const auto originalDir = currentSessionDirectory (session);
    const auto originalStage = engine.getStage();
    const auto originalMode = track.mode.load();
    const auto originalMidiInput = track.midiInputIndex.load();
    const auto originalMonitor = track.inputMonitor.load();
    const auto rendering = std::make_shared<bool> (false);
    const auto restore = ctx.tempDir() / "restore.json";
    const auto soundfont = ctx.tempDir() / "Conversion fixture.sfz";
    const auto bank = ctx.tempDir() / "Conversion fixture.bank.xml";
    if (! SessionSerializer::save (session, restore)
        || ! dusk::fs::writeStringToFile (soundfont, "<region> key=60 sample=*sine\n")
        || ! dusk::fs::writeStringToFile (bank,
            "<AriaBank name=\"Conversion\"><AriaProgram name=\"Sine\">"
            "<AriaElement path=\"Conversion fixture.sfz\"/></AriaProgram></AriaBank>"))
        return ScenarioResult::fail ("could not prepare the soundfont fixture");
    ctx.cleanup ([&host, &engine, &session, originalDir, originalStage, restore, rendering]
    {
        host.closeSfzLibrary (0);
        if (auto* component = host.strip (0)) component->closeEditor();
        drainModals (host);
        if (*rendering) { engine.setAssetRenderActive (false); *rendering = false; }
        host.openSession (restore);
        applySessionDirectory (session, originalDir);
        host.switchToStage (guiStage (originalStage));
    });
    if (readyStrip (host, ctx) == nullptr) return ScenarioResult::fail ("channel strip is unavailable");
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 150, [&host, &ctx]
    { ctx.expect (host.clickInsert (0), "insert button did not receive a click"); } });
   #if DUSKSTUDIO_HAS_NATIVE_UI
    steps->push_back ({ 150, [&host, &ctx]
    {
        ctx.expect (! host.clickModalButton ("Soundfont (.sfz / .sf2 / .bank.xml)"),
                    "insert chooser retained the redundant Soundfont option");
        ctx.expect (host.clickModalButton ("SFZ Manager..."), "SFZ Manager did not open");
    } });
    steps->push_back ({ 0, [&host, &ctx]
    { ctx.expect (host.sfzLibraryPointer (0, "local-folders", 0.5f, true), "local folder button press failed"); },
    [&host] { return host.sfzLibraryPointer (0, "local-folders", 0.5f, false); },
    "SFZ Manager did not offer local folders" });
    steps->push_back ({ 0, [&host, &ctx]
    { ctx.expect (host.sfzLibraryPointer (0, "local-folders", 0.5f, false), "local folder button release failed"); } });
    steps->push_back ({ 0, [] {}, [&host] { return host.clickModalButton ("Open local file..."); },
                        "local library did not offer a file browser" });
   #else
    steps->push_back ({ 150, [&host, &ctx]
    { ctx.expect (host.clickModalButton ("Soundfont (.sfz / .sf2 / .bank.xml)"), "soundfont chooser did not open"); } });
   #endif
    steps->push_back ({ 150, [&host, &ctx, bank]
    {
        expectBrowserInHome (host, ctx, "the soundfont browser");
        ctx.expect (host.focusFileName(), "soundfont filename entry was not available");
       #if defined (__APPLE__)
        host.pressPeerKey ("command + A", 'a');
       #else
        host.pressPeerKey ("ctrl + A", 'a');
       #endif
        for (const char ch : bank.string())
            host.pressPeerKey (ch == ' ' ? "Space" : std::string (1, ch), ch);
    } });
    steps->push_back ({ 200, [&host, &ctx, &engine, rendering, replaceWithHardware]
    {
        if (replaceWithHardware) { engine.setAssetRenderActive (true); *rendering = true; }
        ctx.expect (host.clickModalButton ("Open"), "Open did not accept the soundfont path");
    } });
    if (replaceWithHardware)
    {
        steps->push_back ({ 0, [&host, &ctx, &engine]
        {
            ctx.expect (engine.hasPendingMultisampleLoads(), "the SFZ request was not pending before replacement");
            host.closeSfzLibrary (0);
        } });
        steps->push_back ({ 0, [&host, &ctx]
        { ctx.expect (host.clickInsert (0), "the pending SFZ load blocked the insert chooser"); },
        [&host] { return ! host.sfzLibraryOpen (0) && host.modalStackEmpty(); },
        "closing SFZ Manager did not restore the strip controls" });
        steps->push_back ({ 0, [&host, &ctx]
        { ctx.expect (host.clickModalButton ("Hardware Insert"), "the insert chooser did not offer hardware"); },
        [&host] { return ! host.modalStackEmpty(); }, "the insert chooser did not open" });
        steps->push_back ({ 0, [&ctx, &engine, &strip, rendering]
        {
            ctx.expect (strip.insertMode.load() == ChannelStrip::kInsertHardware,
                        "the hardware selection did not change the insert mode");
            engine.setAssetRenderActive (false); *rendering = false;
        } });
        steps->push_back ({ 0, [&ctx, &track, &strip, originalMode, originalMidiInput, originalMonitor]
        {
            ctx.expect (strip.insertMode.load() == ChannelStrip::kInsertHardware,
                        "the cancelled SFZ completion replaced the hardware insert");
            ctx.expect (! strip.isNativeMultisampleLoaded() && track.nativeMultisamplePath.isEmpty()
                        && ! track.nativeMultisampleLocator,
                        "the cancelled SFZ completion installed its instrument");
            ctx.expect (track.mode.load() == originalMode && track.midiInputIndex.load() == originalMidiInput
                        && track.inputMonitor.load() == originalMonitor,
                        "the cancelled SFZ completion converted the audio track to MIDI");
        }, [&engine]
        {
            engine.serviceMultisampleLoads();
            return ! engine.hasPendingMultisampleLoads();
        }, "the superseded SFZ worker request did not drain" });
        runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
        return std::nullopt;
    }
    steps->push_back ({ 1200, [&ctx, &engine, &track, &strip, soundfont]
    {
        ctx.expect (strip.isNativeMultisampleLoaded(), "soundfont was not loaded");
        ctx.expect (! strip.nativeMultisampleReloadFailed(), "soundfont reported a load failure");
        ctx.expect (strip.getNativeMultisampleSlot().getLoadedSoundfontPath() == soundfont.string(),
                    "loaded soundfont path differs from the chosen file");
        ctx.expect (track.nativeMultisamplePath.toStdString() == soundfont.string(),
                    "session did not retain the soundfont path");
        ctx.expect (track.mode.load() == (int) Track::Mode::Midi, "audio track did not convert to MIDI");
        ctx.expect (engine.getVirtualKeyboardInputIndex() >= 0
                    && track.midiInputIndex.load() == engine.getVirtualKeyboardInputIndex(),
                    "converted track did not select the virtual keyboard");
        ctx.expect (track.inputMonitor.load(), "converted track did not enable input monitoring");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
   #endif
}

const ScenarioRegistrar soundfontConversion { Scenario {
    "gui.soundfont_conversion", { "gui", "plugins", "midi" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runSoundfontConversion (host, ctx); }
} };

const ScenarioRegistrar soundfontInsertSuperseded { Scenario {
    "gui.soundfont_insert_superseded", { "gui", "plugins", "midi", "multisample" }, Needs::Engine | Needs::Gui,
    {}, {}, 20000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runSoundfontConversion (host, ctx, true); }
} };

std::optional<ScenarioResult> runPluginBrowseFile (GuiHost& host, ScenarioContext& ctx)
{
    const auto fixture = ctx.fixture ("relayout.vst3");
    if (! fixture) return ScenarioResult::skip ("requires the VST3 fixture");
    auto& engine = ctx.engine();
    if (! engine.getTransport().isStopped()) return ScenarioResult::skip ("requires stopped transport");
    auto& session = ctx.session();
    auto& slot = engine.getChannelStrip (0).getPluginSlot();
    if (slot.isLoaded()) return ScenarioResult::skip ("requires an empty standard plugin slot");
    const auto originalDir = currentSessionDirectory (session);
    const auto originalStage = engine.getStage();
    const auto restore = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restore))
        return ScenarioResult::fail ("could not save the initial session");
    ctx.cleanup ([&host, &session, originalDir, originalStage, restore]
    {
        if (auto* strip = host.strip (0)) strip->closeEditor();
        drainModals (host);
        host.openSession (restore);
        applySessionDirectory (session, originalDir);
        host.switchToStage (guiStage (originalStage));
    });
    if (readyStrip (host, ctx) == nullptr) return ScenarioResult::fail ("channel strip is unavailable");
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 150, [&host, &ctx]
    { ctx.expect (host.clickInsert (0), "insert button did not receive a click"); } });
    steps->push_back ({ 150, [&host, &ctx]
    { ctx.expect (host.clickModalButton ("Plugin (VST3 / CLAP / LV2 / AU)"), "insert chooser did not open"); } });
    steps->push_back ({ 150, [&host, &ctx]
    { ctx.expect (host.clickModalButton ("Browse file..."), "picker did not offer Browse file"); } });
    steps->push_back ({ 150, [&host, &ctx, fixture]
    {
        ctx.expect (host.focusFileName(), "file browser filename entry was not available");
       #if defined (__APPLE__)
        host.pressPeerKey ("command + A", 'a');
       #else
        host.pressPeerKey ("ctrl + A", 'a');
       #endif
        for (const char ch : fixture->string())
            host.pressPeerKey (ch == ' ' ? "Space" : std::string (1, ch), ch);
    } });
    steps->push_back ({ 200, [&host, &ctx]
    { ctx.expect (host.clickModalButton ("Open"), "Open did not accept the fixture path"); } });
    steps->push_back ({ 1200, [&host, &ctx, &slot]
    {
        ctx.expect (slot.isLoaded(), "Browse file did not load a plugin");
        ctx.expect (slot.isLoadedStandardVst3(), "Browse file did not use the VST3 loader");
        ctx.expect (slot.getLoadedName().toStdString() == "Dusk Runtime Relayout Fixture",
                    "Browse file loaded a different plugin");
        ctx.expect (! slot.isLoadedPluginInstrument(), "effect Browse loaded an instrument");
        ctx.expect (! host.focusFileName(), "file browser remained open after accepting the path");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar pluginBrowseFile { Scenario {
    "gui.plugin_browse_file", { "gui", "plugins" }, Needs::Engine | Needs::Gui,
    { "relayout.vst3" }, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runPluginBrowseFile (host, ctx); }
} };

std::optional<ScenarioResult> runPluginPicker (GuiHost& host, ScenarioContext& ctx)
{
    const auto fixture = ctx.fixture ("multi_bus.clap");
    const auto descriptions = ctx.engine().getPluginManager().getClapEffectDescriptions();
    if (! fixture || std::none_of (descriptions.begin(), descriptions.end(), [] (const auto& row)
        { return row.name == "Picker Alpha"; }))
        return ScenarioResult::skip ("requires the seeded plugin-picker cache");
    auto& session = ctx.session();
    auto& engine = ctx.engine();
    const auto originalDir = currentSessionDirectory (session);
    const auto originalStage = engine.getStage();
    const auto restore = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restore))
        return ScenarioResult::fail ("could not save the initial session");
    ctx.cleanup ([&host, &session, originalDir, originalStage, restore]
    {
        drainModals (host);
        if (auto* strip = host.strip (0)) { strip->closeEditor(); strip->unloadNativePlugins(); }
        host.openSession (restore);
        applySessionDirectory (session, originalDir);
        host.switchToStage (guiStage (originalStage));
    });
    if (readyStrip (host, ctx) == nullptr) return ScenarioResult::fail ("channel strip is unavailable");
    const auto filter = [&host, &ctx] (const std::string& value)
    {
        ctx.expect (host.clickModalAt (0.3f, 0.1f), "picker filter did not receive focus");
       #if defined (__APPLE__)
        host.pressPeerKey ("command + A", 'a');
       #else
        host.pressPeerKey ("ctrl + A", 'a');
       #endif
        host.pressPeerKey ("Backspace");
        for (const char ch : value) host.pressPeerKey (std::string (1, ch), ch);
    };
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 150, [&host, &ctx]
    { ctx.expect (host.clickInsert (0), "insert button did not receive a click"); } });
    steps->push_back ({ 150, [&host, &ctx]
    { ctx.expect (host.clickModalButton ("Plugin (VST3 / CLAP / LV2 / AU)"), "insert chooser did not open"); } });
    steps->push_back ({ 150, [&host, &ctx, filter]
    {
        const auto headers = host.pickerRows (true);
        ctx.expect (! headers.empty() && headers.front() == "BUILT-IN", "built-in group was not first");
        filter ("picker");
    } });
    steps->push_back ({ 150, [&host, &ctx, filter]
    {
        ctx.expect (host.pickerRows (true) == std::vector<std::string> { "SCENARIO MAKER A", "SCENARIO MAKER B" },
                    "manufacturer headings did not group the matching plugins");
       #if DUSKSTUDIO_HAS_NATIVE_LV2
        constexpr const char* betaRow = "Picker Beta  (LV2-Native)";
       #else
        constexpr const char* betaRow = "Picker Beta  (CLAP)";
       #endif
        ctx.expect (host.pickerRows (false) == std::vector<std::string> { "Picker Alpha  (CLAP)", betaRow },
                    "filtered plugin rows omitted names or format labels");
        filter ("pIcKeR aLpHa");
    } });
    steps->push_back ({ 150, [&host, &ctx, filter]
    {
        ctx.expect (host.pickerRows (false) == std::vector<std::string> { "Picker Alpha  (CLAP)" },
                    "name filter was not case insensitive");
        filter ("no-such-picker-plugin");
    } });
    steps->push_back ({ 150, [&host, &ctx, filter]
    {
        ctx.expect (host.pickerRows (true).empty() && host.pickerRows (false).empty(),
                    "unmatched filter retained rows or empty groups");
        filter ("picker");
        ctx.expect (host.clickModalButton ("Group: Maker"), "grouping button was missing");
    } });
    steps->push_back ({ 150, [&host, &ctx, filter]
    {
        ctx.expect (host.pickerRows (true) == std::vector<std::string> { "DELAY", "EQ" },
                    "type grouping did not use plugin categories");
        filter ("");
    } });
    steps->push_back ({ 150, [&host, &ctx, filter]
    {
        const auto headers = host.pickerRows (true);
        ctx.expect (! headers.empty() && headers.front() == "BUILT-IN", "type grouping displaced built-in units");
        filter ("Picker Alpha");
    } });
    steps->push_back ({ 150, [&host, &ctx]
    { ctx.expect (host.clickPickerRow ("Picker Alpha  (CLAP)"), "plugin row did not receive a click"); } });
    steps->push_back ({ 500, [&host, &ctx, &engine, &session, fixture]
    {
        ctx.expect (host.pickerRows (true).empty() && host.pickerRows (false).empty(),
                    "picking a plugin did not dismiss the picker");
        ctx.expect (engine.getChannelStrip (0).isNativeClapLoaded(), "picker did not load the native CLAP instance");
        ctx.expect (session.track (0).nativeClapPath.toStdString() == fixture->string()
                    && session.track (0).nativeClapPluginId.toStdString() == "studio.dusk.test.multi-bus",
                    "the picked plugin identity was not saved to the track");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar pluginPicker { Scenario {
    "gui.plugin_picker_filter_and_load", { "gui", "plugins" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runPluginPicker (host, ctx); }
} };

// ------------------------------------------------ a pick opens the editor

// What a user does to get a unit on the insert: the chooser, then a row in the
// picker. The editor has to be up afterwards with no second click, for a
// knob-panel unit, for a unit that brings its own editor picked over it with
// Replace insert..., and not again when the session holding it is reopened.
std::optional<ScenarioResult> runPickOpensBuiltinEditor (GuiHost& host, ScenarioContext& ctx)
{
   #if ! DUSKSTUDIO_HAS_NATIVE_UI
    (void) host;
    (void) ctx;
    return ScenarioResult::skip ("requires the native UI");
   #else
    static constexpr const char* kPanelUnit  = "dusk.builtin.utility";
    static constexpr const char* kEditorUnit = "dusk.builtin.delay";
    static const std::string kPanelRow  = "Utility  (Built-In)";
    static const std::string kEditorRow = "Tape Echo 2  (Built-In)";
    static const std::string kPluginButton = "Plugin (VST3 / CLAP / LV2 / AU)";

    if (builtin::findUnit (kPanelUnit) == nullptr || builtin::findUnit (kEditorUnit) == nullptr)
        return ScenarioResult::skip ("built without Utility and Tape Echo 2");
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    auto& dsp = engine.getChannelStrip (kStripIndex);
    if (! engine.getTransport().isStopped() || ! host.modalStackEmpty())
        return ScenarioResult::skip ("requires stopped transport and no modal");
    const auto originalDir = currentSessionDirectory (session);
    const auto restore = ctx.tempDir() / "restore.json";
    const auto picked = ctx.tempDir() / "picked" / "session.json";
    std::error_code ec;
    std::filesystem::create_directories (picked.parent_path(), ec);
    if (! SessionSerializer::save (session, restore))
        return ScenarioResult::fail ("could not save the initial session");
    auto* strip = readyStrip (host, ctx);
    if (strip == nullptr) return ScenarioResult::fail ("channel strip is unavailable");
    if (dsp.getPluginSlot().isLoaded() || dsp.isBuiltinLoaded() || dsp.isNativeClapLoaded()
        || dsp.isNativeLv2Loaded() || dsp.isNativeVst3Loaded() || dsp.isNativeAuLoaded()
        || dsp.isNativeMultisampleLoaded() || dsp.nativeInsertRestoreFailed()
        || dsp.insertMode.load() == ChannelStrip::kInsertHardware)
        return ScenarioResult::skip ("requires an empty first insert");
    ctx.cleanup ([&host, &engine, &session, &dsp, originalDir, restore]
    {
        drainModals (host);
        host.closeBuiltin (kStripIndex);
        engine.suspendProcessing();
        dsp.unloadBuiltin();
        engine.resumeProcessing();
        reopenSavedSession (host, restore);
        applySessionDirectory (session, originalDir);
    });

    const auto editorUp = [&host] { auto* s = host.strip (kStripIndex); return s != nullptr && s->hasOpenBuiltinEditor(); };
    const auto rowShown = [&host] (const std::string& row)
    {
        const auto rows = host.pickerRows (false);
        return std::find (rows.begin(), rows.end(), row) != rows.end();
    };
    const auto holds = [&dsp] (const char* unitId)
    { return dsp.isBuiltinLoaded() && dsp.getBuiltinSlot().getPluginId() == unitId; };

    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 0, [&host, &ctx]
    { ctx.expect (host.clickInsert (kStripIndex), "the empty insert did not take a click"); } });
    steps->push_back ({ 0, [&host, &ctx]
    { ctx.expect (host.clickModalButton (kPluginButton), "the insert chooser has no Plugin button"); },
                       [&host] { return ! host.modalStackEmpty(); }, "clicking the empty insert opened no chooser" });
    steps->push_back ({ 0, [&host, &ctx]
    { ctx.expect (host.clickPickerRow (kPanelRow), "the picker row for Utility took no click"); },
                       [rowShown] { return rowShown (kPanelRow); }, "the picker never listed Utility" });
    steps->push_back ({ 0, [&host, &ctx, holds]
    {
        ctx.expect (holds (kPanelUnit), "picking Utility did not load it");
        ctx.expect (host.pickerRows (false).empty(), "the picker stayed up after the pick");
        host.closeBuiltin (kStripIndex);
    }, editorUp, "picking Utility left its editor closed" });

    steps->push_back ({ 0, [&host, &ctx]
    { ctx.expect (host.clickInsert (kStripIndex, true), "the loaded insert took no right-click"); },
                       [editorUp] { return ! editorUp(); }, "Utility's editor did not close" });
    steps->push_back ({ 0, [&host, &ctx]
    { ctx.expect (host.clickContextMenuItem ("Replace insert..."), "the insert menu has no Replace insert..."); },
                       [&host] { return ! host.contextMenuItems().empty(); }, "the right-click opened no menu" });
    steps->push_back ({ 0, [&host, &ctx]
    { ctx.expect (host.clickModalButton (kPluginButton), "the Replace chooser has no Plugin button"); },
                       [&host] { return ! host.modalStackEmpty(); }, "Replace insert... opened no chooser" });
    steps->push_back ({ 0, [&host, &ctx]
    { ctx.expect (host.clickPickerRow (kEditorRow), "the picker row for Tape Echo 2 took no click"); },
                       [rowShown] { return rowShown (kEditorRow); }, "the picker never listed Tape Echo 2" });
    steps->push_back ({ 0, [&host, &ctx, &session, holds, picked]
    {
        ctx.expect (holds (kEditorUnit), "Replace did not put Tape Echo 2 in the insert");
        ctx.expect (SessionSerializer::save (session, picked), "could not save the session holding the pick");
        host.closeBuiltin (kStripIndex);
    }, editorUp, "replacing Utility with Tape Echo 2 left its editor closed" });

    // Emptied first, so the reopened unit arriving on the insert marks the
    // moment the reopen has been through the strip.
    steps->push_back ({ 0, [&host, &engine, &dsp]
    {
        engine.suspendProcessing();
        dsp.unloadBuiltin();
        engine.resumeProcessing();
        if (auto* s = host.strip (kStripIndex)) s->refreshInsertButton();
    }, [editorUp] { return ! editorUp(); }, "Tape Echo 2's editor did not close" });
    steps->push_back ({ 0, [&host, picked] { reopenSavedSession (host, picked); },
                       [&host]
                       {
                           auto* s = host.strip (kStripIndex);
                           return s != nullptr && s->insertLabel() == "Insert";
                       }, "the emptied insert never read Insert" });
    steps->push_back ({ 0, [&host, &ctx, editorUp]
    {
        ctx.expect (! editorUp(), "reopening the session opened the unit's editor");
        ctx.expect (! host.strip (kStripIndex)->hasOpenEditor(), "reopening the session opened a plug-in editor");
        ctx.expect (host.modalStackEmpty(), "reopening the session left a modal up");
    },
    [&host, holds]
    {
        auto* s = host.strip (kStripIndex);
        return holds (kEditorUnit) && s != nullptr
            && s->insertLabel().find ("Tape Echo 2") != std::string::npos;
    }, "the reopened session never showed Tape Echo 2 on the insert" });

    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
   #endif
}

const ScenarioRegistrar pickOpensBuiltinEditor { Scenario {
    "gui.pick_opens_builtin_editor", { "gui", "plugins" }, Needs::Engine | Needs::Gui,
    {}, {}, 30000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runPickOpensBuiltinEditor (host, ctx); }
} };

// The aux lane has no editor to open: the slot's unit draws under its header
// once it is there. Picking one from the lane's picker must still bring it up.
std::optional<ScenarioResult> runAuxPickShowsBuiltinEditor (GuiHost& host, ScenarioContext& ctx)
{
   #if ! DUSKSTUDIO_HAS_NATIVE_UI
    (void) host;
    (void) ctx;
    return ScenarioResult::skip ("requires the native UI");
   #else
    static constexpr const char* kUnit = "dusk.builtin.reverb";
    static const std::string kRow = "DuskVerb 2  (Built-In)";

    if (builtin::findUnit (kUnit) == nullptr)
        return ScenarioResult::skip ("built without DuskVerb 2");
    auto& engine = ctx.engine();
    if (! engine.getTransport().isStopped() || ! host.modalStackEmpty())
        return ScenarioResult::skip ("requires stopped transport and no modal");
    auto& strip = engine.getAuxLaneStrip (kAuxLane);
    if (strip.isBuiltinLoaded (kAuxSlot) || strip.getPluginSlot (kAuxSlot).isLoaded()
        || strip.isNativeClapLoaded (kAuxSlot) || strip.isNativeLv2Loaded (kAuxSlot)
        || strip.isNativeVst3Loaded (kAuxSlot) || strip.isNativeAuLoaded (kAuxSlot)
        || strip.nativeInsertRestoreFailed (kAuxSlot)
        || strip.insertMode[(std::size_t) kAuxSlot].load() == AuxLaneStrip::kInsertHardware)
        return ScenarioResult::skip ("requires an empty first aux insert");

    keepStage (host, ctx);
    ctx.keep (strip.insertMode[(std::size_t) kAuxSlot]);
    host.switchToStage (GuiHost::Stage::Aux);
    const int originalLane = host.activeAuxLane();
    auto& laneParams = ctx.session().auxLane (kAuxLane);
    ctx.cleanup ([&host, &engine, &strip, &laneParams, originalLane]
    {
        drainModals (host);
        engine.suspendProcessing();
        strip.unloadBuiltin (kAuxSlot);
        engine.resumeProcessing();
        laneParams.builtinUnitId[(std::size_t) kAuxSlot].clear();
        laneParams.builtinStateBase64[(std::size_t) kAuxSlot].clear();
        if (auto* lane = host.auxLane (kAuxLane))
        {
            lane->refreshSlot (kAuxSlot);
            lane->rebuildSlots();
        }
        if (originalLane >= 0) host.clickAuxSelector (originalLane);
    });
    if (host.activeAuxLane() != kAuxLane && ! host.clickAuxSelector (kAuxLane))
        return ScenarioResult::skip ("the aux stage cannot show the first lane");
    auto* lane = host.auxLane (kAuxLane);
    if (lane == nullptr)
        return ScenarioResult::skip ("the aux stage realised no lane to drive");

    const auto rowShown = [&host]
    {
        const auto rows = host.pickerRows (false);
        return std::find (rows.begin(), rows.end(), kRow) != rows.end();
    };

    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 0, [&host, &ctx]
    {
        ctx.expect (host.clickStripControl (GuiHost::StripKind::Aux, kAuxLane, "insert", 1, false),
                    "the empty aux insert took no click");
    }, [&host] { return host.activeAuxLane() == kAuxLane; }, "the aux stage never showed the first lane" });
    steps->push_back ({ 0, [&host, &ctx]
    { ctx.expect (host.clickPickerRow (kRow), "the picker row for DuskVerb 2 took no click"); },
                       rowShown, "the aux insert opened no picker listing DuskVerb 2" });
    steps->push_back ({ 0, [&host, &ctx, &strip, lane]
    {
        ctx.expect (strip.isBuiltinLoaded (kAuxSlot) && strip.getBuiltinSlot (kAuxSlot).getPluginId() == kUnit,
                    "picking DuskVerb 2 did not load it on the aux lane");
        ctx.expect (host.pickerRows (false).empty(), "the picker stayed up after the pick");
        ctx.expect (lane->slotLabel (kAuxSlot) == "DuskVerb 2",
                    "the picked slot reads \"" + lane->slotLabel (kAuxSlot) + "\"");
    }, [lane] { return lane->builtinEditorUnit (kAuxSlot) == kUnit; },
    "picking DuskVerb 2 on the aux lane never brought its editor up" });

    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
   #endif
}

const ScenarioRegistrar auxPickShowsBuiltinEditor { Scenario {
    "gui.aux_pick_shows_builtin_editor", { "gui", "aux" }, Needs::Engine | Needs::Gui,
    {}, {}, 30000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runAuxPickShowsBuiltinEditor (host, ctx); }
} };

// A native CLAP row, which only a seeded picker cache lists: the suite runs it
// through tests/gui_plugin_picker.sh and skips it elsewhere.
std::optional<ScenarioResult> runPickOpensNativeEditor (GuiHost& host, ScenarioContext& ctx)
{
    static const std::string kRow = "Window Pick  (CLAP)";
    const auto fixture = ctx.fixture ("no_window.clap");
    const auto descriptions = ctx.engine().getPluginManager().getClapEffectDescriptions();
    if (! fixture || std::none_of (descriptions.begin(), descriptions.end(), [] (const auto& row)
        { return row.name == "Window Pick"; }))
        return ScenarioResult::skip ("requires the seeded plugin-picker cache");
    if (! host.canEmbedPluginEditors())
        return ScenarioResult::skip ("this display embeds no plug-in editors");
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    const auto originalDir = currentSessionDirectory (session);
    const auto restore = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restore))
        return ScenarioResult::fail ("could not save the initial session");
    auto* strip = readyStrip (host, ctx);
    if (strip == nullptr) return ScenarioResult::fail ("channel strip is unavailable");
    ctx.cleanup ([&host, &session, originalDir, restore]
    {
        drainModals (host);
        if (auto* s = host.strip (kStripIndex)) { s->closeEditor(); s->unloadNativePlugins(); }
        host.openSession (restore);
        applySessionDirectory (session, originalDir);
    });

    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 0, [&host, &ctx]
    { ctx.expect (host.clickInsert (kStripIndex), "the empty insert did not take a click"); } });
    steps->push_back ({ 0, [&host, &ctx]
    { ctx.expect (host.clickModalButton ("Plugin (VST3 / CLAP / LV2 / AU)"), "the insert chooser has no Plugin button"); },
                       [&host] { return ! host.modalStackEmpty(); }, "clicking the empty insert opened no chooser" });
    steps->push_back ({ 0, [&host, &ctx]
    {
        ctx.expect (host.clickModalAt (0.3f, 0.1f), "the picker filter took no click");
        typeReplacing (host, "Window Pick");
    }, [&host] { return ! host.pickerRows (true).empty(); }, "the chooser opened no picker" });
    steps->push_back ({ 0, [&host, &ctx]
    { ctx.expect (host.clickPickerRow (kRow), "the picker row for the CLAP fixture took no click"); },
                       [&host]
                       {
                           const auto rows = host.pickerRows (false);
                           return std::find (rows.begin(), rows.end(), kRow) != rows.end();
                       }, "the picker never listed the seeded CLAP row" });
    steps->push_back ({ 0, [&host, &ctx, &engine]
    {
        ctx.expect (engine.getChannelStrip (kStripIndex).isNativeClapLoaded(), "the pick did not load the CLAP fixture");
        ctx.expect (host.pickerRows (false).empty(), "the picker stayed up after the pick");
    },
    [&host] { auto* s = host.strip (kStripIndex); return s != nullptr && s->hasOpenEditor(); },
    "picking the CLAP fixture left its editor closed" });

    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar pickOpensNativeEditor { Scenario {
    "gui.pick_opens_native_editor", { "gui", "plugins" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runPickOpensNativeEditor (host, ctx); }
} };

// macOS reports a wheel click at a fifth of an X11 one and a trackpad point at
// a fraction of that, which the list used to truncate to a pixel or to nothing.
std::optional<ScenarioResult> runPluginPickerWheel (GuiHost& host, ScenarioContext& ctx)
{
    static constexpr int kRowH = 22;
    ctx.cleanup ([&host]
    {
        wheel::setProfileForScenario (wheel::kPlatformProfile);
        drainModals (host);
    });
    const auto offset = [&host] { return host.pickerScroll()[0]; };
    const auto before = std::make_shared<int> (0);
    const auto wheelOf = [&host, &ctx, offset, before] (float delta, bool smooth, int events)
    {
        *before = offset();
        for (int i = 0; i < events; ++i)
            ctx.expect (host.wheelPicker (delta, smooth), "the picker list did not take the wheel");
    };
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 0, [&host, &ctx]
    { ctx.expect (host.openScrollingPicker (60), "the picker did not open"); } });
    steps->push_back ({ 0, [wheelOf]
    {
        wheel::setProfileForScenario (wheel::kMacProfile);
        wheelOf (-wheel::kMacProfile.lineNotchDelta, false, 1);
    }, [&host] { return host.pickerScroll()[1] > 10 * kRowH; }, "the picker list was not long enough to scroll" });
    steps->push_back ({ 0, [&ctx, offset, before, wheelOf]
    {
        ctx.expect (offset() - *before >= kRowH, "a macOS wheel click scrolled the picker less than a row");
        wheelOf (-wheel::kSmoothDeltaPerPoint, true, kRowH);
    }, [offset, before] { return offset() > *before; }, "a macOS wheel click did not scroll the picker" });
    steps->push_back ({ 0, [&ctx, offset, before, wheelOf]
    {
        ctx.expect (offset() - *before >= kRowH - 1, "a row of single trackpad points did not scroll the picker a row");
        wheelOf (wheel::kMacProfile.lineNotchDelta, false, 1);
    }, [offset, before] { return offset() > *before; }, "single trackpad points did not scroll the picker" });
    steps->push_back ({ 0, [&ctx, offset, before, wheelOf]
    {
        ctx.expect (*before - offset() >= kRowH, "a macOS wheel click up scrolled the picker less than a row");
        wheel::setProfileForScenario (wheel::kPlatformProfile);
        wheelOf (notch (-1.0f), false, 1);
    }, [offset, before] { return offset() < *before; }, "a macOS wheel click up did not scroll the picker" });
    steps->push_back ({ 0, [&ctx, offset, before]
    { ctx.expect (offset() - *before >= kRowH, "a wheel notch on this platform scrolled the picker less than a row"); },
      [offset, before] { return offset() > *before; }, "a wheel notch on this platform did not scroll the picker" });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar pluginPickerWheel { Scenario {
    "gui.plugin_picker_wheel", { "gui", "plugins" }, Needs::Engine | Needs::Gui,
    {}, {}, 20000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runPluginPickerWheel (host, ctx); }
} };

std::optional<ScenarioResult> runPluginKindMismatch (GuiHost& host, ScenarioContext& ctx)
{
    const auto instrument = ctx.fixture ("panic_probe.vst3");
    const auto effect = ctx.fixture ("relayout.vst3");
    if (! instrument || ! effect) return ScenarioResult::skip ("requires both VST3 fixtures");
    auto& engine = ctx.engine();
    if (! engine.getTransport().isStopped()) return ScenarioResult::skip ("requires stopped transport");
    auto& session = ctx.session();
    auto& track = session.track (0);
    auto& slot = engine.getChannelStrip (0).getPluginSlot();
    if (slot.isLoaded()) return ScenarioResult::skip ("requires an empty standard plugin slot");
    const auto originalDir = currentSessionDirectory (session);
    const auto originalStage = engine.getStage();
    const auto restore = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restore))
        return ScenarioResult::fail ("could not save the initial session");
    ctx.keep (track.mode);
    ctx.cleanup ([&host, &session, originalDir, originalStage, restore]
    {
        if (auto* strip = host.strip (0)) strip->closeEditor();
        drainModals (host);
        host.openSession (restore);
        applySessionDirectory (session, originalDir);
        host.switchToStage (guiStage (originalStage));
    });
    if (readyStrip (host, ctx) == nullptr) return ScenarioResult::fail ("channel strip is unavailable");

    struct Leg
    {
        int trackMode;
        std::filesystem::path file;
        std::string wanted, got;
    };
    // An audio track's insert takes effects, a MIDI track's takes the
    // instrument, so one strip covers the refusal in both directions.
    const std::vector<Leg> legs {
        { (int) Track::Mode::Mono, *instrument, "effect", "instrument" },
        { (int) Track::Mode::Midi, *effect,     "instrument", "effect" },
    };

    auto steps = std::make_shared<std::vector<Step>>();
    for (const auto& leg : legs)
    {
        steps->push_back ({ 150, [&host, &ctx, &track, leg]
        {
            track.mode.store (leg.trackMode);
            if (auto* strip = host.strip (0)) strip->refreshInsertButton();
            ctx.expect (host.clickInsert (0), "insert button did not receive a click");
        } });
        steps->push_back ({ 150, [&host, &ctx]
        { ctx.expect (host.clickModalButton ("Plugin (VST3 / CLAP / LV2 / AU)"), "insert chooser did not open"); } });
        steps->push_back ({ 150, [&host, &ctx]
        { ctx.expect (host.clickModalButton ("Browse file..."), "picker did not offer Browse file"); } });
        steps->push_back ({ 150, [&host, &ctx, leg]
        {
            ctx.expect (host.focusFileName(), "file browser filename entry was not available");
           #if defined (__APPLE__)
            host.pressPeerKey ("command + A", 'a');
           #else
            host.pressPeerKey ("ctrl + A", 'a');
           #endif
            for (const char ch : leg.file.string())
                host.pressPeerKey (ch == ' ' ? "Space" : std::string (1, ch), ch);
        } });
        steps->push_back ({ 200, [&host, &ctx]
        { ctx.expect (host.clickModalButton ("Open"), "Open did not accept the fixture path"); } });
        steps->push_back ({ 200, [&host, &ctx, &slot, leg]
        {
            ctx.expect (host.modalText() ==
                "Plugin kind mismatch\nThis slot expects an " + leg.wanted
                + " plugin but the chosen file is an " + leg.got
                + ". The slot was left empty. Use a MIDI track for instrument "
                  "plugins and an audio track for effect plugins.",
                "the refusal did not name both kinds and the emptied slot: " + host.modalText());
            ctx.expect (! slot.isLoaded(), "the refused plugin stayed on the slot");
            ctx.expect (host.clickModalButton ("OK"), "the refusal has no usable OK button");
        }, [&host] { return host.fileBrowserPanels() == 0 && ! host.modalStackEmpty(); },
           "Open left the file browser up, or it closed without an alert", {}, 5000 });
    }
    runSteps (ctx, steps, [&host, &ctx]
    {
        ctx.waitUntil ([&host] { return host.modalStackEmpty(); }, 3000,
                       [&ctx] { ctx.complete (ctx.verdict()); }, "OK did not dismiss the refusal");
    });
    return std::nullopt;
}

const ScenarioRegistrar pluginKindMismatch { Scenario {
    "gui.plugin_kind_mismatch", { "gui", "plugins", "messages" }, Needs::Engine | Needs::Gui,
    { "panic_probe.vst3", "relayout.vst3" }, {}, 20000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runPluginKindMismatch (host, ctx); }
} };
} // namespace
} // namespace duskstudio::scenario::gui
