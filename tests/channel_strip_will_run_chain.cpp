#include <catch2/catch_test_macros.hpp>

#include "dsp/ChannelStrip.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <string>

// ChannelStrip::willRunChain is the engine's guess, made before a block, of
// whether the strip will run its chain in that block. These cases hold the
// guess to what processAndAccumulate then does, block by block, in the states
// the engine's multicore dispatch meets.

namespace
{
using duskstudio::ChannelStrip;

constexpr int kBlock = 64;
constexpr double kRate = 48000.0;
using Block = std::array<float, kBlock>;

Block toneBlock (float amplitude)
{
    Block block {};
    for (std::size_t i = 0; i < block.size(); ++i)
        block[i] = amplitude * (float) std::sin (6.283185307179586 * (double) i / 16.0);
    return block;
}

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

    int ran = 0, skipped = 0;

    explicit Strip (int insertMode)
    {
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

    // One block: the guess first, then the block itself, which has to agree.
    // Returns whether the chain ran.
    bool block (const float* inL, const float* inR, bool isMidi, bool passByGate,
                bool isFrozen = false, int numSamples = kBlock)
    {
        const bool predicted = strip.willRunChain (inL, inR, isMidi, numSamples, passByGate, isFrozen);
        strip.processAndAccumulate (inL, inR, midi, isMidi,
                                    masterL.data(), masterR.data(),
                                    busLPtrs, busRPtrs, auxLPtrs, auxRPtrs,
                                    numSamples, passByGate,
                                    nullptr, 0, nullptr, 0, isFrozen);
        const bool chainRan = strip.getLastProcessedSamples() > 0;
        INFO ("block " << (ran + skipped) << ": predicted " << predicted << ", chain ran " << chainRan);
        REQUIRE (predicted == chainRan);
        ++(chainRan ? ran : skipped);
        return chainRan;
    }

    // Blocks of the same input until the strip has been asleep for a few
    // blocks in a row, or the limit runs out. Returns the blocks that ran.
    int untilAsleep (const float* inL, const float* inR, bool isMidi = false, bool isFrozen = false)
    {
        int ranHere = 0, asleepInRow = 0;
        for (int b = 0; b < 2000 && asleepInRow < 4; ++b)
        {
            if (block (inL, inR, isMidi, true, isFrozen)) { ++ranHere; asleepInRow = 0; }
            else ++asleepInRow;
        }
        REQUIRE (asleepInRow == 4);
        return ranHere;
    }
};

const Block silence {};
} // namespace

TEST_CASE ("A strip's chain prediction matches its next block",
           "[channel-strip][multicore]")
{
    const Block tone = toneBlock (0.25f);

    SECTION ("an audio track with no input does not run")
    {
        Strip s { ChannelStrip::kInsertEmpty };
        for (int b = 0; b < 4; ++b)
            REQUIRE_FALSE (s.block (nullptr, nullptr, false, true));
    }

    SECTION ("silent input on an empty insert sleeps from the first block")
    {
        Strip s { ChannelStrip::kInsertEmpty };
        REQUIRE (s.untilAsleep (silence.data(), nullptr) == 0);
        s.untilAsleep (silence.data(), silence.data());
    }

    SECTION ("playing, then a silent tail, then asleep")
    {
        Strip s { ChannelStrip::kInsertEmpty };
        for (int b = 0; b < 8; ++b)
            REQUIRE (s.block (tone.data(), nullptr, false, true));
        // The tail runs on silent input until its output has been quiet for
        // the hold, and every block of it has to be predicted.
        REQUIRE (s.untilAsleep (silence.data(), nullptr) > 0);
        REQUIRE (s.block (tone.data(), nullptr, false, true));
    }

    SECTION ("a stereo track wakes on either side")
    {
        Strip s { ChannelStrip::kInsertEmpty };
        s.untilAsleep (silence.data(), silence.data());
        REQUIRE (s.block (silence.data(), tone.data(), false, true));
        s.untilAsleep (silence.data(), silence.data());
        REQUIRE (s.block (tone.data(), silence.data(), false, true));
    }

    SECTION ("monitored input far below the meter floor still runs")
    {
        Strip s { ChannelStrip::kInsertEmpty };
        s.untilAsleep (silence.data(), nullptr);
        const Block faint = toneBlock (1.0e-9f);
        REQUIRE (s.block (faint.data(), nullptr, false, true));
    }

   #if DUSKSTUDIO_HAS_DUSK_DSP
    SECTION ("an oversampled strip holds its tail for the filters' latency")
    {
        Strip s { ChannelStrip::kInsertEmpty };
        s.strip.prepare (kRate, kBlock, 4);
        REQUIRE (s.strip.getOversamplingLatencySamples() > 0);
        for (int b = 0; b < 4; ++b)
            REQUIRE (s.block (tone.data(), nullptr, false, true));
        REQUIRE (s.untilAsleep (silence.data(), nullptr) > 0);
    }
   #endif

    SECTION ("an insert keeps the strip running on silence")
    {
        SECTION ("plug-in mode with nothing loaded")
        {
            Strip s { ChannelStrip::kInsertPlugin };
            for (int b = 0; b < 4; ++b)
                REQUIRE (s.block (silence.data(), nullptr, false, true));
        }
        SECTION ("a built-in unit loaded")
        {
            Strip s { ChannelStrip::kInsertPlugin };
            std::string error;
            REQUIRE (s.strip.loadBuiltin ("dusk.builtin.utility", error));
            for (int b = 0; b < 4; ++b)
                REQUIRE (s.block (silence.data(), nullptr, false, true));
        }
        SECTION ("a hardware insert")
        {
            Strip s { ChannelStrip::kInsertHardware };
            for (int b = 0; b < 4; ++b)
                REQUIRE (s.block (silence.data(), nullptr, false, true));
        }
    }

    SECTION ("muted or soloed out, the gate alone decides")
    {
        Strip s { ChannelStrip::kInsertEmpty };
        for (int b = 0; b < 4; ++b)
            REQUIRE_FALSE (s.block (tone.data(), nullptr, false, false));

        s.strip.setNeedsProcessedMono (true);
        REQUIRE (s.block (tone.data(), nullptr, false, false));
        s.strip.setNeedsProcessedMono (false);
        REQUIRE_FALSE (s.block (tone.data(), nullptr, false, false));

        std::array<float, kBlock> captureL {}, captureR {};
        s.strip.setFreezeCapture (captureL.data(), captureR.data());
        REQUIRE (s.block (tone.data(), nullptr, false, false));
        s.strip.setFreezeCapture (nullptr, nullptr);
        REQUIRE_FALSE (s.block (tone.data(), nullptr, false, false));
    }

    SECTION ("a hardware-insert ping runs a muted strip")
    {
        Strip s { ChannelStrip::kInsertHardware };
        REQUIRE_FALSE (s.block (silence.data(), nullptr, false, false));
        s.hardware.pingPending.store (true, std::memory_order_release);
        REQUIRE (s.block (silence.data(), nullptr, false, false));
    }

    SECTION ("routed to a bus and the aux sends only")
    {
        Strip s { ChannelStrip::kInsertEmpty };
        s.params.busAssign[0].store (true, std::memory_order_relaxed);
        for (int a = 0; a < ChannelStrip::kNumAuxSends; ++a)
            s.params.auxSendDb[(std::size_t) a].store (-6.0f, std::memory_order_relaxed);
        for (int b = 0; b < 4; ++b)
            REQUIRE (s.block (tone.data(), nullptr, false, true));
        REQUIRE (s.untilAsleep (silence.data(), nullptr) > 0);
    }

    SECTION ("a MIDI track runs only with something loaded to play")
    {
        Strip s { ChannelStrip::kInsertPlugin };
        REQUIRE_FALSE (s.block (nullptr, nullptr, true, true));
        REQUIRE_FALSE (s.block (nullptr, nullptr, true, false));
        std::string error;
        REQUIRE (s.strip.loadBuiltin ("dusk.builtin.utility", error));
        REQUIRE (s.block (nullptr, nullptr, true, true));
        REQUIRE (s.block (nullptr, nullptr, true, false));
    }

    SECTION ("an insert mode change is predicted across the crossfade")
    {
        Strip s { ChannelStrip::kInsertEmpty };
        s.untilAsleep (silence.data(), nullptr);
        s.strip.insertMode.store (ChannelStrip::kInsertPlugin, std::memory_order_release);
        REQUIRE (s.block (silence.data(), nullptr, false, true));
        s.strip.insertMode.store (ChannelStrip::kInsertEmpty, std::memory_order_release);
        REQUIRE (s.untilAsleep (silence.data(), nullptr) > 0);
    }

    SECTION ("a frozen track plays its render and sleeps on silence")
    {
        Strip s { ChannelStrip::kInsertEmpty };
        REQUIRE (s.block (tone.data(), tone.data(), false, true, true));
        REQUIRE (s.untilAsleep (silence.data(), silence.data(), false, true) > 0);
    }

    SECTION ("an empty or oversized block does not run")
    {
        Strip s { ChannelStrip::kInsertPlugin };
        REQUIRE_FALSE (s.block (tone.data(), nullptr, false, true, false, 0));
        std::array<float, 2 * kBlock> big {};
        for (std::size_t i = 0; i < big.size(); ++i) big[i] = 0.25f;
        REQUIRE_FALSE (s.block (big.data(), nullptr, false, true, false, 2 * kBlock));
    }
}
