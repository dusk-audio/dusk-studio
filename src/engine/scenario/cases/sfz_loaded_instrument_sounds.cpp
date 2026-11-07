#include "../Scenario.h"
#include "../ScenarioContext.h"
#include "../../AudioEngine.h"
#include "../../../foundation/Fs.h"

#if DUSKSTUDIO_HAS_MULTISAMPLE
#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace duskstudio::scenario
{
namespace
{
// A slot keeps the frame limit its last occupant published, so a slot any
// earlier case loaded a soundfont into would play even with a broken commit.
// No other case loads one on these tracks, and each variant gets its own.
constexpr int kLoadedTrack   = 21;
constexpr int kRestoredTrack = 22;

// SFZ Manager, Browse, an editor's in-place swap and opening a session all load
// through requestNativeMultisampleLoad while the engine runs. The instrument
// has to sound on the first note after the load, with no device restart.
std::optional<ScenarioResult> loadedSoundfontSounds (ScenarioContext& ctx, bool restoring)
{
    auto& engine = ctx.engine();
    const int trackIndex = restoring ? kRestoredTrack : kLoadedTrack;
    auto& track = ctx.session().track (trackIndex);
    if (engine.getChannelStrip (trackIndex).getNativeMultisampleSlot().getInstance() != nullptr)
        return ScenarioResult::skip ("requires an empty soundfont slot");

    ctx.keep (track.mode);
    ctx.keep (track.inputMonitor);
    ctx.keep (track.midiInputIndex);
    ctx.keep (track.midiChannel);
    ctx.cleanup ([&engine, trackIndex]
    {
        engine.cancelNativeMultisampleLoad (trackIndex);
        engine.suspendProcessing();
        engine.getChannelStrip (trackIndex).unloadNativeMultisample();
        engine.resumeProcessing();
    });

    const auto soundfont = ctx.tempDir() / "loaded.sfz";
    if (! ctx.expect (dusk::fs::writeStringToFile (soundfont, "<region> key=60 sample=*sine"),
                      "could not create the instrument"))
        return ctx.verdict();

    const int keyboard = engine.getVirtualKeyboardInputIndex();
    track.mode.store ((int) Track::Mode::Midi);
    track.inputMonitor.store (true);
    track.midiInputIndex.store (keyboard);
    track.midiChannel.store (0);

    struct Load { bool done = false, succeeded = false; std::string error; };
    const auto load = std::make_shared<Load>();
    if (! ctx.expect (engine.requestNativeMultisampleLoad (trackIndex, soundfont, {},
                          [load] (bool success, std::string error)
                          { load->done = true; load->succeeded = success; load->error = std::move (error); },
                          -1, restoring),
                      "the soundfont load was refused"))
        return ctx.verdict();

    ctx.waitUntil ([&engine, load]
    {
        engine.serviceMultisampleLoads();
        return load->done && ! engine.hasPendingMultisampleLoads();
    }, 10000, [&ctx, &engine, load, keyboard]
    {
        if (! ctx.expect (load->succeeded, "the soundfont did not load: " + load->error))
        { ctx.complete (ctx.verdict()); return; }

        dusk::MidiBuffer noteOn;
        const std::uint8_t down[] { 0x90, 60, 100 };
        noteOn.addEvent (down, 3, 0);
        float peak = ctx.pumpWithMidi (keyboard, std::move (noteOn));
        peak = std::max (peak, ctx.pump (8));

        dusk::MidiBuffer noteOff;
        const std::uint8_t up[] { 0x80, 60, 0 };
        noteOff.addEvent (up, 3, 0);
        ctx.pumpWithMidi (keyboard, std::move (noteOff));

        ctx.expect (peak > 1.0e-3f, "the loaded soundfont made no sound (peak "
                                    + std::to_string (peak) + ")");
        ctx.complete (ctx.verdict());
    }, "the soundfont load did not finish");
    return std::nullopt;
}

const ScenarioRegistrar loadedRegistrar { Scenario {
    "sfz.loaded_instrument_sounds", { "sfz", "midi" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return loadedSoundfontSounds (ctx, false); } } };
const ScenarioRegistrar restoredRegistrar { Scenario {
    "sfz.restored_instrument_sounds", { "sfz", "midi", "session" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return loadedSoundfontSounds (ctx, true); } } };
} // namespace
} // namespace duskstudio::scenario
#endif
