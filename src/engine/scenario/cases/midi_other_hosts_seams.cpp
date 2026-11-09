#include "../Scenario.h"
#include "MidiProbeHarness.h"
#include "../../Transport.h"

#include <array>
#include <cmath>
#include <initializer_list>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace duskstudio::scenario
{
namespace
{
#if DUSKSTUDIO_HAS_NATIVE_LV2 || DUSKSTUDIO_HAS_NATIVE_VST3
enum Counter { Held, Offs, Panics, Retriggers, Sustained, Pedals, Lifts, Tails, TailsCut };
using Counters = std::array<int, 9>;
constexpr int kTrack = 0;

Counters counters (ScenarioContext& ctx, bool legacy)
{
    auto& strip = ctx.engine().getChannelStrip (kTrack);
    Counters values;
    values.fill (-1);
    if (legacy)
    {
        const std::array<const char*, 9> names {
            "Held Notes", "Note Offs Seen", "Panic Points Seen", "Retriggers", "Sustained Notes",
            "Pedal Down Channels", "Pedal Lifts Seen", "Tails Ringing", "Tails Cut"
        };
        auto* instance = strip.getPluginSlot().getInstance();
        if (instance)
            for (auto* parameter : instance->getParameters())
                for (std::size_t index = 0; index < names.size(); ++index)
                    if (parameter->getName (64).toStdString() == names[index])
                        values[index] = (int) std::lround (parameter->getValue() * (index == Held ? 16.0 : 65535.0));
    }
    else
    {
        std::vector<std::uint8_t> state;
        ctx.engine().suspendProcessing();
        const bool saved = strip.getNativeLv2Slot().saveState (state);
        ctx.engine().resumeProcessing();
        const std::string text (state.begin(), state.end());
        const auto at = text.find ("DUSK-MIDI-PROBE ");
        if (saved && at != std::string::npos)
        {
            std::istringstream numbers (text.substr (at + std::string ("DUSK-MIDI-PROBE ").size()));
            for (auto& value : values) numbers >> value;
            ctx.expect (! numbers.fail(), "the LV2 probe did not expose every diagnostic counter");
        }
    }
    for (const auto value : values) ctx.expect (value >= 0, "a host probe counter was unavailable");
    return values;
}

void send (ScenarioContext& ctx, int input, std::initializer_list<std::array<std::uint8_t, 3>> messages)
{
    dusk::MidiBuffer midi;
    for (const auto& message : messages)
        midiprobe::addMessage (midi, message[0], message[1], message[2]);
    ctx.pumpWithMidi (input, std::move (midi));
}

void publish (Track& track, bool dense = false)
{
    MidiRegion region;
    region.lengthInTicks = 1200;
    region.lengthInSamples = 60000;
    region.ccs.push_back ({ 1, 64, 127, 50 });
    if (dense)
        for (int note = 0; note < 10000; ++note)
            region.notes.push_back ({ 1, 48 + note % 12, 100, 0, 900 });
    else
    {
        region.notes.push_back ({ 1, 60, 100, 100, 800 });
        region.notes.push_back ({ 1, 64, 100, 600, 100 });
    }
    auto regions = std::make_unique<std::vector<MidiRegion>>();
    regions->push_back (std::move (region));
    track.midiRegions.publish (std::move (regions));
}

ScenarioResult runOtherHostSeams (ScenarioContext& ctx, bool legacy)
{
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    auto& transport = engine.getTransport();
    auto& track = session.track (kTrack);
    auto& strip = engine.getChannelStrip (kTrack);
    const int input = engine.getVirtualKeyboardInputIndex();
    if (input < 0) return ScenarioResult::skip ("no injectable MIDI input");
    midiprobe::makeLiveMidiTrack (ctx, kTrack, input);
    ctx.keep (session.tempoBpm);
    session.tempoBpm.store (120.0f, std::memory_order_release);
    session.recomputeRtCounters();
    auto& manager = engine.getPluginManager();
    ctx.cleanup ([&engine, &transport, &track, &strip, &manager, oop = manager.isOopEnabled()]
    {
        engine.stop();
        transport.setLoopEnabled (false);
        track.midiRegions.publish (std::make_unique<std::vector<MidiRegion>>());
        strip.getPluginSlot().unload();
        strip.unloadNativeLv2();
        manager.setOopEnabled (oop);
    });
    if (legacy)
    {
        manager.setOopEnabled (false);
        PluginDescriptor descriptor;
        descriptor.name = "Dusk Every-CC Probe Fixture";
        descriptor.descriptiveName = descriptor.name;
        descriptor.formatName = "VST3";
        descriptor.backend = PluginBackend::JuceLegacy;
        descriptor.location = ctx.fixture ("panic_probe.vst3")->u8string();
        // JUCE's hash of the Every-CC instrument class ID.
        descriptor.uniqueId = -426199743;
        descriptor.isInstrument = true;
        descriptor.numOutputChannels = 2;
        auto& slot = strip.getPluginSlot();
        decltype (slot.getLoadedName()) error;
        const bool loaded = slot.loadFromDescriptor (descriptor, error);
        if (! ctx.expect (loaded, "standard VST3 load failed: " + error.toStdString()))
            return ctx.verdict();
        ctx.expect (slot.isLoadedStandardVst3(), "the standard hosting path was not selected");
    }
    else
    {
        std::string error;
        const bool loaded = strip.getNativeLv2Slot().load (*ctx.fixture ("midi_seam_probe.lv2"),
            ScenarioContext::kSampleRate, ScenarioContext::kBlockSize, error, "urn:duskstudio:test:midi-seam-probe");
        if (! ctx.expect (loaded, "LV2 load failed: " + error))
            return ctx.verdict();
    }

    const auto stop = [&]
    {
        engine.pressStop();
        ctx.pump (3);
        const auto after = counters (ctx, legacy);
        ctx.expect (after[Held] == 0 && after[Sustained] == 0 && after[Pedals] == 0,
                    "Stop left held or sustained notes on the instrument");
        track.inputMonitor.store (true, std::memory_order_relaxed);
    };
    for (const int destination : { 0, 100 })
    {
        publish (track);
        transport.setLoopRange (destination * 50, 40000);
        transport.setLoopEnabled (true);
        transport.setPlayhead (32500);
        engine.play();
        ctx.pump (1);
        send (ctx, input, { { 0x91, 72, 100 }, { 0xb1, 64, 127 }, { 0x91, 74, 100 } });
        send (ctx, input, { { 0x81, 74, 0 } });
        for (int block = 0; block < 32 && transport.getPlayhead() < 38976; ++block) ctx.pump (1);
        send (ctx, input, { { 0x92, 76, 100 } });
        send (ctx, input, { { 0x82, 76, 0 } });
        const auto before = counters (ctx, legacy);
        for (int block = 0; block < 8 && transport.getPlayhead() > 20000; ++block) ctx.pump (1);
        const auto after = counters (ctx, legacy);
        ctx.note ("loop target " + std::to_string (destination) + ": held " + std::to_string (after[Held])
                  + ", sustained " + std::to_string (after[Sustained]) + ", pedals " + std::to_string (after[Pedals]));
        ctx.expect (before[Sustained] == 2 && before[Tails] > 0, "the loop test never held its sustained voices and tail");
        ctx.expect (transport.getPlayhead() < 20000, "the loop never wrapped");
        ctx.expect (after[Held] == (destination == 0 ? 1 : 2) && after[Sustained] == 1,
                    "the loop cut a live key or kept a timeline voice");
        ctx.expect (after[Pedals] == (destination == 0 ? 2 : 3), "the loop restored the wrong pedal channels");
        ctx.expect (after[Tails] > 0 && after[TailsCut] == before[TailsCut] && after[Panics] == before[Panics],
                    "the loop used a blanket panic or cut a live release tail");
        ctx.expect (after[Retriggers] == before[Retriggers], "the loop retriggered an already held note");
        stop();
    }

    for (const bool scrub : { false, true })
    {
        transport.setLoopEnabled (false);
        transport.setPlayhead (32500);
        engine.play();
        ctx.pump (1);
        send (ctx, input, { { 0x91, 72, 100 }, { 0xb1, 64, 127 }, { 0x91, 74, 100 } });
        send (ctx, input, { { 0x81, 74, 0 }, { 0x92, 76, 100 } });
        send (ctx, input, { { 0x82, 76, 0 } });
        const auto before = counters (ctx, legacy);
        if (scrub)
            for (int step = 1; step <= 6; ++step) { transport.locate (32500 - step * 5250); ctx.pump (1); }
        else { transport.locate (1000); ctx.pump (3); }
        const auto after = counters (ctx, legacy);
        ctx.expect (after[Held] == 1 && after[Sustained] == 1 && after[Pedals] == 2,
                    "seek or scrub left timeline voices or cut live keys");
        ctx.expect (after[Tails] > 0 && after[TailsCut] == before[TailsCut] && after[Panics] == before[Panics],
                    "seek or scrub cut a release tail");
        stop();
    }

    track.inputMonitor.store (false, std::memory_order_relaxed);
    transport.setPlayhead (32500);
    engine.play();
    ctx.pump (3);
    ctx.expect (counters (ctx, legacy)[Held] > 0, "IN-off playback never started the timeline notes");
    stop();
    send (ctx, input, { { 0x91, 72, 100 } });
    track.midiChannel.store (1, std::memory_order_relaxed);
    ctx.pump (3);
    ctx.expect (counters (ctx, legacy)[Held] == 0, "narrowing the input route left a live key held");
    track.midiChannel.store (0, std::memory_order_relaxed);
    send (ctx, input, { { 0x91, 72, 100 } });
    publish (track, true);
    transport.setPlayhead (0);
    engine.play();
    ctx.pump (1);
    ctx.expect (counters (ctx, legacy)[Held] == 0, "an overflowing timeline left a live key held");
    stop();
    return ctx.verdict();
}
#endif

const ScenarioRegistrar lv2Seams { Scenario {
    "lv2.midi_seams_keep_live_keys", { "lv2", "midi", "transport" }, Needs::Engine,
    { "midi_seam_probe.lv2" },
    [] (ScenarioContext& ctx) -> std::optional<ScenarioResult>
    {
       #if DUSKSTUDIO_HAS_NATIVE_LV2
        return runOtherHostSeams (ctx, false);
       #else
        (void) ctx; return ScenarioResult::skip ("built without native LV2");
       #endif
    }
} };
const ScenarioRegistrar standardSeams { Scenario {
    "vst3.standard_midi_seams_keep_live_keys", { "vst3", "midi", "transport" }, Needs::Engine,
    { "panic_probe.vst3" },
    [] (ScenarioContext& ctx) -> std::optional<ScenarioResult>
    {
       #if DUSKSTUDIO_HAS_NATIVE_VST3
        return runOtherHostSeams (ctx, true);
       #else
        (void) ctx; return ScenarioResult::skip ("built without the VST3 fixture");
       #endif
    }
} };
}
}
