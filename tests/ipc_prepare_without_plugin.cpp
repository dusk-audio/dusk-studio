#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/ipc/PluginIpc.h"
#include "engine/ipc/RemotePluginConnection.h"
#include "engine/DuskStudioPlayHead.h"

#include <juce_audio_processors/juce_audio_processors.h>

#if defined(DUSKSTUDIO_RUNTIME_RELAYOUT_VST3_FIXTURE_PATH)
 #if defined(__GNUC__)
  #pragma GCC diagnostic push
  #pragma GCC diagnostic ignored "-Wshadow"
 #endif
 #include <pluginterfaces/vst/ivstprocesscontext.h>
 #if defined(__GNUC__)
  #pragma GCC diagnostic pop
 #endif
#endif

#include <array>
#include <chrono>
#include <cmath>
#include <string>

// A Release leaves the child alive with no instance. Answering the next
// PrepareToPlay with success let the parent republish the connection and route
// audio into a child with nothing to process, which reads as a working slot
// that is simply silent.
TEST_CASE ("PrepareToPlay is refused when the child holds no plug-in")
{
    duskstudio::ipc::RemotePluginConnection connection;

    std::string error;
    REQUIRE (connection.connect (DUSKSTUDIO_PLUGIN_HOST_PATH, "--ipc-host", error));

    CHECK_FALSE (connection.prepareToPlay (48000.0, 64, error));
    CHECK_FALSE (error.empty());
}

TEST_CASE ("atomic transport playheads retain whole snapshots and clear absent positions", "[transport]")
{
    using Catch::Matchers::WithinAbs;
    dusk::AtomicTransportPosition snapshots;
    duskstudio::DuskStudioPlayHead playhead (snapshots);
    CHECK_FALSE (snapshots.read());
    CHECK_FALSE (playhead.getPosition());
    for (int generation = 0; generation < 8; ++generation)
    {
        dusk::TransportPosition source;
        source.bpm = 90.0 + generation;
        source.ppqPosition = 7.25 + generation;
        source.timeInSamples = 123456 + generation;
        source.timeInSeconds = (double) source.timeInSamples / 48000.0;
        source.timeSignatureNumerator = 6;
        source.timeSignatureDenominator = 8;
        source.loopStartSamples = 24000 + generation;
        source.loopEndSamples = 96000 + generation;
        source.ppqLoopStart = 1.25 + generation;
        source.ppqLoopEnd = 9.75 + generation;
        source.isRecording = (generation & 2) != 0;
        source.isPlaying = (generation & 1) != 0 || source.isRecording;
        source.isLooping = (generation & 4) != 0;
        snapshots.publish (&source);
        const auto captured = snapshots.read();
        REQUIRE (captured);
        CHECK_THAT (captured->bpm, WithinAbs (source.bpm, 1.0e-12));
        CHECK_THAT (captured->ppqPosition, WithinAbs (source.ppqPosition, 1.0e-12));
        CHECK_THAT (captured->timeInSeconds, WithinAbs (source.timeInSeconds, 1.0e-12));
        CHECK (captured->timeInSamples == source.timeInSamples);
        CHECK (captured->timeSignatureNumerator == 6);
        CHECK (captured->timeSignatureDenominator == 8);
        CHECK (captured->loopStartSamples == source.loopStartSamples);
        CHECK (captured->loopEndSamples == source.loopEndSamples);
        CHECK_THAT (captured->ppqLoopStart, WithinAbs (source.ppqLoopStart, 1.0e-12));
        CHECK_THAT (captured->ppqLoopEnd, WithinAbs (source.ppqLoopEnd, 1.0e-12));
        CHECK (captured->isPlaying == source.isPlaying);
        CHECK (captured->isRecording == source.isRecording);
        CHECK (captured->isLooping == source.isLooping);
        const auto info = playhead.getPosition();
        REQUIRE (info);
        REQUIRE (info->getPpqPosition());
        REQUIRE (info->getTimeInSeconds());
        REQUIRE (info->getLoopPoints());
        CHECK_THAT (*info->getPpqPosition(), WithinAbs (source.ppqPosition, 1.0e-12));
        CHECK_THAT (*info->getTimeInSeconds(), WithinAbs (source.timeInSeconds, 1.0e-12));
        CHECK_THAT (info->getLoopPoints()->ppqStart, WithinAbs (source.ppqLoopStart, 1.0e-12));
        CHECK_THAT (info->getLoopPoints()->ppqEnd, WithinAbs (source.ppqLoopEnd, 1.0e-12));
        CHECK (info->getIsPlaying() == source.isPlaying);
        CHECK (info->getIsRecording() == source.isRecording);
        CHECK (info->getIsLooping() == source.isLooping);
    }
    snapshots.publish (nullptr);
    CHECK_FALSE (snapshots.read());
    CHECK_FALSE (playhead.getPosition());
}

#if defined(DUSKSTUDIO_RUNTIME_RELAYOUT_VST3_FIXTURE_PATH)
TEST_CASE ("an out-of-process VST3 receives the current transport snapshot", "[ipc][transport]")
{
    using Catch::Matchers::WithinAbs;
    using Context = Steinberg::Vst::ProcessContext;
    juce::PluginDescription description;
    description.name = "Dusk Runtime Relayout Fixture";
    description.pluginFormatName = "VST3";
    description.fileOrIdentifier = DUSKSTUDIO_RUNTIME_RELAYOUT_VST3_FIXTURE_PATH;
    description.uniqueId = -216559363;
    description.numInputChannels = description.numOutputChannels = 2;
    const auto xml = description.createXml()->toString().toStdString();
    duskstudio::ipc::RemotePluginConnection connection;
    std::string error;
    REQUIRE (connection.connect (DUSKSTUDIO_PLUGIN_HOST_PATH, "--ipc-host", error));
    int numIn = 0, numOut = 0, latency = 0;
    bool instrument = false;
    REQUIRE (connection.loadPlugin (xml, 48000.0, 64, numIn, numOut, latency, instrument, error));
    REQUIRE (numIn == 2);
    REQUIRE (numOut == 2);
    REQUIRE (connection.prepareToPlay (48000.0, 64, error));
    std::array<float, 64> left {}, right {};
    left.fill (0.25f);
    right.fill (-0.25f);
    const float* inputs[] { left.data(), right.data() };
    dusk::MidiBuffer midi;
    const auto process = [&] (const dusk::TransportPosition* position)
    {
        REQUIRE (connection.processBlockSync (inputs, 2, 2, 64, midi, 500000000, position));
        CHECK_THAT (connection.readOutChannel (0)[63], WithinAbs (0.0f, 1.0e-6f));
    };
    const auto report = [&] (int sample) { return connection.readOutChannel (0)[sample]; };
    const auto flags = [&] { return (std::uint32_t) std::lround (report (8)); };
    const auto enableReport = [&] (double sampleRate)
    {
        REQUIRE (connection.setRemoteParam (6, 1.0f));
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (2);
        bool active = false;
        bool roundTrip = true;
        do
        {
            roundTrip = connection.processBlockSync (inputs, 2, 2, 64, midi, 500000000);
            if (! roundTrip) break;
            active = std::abs (report (0) - 1.0f) < 1.0e-6f
                  && std::abs ((double) report (1) - sampleRate) < 1.0e-6;
        }
        while (! active && std::chrono::steady_clock::now() < deadline);
        REQUIRE (roundTrip);
        REQUIRE (active);
    };
    enableReport (48000.0);
    const auto valid = Context::kProjectTimeMusicValid | Context::kBarPositionValid
                     | Context::kTempoValid | Context::kTimeSigValid | Context::kCycleValid;
    dusk::TransportPosition position;
    position.bpm = 93.5;
    position.timeInSamples = 123456;
    position.timeInSeconds = 123456.0 / 48000.0;
    position.ppqPosition = 17.25;
    position.timeSignatureNumerator = 6;
    position.timeSignatureDenominator = 8;
    position.isPlaying = position.isRecording = position.isLooping = true;
    position.loopStartSamples = 24000;
    position.loopEndSamples = 96000;
    position.ppqLoopStart = 1.25;
    position.ppqLoopEnd = 9.75;
    process (&position);
    CHECK_THAT (report (0), WithinAbs (1.0f, 1.0e-6f));
    CHECK_THAT (report (1), WithinAbs (48000.0f, 1.0e-6f));
    CHECK_THAT (report (2), WithinAbs (123456.0f, 1.0e-6f));
    CHECK_THAT (report (3), WithinAbs (17.25f, 1.0e-6f));
    CHECK_THAT (report (4), WithinAbs (15.0f, 1.0e-6f));
    CHECK_THAT (report (5), WithinAbs (93.5f, 1.0e-6f));
    CHECK_THAT (report (6), WithinAbs (6.0f, 1.0e-6f));
    CHECK_THAT (report (7), WithinAbs (8.0f, 1.0e-6f));
    CHECK_THAT (report (9), WithinAbs (1.25f, 1.0e-6f));
    CHECK_THAT (report (10), WithinAbs (9.75f, 1.0e-6f));
    CHECK (flags() == (std::uint32_t) (valid | Context::kPlaying | Context::kRecording | Context::kCycleActive));

    position.isPlaying = position.isRecording = position.isLooping = false;
    position.timeInSamples = -6000;
    position.timeInSeconds = -0.125;
    position.ppqPosition = -0.75;
    position.bpm = 137.0;
    process (&position);
    CHECK_THAT (report (2), WithinAbs (-6000.0f, 1.0e-6f));
    CHECK_THAT (report (3), WithinAbs (-0.75f, 1.0e-6f));
    CHECK_THAT (report (4), WithinAbs (-3.0f, 1.0e-6f));
    CHECK_THAT (report (5), WithinAbs (137.0f, 1.0e-6f));
    CHECK (flags() == (std::uint32_t) valid);

    REQUIRE (connection.prepareToPlay (96000.0, 64, error));
    enableReport (96000.0);
    process (nullptr);
    CHECK_THAT (report (1), WithinAbs (96000.0f, 1.0e-6f));
    CHECK_THAT (report (2), WithinAbs (0.0f, 1.0e-6f));
    CHECK_THAT (report (3), WithinAbs (0.0f, 1.0e-6f));
    CHECK (flags() == 0);
    position.isPlaying = true;
    position.timeInSamples = 96000;
    position.timeInSeconds = 1.0;
    position.ppqPosition = 2.5;
    process (&position);
    CHECK_THAT (report (1), WithinAbs (96000.0f, 1.0e-6f));
    CHECK_THAT (report (2), WithinAbs (96000.0f, 1.0e-6f));
    CHECK_THAT (report (3), WithinAbs (2.5f, 1.0e-6f));
    CHECK (flags() == (std::uint32_t) (valid | Context::kPlaying));
}
#endif
