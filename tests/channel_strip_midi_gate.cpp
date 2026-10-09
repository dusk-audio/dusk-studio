#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "dsp/ChannelStrip.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <random>
#include <system_error>

using Catch::Matchers::WithinAbs;

namespace
{
constexpr int kBlock = 64;

struct StripHarness
{
    duskstudio::ChannelStripParams params;
    duskstudio::ChannelStrip strip;

    std::array<float, kBlock> masterL {}, masterR {};
    std::array<std::array<float, kBlock>, duskstudio::ChannelStrip::kNumBuses> busL {}, busR {};
    std::array<std::array<float, kBlock>, duskstudio::ChannelStrip::kNumAuxSends> auxL {}, auxR {};
    std::array<float*, duskstudio::ChannelStrip::kNumBuses> busLPtrs {}, busRPtrs {};
    std::array<float*, duskstudio::ChannelStrip::kNumAuxSends> auxLPtrs {}, auxRPtrs {};

    explicit StripHarness (int insertMode)
    {
        strip.bind (params);
        strip.insertMode.store (insertMode, std::memory_order_release);
        strip.prepare (48000.0, kBlock);

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

    void runMidiBlock (juce::MidiBuffer& midi, bool passByGate)
    {
        strip.processAndAccumulate (nullptr, nullptr, midi, true,
                                    masterL.data(), masterR.data(),
                                    busLPtrs, busRPtrs, auxLPtrs, auxRPtrs,
                                    kBlock, passByGate);
    }

    // A block of a stereo audio track: silent input, no MIDI.
    void runStereoBlock()
    {
        static const std::array<float, kBlock> silence {};
        juce::MidiBuffer none;
        strip.processAndAccumulate (silence.data(), silence.data(), none, false,
                                    masterL.data(), masterR.data(),
                                    busLPtrs, busRPtrs, auxLPtrs, auxRPtrs,
                                    kBlock, true);
    }

    void clearAccumulators()
    {
        masterL.fill (0.0f);
        masterR.fill (0.0f);
        for (auto& b : busL) b.fill (0.0f);
        for (auto& b : busR) b.fill (0.0f);
        for (auto& a : auxL) a.fill (0.0f);
        for (auto& a : auxR) a.fill (0.0f);
    }

    float accumulatorPeak() const
    {
        float peak = 0.0f;
        const auto scan = [&peak] (const std::array<float, kBlock>& buf)
        {
            for (const auto v : buf) peak = std::max (peak, std::abs (v));
        };
        scan (masterL);
        scan (masterR);
        for (const auto& b : busL) scan (b);
        for (const auto& b : busR) scan (b);
        for (const auto& a : auxL) scan (a);
        for (const auto& a : auxR) scan (a);
        return peak;
    }
};
} // namespace

TEST_CASE ("muted MIDI strips still process transport panic events",
           "[channel-strip][midi][regression][issue-460]")
{
   #if ! DUSKSTUDIO_HAS_NATIVE_CLAP
    SKIP ("needs the native CLAP host to load an instrument into the strip");
   #else
    StripHarness h { duskstudio::ChannelStrip::kInsertPlugin };

    // The gate keys on a loaded plugin, not the insert mode. The fixture is an
    // effect on purpose: an effect on a MIDI track still receives the block's
    // MIDI and can hold notes, so it must keep getting panic events while the
    // track is muted.
    std::string error;
    REQUIRE (h.strip.getNativeClapSlot().load (
        std::filesystem::u8path (DUSKSTUDIO_MULTI_BUS_CLAP_FIXTURE_PATH),
        48000.0, kBlock, error));

    juce::MidiBuffer panic;
    panic.addEvent (juce::MidiMessage::controllerEvent (1, 123, 0), 0);
    h.runMidiBlock (panic, false);

    REQUIRE (h.strip.getLastProcessedSamples() == kBlock);
   #endif
}

TEST_CASE ("MIDI strips with no instrument skip the chain",
           "[channel-strip][midi][regression][issue-492]")
{
    // Every insert mode, including the kInsertPlugin a fresh strip starts in:
    // none of them loads an instrument by itself.
    const int mode = GENERATE (duskstudio::ChannelStrip::kInsertEmpty,
                               duskstudio::ChannelStrip::kInsertPlugin,
                               duskstudio::ChannelStrip::kInsertHardware);
    StripHarness h { mode };
    juce::MidiBuffer notes;
    notes.addEvent (juce::MidiMessage::noteOn (1, 60, (std::uint8_t) 100), 0);

    SECTION ("muted or soloed out")
    {
        h.runMidiBlock (notes, false);
    }

    SECTION ("passing to master")
    {
        h.runMidiBlock (notes, true);
    }

    REQUIRE (h.strip.getLastProcessedSamples() == 0);
    REQUIRE_THAT (h.strip.getOutLDb(), WithinAbs (-100.0f, 1e-6f));
    REQUIRE (h.accumulatorPeak() <= 0.0f);
}

TEST_CASE ("a multisample instrument that leaves MIDI mode is reset when the track comes back",
           "[channel-strip][midi][multisample][regression]")
{
   #if ! DUSKSTUDIO_HAS_MULTISAMPLE
    SKIP ("needs the multisample host to load an instrument into the strip");
   #else
    // The multisample slot plays only on a MIDI track; as an effect insert the
    // strip runs the other hosts. An audio-mode block must therefore leave the
    // owed reset for the engine to send when the track is fed MIDI again (an
    // import's switch to Stereo, then its undo), or the key held across the
    // switch sounds until Stop.
    const auto sfz = std::filesystem::temp_directory_path()
                   / ("dusk-strip-owed-reset-" + std::to_string (std::random_device{}()) + ".sfz");
    struct Remove
    {
        std::filesystem::path path;
        ~Remove() { std::error_code ec; std::filesystem::remove (path, ec); }
    } remove { sfz };
    {
        std::ofstream out (sfz);
        out << "<region> key=60 sample=*sine\n";
    }

    StripHarness h { duskstudio::ChannelStrip::kInsertPlugin };
    std::string error;
    REQUIRE (h.strip.loadNativeMultisample (juce::File (sfz.string()), error));

    const auto runMidi = [&h] (bool keyDown)
    {
        juce::MidiBuffer midi;
        if (keyDown)
            midi.addEvent (juce::MidiMessage::noteOn (1, 60, (std::uint8_t) 100), 0);
        // What AudioEngine adds when the strip reports the debt: the reset
        // heads the first block the track is fed MIDI again.
        if (h.strip.owesMidiReset())
            for (int ch = 1; ch <= 16; ++ch)
                for (const int cc : { 64, 123, 120 })
                    midi.addEvent (juce::MidiMessage::controllerEvent (ch, cc, 0), 0);
        h.clearAccumulators();
        h.runMidiBlock (midi, true);
        return h.accumulatorPeak();
    };

    runMidi (true);
    float sounding = 0.0f;
    for (int b = 0; b < 8; ++b)
        sounding = runMidi (false);
    REQUIRE (sounding > 0.01f);

    h.runStereoBlock();
    REQUIRE (h.strip.owesMidiReset());

    float after = 1.0f;
    for (int b = 0; b < 64; ++b)
        after = runMidi (false);
    REQUIRE (after < 1.0e-4f);
   #endif
}
