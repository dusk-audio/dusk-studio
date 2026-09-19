#include "../Scenario.h"
#include "MidiProbeHarness.h"

#if DUSKSTUDIO_HAS_NATIVE_VST3
 #include "../../vst3/Vst3Bundle.h"
#endif

#include <cmath>
#include <string>

namespace duskstudio::scenario
{
namespace
{
#if DUSKSTUDIO_HAS_NATIVE_VST3
constexpr int kTrackIndex   = 0;
constexpr int kSettleBlocks = 3;
constexpr int kSilenceBlocks = 256;

// The fixture advertises exactly one read-only parameter, its held-voice count
// normalised over the polyphony ceiling.
double heldNotes (const vst3::NativeVst3Slot& slot)
{
    for (int i = 0; i < slot.paramCount(); ++i)
    {
        const auto* info = slot.paramInfo (i);
        if (info == nullptr || ! info->isReadOnly) continue;
        double value = 0.0;
        if (slot.getParamValue (info->id, value)) return value;
    }
    return -1.0;
}

ScenarioResult runNoteOffFallback (ScenarioContext& ctx)
{
    auto& session = ctx.session();
    auto& engine = ctx.engine();

    const int input = engine.getVirtualKeyboardInputIndex();
    if (input < 0)
        return ScenarioResult::skip ("the MIDI input bank has no injectable input");

    const auto fixture = *ctx.fixture ("panic_probe.vst3");

    // The module's only class is an instrument, and the slot's default pick is
    // the first effect class, so the class id has to be named explicitly.
    vst3::Vst3Bundle bundle;
    std::string error;
    if (! ctx.expect (bundle.load (fixture.string(), error), "the fixture module did not load"))
    {
        ctx.note ("module error: " + error);
        return ctx.verdict();
    }

    std::string classId;
    for (const auto& plugin : bundle.plugins())
        if (plugin.isInstrument) { classId = plugin.id; break; }
    if (! ctx.expect (! classId.empty(), "the fixture advertises no instrument class"))
        return ctx.verdict();

    midiprobe::makeLiveMidiTrack (ctx, kTrackIndex, input);
    session.recomputeRtCounters();

    auto& strip = engine.getChannelStrip (kTrackIndex);
    auto& slot = strip.getNativeVst3Slot();
    if (! ctx.expect (slot.load (fixture, ScenarioContext::kSampleRate,
                                 ScenarioContext::kBlockSize, error, classId),
                      "the track could not load the probe"))
    {
        ctx.note ("load error: " + error);
        return ctx.verdict();
    }

    engine.play();
    ctx.pump (kSettleBlocks);

    dusk::MidiBuffer notes;
    midiprobe::addMessage (notes, 0x90, 60, 100);
    midiprobe::addMessage (notes, 0x90, 67, 100);
    ctx.pumpWithMidi (input, std::move (notes));
    ctx.pump (1);

    ctx.note ("held: heldNotes=" + std::to_string (heldNotes (slot))
              + " outLDb=" + std::to_string (strip.getOutLDb()));
    ctx.expect (heldNotes (slot) > 0.0, "the instrument never received the notes");
    ctx.expect (strip.getOutLDb() > -60.0f, "the instrument produced no output while holding voices");

    engine.stop();
    ctx.pump (kSettleBlocks);

    const double afterStop = heldNotes (slot);
    ctx.note ("after stop: heldNotes=" + std::to_string (afterStop)
              + " outLDb=" + std::to_string (strip.getOutLDb()));
    // No CC 120 / 123 mapping on this plugin, so the only thing that can release
    // the voices is the host turning the panic into per-note note-offs.
    ctx.expect (std::fpclassify (afterStop) == FP_ZERO,
                "the voice counter was unavailable or voices survived the panic");

    const int silentAfter = midiprobe::pumpUntilSilent (ctx, kTrackIndex, kSilenceBlocks);
    ctx.note ("the strip fell silent " + std::to_string (silentAfter)
              + " blocks after the panic");
    ctx.expect (silentAfter >= 0, "the instrument was still audible after the panic");

    strip.unloadNativeVst3();
    return ctx.verdict();
}
#endif

const ScenarioRegistrar registrar { Scenario {
    "vst3.noteoff_fallback",
    { "vst3", "midi", "panic" },
    Needs::Engine,
    { "panic_probe.vst3" },
    [] (ScenarioContext& ctx) -> std::optional<ScenarioResult>
    {
       #if DUSKSTUDIO_HAS_NATIVE_VST3
        return runNoteOffFallback (ctx);
       #else
        (void) ctx;
        return ScenarioResult::skip ("built without the native VST3 host");
       #endif
    }
} };
} // namespace
} // namespace duskstudio::scenario
