#include "../Scenario.h"
#include "../RecordingMidiBackend.h"
#include "MidiProbeHarness.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace duskstudio::scenario
{
namespace
{
constexpr int kTrackIndex = 0;
// Two events per sample offset over the whole block: dense enough to exercise
// the sort + whole-block copy on the way out, and every event distinct so the
// recorder's order can be checked exactly.
constexpr int kEventCount = 512;
constexpr int kSettleBlocks = 3;
constexpr int kDrainTimeoutMs = 5000;
constexpr int kStablePolls = 3;

struct DrainState
{
    std::size_t lastCount = 0;
    int stablePolls = 0;
};

// Every event is a controller message whose number and value spell out its
// index, so a reordered or dropped one is named rather than just counted.
std::uint8_t controllerFor (int index) { return (std::uint8_t) (index & 0x7F); }
std::uint8_t valueFor      (int index) { return (std::uint8_t) ((index >> 7) & 0x7F); }
int          offsetFor     (int index) { return index / 2; }

void finish (ScenarioContext& ctx, RecordingMidiBackend& recorder)
{
    const auto captured = recorder.captured();
    ctx.note ("captured " + std::to_string (captured.size()) + " of "
              + std::to_string (kEventCount) + " events");

    ctx.expect (! recorder.hasDropped(), "the recorder ran out of storage");
    if (ctx.expect (captured.size() == (std::size_t) kEventCount,
                    "the engine did not deliver every event of a dense block"))
    {
        for (std::size_t i = 0; i < captured.size(); ++i)
        {
            const auto& message = captured[i];
            const int index = (int) i;
            if (! ctx.expect (message.numBytes == 3
                                  && message.bytes[0] == 0xB0
                                  && message.bytes[1] == controllerFor (index)
                                  && message.bytes[2] == valueFor (index)
                                  && message.sampleOffset == offsetFor (index),
                              "event " + std::to_string (index)
                                  + " came out of order or altered"))
                break;
        }
    }

    ctx.complete (ctx.verdict());
}

std::optional<ScenarioResult> runDenseBlock (ScenarioContext& ctx)
{
    auto& session = ctx.session();
    auto& engine = ctx.engine();

    const int input = engine.getVirtualKeyboardInputIndex();
    if (input < 0)
        return ScenarioResult::skip ("the MIDI input bank has no injectable input");

    // One-way for the rest of the suite: the bank has no route back to the OS
    // backend. Nothing after this scenario routes a track to a MIDI output, and
    // reset() puts every track's output index back to none.
    auto owned = std::make_unique<RecordingMidiBackend>();
    auto* const recorder = owned.get();
    engine.installMidiOutputBackend (std::move (owned));

    if (engine.getMidiOutputDevices().empty())
        return ScenarioResult::fail ("the recording backend advertised no output port");

    midiprobe::makeLiveMidiTrack (ctx, kTrackIndex, input);
    session.recomputeRtCounters();

    // Settle with the track unrouted: pointing a MIDI track at a fresh input is
    // itself a hanging-note flush, and those 48 messages would land in the
    // recorder ahead of the block under test.
    ctx.pump (kSettleBlocks);

    if (! engine.ensureMidiOutputOpen (0))
        return ScenarioResult::fail ("the recording backend's port would not open");
    session.track (kTrackIndex).midiOutputIndex.store (0, std::memory_order_relaxed);

    dusk::MidiBuffer dense;
    for (int i = 0; i < kEventCount; ++i)
        midiprobe::addMessage (dense, 0xB0, controllerFor (i), valueFor (i), offsetFor (i));
    ctx.pumpWithMidi (input, std::move (dense));
    ctx.pump (kSettleBlocks);

    // The bank hands blocks to its pump thread, so the count settles a little
    // after the last block rather than inside it.
    auto drain = std::make_shared<DrainState>();
    ctx.waitUntil ([drain, recorder]
                   {
                       const auto count = recorder->count();
                       if (count == drain->lastCount) ++drain->stablePolls;
                       else { drain->lastCount = count; drain->stablePolls = 0; }
                       return drain->stablePolls >= kStablePolls;
                   },
                   kDrainTimeoutMs,
                   [&ctx, recorder] { finish (ctx, *recorder); },
                   "the MIDI output recorder never stopped receiving events");
    return std::nullopt;
}

const ScenarioRegistrar registrar { Scenario {
    "midi.out_dense_block",
    { "midi", "output" },
    Needs::Engine,
    {},
    [] (ScenarioContext& ctx) -> std::optional<ScenarioResult> { return runDenseBlock (ctx); }
} };
} // namespace
} // namespace duskstudio::scenario
