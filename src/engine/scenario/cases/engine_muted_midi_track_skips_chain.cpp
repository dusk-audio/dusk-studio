#include "../Scenario.h"
#include "MidiProbeHarness.h"

#include <string>

namespace duskstudio::scenario
{
namespace
{
constexpr int kMutedTracks  = 8;
constexpr int kAudioTrack   = 8;
constexpr int kSettleBlocks = 3;

ScenarioResult runMutedSkip (ScenarioContext& ctx)
{
    auto& session = ctx.session();
    auto& engine = ctx.engine();

    for (int t = 0; t < kMutedTracks; ++t)
    {
        session.track (t).mode.store ((int) Track::Mode::Midi, std::memory_order_relaxed);
        session.track (t).strip.mute.store (true, std::memory_order_relaxed);
    }
    session.track (kAudioTrack).mode.store ((int) Track::Mode::Mono, std::memory_order_relaxed);
    session.track (kAudioTrack).strip.mute.store (false, std::memory_order_relaxed);
    session.recomputeRtCounters();

    engine.play();
    ctx.pump (kSettleBlocks);

    for (int t = 0; t < kMutedTracks; ++t)
        ctx.expect (engine.getChannelStrip (t).getLastProcessedSamples() == 0,
                    "muted empty MIDI track " + std::to_string (t + 1)
                        + " still ran its chain");
    ctx.note ("audio track processed samples: "
              + std::to_string (engine.getChannelStrip (kAudioTrack).getLastProcessedSamples()));
    ctx.expect (engine.getChannelStrip (kAudioTrack).getLastProcessedSamples() > 0,
                "the audio track was skipped alongside the muted MIDI tracks");

   #if DUSKSTUDIO_HAS_NATIVE_CLAP
    // A muted MIDI track with an instrument loaded stays on the full pass: that
    // block's MIDI is the only thing a panic's note-offs can flush.
    std::string error;
    if (ctx.expect (midiprobe::loadPanicProbe (ctx, 0, error),
                    "the muted track could not load the probe"))
    {
        ctx.pump (kSettleBlocks);
        ctx.note ("muted track with an instrument processed samples: "
                  + std::to_string (engine.getChannelStrip (0).getLastProcessedSamples()));
        ctx.expect (engine.getChannelStrip (0).getLastProcessedSamples() > 0,
                    "a muted MIDI track with an instrument stopped processing");
    }
    else
    {
        ctx.note ("load error: " + error);
    }
   #endif

    engine.stop();
    ctx.pump (1);
    return ctx.verdict();
}

const ScenarioRegistrar registrar { Scenario {
    "engine.muted_midi_track_skips_chain",
    { "engine", "midi", "mute" },
    Needs::Engine,
    { "panic_probe.clap" },
    [] (ScenarioContext& ctx) -> std::optional<ScenarioResult> { return runMutedSkip (ctx); }
} };
} // namespace
} // namespace duskstudio::scenario
