// Increment 3 foundation: the aux NativeClapSlot load -> process -> unload
// lifecycle. DUSKSTUDIO_TEST_CLAP selects a local plugin for the broad smoke
// test; the state regression below defaults to the bundled fixture.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/PdcMath.h"
#include "engine/clap/NativeClapSlot.h"
#include "dsp/ChannelStrip.h"
#include "dsp/AuxLaneStrip.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace
{
std::string readSourceFile (const char* relativePath)
{
    std::ifstream input (std::string (DUSKSTUDIO_SOURCE_DIR) + "/" + relativePath);
    return { std::istreambuf_iterator<char> (input), std::istreambuf_iterator<char>() };
}

bool nearestGateIncludes (const std::string& source, const char* marker,
                          const char* requiredMacro)
{
    const auto markerPos = source.find (marker);
    if (markerPos == std::string::npos) return false;
    const auto gatePos = source.rfind ("#if", markerPos);
    if (gatePos == std::string::npos) return false;
    return source.substr (gatePos, markerPos - gatePos).find (requiredMacro)
        != std::string::npos;
}

struct TestClapBundle
{
    std::filesystem::path path;
    bool isBundledFixture = false;
};

TestClapBundle testClapBundle()
{
    const char* overridePath = std::getenv ("DUSKSTUDIO_TEST_CLAP");
    if (overridePath != nullptr && *overridePath != '\0')
        return { std::filesystem::u8path (overridePath), false };
    return { std::filesystem::u8path (DUSKSTUDIO_MULTI_BUS_CLAP_FIXTURE_PATH), true };
}
} // namespace

TEST_CASE ("Channel native inserts receive the current transport through audio and silent blocks", "[clap][channel-strip][transport]")
{
    using Catch::Matchers::WithinAbs;
    using duskstudio::ChannelStrip;
    constexpr int blockSize = 64;
    const bool stereo = GENERATE (false, true);
    CAPTURE (stereo);
    duskstudio::ChannelStripParams params;
    ChannelStrip strip;
    strip.bind (params);
    strip.prepare (48000.0, blockSize);
    std::string error;
    REQUIRE (strip.loadNativeClap (juce::File { DUSKSTUDIO_MULTI_BUS_CLAP_FIXTURE_PATH }, error));
    auto* instance = strip.getNativeClapSlot().getInstance();
    REQUIRE (instance != nullptr);
    REQUIRE_FALSE (strip.insertIsNativeInstrument());
    const auto* plugin = instance->getPlugin();
    REQUIRE (plugin != nullptr);
    using Block = std::array<float, blockSize>;
    Block left {}, right {}, masterL {}, masterR {};
    std::array<Block, ChannelStrip::kNumBuses> busesL {}, busesR {};
    std::array<Block, ChannelStrip::kNumAuxSends> auxL {}, auxR {};
    std::array<float*, ChannelStrip::kNumBuses> busPtrsL {}, busPtrsR {};
    std::array<float*, ChannelStrip::kNumAuxSends> auxPtrsL {}, auxPtrsR {};
    for (std::size_t i = 0; i < busesL.size(); ++i)
    {
        busPtrsL[i] = busesL[i].data(); busPtrsR[i] = busesR[i].data();
    }
    for (std::size_t i = 0; i < auxL.size(); ++i)
    {
        auxPtrsL[i] = auxL[i].data(); auxPtrsR[i] = auxR[i].data();
    }
    juce::MidiBuffer midi;
    const auto run = [&]
    {
        masterL.fill (0.0f); masterR.fill (0.0f);
        strip.processAndAccumulate (left.data(), stereo ? right.data() : nullptr, midi, false,
                                    masterL.data(), masterR.data(), busPtrsL, busPtrsR,
                                    auxPtrsL, auxPtrsR, blockSize, true);
        REQUIRE (strip.getLastProcessedSamples() == blockSize);
        return static_cast<const clap_event_transport_t*> (
            plugin->get_extension (plugin, "studio.dusk.test.transport"));
    };
    const auto beats = [] (double value) { return (clap_beattime) std::llround (value * (double) CLAP_BEATTIME_FACTOR); };
    const auto seconds = [] (double value) { return (clap_sectime) std::llround (value * (double) CLAP_SECTIME_FACTOR); };
    const auto valid = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_BEATS_TIMELINE
                     | CLAP_TRANSPORT_HAS_SECONDS_TIMELINE | CLAP_TRANSPORT_HAS_TIME_SIGNATURE;
    dusk::TransportPosition position;
    position.bpm = 93.5;
    position.timeInSamples = 123456;
    position.timeInSeconds = 123456.0 / 48000.0;
    position.ppqPosition = 17.25;
    position.timeSignatureNumerator = 7;
    position.timeSignatureDenominator = 8;
    position.isPlaying = position.isRecording = position.isLooping = true;
    position.loopStartSamples = 24000;
    position.loopEndSamples = 96000;
    position.ppqLoopStart = 1.25;
    position.ppqLoopEnd = 9.75;
    strip.setTransport (&position);
    left.fill (0.25f); right.fill (-0.125f);
    const auto* received = run();
    REQUIRE (received != nullptr);
    CHECK_THAT (received->tempo, WithinAbs (position.bpm, 1e-12));
    CHECK (received->song_pos_beats == beats (position.ppqPosition));
    CHECK (received->song_pos_seconds == seconds (position.timeInSeconds));
    CHECK (received->flags == (uint32_t) (valid | CLAP_TRANSPORT_IS_PLAYING
        | CLAP_TRANSPORT_IS_RECORDING | CLAP_TRANSPORT_IS_LOOP_ACTIVE));
    CHECK (received->tsig_num == 7);
    CHECK (received->tsig_denom == 8);
    CHECK (received->loop_start_beats == beats (1.25));
    CHECK (received->loop_end_beats == beats (9.75));
    CHECK (received->loop_start_seconds == seconds (0.5));
    CHECK (received->loop_end_seconds == seconds (2.0));

    left.fill (0.0f); right.fill (0.0f);
    params.insertBypassed.store (true);
    position.isPlaying = position.isRecording = position.isLooping = false;
    position.bpm = 137.0;
    position.ppqPosition = -0.75;
    position.timeInSamples = -12000;
    position.timeInSeconds = -0.25;
    position.timeSignatureNumerator = 6;
    received = run();
    REQUIRE (received != nullptr);
    CHECK_THAT (received->tempo, WithinAbs (137.0, 1e-12));
    CHECK (received->song_pos_beats == beats (-0.75));
    CHECK (received->song_pos_seconds == seconds (-0.25));
    CHECK (received->flags == (uint32_t) valid);
    CHECK (received->tsig_num == 6);
    CHECK (received->bar_number == -1);
    CHECK (received->bar_start == beats (-3.0));
    strip.setTransport (nullptr);
    CHECK (run() == nullptr);
}

TEST_CASE ("Auxiliary native inserts receive current transport through silent tails", "[clap][aux-lane][transport]")
{
    using Catch::Matchers::WithinAbs;
    using duskstudio::AuxLaneStrip;
    constexpr int blockSize = 64;
    constexpr int slot = 0;
    const bool closedReturn = GENERATE (false, true);
    CAPTURE (closedReturn);
    duskstudio::AuxLaneParams params;
    params.returnLevelDb.store (closedReturn ? -100.0f : 0.0f);
    params.liveReturnLevelDb.store (closedReturn ? -100.0f : 0.0f);
    AuxLaneStrip lane;
    lane.bind (params);
    lane.insertMode[(std::size_t) slot].store (AuxLaneStrip::kInsertPlugin);
    lane.prepare (48000.0, blockSize);
    std::string error;
    REQUIRE (lane.loadNativeClap (slot, juce::File { DUSKSTUDIO_MULTI_BUS_CLAP_FIXTURE_PATH }, error));
    auto* instance = lane.getNativeClapSlot (slot).getInstance();
    REQUIRE (instance != nullptr);
    REQUIRE_FALSE (lane.getNativeClapSlot (slot).isLoadedInstrument());
    const auto* plugin = instance->getPlugin();
    std::array<float, blockSize> left {}, right {};
    const auto run = [&]
    {
        lane.processStereoBlock (left.data(), right.data(), blockSize);
        return static_cast<const clap_event_transport_t*> (
            plugin->get_extension (plugin, "studio.dusk.test.transport"));
    };
    const auto beats = [] (double value) { return (clap_beattime) std::llround (value * (double) CLAP_BEATTIME_FACTOR); };
    const auto valid = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_BEATS_TIMELINE
                     | CLAP_TRANSPORT_HAS_SECONDS_TIMELINE | CLAP_TRANSPORT_HAS_TIME_SIGNATURE;
    dusk::TransportPosition position;
    position.bpm = 93.5;
    position.ppqPosition = 17.25;
    position.timeInSamples = 123456;
    position.timeInSeconds = 123456.0 / 48000.0;
    position.timeSignatureNumerator = 7;
    position.timeSignatureDenominator = 8;
    position.isPlaying = position.isRecording = position.isLooping = true;
    position.loopStartSamples = 24000;
    position.loopEndSamples = 96000;
    position.ppqLoopStart = 1.25;
    position.ppqLoopEnd = 9.75;
    lane.setTransport (&position);
    left.fill (0.25f); right.fill (-0.125f);
    const auto* received = run();
    REQUIRE (received != nullptr);
    CHECK_THAT (received->tempo, WithinAbs (93.5, 1e-12));
    CHECK (received->song_pos_beats == beats (17.25));
    CHECK (received->tsig_num == 7);
    CHECK (received->tsig_denom == 8);
    CHECK (received->flags == (std::uint32_t) (valid | CLAP_TRANSPORT_IS_PLAYING
        | CLAP_TRANSPORT_IS_RECORDING | CLAP_TRANSPORT_IS_LOOP_ACTIVE));
    CHECK (received->loop_start_beats == beats (1.25));
    CHECK (received->loop_end_beats == beats (9.75));
    left.fill (0.0f); right.fill (0.0f);
    position.bpm = 137.0;
    position.ppqPosition = -0.75;
    position.timeInSamples = -12000;
    position.timeInSeconds = -0.25;
    position.timeSignatureNumerator = 6;
    position.isPlaying = position.isRecording = position.isLooping = false;
    received = run();
    REQUIRE (received != nullptr);
    CHECK_THAT (received->tempo, WithinAbs (137.0, 1e-12));
    CHECK (received->song_pos_beats == beats (-0.75));
    CHECK (received->tsig_num == 6);
    CHECK (received->flags == (std::uint32_t) valid);
    lane.setTransport (nullptr);
    CHECK (run() == nullptr);
}

TEST_CASE ("NativeClapSlot loads, processes, and unloads cleanly", "[clap][slot]")
{
    const char* path = std::getenv ("DUSKSTUDIO_TEST_CLAP");
    if (path == nullptr || *path == '\0')
    {
        SUCCEED ("DUSKSTUDIO_TEST_CLAP not set - skipping live CLAP-slot test");
        return;
    }

    constexpr int kBlock = 512;
    duskstudio::clap::NativeClapSlot slot;
    std::string err;

    REQUIRE (slot.load (std::filesystem::u8path (path), 48000.0, kBlock, err));
    REQUIRE (slot.isLoaded());
    REQUIRE (slot.getInstance() != nullptr);

    std::vector<float> inL ((size_t) kBlock), inR ((size_t) kBlock),
                       outL ((size_t) kBlock), outR ((size_t) kBlock);


    SECTION ("explicit plugin id: round-trips, bogus id refuses to load")
    {
        const std::string pickedId = slot.getPluginId();
        REQUIRE (! pickedId.empty());

        slot.unload();
        REQUIRE (slot.load (std::filesystem::u8path (path), 48000.0, kBlock, err, pickedId));
        REQUIRE (slot.getPluginId() == pickedId);

        slot.unload();
        REQUIRE_FALSE (slot.load (std::filesystem::u8path (path), 48000.0, kBlock, err,
                                  "urn:duskstudio:no-such-plugin"));
        REQUIRE_FALSE (slot.isLoaded());
    }

    SECTION ("MIDI-binding writes reach the parameter surface")
    {
        // queueParamBinding is the audio-thread half of a binding apply;
        // drainQueuedParamBindings is the engine timer's message-thread half.
        // The value lands plugin-side on the next process block (UI→RT ring).
        int targetIdx = -1;
        for (int i = 0; i < slot.paramCount(); ++i)
        {
            const auto* p = slot.paramInfo (i);
            REQUIRE (p != nullptr);
            if ((p->flags & CLAP_PARAM_IS_STEPPED) == 0 && p->maxValue > p->minValue)
            { targetIdx = i; break; }
        }
        REQUIRE (targetIdx >= 0);
        const auto* target = slot.paramInfo (targetIdx);

        slot.queueParamBinding ((uint32_t) targetIdx, 1.0f);   // frac 1 → param max
        slot.drainQueuedParamBindings();
        std::fill (inL.begin(), inL.end(), 0.0f);
        std::fill (inR.begin(), inR.end(), 0.0f);
        slot.processStereo (inL.data(), inR.data(), outL.data(), outR.data(), kBlock);

        double v = 0.0;
        REQUIRE (slot.getParamValue (target->id, v));
        const double range = target->maxValue - target->minValue;
        REQUIRE_THAT (v, Catch::Matchers::WithinAbs (target->maxValue, 1.0e-4 * range + 1.0e-9));
    }

    SECTION ("loaded: processing keeps every output sample finite")
    {
        double phase = 0.0;
        constexpr double kPi = 3.14159265358979323846;   // M_PI is non-standard
        const double dw = 2.0 * kPi * 1000.0 / 48000.0;
        for (int b = 0; b < 40; ++b)
        {
            for (int i = 0; i < kBlock; ++i) { const float s = 0.25f * (float) std::sin (phase); phase += dw; inL[(size_t) i] = inR[(size_t) i] = s; }
            slot.processStereo (inL.data(), inR.data(), outL.data(), outR.data(), kBlock);
            for (int i = 0; i < kBlock; ++i)
            {
                REQUIRE (std::isfinite (outL[(size_t) i]));
                REQUIRE (std::isfinite (outR[(size_t) i]));
            }
        }
    }

    SECTION ("unloaded: process clears the outputs and is a safe no-op")
    {
        slot.unload();
        REQUIRE_FALSE (slot.isLoaded());
        REQUIRE (slot.getInstance() == nullptr);

        std::fill (outL.begin(), outL.end(), 1.0f);   // dirty the buffers
        std::fill (outR.begin(), outR.end(), 1.0f);
        std::fill (inL.begin(),  inL.end(),  0.5f);
        std::fill (inR.begin(),  inR.end(),  0.5f);
        slot.processStereo (inL.data(), inR.data(), outL.data(), outR.data(), kBlock);
        for (int i = 0; i < kBlock; ++i)
        {
            REQUIRE (outL[(size_t) i] == 0.0f);   // cleared, not leaked
            REQUIRE (outR[(size_t) i] == 0.0f);
        }
    }
}

TEST_CASE ("NativeClapSlot reactivate keeps the same instance and signal path",
           "[clap][slot][reactivate][regression][issue-397]")
{
    // Regression: changing the global oversampling factor (or device rate) re-prepares
    // the strip. The old code did a full reload, destroying the instance the editor's
    // GUI was attached to — plugin->destroy with a live GUI aborts u-he plugins
    // ("host forgot to destroy the gui" → terminate). reactivate must re-activate the
    // SAME instance so the GUI and parameter state survive.
    duskstudio::clap::NativeClapSlot slot;
    std::string err;
    REQUIRE (slot.load (std::filesystem::u8path (DUSKSTUDIO_MULTI_BUS_CLAP_FIXTURE_PATH),
                        48000.0, 512, err));
    auto* before = slot.getInstance();
    REQUIRE (before != nullptr);

    // Re-activate at a different rate + block size (4× → 1× changes both for an
    // oversampled chain). The instance pointer must be identical afterwards.
    REQUIRE (slot.reactivate (96000.0, 256, err));
    REQUIRE (slot.isLoaded());
    REQUIRE (slot.getInstance() == before);   // not torn down + rebuilt

    constexpr int kBlock = 256;
    std::vector<float> inL ((size_t) kBlock), inR ((size_t) kBlock),
                       outL ((size_t) kBlock), outR ((size_t) kBlock);
    double phase = 0.0;
    constexpr double kPi = 3.14159265358979323846;
    const double dw = 2.0 * kPi * 1000.0 / 96000.0;
    float peakOut = 0.0f;
    float maxError = 0.0f;
    for (int b = 0; b < 40; ++b)
    {
        for (int i = 0; i < kBlock; ++i) { const float s = 0.25f * (float) std::sin (phase); phase += dw; inL[(size_t) i] = inR[(size_t) i] = s; }
        slot.processStereo (inL.data(), inR.data(), outL.data(), outR.data(), kBlock);
        for (int i = 0; i < kBlock; ++i)
        {
            REQUIRE (std::isfinite (outL[(size_t) i]));
            REQUIRE (std::isfinite (outR[(size_t) i]));
            peakOut = std::max ({ peakOut, std::abs (outL[(size_t) i]),
                                  std::abs (outR[(size_t) i]) });
            maxError = std::max ({ maxError,
                                   std::abs (outL[(size_t) i] - inL[(size_t) i]),
                                   std::abs (outR[(size_t) i] - inR[(size_t) i]) });
        }
    }
    REQUIRE (peakOut > 0.2f);
    REQUIRE (maxError < 1.0e-7f);
}

TEST_CASE ("NativeClapSlot state round-trips into a fresh slot",
           "[clap][slot][state][regression][issue-355]")
{
    const auto testBundle = testClapBundle();
    constexpr int kBlock = 512;

    duskstudio::clap::NativeClapSlot a;
    std::string err;
    REQUIRE (a.load (testBundle.path, 48000.0, kBlock, err));

    int targetIdx = -1;
    double changedValue = 0.0;
    for (int i = 0; i < a.paramCount(); ++i)
    {
        const auto* p = a.paramInfo (i);
        double current = 0.0;
        if (p != nullptr && (p->flags & CLAP_PARAM_IS_READONLY) == 0
            && p->maxValue > p->minValue && a.getParamValue (p->id, current))
        {
            targetIdx = i;
            changedValue = std::abs (current - p->minValue)
                               > 0.5 * (p->maxValue - p->minValue)
                             ? p->minValue : p->maxValue;
            a.setParamValue (p->id, changedValue);
            break;
        }
    }
    if (targetIdx < 0 && ! testBundle.isBundledFixture)
    {
        SUCCEED ("plugin exposes no writable ranged parameter - skipping state round-trip");
        return;
    }

    std::vector<float> inL ((size_t) kBlock), inR ((size_t) kBlock),
                       outL ((size_t) kBlock), outR ((size_t) kBlock);
    std::fill (inL.begin(), inL.end(), 0.1f);
    std::fill (inR.begin(), inR.end(), 0.1f);
    a.processStereo (inL.data(), inR.data(), outL.data(), outR.data(), kBlock);

    const auto* target = targetIdx >= 0 ? a.paramInfo (targetIdx) : nullptr;
    double savedValue = 0.0;
    double tolerance = 0.0;
    if (target != nullptr)
    {
        REQUIRE (a.getParamValue (target->id, savedValue));
        tolerance = 1.0e-5 * (target->maxValue - target->minValue) + 1.0e-9;
        REQUIRE_THAT (savedValue, Catch::Matchers::WithinAbs (changedValue, tolerance));
    }

    std::vector<uint8_t> blob;
    if (! a.saveState (blob))
    {
        SUCCEED ("plugin has no CLAP state extension — nothing to round-trip");
        return;
    }
    REQUIRE_FALSE (blob.empty());

    // Restore into a fresh slot and confirm it loads + processes finite audio.
    duskstudio::clap::NativeClapSlot b;
    REQUIRE (b.load (testBundle.path, 48000.0, kBlock, err));
    REQUIRE (b.loadState (blob));

    if (target != nullptr)
    {
        double restoredValue = 0.0;
        REQUIRE (b.getParamValue (target->id, restoredValue));
        REQUIRE_THAT (restoredValue, Catch::Matchers::WithinAbs (savedValue, tolerance));
    }

    if (testBundle.isBundledFixture)
    {
        std::vector<uint8_t> restoredState;
        REQUIRE (b.saveState (restoredState));
        REQUIRE (restoredState == blob);
    }

    for (int blk = 0; blk < 8; ++blk)
        b.processStereo (inL.data(), inR.data(), outL.data(), outR.data(), kBlock);
    for (int i = 0; i < kBlock; ++i)
    {
        REQUIRE (std::isfinite (outL[(size_t) i]));
        REQUIRE (std::isfinite (outR[(size_t) i]));
    }
}

TEST_CASE ("Linux track clone keeps native CLAP identity and state",
           "[clap][slot][state][regression][issue-462]")
{
    const auto cloneSource = readSourceFile ("src/session/RegionEditActions.cpp");
    REQUIRE_FALSE (cloneSource.empty());
    REQUIRE (nearestGateIncludes (cloneSource, "bool liveNativeCaptured",
                                  "DUSKSTUDIO_HAS_NATIVE_CLAP"));
    REQUIRE (nearestGateIncludes (cloneSource,
                                  "Replay exactly one native owner",
                                  "DUSKSTUDIO_HAS_NATIVE_CLAP"));
    constexpr int kBlock = 64;
    const auto bundle = std::filesystem::u8path (
        DUSKSTUDIO_MULTI_BUS_CLAP_FIXTURE_PATH);
    duskstudio::clap::NativeClapSlot source;
    std::string error;
    REQUIRE (source.load (bundle, 48000.0, kBlock, error));

    std::vector<float> left ((size_t) kBlock, 0.0f);
    std::vector<float> right ((size_t) kBlock, 0.0f);
    for (int block = 0; block < 3; ++block)
        source.processStereo (left.data(), right.data(), left.data(), right.data(), kBlock);

    const auto capturedPath = source.getPath();
    const auto capturedId = source.getPluginId();
    std::vector<uint8_t> capturedState;
    REQUIRE (source.saveState (capturedState));
    REQUIRE_FALSE (capturedState.empty());

    duskstudio::clap::NativeClapSlot destination;
    REQUIRE (destination.load (bundle, 48000.0, kBlock, error));
    destination.processStereo (left.data(), right.data(), left.data(), right.data(), kBlock);
    std::vector<uint8_t> staleState;
    REQUIRE (destination.saveState (staleState));
    REQUIRE (staleState != capturedState);

    REQUIRE (destination.load (std::filesystem::u8path (capturedPath), 48000.0,
                               kBlock, error, capturedId));
    REQUIRE (destination.loadState (capturedState));
    REQUIRE (destination.getPath() == capturedPath);
    REQUIRE (destination.getPluginId() == capturedId);

    std::vector<uint8_t> replayedState;
    REQUIRE (destination.saveState (replayedState));
    REQUIRE (replayedState == capturedState);
}

// CLAP lets a plug-in change its latency only inside activate(), so an active
// plug-in that wants a longer look-ahead asks the host for a restart, here from
// process() on the audio thread. The engine's drain consumes the request and
// reactivates the slot, which is when the new latency reaches delay
// compensation.
TEST_CASE ("NativeClapSlot follows a latency the plug-in changes while it runs",
           "[clap][slot][latency][pdc][issue-764]")
{
    static constexpr int kBlock = 64;
    static constexpr double kLookAhead = 256.0;
    duskstudio::clap::NativeClapSlot slot;
    std::string err;
    REQUIRE (slot.load (std::filesystem::u8path (DUSKSTUDIO_LATENCY_CLAP_FIXTURE_PATH),
                        48000.0, kBlock, err));
    REQUIRE (slot.getLatencySamples() == 0);
    REQUIRE_FALSE (slot.consumeRestartRequest());

    const auto* lookAhead = slot.paramInfo (0);
    REQUIRE (lookAhead != nullptr);
    slot.setParamValue (lookAhead->id, kLookAhead);

    std::vector<float> left ((size_t) kBlock, 0.0f), right ((size_t) kBlock, 0.0f);
    slot.processStereo (left.data(), right.data(), left.data(), right.data(), kBlock);

    // The plug-in keeps its old latency until the restart.
    CHECK (slot.getLatencySamples() == 0);
    REQUIRE (slot.consumeRestartRequest());
    REQUIRE_FALSE (slot.consumeRestartRequest());

    REQUIRE (slot.reactivate (48000.0, kBlock, err));
    CHECK (slot.getLatencySamples() == (int) kLookAhead);

    const int latency[2] { slot.getLatencySamples(), 0 };
    int compensation[2] {};
    duskstudio::pdc::computeCompensations (latency, compensation, 2);
    CHECK (compensation[1] == (int) kLookAhead);

    // Bypassed, the plug-in is out of the path and its delay with it.
    slot.setBypassed (true);
    CHECK (slot.getLatencySamples() == 0);
}
