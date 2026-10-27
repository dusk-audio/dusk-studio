// Increment 1 of the native CLAP host: load a real .clap, create + activate the
// instance, and process audio offline. Gated on DUSKSTUDIO_TEST_CLAP=/path/to.clap
// (e.g. ~/.clap/DuskVerb.clap) so CI without a CLAP plugin stays green.
// See docs/archive/native-clap-host-plan.md.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/clap/ClapBundle.h"
#include "engine/clap/ClapInstance.h"
#include "engine/hosting/InsertAdapter.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

TEST_CASE ("ClapInstance connects every advertised bus and tolerates duplicate main flags",
           "[clap][instance][regression][issue-361]")
{
    const std::string fixture = DUSKSTUDIO_MULTI_BUS_CLAP_FIXTURE_PATH;
    duskstudio::clap::ClapBundle bundle;
    std::string err;
    REQUIRE (bundle.load (fixture, err));
    REQUIRE (bundle.plugins().size() == 1);

    duskstudio::clap::ClapInstance inst;
    REQUIRE (inst.create (bundle, bundle.plugins().front().id, err));
    REQUIRE (inst.portLayout().inputs.size() == 3);
    REQUIRE (inst.portLayout().outputs.size() == 2);
    REQUIRE (inst.portLayout().mainInIndex == 0);
    REQUIRE (inst.portLayout().mainOutIndex == 0);
    REQUIRE (inst.portLayout().eventInIndex == 2);
    REQUIRE (inst.activate (48000.0, 64, err));

    duskstudio::hosting::InsertAdapter adapter;
    adapter.prepare (inst.portLayout(), 64);
    std::vector<float> left (64), right (64);
    for (size_t i = 0; i < left.size(); ++i)
    {
        left[i] = (float) i / 64.0f;
        right[i] = -(float) i / 64.0f;
    }
    const auto expectedLeft = left;
    const auto expectedRight = right;

    adapter.process (inst, left.data(), right.data(), 64);
    for (size_t i = 0; i < left.size(); ++i)
    {
        REQUIRE_THAT (left[i], Catch::Matchers::WithinAbs (expectedLeft[i], 1.0e-7f));
        REQUIRE_THAT (right[i], Catch::Matchers::WithinAbs (expectedRight[i], 1.0e-7f));
    }
}

TEST_CASE ("ClapInstance chokes every CLAP note-input port on the transport panic",
           "[clap][instance][midi][regression][issue-402][issue-460]")
{
    duskstudio::clap::ClapBundle bundle;
    std::string err;
    REQUIRE (bundle.load (DUSKSTUDIO_MULTI_BUS_CLAP_FIXTURE_PATH, err));
    REQUIRE (bundle.plugins().size() == 1);

    duskstudio::clap::ClapInstance inst;
    REQUIRE (inst.create (bundle, bundle.plugins().front().id, err));
    REQUIRE (inst.activate (48000.0, 64, err));

    std::array<float, 64> inL {}, inR {}, outL {}, outR {};
    float* inputs[] = { inL.data(), inR.data() };
    float* outputs[] = { outL.data(), outR.data() };
    duskstudio::hosting::PortBuffers io;
    io.mainIn = inputs;
    io.mainInChannels = 2;
    io.mainOut = outputs;
    io.mainOutChannels = 2;
    io.numFrames = 64;

    dusk::MidiBuffer noteOn;
    const std::array<std::uint8_t, 3> note { 0x90, 60, 100 };
    REQUIRE (noteOn.addEvent (note.data(), (int) note.size(), 0));
    io.midiIn = &noteOn;
    inst.processBlock (io);
    REQUIRE_THAT (outL[0], Catch::Matchers::WithinAbs (0.25f, 1.0e-7f));

    // This is the engine's transport-stop sequence. The fixture advertises
    // only CLAP note events, so raw CCs cannot reach it; CC 120 must become a
    // wildcard CLAP NOTE_CHOKE on every declared note-input port and channel.
    // The fixture exposes 80 ports so the full reset exceeds the old fixed
    // 1,024-event scratch capacity.
    dusk::MidiBuffer panic;
    for (int channel = 0; channel < 16; ++channel)
    {
        for (const std::uint8_t controller : { std::uint8_t { 64 },
                                               std::uint8_t { 123 },
                                               std::uint8_t { 120 } })
        {
            const std::array<std::uint8_t, 3> cc {
                (std::uint8_t) (0xB0 | channel), controller, 0 };
            REQUIRE (panic.addEvent (cc.data(), (int) cc.size(), 0));
        }
    }
    io.midiIn = &panic;
    inst.processBlock (io);
    REQUIRE_THAT (outL[0], Catch::Matchers::WithinAbs (0.0f, 1.0e-7f));
    REQUIRE_THAT (outR[0], Catch::Matchers::WithinAbs (0.0f, 1.0e-7f));
}

TEST_CASE ("ClapInstance supplies transport fields without retaining a previous block",
           "[clap][instance][transport]")
{
    using Catch::Matchers::WithinAbs;
    duskstudio::clap::ClapBundle bundle;
    std::string error;
    REQUIRE (bundle.load (DUSKSTUDIO_MULTI_BUS_CLAP_FIXTURE_PATH, error));
    duskstudio::clap::ClapInstance instance;
    REQUIRE (instance.create (bundle, bundle.plugins().front().id, error));
    REQUIRE (instance.activate (48000.0, 64, error));
    duskstudio::hosting::InsertAdapter adapter;
    adapter.prepare (instance.portLayout(), 64);
    std::array<float, 64> left {}, right {};
    const auto process = [&] (const dusk::TransportPosition* position)
    {
        adapter.process (instance, left.data(), right.data(), 64,
                         nullptr, nullptr, nullptr, position);
        const auto* plugin = instance.getPlugin();
        const auto* steadyTime = static_cast<const std::int64_t*> (
            plugin->get_extension (plugin, "studio.dusk.test.steady-time"));
        REQUIRE (steadyTime != nullptr);
        CHECK (*steadyTime == -1);
        return static_cast<const clap_event_transport_t*> (
            plugin->get_extension (plugin, "studio.dusk.test.transport"));
    };
    const auto fixedBeats = [] (double value)
    {
        return (clap_beattime) std::llround (value * (double) CLAP_BEATTIME_FACTOR);
    };
    const auto fixedSeconds = [] (double value)
    {
        return (clap_sectime) std::llround (value * (double) CLAP_SECTIME_FACTOR);
    };
    const auto valid = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_BEATS_TIMELINE
                     | CLAP_TRANSPORT_HAS_SECONDS_TIMELINE | CLAP_TRANSPORT_HAS_TIME_SIGNATURE;
    dusk::TransportPosition position;
    position.bpm = 93.5;
    position.ppqPosition = 17.25;
    position.timeInSamples = 123456;
    position.timeInSeconds = 123456.0 / 48000.0;
    position.timeSignatureNumerator = 7;
    position.timeSignatureDenominator = 8;
    position.isRecording = true;
    position.isLooping = true;
    position.loopStartSamples = 24000;
    position.loopEndSamples = 96000;
    position.ppqLoopStart = 1.25;
    position.ppqLoopEnd = 9.75;
    const auto* transport = process (&position);
    REQUIRE (transport != nullptr);
    CHECK (transport->header.size == sizeof (clap_event_transport_t));
    CHECK (transport->header.time == 0);
    CHECK (transport->header.space_id == CLAP_CORE_EVENT_SPACE_ID);
    CHECK (transport->header.type == CLAP_EVENT_TRANSPORT);
    CHECK (transport->header.flags == 0);
    CHECK (transport->flags == (uint32_t) (valid | CLAP_TRANSPORT_IS_PLAYING
                                          | CLAP_TRANSPORT_IS_RECORDING | CLAP_TRANSPORT_IS_LOOP_ACTIVE));
    CHECK (transport->song_pos_beats == fixedBeats (17.25));
    CHECK (transport->song_pos_seconds == fixedSeconds (123456.0 / 48000.0));
    CHECK_THAT (transport->tempo, WithinAbs (93.5, 1.0e-12));
    CHECK_THAT (transport->tempo_inc, WithinAbs (0.0, 1.0e-12));
    CHECK (transport->tsig_num == 7);
    CHECK (transport->tsig_denom == 8);
    CHECK (transport->bar_start == fixedBeats (14.0));
    CHECK (transport->bar_number == 4);
    CHECK (transport->loop_start_beats == fixedBeats (1.25));
    CHECK (transport->loop_end_beats == fixedBeats (9.75));
    CHECK (transport->loop_start_seconds == fixedSeconds (0.5));
    CHECK (transport->loop_end_seconds == fixedSeconds (2.0));

    position.isRecording = false;
    position.isLooping = false;
    position.bpm = 137.0;
    position.ppqPosition = -0.75;
    position.timeInSamples = -12000;
    position.timeInSeconds = -0.25;
    transport = process (&position);
    REQUIRE (transport != nullptr);
    CHECK (transport->flags == (uint32_t) valid);
    CHECK (transport->song_pos_beats == fixedBeats (-0.75));
    CHECK (transport->song_pos_seconds == fixedSeconds (-0.25));
    CHECK_THAT (transport->tempo, WithinAbs (137.0, 1.0e-12));
    CHECK (transport->bar_number == -1);
    CHECK (transport->bar_start == fixedBeats (-3.5));

    REQUIRE (instance.reactivate (96000.0, 64, error));
    position.isPlaying = true;
    position.isLooping = true;
    transport = process (&position);
    REQUIRE (transport != nullptr);
    CHECK ((transport->flags & CLAP_TRANSPORT_IS_PLAYING) != 0);
    CHECK ((transport->flags & CLAP_TRANSPORT_IS_LOOP_ACTIVE) != 0);
    CHECK (transport->loop_start_seconds == fixedSeconds (0.25));
    CHECK (transport->loop_end_seconds == fixedSeconds (1.0));
    CHECK (process (nullptr) == nullptr);

    position.isPlaying = false;
    position.isLooping = true;
    position.bpm = std::numeric_limits<double>::quiet_NaN();
    position.ppqPosition = std::numeric_limits<double>::infinity();
    position.timeInSeconds = 4294967296.0;
    position.timeSignatureNumerator = 65536;
    position.ppqLoopEnd = std::numeric_limits<double>::infinity();
    transport = process (&position);
    REQUIRE (transport != nullptr);
    CHECK (transport->flags == 0);
    CHECK (transport->song_pos_beats == 0);
    CHECK (transport->song_pos_seconds == 0);
    CHECK_THAT (transport->tempo, WithinAbs (0.0, 1.0e-12));
    CHECK (transport->tsig_num == 0);
    CHECK (transport->tsig_denom == 0);
}

TEST_CASE ("ClapInstance loads + processes a real CLAP plugin", "[clap][instance]")
{
    const char* path = std::getenv ("DUSKSTUDIO_TEST_CLAP");
    if (path == nullptr || *path == '\0')
    {
        // Conditional fixture: pass (don't fail the suite) when no .clap is
        // provided. Point DUSKSTUDIO_TEST_CLAP at a plugin to actually exercise it.
        SUCCEED ("DUSKSTUDIO_TEST_CLAP not set — skipping live CLAP-plugin process test");
        return;
    }

    duskstudio::clap::ClapBundle bundle;
    std::string err;
    REQUIRE (bundle.load (path, err));
    REQUIRE_FALSE (bundle.plugins().empty());

    duskstudio::clap::ClapInstance inst;
    // Single-plugin fixture (e.g. DuskVerb), so front() is deterministic.
    REQUIRE (inst.create (bundle, bundle.plugins().front().id, err));
    REQUIRE (inst.activate (48000.0, 512, err));
    // This test exercises the compatibility stereo adapter, so skip plugins whose
    // main output cannot provide its left/right pair.
    if (inst.outputChannels() < 2)
    {
        SUCCEED ("plugin has fewer than two main outputs — skipping stereo process test");
        return;
    }

    constexpr int kBlock = 512;
    constexpr int kBlocks = 40;   // drive enough blocks for any tail to settle
    std::vector<float> inL ((size_t) kBlock), inR ((size_t) kBlock),
                       outL ((size_t) kBlock), outR ((size_t) kBlock);

    SECTION ("silence in stays finite and silent")
    {
        std::fill (inL.begin(), inL.end(), 0.0f);
        std::fill (inR.begin(), inR.end(), 0.0f);

        float peak = 0.0f;
        for (int b = 0; b < kBlocks; ++b)
        {
            inst.processStereo (inL.data(), inR.data(), outL.data(), outR.data(), kBlock);
            for (int i = 0; i < kBlock; ++i)
            {
                REQUIRE (std::isfinite (outL[(size_t) i]));
                REQUIRE (std::isfinite (outR[(size_t) i]));
                peak = std::max ({ peak, std::abs (outL[(size_t) i]), std::abs (outR[(size_t) i]) });
            }
        }
        REQUIRE (peak < 1.0e-3f);   // silence in → silence out
    }

    SECTION ("signal in produces finite, non-silent output")
    {
        double phase = 0.0;
        constexpr double kPi = 3.14159265358979323846;   // M_PI is non-standard
        const double dw = 2.0 * kPi * 1000.0 / 48000.0;
        float peakOut = 0.0f;
        for (int b = 0; b < kBlocks; ++b)
        {
            for (int i = 0; i < kBlock; ++i)
            {
                const float s = 0.25f * (float) std::sin (phase);
                phase += dw;
                inL[(size_t) i] = inR[(size_t) i] = s;
            }
            inst.processStereo (inL.data(), inR.data(), outL.data(), outR.data(), kBlock);
            for (int i = 0; i < kBlock; ++i)
            {
                REQUIRE (std::isfinite (outL[(size_t) i]));
                REQUIRE (std::isfinite (outR[(size_t) i]));
                peakOut = std::max ({ peakOut, std::abs (outL[(size_t) i]), std::abs (outR[(size_t) i]) });
            }
        }
        REQUIRE (peakOut > 1.0e-3f);   // signal passes through the plugin
    }
}
