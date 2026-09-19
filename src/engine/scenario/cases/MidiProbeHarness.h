#pragma once

#include "../ScenarioContext.h"
#include "../../AudioEngine.h"
#include "../../../dsp/ChannelStrip.h"
#include "../../../session/Session.h"

#include <array>
#include <cstdint>
#include <string>

// Shared rigging for the MIDI scenarios: they all build live MIDI tracks, push
// raw messages at an input, and read the panic-probe fixture's counters back.
namespace duskstudio::scenario::midiprobe
{
inline void addMessage (dusk::MidiBuffer& buffer, std::uint8_t status,
                        std::uint8_t data1, std::uint8_t data2, int sampleOffset = 0)
{
    const std::array<std::uint8_t, 3> bytes { status, data1, data2 };
    buffer.addEvent (bytes.data(), (int) bytes.size(), sampleOffset);
}

// A track the engine treats as a live MIDI destination: MIDI mode, listening to
// `input` on every channel, and monitoring so the live pull also runs while the
// transport rolls. The caller recomputes the RT counters once for the set.
inline void makeLiveMidiTrack (ScenarioContext& ctx, int trackIndex, int input)
{
    auto& track = ctx.session().track (trackIndex);
    track.mode.store ((int) Track::Mode::Midi, std::memory_order_relaxed);
    track.inputMonitor.store (true, std::memory_order_relaxed);
    track.midiInputIndex.store (input, std::memory_order_relaxed);
    track.midiChannel.store (0, std::memory_order_relaxed);
}

constexpr float kSilentDb = -90.0f;

// The probe fixtures hold a voice as a DC offset, and the strip's own
// low-frequency response drains one over tens of blocks rather than cutting it
// at the block boundary the panic lands on. Pumps until the strip reads silent
// and returns how many blocks that took, or -1 if it never got there.
inline int pumpUntilSilent (ScenarioContext& ctx, int trackIndex, int maxBlocks)
{
    const auto& strip = ctx.engine().getChannelStrip (trackIndex);
    for (int b = 0; b <= maxBlocks; ++b)
    {
        if (strip.getOutLDb() <= kSilentDb) return b;
        ctx.pump (1);
    }
    return -1;
}

#if DUSKSTUDIO_HAS_NATIVE_CLAP
constexpr const char* kPanicProbeClapId = "studio.dusk.test.panic-probe";

inline bool loadPanicProbe (ScenarioContext& ctx, int trackIndex, std::string& errorOut)
{
    const auto fixture = ctx.fixture ("panic_probe.clap");
    if (! fixture)
    {
        errorOut = "panic_probe.clap did not resolve";
        return false;
    }
    return ctx.engine().getChannelStrip (trackIndex).getNativeClapSlot().load (
        *fixture, ScenarioContext::kSampleRate, ScenarioContext::kBlockSize,
        errorOut, kPanicProbeClapId);
}

// The fixture publishes its event counters as read-only parameters; -1 means
// the slot does not advertise one by that name.
inline double counter (const clap::NativeClapSlot& slot, const char* name)
{
    for (int i = 0; i < slot.paramCount(); ++i)
    {
        const auto* info = slot.paramInfo (i);
        if (info == nullptr || info->name != name) continue;
        double value = 0.0;
        if (slot.getParamValue (info->id, value)) return value;
    }
    return -1.0;
}

inline double counter (ScenarioContext& ctx, int trackIndex, const char* name)
{
    return counter (ctx.engine().getChannelStrip (trackIndex).getNativeClapSlot(), name);
}
#endif
} // namespace duskstudio::scenario::midiprobe
