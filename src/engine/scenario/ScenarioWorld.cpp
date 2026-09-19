#include "ScenarioWorld.h"

#include "ScenarioContext.h"
#include "../AudioEngine.h"
#include "../../dsp/AuxLaneStrip.h"
#include "../../dsp/ChannelStrip.h"
#include "../../session/Session.h"

#include <cstddef>
#include <string>
#include <vector>

namespace duskstudio::scenario
{
namespace
{
// Every persisted insert identity, so a plugin one scenario saved cannot be
// restored into the next one's strips.
void clearPluginState (Track& track)
{
    track.pluginDescriptor.reset();
    track.pluginLegacyDescriptionXml.clear();
    track.pluginStateBase64.clear();
    track.nativeClapPath.clear();
    track.nativeClapPluginId.clear();
    track.nativeClapStateBase64.clear();
    track.nativeLv2Path.clear();
    track.nativeLv2PluginId.clear();
    track.nativeLv2StateBase64.clear();
    track.nativeVst3Path.clear();
    track.nativeVst3PluginId.clear();
    track.nativeVst3StateBase64.clear();
    track.nativeAuIdentifier.clear();
    track.nativeAuStateBase64.clear();
    track.nativeMultisamplePath.clear();
    track.nativeMultisampleStateBase64.clear();
}

void clearPluginState (AuxLane& lane, int slot)
{
    const auto s = (std::size_t) slot;
    lane.pluginDescriptor[s].reset();
    lane.pluginLegacyDescriptionXml[s].clear();
    lane.pluginStateBase64[s].clear();
    lane.nativeClapPath[s].clear();
    lane.nativeClapPluginId[s].clear();
    lane.nativeClapStateBase64[s].clear();
    lane.nativeLv2Path[s].clear();
    lane.nativeLv2PluginId[s].clear();
    lane.nativeLv2StateBase64[s].clear();
    lane.nativeVst3Path[s].clear();
    lane.nativeVst3PluginId[s].clear();
    lane.nativeVst3StateBase64[s].clear();
    lane.nativeAuIdentifier[s].clear();
    lane.nativeAuStateBase64[s].clear();
}
} // namespace

ScenarioWorld::ScenarioWorld()
    : sessionPtr (std::make_unique<Session>()),
      enginePtr (std::make_unique<AudioEngine> (*sessionPtr)),
      bootstrapSessionDir (currentSessionDirectory (*sessionPtr))
{
    prepareOffline();
}

ScenarioWorld::~ScenarioWorld() = default;

void ScenarioWorld::prepareOffline()
{
    // The engine constructor opened a real device and attached itself. Drop both
    // before anything runs: a suite invocation must not hold hardware, and every
    // block it processes comes from ScenarioContext::pump.
    enginePtr->detachAudioCallback();
    enginePtr->getDeviceManager().closeDevice();
    enginePtr->prepareForSelfTest (ScenarioContext::kSampleRate, ScenarioContext::kBlockSize);
}

void ScenarioWorld::reset()
{
    auto& engineRef = *enginePtr;
    auto& sessionRef = *sessionPtr;

    engineRef.stop();

    auto& transport = engineRef.getTransport();
    transport.setPlayhead (0);
    transport.setLoopEnabled (false);
    transport.setLoopRange (0, 0);
    transport.setPunchEnabled (false);

    for (int t = 0; t < Session::kNumTracks; ++t)
    {
        auto& track = sessionRef.track (t);
        auto& strip = track.strip;

        track.mode.store ((int) Track::Mode::Mono, std::memory_order_relaxed);
        track.recordArmed.store (false, std::memory_order_relaxed);
        track.inputMonitor.store (false, std::memory_order_relaxed);
        track.inputSource.store (-2, std::memory_order_relaxed);
        track.frozen.store (false, std::memory_order_relaxed);
        track.name = std::to_string (t + 1);
        strip.faderDb.store (0.0f, std::memory_order_relaxed);
        strip.liveFaderDb.store (0.0f, std::memory_order_relaxed);
        strip.pan.store (0.0f, std::memory_order_relaxed);
        strip.livePan.store (0.0f, std::memory_order_relaxed);
        for (int a = 0; a < ChannelStripParams::kNumAuxSends; ++a)
        {
            strip.auxSendDb[(std::size_t) a].store (ChannelStripParams::kAuxSendOffDb,
                                                   std::memory_order_relaxed);
            strip.liveAuxSendDb[(std::size_t) a].store (ChannelStripParams::kAuxSendOffDb,
                                                       std::memory_order_relaxed);
        }
        strip.mute.store (false, std::memory_order_relaxed);
        strip.solo.store (false, std::memory_order_relaxed);
        // The routed twins the audio thread actually gates on. They only catch
        // up on the next block, and the solo gate is sampled before that.
        strip.liveMute.store (false, std::memory_order_relaxed);
        strip.liveSolo.store (false, std::memory_order_relaxed);

        // Routing leaks are as poisonous to the next scenario as a stuck solo:
        // a track left assigned to a bus never reaches master directly again.
        for (int b = 0; b < ChannelStripParams::kNumBuses; ++b)
            strip.busAssign[(std::size_t) b].store (false, std::memory_order_relaxed);

        track.regions.clear();
        track.midiRegions.publish (std::make_unique<std::vector<MidiRegion>>());

        track.midiInputIndex.store (-1, std::memory_order_relaxed);
        track.midiOutputIndex.store (-1, std::memory_order_relaxed);
        track.midiChannel.store (0, std::memory_order_relaxed);
        track.midiActivity.store (false, std::memory_order_relaxed);

        auto& channelStrip = engineRef.getChannelStrip (t);
        channelStrip.unloadNativeClap();
        channelStrip.unloadNativeLv2();
        channelStrip.unloadNativeVst3();
        channelStrip.unloadNativeAu();
        channelStrip.unloadNativeMultisample();
        channelStrip.getPluginSlot().unload();

        // A restore reconstructs the mode from what the session holds, so a
        // scenario that loaded a session leaves plugin-less strips routing
        // around their insert. Back to the as-constructed default, or the next
        // scenario's insert is live but starved.
        channelStrip.insertMode.store (ChannelStrip::kInsertPlugin, std::memory_order_release);

        clearPluginState (track);
    }

    for (int b = 0; b < Session::kNumBuses; ++b)
    {
        sessionRef.bus (b).strip.mute.store (false, std::memory_order_relaxed);
        sessionRef.bus (b).strip.solo.store (false, std::memory_order_relaxed);
    }

    sessionRef.master().faderDb.store (0.0f, std::memory_order_relaxed);
    sessionRef.master().liveFaderDb.store (0.0f, std::memory_order_relaxed);
    sessionRef.master().mute.store (false, std::memory_order_relaxed);

    for (int lane = 0; lane < Session::kNumAuxLanes; ++lane)
    {
        auto& auxStrip = engineRef.getAuxLaneStrip (lane);
        auto& auxParams = sessionRef.auxLane (lane);
        for (int slot = 0; slot < AuxLaneStrip::kMaxPlugins; ++slot)
        {
            auxStrip.unloadNativeClap (slot);
            auxStrip.unloadNativeLv2 (slot);
            auxStrip.unloadNativeVst3 (slot);
            auxStrip.unloadNativeAu (slot);
            auxStrip.getPluginSlot (slot).unload();
            auxStrip.insertMode[(std::size_t) slot].store (AuxLaneStrip::kInsertEmpty,
                                                           std::memory_order_release);
            clearPluginState (auxParams, slot);
        }
    }

    // A scenario that mints or loads a session leaves the directory pointing at
    // its own scratch, which is gone by the time the next one runs. An empty
    // bootstrap is left alone: setting one resolves the audio subdirectory
    // against the working directory and creates it there.
    if (! bootstrapSessionDir.empty())
        applySessionDirectory (sessionRef, bootstrapSessionDir);

    // The bulk stores above bypassed the counter-aware setters.
    sessionRef.recomputeRtCounters();

    // Re-detached, not just re-prepared: a scenario may have handed the engine
    // back to a real device (the pipeline self-test reattaches at its end), and
    // the next one must drive blocks itself with no hardware running.
    prepareOffline();
    engineRef.recomputePdc();
}
} // namespace duskstudio::scenario
