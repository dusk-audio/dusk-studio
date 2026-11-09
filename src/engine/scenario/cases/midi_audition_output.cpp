#include "../Scenario.h"
#include "../RecordingMidiBackend.h"
#include "MidiProbeHarness.h"

#include <memory>
#include <string>

namespace duskstudio::scenario
{
namespace
{
std::optional<ScenarioResult> runAuditionOutput (ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    auto& track = session.track (0);
    auto owned = std::make_unique<RecordingMidiBackend> (2);
    auto* recorder = owned.get();
    engine.installMidiOutputBackend (std::move (owned));
    track.mode.store ((int) Track::Mode::Midi);
    track.inputMonitor.store (false);
    track.recordArmed.store (false);
    track.midiInputIndex.store (-1);
    track.midiOutputIndex.store (-1);
    track.midiChannel.store (5);
    session.recomputeRtCounters();
    ctx.pump (3);
    if (! ctx.expect (engine.ensureMidiOutputOpen (1), "the second MIDI output did not open"))
        return ctx.verdict();
    track.midiOutputIndex.store (1);
    ctx.cleanup ([&engine, &track]
    {
        engine.releaseAuditions();
        track.midiOutputIndex.store (-1);
    });
    const auto before = track.midiRegions.edits();
    ctx.expect (engine.auditionNoteOn (0, 3, 60, 100), "the audition onset was rejected");
    ctx.pump (1);
    engine.auditionNoteOff (0, 3, 60);
    ctx.pump (3);
    ctx.waitUntil ([recorder] { return recorder->count() >= 2; }, 4000,
        [&ctx, &engine, &track, recorder, before]
        {
            const auto messages = recorder->captured();
            ctx.expect (! recorder->hasDropped(), "the MIDI output recorder overflowed");
            if (ctx.expect (messages.size() == 2, "external audition did not deliver exactly one onset and release"))
                for (std::size_t index = 0; index < messages.size(); ++index)
                {
                    const auto& message = messages[index];
                    ctx.expect (message.port == 1 && message.numBytes == 3 && message.bytes[1] == 60
                                    && message.bytes[0] == (index == 0 ? 0x92 : 0x82)
                                    && message.bytes[2] == (index == 0 ? 100 : 0),
                                "external audition changed its destination, channel, pitch, velocity or release");
                }
            ctx.expect (! engine.getRecordManager().isActive(), "external audition started a recording");
            ctx.expect (track.midiRegions.edits() == before, "external audition edited the MIDI timeline");
            ctx.complete (ctx.verdict());
        }, "the external output did not receive the audition's release");
    return std::nullopt;
}

const ScenarioRegistrar auditionOutput { Scenario {
    "midi.out_audition", { "midi", "output", "piano" }, Needs::Engine, {}, runAuditionOutput
} };
}
}
