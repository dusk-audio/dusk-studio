#include "../Scenario.h"
#include "MidiProbeHarness.h"

#if DUSKSTUDIO_HAS_NATIVE_VST3
 #include "../../vst3/Vst3Bundle.h"
#endif

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// The audio callback with every real-time path it has running at once: the
// Multicore DSP pool takes each block, one track records, and region MIDI and
// live keys play a fixture instrument of each native format. In an RTSan build
// (-DDUSKSTUDIO_ENABLE_RTSAN=ON) the callback and the lanes are checked as they
// run, so an allocation, lock or blocking call on any of these paths is
// reported.
namespace duskstudio::scenario
{
namespace
{
#if DUSKSTUDIO_HAS_NATIVE_CLAP
constexpr int kRecordTrack = 0;
constexpr int kClapTrack   = 8;
constexpr int kVst3Track   = 12;
constexpr int kLv2Track    = 16;
constexpr int kWorkers     = 2;
constexpr int kBlocks      = 96;

void publishNotes (ScenarioContext& ctx, int trackIndex, int note)
{
    auto& session = ctx.session();
    auto& track = session.track (trackIndex);
    track.mode.store ((int) Track::Mode::Midi, std::memory_order_relaxed);
    track.inputMonitor.store (true, std::memory_order_relaxed);
    auto regions = std::make_unique<std::vector<MidiRegion>>();
    MidiRegion region;
    region.lengthInTicks = 8 * kMidiTicksPerQuarter;
    region.lengthInSamples = ticksToSamples (region.lengthInTicks, ScenarioContext::kSampleRate,
                                             session.tempoBpm.load (std::memory_order_acquire));
    for (int beat = 0; beat < 8; ++beat)
        region.notes.push_back ({ 1, note + (beat % 3) * 4, 100, beat * kMidiTicksPerQuarter,
                                  kMidiTicksPerQuarter / 2 });
    region.notes.push_back ({ 1, note - 12, 90, 0, region.lengthInTicks });
    regions->push_back (std::move (region));
    track.midiRegions.publish (std::move (regions));
}

#if DUSKSTUDIO_HAS_NATIVE_VST3
bool loadVst3Probe (ScenarioContext& ctx, std::string& error)
{
    const auto fixture = ctx.fixture ("panic_probe.vst3");
    if (! fixture)
    {
        error = "panic_probe.vst3 did not resolve";
        return false;
    }
    // The slot's default pick is the first effect class and the module holds
    // instruments only, so its first, the plain panic probe, is named.
    vst3::Vst3Bundle bundle;
    if (! bundle.load (fixture->string(), error)) return false;
    for (const auto& plugin : bundle.plugins())
        if (plugin.isInstrument)
            return ctx.engine().getChannelStrip (kVst3Track).getNativeVst3Slot().load (
                *fixture, ScenarioContext::kSampleRate, ScenarioContext::kBlockSize, error, plugin.id);
    error = "the fixture advertises no instrument class";
    return false;
}
#endif

ScenarioResult runFullLoad (ScenarioContext& ctx)
{
    auto& session = ctx.session();
    auto& engine = ctx.engine();
    auto& transport = engine.getTransport();

    const int input = engine.getVirtualKeyboardInputIndex();
    if (input < 0)
        return ScenarioResult::skip ("the MIDI input bank has no injectable input");

    engine.setWorkerCountForTest (kWorkers);
    engine.setPoolDispatchForcedForTest (true);
    ctx.cleanup ([&engine]
    {
        engine.setPoolDispatchForcedForTest (false);
        engine.applyDesiredWorkers();
    });
    if (engine.activeDspWorkersForTest() < 1)
        return ScenarioResult::skip ("too few cores here for a Multicore DSP worker");

    ctx.keep (session.deviceCaptureChannels);
    ctx.keep (session.countInEnabled);
    session.deviceCaptureChannels.store (2);
    session.countInEnabled.store (false);
    auto& recorded = session.track (kRecordTrack);
    recorded.mode.store ((int) Track::Mode::Mono, std::memory_order_relaxed);
    recorded.inputSource.store (0, std::memory_order_relaxed);
    recorded.inputMonitor.store (true, std::memory_order_relaxed);
    session.setTrackArmed (kRecordTrack, true);

    std::string error;
    if (! ctx.expect (midiprobe::loadPanicProbe (ctx, kClapTrack, error), "the CLAP probe did not load: " + error))
        return ctx.verdict();
    midiprobe::makeLiveMidiTrack (ctx, kClapTrack, input);
    publishNotes (ctx, kClapTrack, 60);
    std::vector<int> instruments { kClapTrack };

   #if DUSKSTUDIO_HAS_NATIVE_VST3
    if (ctx.fixture ("panic_probe.vst3"))
    {
        if (! ctx.expect (loadVst3Probe (ctx, error), "the VST3 probe did not load: " + error))
            return ctx.verdict();
        publishNotes (ctx, kVst3Track, 64);
        instruments.push_back (kVst3Track);
    }
    else
        ctx.note ("panic_probe.vst3 did not resolve; no VST3 track");
   #endif
   #if DUSKSTUDIO_HAS_NATIVE_LV2
    if (const auto lv2 = ctx.fixture ("midi_seam_probe.lv2"))
    {
        if (! ctx.expect (engine.getChannelStrip (kLv2Track).getNativeLv2Slot().load (
                              *lv2, ScenarioContext::kSampleRate, ScenarioContext::kBlockSize, error,
                              "urn:duskstudio:test:midi-seam-probe"),
                          "the LV2 probe did not load: " + error))
            return ctx.verdict();
        publishNotes (ctx, kLv2Track, 67);
        instruments.push_back (kLv2Track);
    }
    else
        ctx.note ("midi_seam_probe.lv2 did not resolve; no LV2 track");
   #endif
    session.recomputeRtCounters();

    const auto dispatchesBefore = engine.getPoolDispatchCount();
    transport.setPlayhead (0);
    engine.record();
    if (! ctx.expect (transport.isRecording(), "Record did not start with the track armed"))
        return ctx.verdict();

    // The probes play a held voice as a DC offset, which the strip drains over
    // tens of blocks, so the level is read early.
    ctx.pump (4);
    std::vector<float> heard;
    for (const int t : instruments)
        heard.push_back (engine.getChannelStrip (t).getOutLDb());
    ctx.pump (kBlocks / 3 - 4);

    dusk::MidiBuffer keys;
    midiprobe::addMessage (keys, 0x90, 72, 110);
    midiprobe::addMessage (keys, 0xB0, 64, 127, 17);
    ctx.pumpWithMidi (input, std::move (keys));
    ctx.pump (kBlocks / 3);
    dusk::MidiBuffer release;
    midiprobe::addMessage (release, 0xB0, 64, 0);
    midiprobe::addMessage (release, 0x80, 72, 0, 5);
    ctx.pumpWithMidi (input, std::move (release));
    ctx.pump (kBlocks - 2 * (kBlocks / 3) - 2);

    const auto dispatched = engine.getPoolDispatchCount() - dispatchesBefore;
    engine.stop();
    ctx.pump (2);

    ctx.note ("pool dispatched " + std::to_string (dispatched) + " of " + std::to_string (kBlocks)
              + " blocks on " + std::to_string (engine.activeDspWorkersForTest()) + " worker(s)");
    ctx.expect (dispatched == (std::uint64_t) kBlocks, "the worker pool did not take every block");
    for (std::size_t i = 0; i < instruments.size(); ++i)
    {
        ctx.note ("track " + std::to_string (instruments[i] + 1) + " level " + std::to_string (heard[i]) + " dB");
        ctx.expect (heard[i] > -60.0f, "the instrument on track " + std::to_string (instruments[i] + 1)
                                           + " played nothing from its region");
    }
    const auto& takes = recorded.regions;
    if (ctx.expect (takes.size() == 1, "the armed track did not commit exactly one take"))
        ctx.expect (takes.front().lengthInSamples >= (std::int64_t) (kBlocks - 1) * ScenarioContext::kBlockSize,
                    "the take is shorter than the blocks recorded");
    return ctx.verdict();
}
#endif

const ScenarioRegistrar registrar { Scenario {
    "engine.rt_callback_full_load",
    { "engine", "rtsan", "recording", "midi", "clap" },
    Needs::Engine,
    { "panic_probe.clap" },
    [] (ScenarioContext& ctx) -> std::optional<ScenarioResult>
    {
       #if DUSKSTUDIO_HAS_NATIVE_CLAP
        return runFullLoad (ctx);
       #else
        (void) ctx;
        return ScenarioResult::skip ("built without the native CLAP host");
       #endif
    }
} };
} // namespace
} // namespace duskstudio::scenario
