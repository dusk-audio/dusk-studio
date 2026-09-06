#include "ScenarioWorld.h"

#include "ScenarioContext.h"
#include "../AudioEngine.h"
#include "../../dsp/AuxLaneStrip.h"
#include "../../dsp/ChannelStrip.h"
#include "../../session/Session.h"

#include <vector>

namespace duskstudio::scenario
{
ScenarioWorld::ScenarioWorld()
    : sessionPtr (std::make_unique<Session>()),
      enginePtr (std::make_unique<AudioEngine> (*sessionPtr))
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
        strip.mute.store (false, std::memory_order_relaxed);
        strip.solo.store (false, std::memory_order_relaxed);

        // Routing leaks are as poisonous to the next scenario as a stuck solo:
        // a track left assigned to a bus never reaches master directly again.
        for (int b = 0; b < ChannelStripParams::kNumBuses; ++b)
            strip.busAssign[(std::size_t) b].store (false, std::memory_order_relaxed);

        track.regions.clear();
        track.midiRegions.publish (std::make_unique<std::vector<MidiRegion>>());

        auto& channelStrip = engineRef.getChannelStrip (t);
        channelStrip.unloadNativeClap();
        channelStrip.unloadNativeLv2();
        channelStrip.unloadNativeVst3();
        channelStrip.unloadNativeAu();
        channelStrip.unloadNativeMultisample();
    }

    for (int b = 0; b < Session::kNumBuses; ++b)
    {
        sessionRef.bus (b).strip.mute.store (false, std::memory_order_relaxed);
        sessionRef.bus (b).strip.solo.store (false, std::memory_order_relaxed);
    }

    for (int lane = 0; lane < Session::kNumAuxLanes; ++lane)
    {
        auto& auxStrip = engineRef.getAuxLaneStrip (lane);
        for (int slot = 0; slot < AuxLaneStrip::kMaxPlugins; ++slot)
        {
            auxStrip.unloadNativeClap (slot);
            auxStrip.unloadNativeLv2 (slot);
            auxStrip.unloadNativeVst3 (slot);
            auxStrip.unloadNativeAu (slot);
        }
    }

    // The bulk stores above bypassed the counter-aware setters.
    sessionRef.recomputeRtCounters();

    // Re-detached, not just re-prepared: a scenario may have handed the engine
    // back to a real device (the pipeline self-test reattaches at its end), and
    // the next one must drive blocks itself with no hardware running.
    prepareOffline();
    engineRef.recomputePdc();
}
} // namespace duskstudio::scenario
