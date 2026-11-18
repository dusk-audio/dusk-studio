#include <catch2/catch_test_macros.hpp>

#include "dsp/ChannelStrip.h"
#include "engine/AudioWorkerPool.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <memory>

// A hardware insert's send adds into device outputs that every strip shares,
// so the engine runs a strip on the audio thread whenever
// ChannelStrip::mayRunHardwareInsert says its next block may run the insert,
// and hands the rest to the Multicore DSP lanes. These cases hold that answer
// to what each block then does, through the crossfades into and out of the
// insert, and run two strips on one send pair through the engine's split.

namespace
{
using duskstudio::ChannelStrip;

constexpr int kBlock = 64;
constexpr double kRate = 48000.0;
constexpr int kDeviceChannels = 4;
constexpr int kSendL = 2, kSendR = 3;
using Block = std::array<float, kBlock>;

Block toneBlock()
{
    Block block {};
    for (std::size_t i = 0; i < block.size(); ++i)
        block[i] = 0.25f * (float) std::sin (6.283185307179586 * ((double) i + 0.5) / 16.0);
    return block;
}

const Block tone = toneBlock();

// The device the engine hands every strip: one set of inputs and outputs.
struct Device
{
    std::array<Block, kDeviceChannels> in {}, out {};
    std::array<const float*, kDeviceChannels> inPtrs {};
    std::array<float*, kDeviceChannels> outPtrs {};

    Device()
    {
        for (std::size_t c = 0; c < in.size(); ++c)
        {
            in[c] = tone;
            inPtrs[c] = in[c].data();
            outPtrs[c] = out[c].data();
        }
    }

    void clearOutputs()
    {
        for (auto& c : out) c.fill (0.0f);
    }

    bool sendWritten() const
    {
        for (const int c : { kSendL, kSendR })
            for (const float v : out[(std::size_t) c])
                if (std::abs (v) > 0.0f) return true;
        return false;
    }
};

// A mono strip on a tone, its hardware insert sending on device outputs 2/3
// and returning on inputs 0/1. It owns its mix buffers, as an engine lane does.
struct Strip
{
    duskstudio::ChannelStripParams params;
    duskstudio::HardwareInsertParams hardware;
    ChannelStrip strip;

    Block masterL {}, masterR {};
    std::array<Block, ChannelStrip::kNumBuses> busL {}, busR {};
    std::array<Block, ChannelStrip::kNumAuxSends> auxL {}, auxR {};
    std::array<float*, ChannelStrip::kNumBuses> busLPtrs {}, busRPtrs {};
    std::array<float*, ChannelStrip::kNumAuxSends> auxLPtrs {}, auxRPtrs {};
    juce::MidiBuffer midi;

    explicit Strip (int insertMode)
    {
        hardware.enabled.store (true);
        hardware.routing.publish (std::make_unique<duskstudio::HardwareInsertRouting> (
            duskstudio::HardwareInsertRouting { kSendL, kSendR, 0, 1, 0, 0 }));
        strip.bind (params);
        strip.bindHardwareInsert (hardware);
        strip.insertMode.store (insertMode, std::memory_order_release);
        strip.prepare (kRate, kBlock);
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
    }

    // What the engine asks before a block.
    bool latchForBlock()
    {
        strip.latchInsertRequest();
        return strip.mayRunHardwareInsert();
    }

    void process (Device& device)
    {
        masterL.fill (0.0f);
        masterR.fill (0.0f);
        strip.processAndAccumulate (tone.data(), nullptr, midi, false,
                                    masterL.data(), masterR.data(),
                                    busLPtrs, busRPtrs, auxLPtrs, auxRPtrs,
                                    kBlock, true,
                                    device.inPtrs.data(), kDeviceChannels,
                                    device.outPtrs.data(), kDeviceChannels);
    }

    bool requestedHardware() const
    {
        return strip.insertMode.load (std::memory_order_acquire) == ChannelStrip::kInsertHardware;
    }
    bool runningHardware() const { return strip.getActiveInsertMode() == ChannelStrip::kInsertHardware; }
};

// One block on its own device; returns whether it wrote the send. A block may
// write it only when the engine's question before the block said so.
struct Run
{
    int blocks = 0;
    int sendBlocksOffRequest = 0;   // the send written with another mode requested

    bool block (Strip& s)
    {
        Device device;
        const bool requested = s.requestedHardware();
        const bool mayRun = s.latchForBlock();
        s.process (device);
        const bool wrote = device.sendWritten();
        INFO ("block " << blocks << ": requested hardware " << requested
              << ", may run it " << mayRun << ", wrote the send " << wrote);
        REQUIRE (wrote == s.runningHardware());
        if (wrote) REQUIRE (mayRun);
        ++blocks;
        sendBlocksOffRequest += (wrote && ! requested) ? 1 : 0;
        return wrote;
    }
};
} // namespace

TEST_CASE ("A strip runs on the audio thread for every block that writes its hardware send",
           "[channel-strip][multicore][hardware-insert]")
{
    SECTION ("leaving the hardware insert, through the crossfade out")
    {
        for (const int leaveTo : { (int) ChannelStrip::kInsertEmpty, (int) ChannelStrip::kInsertPlugin })
        {
            INFO ("leaving for insert mode " << leaveTo);
            Strip s { ChannelStrip::kInsertHardware };
            Run run;
            for (int b = 0; b < 4; ++b)
                REQUIRE (run.block (s));

            s.strip.insertMode.store (leaveTo, std::memory_order_release);
            // Requested and running modes differ until the crossfade is down.
            REQUIRE_FALSE (s.requestedHardware());
            REQUIRE (s.runningHardware());
            REQUIRE (s.latchForBlock());
            int b = 0;
            while (b < 200 && run.block (s)) ++b;
            REQUIRE (b < 200);
            REQUIRE (run.sendBlocksOffRequest > 0);
            REQUIRE (s.strip.getActiveInsertMode() == leaveTo);
            REQUIRE_FALSE (s.latchForBlock());
            for (int i = 0; i < 4; ++i)
                REQUIRE_FALSE (run.block (s));
        }
    }

    SECTION ("entering the hardware insert, from its first block")
    {
        Strip s { ChannelStrip::kInsertEmpty };
        Run run;
        for (int b = 0; b < 4; ++b)
            REQUIRE_FALSE (run.block (s));
        REQUIRE_FALSE (s.latchForBlock());

        s.strip.insertMode.store (ChannelStrip::kInsertHardware, std::memory_order_release);
        REQUIRE (run.block (s));
        for (int b = 0; b < 4; ++b)
            REQUIRE (run.block (s));
    }

    SECTION ("a request landing after the engine asked waits for the next block")
    {
        // The engine asks in its prep pass and the block may run later on a
        // worker; an empty insert has no crossfade to wait for, so the block
        // would switch to the hardware insert on whatever it read itself.
        Strip s { ChannelStrip::kInsertEmpty };
        Run run;
        for (int b = 0; b < 4; ++b)
            REQUIRE_FALSE (run.block (s));

        REQUIRE_FALSE (s.latchForBlock());
        s.strip.insertMode.store (ChannelStrip::kInsertHardware, std::memory_order_release);
        Device device;
        s.process (device);
        REQUIRE_FALSE (device.sendWritten());
        REQUIRE (s.strip.getActiveInsertMode() == ChannelStrip::kInsertEmpty);

        REQUIRE (run.block (s));
    }
}

TEST_CASE ("Two strips leaving hardware inserts on one send pair never send from a worker lane",
           "[channel-strip][multicore][hardware-insert]")
{
    // The engine's split with two lanes: the pool's worker runs lane 0 and
    // the calling thread lane 1, each a strip the engine did not hold back,
    // then the calling thread runs the held strips after the join. Both
    // strips send on the same device outputs, so one sending from a lane
    // while the other sends from the other lane is a data race on them.
    Strip first { ChannelStrip::kInsertHardware }, second { ChannelStrip::kInsertHardware };
    const std::array<Strip*, 2> strips { &first, &second };
    Device device;
    std::array<bool, 2> held {};
    std::array<int, 2> laneBlocks {}, laneSends {};

    duskstudio::AudioWorkerPool pool;
    pool.start (1, [&] (int lane)
    {
        auto& s = *strips[(std::size_t) lane];
        if (held[(std::size_t) lane]) return;
        s.process (device);
        ++laneBlocks[(std::size_t) lane];
        laneSends[(std::size_t) lane] += s.runningHardware() ? 1 : 0;
    });
    REQUIRE (pool.laneCount() == 2);

    int heldSends = 0;
    const auto block = [&]
    {
        device.clearOutputs();
        for (std::size_t i = 0; i < strips.size(); ++i)
            held[i] = strips[i]->latchForBlock();
        pool.runBlock();
        for (std::size_t i = 0; i < strips.size(); ++i)
            if (held[i])
            {
                strips[i]->process (device);
                heldSends += strips[i]->runningHardware() ? 1 : 0;
            }
    };

    // Read here after the pool's join, which orders the lanes' writes.
    const auto requireNoLaneSent = [&]
    {
        INFO ("lane blocks " << laneBlocks[0] << "/" << laneBlocks[1]
              << ", lane blocks that sent " << laneSends[0] << "/" << laneSends[1]);
        REQUIRE (laneSends[0] == 0);
        REQUIRE (laneSends[1] == 0);
    };

    for (int b = 0; b < 4; ++b) block();
    REQUIRE (heldSends == 8);

    for (auto* s : strips)
        s->strip.insertMode.store (ChannelStrip::kInsertEmpty, std::memory_order_release);
    for (int b = 0; b < 40; ++b) block();
    requireNoLaneSent();
    REQUIRE (heldSends > 8);
    REQUIRE (laneBlocks[0] > 0);
    REQUIRE (laneBlocks[1] > 0);
    REQUIRE_FALSE (device.sendWritten());

    // And back in, with the request landing between the engine's question and
    // the lanes for one strip.
    first.strip.insertMode.store (ChannelStrip::kInsertHardware, std::memory_order_release);
    device.clearOutputs();
    for (std::size_t i = 0; i < strips.size(); ++i)
        held[i] = strips[i]->latchForBlock();
    second.strip.insertMode.store (ChannelStrip::kInsertHardware, std::memory_order_release);
    pool.runBlock();
    for (std::size_t i = 0; i < strips.size(); ++i)
        if (held[i]) strips[i]->process (device);
    for (int b = 0; b < 4; ++b) block();
    requireNoLaneSent();
    REQUIRE (device.sendWritten());

    pool.stop();
}
