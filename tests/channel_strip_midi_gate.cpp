#include <catch2/catch_test_macros.hpp>

#include "dsp/ChannelStrip.h"

#include <array>
#include <cstdint>

TEST_CASE ("muted MIDI strips still process transport panic events",
           "[channel-strip][midi][regression][issue-460]")
{
    constexpr int kBlock = 64;
    duskstudio::ChannelStripParams params;
    duskstudio::ChannelStrip strip;
    strip.bind (params);
    strip.prepare (48000.0, kBlock);

    std::array<float, kBlock> masterL {}, masterR {};
    std::array<std::array<float, kBlock>, duskstudio::ChannelStrip::kNumBuses> busL {}, busR {};
    std::array<std::array<float, kBlock>, duskstudio::ChannelStrip::kNumAuxSends> auxL {}, auxR {};
    std::array<float*, duskstudio::ChannelStrip::kNumBuses> busLPtrs {}, busRPtrs {};
    std::array<float*, duskstudio::ChannelStrip::kNumAuxSends> auxLPtrs {}, auxRPtrs {};
    for (std::size_t i = 0; i < busLPtrs.size(); ++i)
    {
        busLPtrs[i] = busL[i].data();
        busRPtrs[i] = busR[i].data();
    }
    for (std::size_t i = 0; i < auxLPtrs.size(); ++i)
    {
        auxLPtrs[i] = auxL[i].data();
        auxRPtrs[i] = auxR[i].data();
    }

    juce::MidiBuffer panic;
    panic.addEvent (juce::MidiMessage::controllerEvent (1, 123, 0), 0);
    strip.processAndAccumulate (nullptr, nullptr, panic, true,
                                masterL.data(), masterR.data(),
                                busLPtrs, busRPtrs, auxLPtrs, auxRPtrs,
                                kBlock, false);

    REQUIRE (strip.getLastProcessedSamples() == kBlock);
}
