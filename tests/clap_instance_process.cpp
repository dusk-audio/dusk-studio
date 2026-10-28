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

namespace
{
struct ParamEventControl
{
    bool (CLAP_ABI *queue) (const clap_plugin_t*, const clap_event_header_t*);
};

struct RevisionFixture
{
    explicit RevisionFixture (const char* path = DUSKSTUDIO_PARAM_TOUCH_CLAP_FIXTURE_PATH)
    {
        REQUIRE (bundle.load (path, error));
        REQUIRE (bundle.plugins().size() == 1);
        REQUIRE (instance.create (bundle, bundle.plugins().front().id, error));
        REQUIRE (instance.activate (48000.0, 64, error));
    }
    void process()
    { instance.processStereo (left.data(), right.data(), left.data(), right.data(), 64); }
    void emit (const clap_event_header_t& event)
    {
        const auto* plugin = instance.getPlugin();
        const auto* control = static_cast<const ParamEventControl*> (
            plugin->get_extension (plugin, "studio.dusk.test.param-touch-events"));
        REQUIRE (control != nullptr);
        REQUIRE (control->queue (plugin, &event));
        process();
    }
    double value (clap_id id)
    {
        double result = 0.0;
        REQUIRE (instance.getParamValue (id, result));
        return result;
    }
    duskstudio::clap::ClapBundle bundle;
    duskstudio::clap::ClapInstance instance;
    std::string error;
    std::array<float, 64> left {}, right {};
};

clap_event_param_value_t globalParamValue (clap_id id, double value)
{
    clap_event_param_value_t event {};
    event.header = { sizeof (event), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0 };
    event.param_id = id;
    event.note_id = event.port_index = event.channel = event.key = -1;
    event.value = value;
    return event;
}
}

TEST_CASE ("ClapInstance revisions distinguish persistent output values from transient events", "[clap][instance][state]")
{
    RevisionFixture fixture;
    for (const std::uint32_t flags : { 0u, (std::uint32_t) CLAP_EVENT_DONT_RECORD })
    {
        const auto before = fixture.instance.stateRevision();
        auto event = globalParamValue (40, flags == 0 ? 0.25 : 0.75);
        event.header.flags = flags;
        fixture.emit (event.header);
        CHECK (fixture.instance.stateRevision() > before);
        CHECK_THAT (fixture.value (40), Catch::Matchers::WithinAbs (event.value, 1e-12));
    }
    for (int scope = 0; scope < 6; ++scope)
    {
        CAPTURE (scope);
        const auto before = fixture.instance.stateRevision();
        auto event = globalParamValue (scope == 4 ? 42 : scope == 5 ? 999 : 40, 0.5);
        if (scope == 0) event.note_id = 7;
        if (scope == 1) event.port_index = 0;
        if (scope == 2) event.channel = 0;
        if (scope == 3) event.key = 60;
        fixture.emit (event.header);
        CHECK (fixture.instance.stateRevision() == before);
        if (scope == 4)
            CHECK_THAT (fixture.value (42), Catch::Matchers::WithinAbs (0.5, 1e-12));
    }
    clap_event_param_mod_t modulation {};
    modulation.header = { sizeof (modulation), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_MOD, 0 };
    modulation.param_id = 40;
    modulation.note_id = modulation.port_index = modulation.channel = modulation.key = -1;
    modulation.amount = 0.5;
    const auto beforeTransient = fixture.instance.stateRevision();
    fixture.emit (modulation.header);
    for (const auto type : { CLAP_EVENT_PARAM_GESTURE_BEGIN, CLAP_EVENT_PARAM_GESTURE_END })
    {
        clap_event_param_gesture_t gesture {};
        gesture.header = { sizeof (gesture), 0, CLAP_CORE_EVENT_SPACE_ID, (std::uint16_t) type, 0 };
        gesture.param_id = 40;
        fixture.emit (gesture.header);
    }
    CHECK (fixture.instance.stateRevision() == beforeTransient);
    CHECK (fixture.instance.lastTouchedParamIndex() == 0);
}

TEST_CASE ("ClapInstance revisions mark accepted host changes and their later application", "[clap][instance][state]")
{
    RevisionFixture fixture (DUSKSTUDIO_LATENCY_CLAP_FIXTURE_PATH);
    const auto initial = fixture.instance.stateRevision();
    fixture.instance.setParamValue (999, 1.0);
    fixture.instance.setParamValue (11, 1.0);
    CHECK (fixture.instance.stateRevision() == initial);
    fixture.instance.setParamValue (7, 8.0);
    const auto queued = fixture.instance.stateRevision();
    CHECK (queued > initial);
    CHECK_THAT (fixture.value (7), Catch::Matchers::WithinAbs (0.0, 1e-12));
    fixture.process();
    CHECK (fixture.instance.stateRevision() > queued);
    CHECK_THAT (fixture.value (7), Catch::Matchers::WithinAbs (8.0, 1e-12));

    for (int index = 0; index < 512; ++index)
        fixture.instance.setParamValue (7, 16.0);
    const auto full = fixture.instance.stateRevision();
    fixture.instance.setParamValue (7, 32.0);
    CHECK (fixture.instance.stateRevision() == full);
    fixture.process();
    CHECK (fixture.instance.stateRevision() > full);
    CHECK_THAT (fixture.value (7), Catch::Matchers::WithinAbs (16.0, 1e-12));
}

TEST_CASE ("ClapInstance revisions survive state capture and reactivation", "[clap][instance][state]")
{
    RevisionFixture fixture;
    fixture.instance.setParamValue (40, 0.25);
    const auto pending = fixture.instance.stateRevision();
    std::vector<std::uint8_t> beforeApplication;
    REQUIRE (fixture.instance.saveState (beforeApplication));
    CHECK (fixture.instance.stateRevision() == pending);
    fixture.process();
    CHECK (fixture.instance.stateRevision() > pending);
    const auto beforeLoad = fixture.instance.stateRevision();
    REQUIRE (fixture.instance.loadState (beforeApplication));
    CHECK (fixture.instance.stateRevision() > beforeLoad);
    CHECK_THAT (fixture.value (40), Catch::Matchers::WithinAbs (0.0, 1e-12));
    auto corrupt = beforeApplication;
    corrupt.front() = 0;
    const auto restored = fixture.instance.stateRevision();
    CHECK_FALSE (fixture.instance.loadState (corrupt));
    CHECK_FALSE (fixture.instance.loadState ({}));
    CHECK (fixture.instance.stateRevision() == restored);
    std::vector<std::uint8_t> saved;
    REQUIRE (fixture.instance.saveState (saved));
    CHECK (fixture.instance.stateRevision() == restored);
    REQUIRE (fixture.instance.reactivate (96000.0, 32, fixture.error));
    CHECK (fixture.instance.stateRevision() == restored);
    fixture.instance.getPlugin()->reset (fixture.instance.getPlugin());
    CHECK (fixture.instance.stateRevision() == restored);
}

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
