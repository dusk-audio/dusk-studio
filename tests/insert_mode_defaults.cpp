#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "dsp/AuxLaneStrip.h"
#include "dsp/ChannelStrip.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <string>

using namespace duskstudio;
using Catch::Matchers::WithinAbs;

namespace
{
constexpr int kFrames = 128;
constexpr int kSettleBlocks = 50;
using Block = std::array<float, kFrames>;

void invertUtility (builtin::NativeBuiltinSlot& slot)
{
    for (int i = 0; i < slot.paramCount(); ++i)
        if (std::string (slot.paramInfo (i)->id) == "polarity")
        {
            slot.setParamValue (i, 1.0f);
            return;
        }
    FAIL ("Utility has no polarity parameter");
}

void routeHardware (HardwareInsertParams& params)
{
    params.enabled.store (true);
    params.routing.publish (std::make_unique<HardwareInsertRouting> (
        HardwareInsertRouting { 0, 1, 0, 1, 0, 0 }));
}

struct Choice
{
    int mode;
    float left, right;
};

double toneComponent (const float* samples, bool quadrature)
{
    double sum = 0;
    for (int i = 0; i < kFrames; ++i)
    {
        const double phase = 6.283185307179586 * i / 32.0;
        sum += samples[i] * (quadrature ? std::cos (phase) : std::sin (phase));
    }
    return 2.0 * sum / kFrames;
}
}

TEST_CASE ("Appendix A: channel insert defaults to Plugin and routes every mode",
           "[appendix][channel-strip]")
{
    ChannelStripParams params;
    HardwareInsertParams hardware;
    auto strip = std::make_unique<ChannelStrip>();
    REQUIRE (strip->insertMode.load() == ChannelStrip::kInsertPlugin);
    routeHardware (hardware);
    strip->bind (params);
    strip->bindHardwareInsert (hardware);
    strip->prepare (48000.0, kFrames);
    std::string error;
    REQUIRE (strip->loadBuiltin ("dusk.builtin.utility", error));
    invertUtility (strip->getBuiltinSlot());

    Block inputL, inputR, returnL, returnR, sendL {}, sendR {}, masterL {}, masterR {}, sink {};
    for (size_t i = 0; i < inputL.size(); ++i)
    {
        const float wave = (float) std::sin (6.283185307179586 * (double) i / 32.0);
        inputL[i] = 0.25f * wave; inputR[i] = 0.125f * wave;
        returnL[i] = 0.75f * wave; returnR[i] = -0.5f * wave;
    }
    const float* inputs[] = { returnL.data(), returnR.data() };
    float* outputs[] = { sendL.data(), sendR.data() };
    std::array<float*, 4> sinks { sink.data(), sink.data(), sink.data(), sink.data() };
    juce::MidiBuffer midi;

    for (const auto choice : {
             Choice { ChannelStrip::kInsertPlugin, -0.25f, -0.125f },
             Choice { ChannelStrip::kInsertEmpty, 0.25f, 0.125f },
             Choice { ChannelStrip::kInsertHardware, 0.75f, -0.5f },
             Choice { ChannelStrip::kInsertPlugin, -0.25f, -0.125f } })
    {
        CAPTURE (choice.mode);
        strip->insertMode.store (choice.mode);
        // The strip always runs console coloration, even with EQ disabled.
        // Feed the expected insert signal through an empty reference strip.
        auto reference = std::make_unique<ChannelStrip>();
        reference->bind (params);
        reference->insertMode.store (ChannelStrip::kInsertEmpty);
        reference->prepare (48000.0, kFrames);
        Block expectedL, expectedR;
        for (size_t i = 0; i < expectedL.size(); ++i)
        {
            const float wave = (float) std::sin (6.283185307179586 * (double) i / 32.0);
            expectedL[i] = choice.left * wave;
            expectedR[i] = choice.right * wave;
        }
        for (int block = 0; block < kSettleBlocks; ++block)
        {
            sendL.fill (0); sendR.fill (0);
            masterL.fill (0); masterR.fill (0); sink.fill (0);
            strip->processAndAccumulate (inputL.data(), inputR.data(), midi, false,
                                        masterL.data(), masterR.data(), sinks, sinks, sinks, sinks,
                                        kFrames, true, inputs, 2, outputs, 2);
            reference->processAndAccumulate (expectedL.data(), expectedR.data(), midi, false,
                                            masterL.data(), masterR.data(), sinks, sinks, sinks, sinks,
                                            kFrames, true);
        }
        REQUIRE (strip->getLastProcessedSamples() == kFrames);
        REQUIRE (strip->getLastProcessedMono() != nullptr);
        REQUIRE (strip->getLastProcessedR() != nullptr);
        REQUIRE (reference->getLastProcessedMono() != nullptr);
        REQUIRE (reference->getLastProcessedR() != nullptr);
        // Whole tone periods reject the DC transient left by a mode crossfade.
        for (bool quadrature : { false, true })
        {
            CHECK_THAT (toneComponent (strip->getLastProcessedMono(), quadrature),
                        WithinAbs (toneComponent (reference->getLastProcessedMono(), quadrature), 1e-4));
            CHECK_THAT (toneComponent (strip->getLastProcessedR(), quadrature),
                        WithinAbs (toneComponent (reference->getLastProcessedR(), quadrature), 1e-4));
        }
    }
}

TEST_CASE ("Appendix A: aux insert defaults to Empty and routes every mode",
           "[appendix][aux-lane]")
{
    AuxLaneParams params;
    HardwareInsertParams hardware;
    auto strip = std::make_unique<AuxLaneStrip>();
    for (const auto& mode : strip->insertMode)
        REQUIRE (mode.load() == AuxLaneStrip::kInsertEmpty);
    routeHardware (hardware);
    strip->bind (params);
    strip->bindHardwareInsert (0, hardware);
    strip->prepare (48000.0, kFrames);
    std::string error;
    REQUIRE (strip->loadBuiltin (0, "dusk.builtin.utility", error));
    invertUtility (strip->getBuiltinSlot (0));

    Block left, right, returnL, returnR, sendL {}, sendR {};
    returnL.fill (0.75f); returnR.fill (-0.5f);
    const float* inputs[] = { returnL.data(), returnR.data() };
    float* outputs[] = { sendL.data(), sendR.data() };
    for (const auto choice : {
             Choice { AuxLaneStrip::kInsertEmpty, 0.25f, 0.125f },
             Choice { AuxLaneStrip::kInsertPlugin, -0.25f, -0.125f },
             Choice { AuxLaneStrip::kInsertHardware, 0.75f, -0.5f },
             Choice { AuxLaneStrip::kInsertEmpty, 0.25f, 0.125f } })
    {
        CAPTURE (choice.mode);
        strip->insertMode[0].store (choice.mode);
        for (int block = 0; block < kSettleBlocks; ++block)
        {
            left.fill (0.25f); right.fill (0.125f);
            sendL.fill (0); sendR.fill (0);
            strip->processStereoBlock (left.data(), right.data(), kFrames,
                                       inputs, 2, outputs, 2);
        }
        for (size_t i = 0; i < left.size(); ++i)
        {
            CHECK_THAT (left[i], WithinAbs (choice.left, 1e-5));
            CHECK_THAT (right[i], WithinAbs (choice.right, 1e-5));
        }
    }
}

TEST_CASE ("Aux cue keeps processed audio while the master return is muted", "[aux-lane][cue]")
{
    for (const bool hardwareMode : { false, true })
    {
        CAPTURE (hardwareMode);
        AuxLaneParams params;
        HardwareInsertParams hardware;
        routeHardware (hardware);
        auto strip = std::make_unique<AuxLaneStrip>();
        strip->bind (params);
        strip->bindHardwareInsert (0, hardware);
        strip->prepare (48000.0, kFrames);
        std::string error;
        REQUIRE (strip->loadBuiltin (0, "dusk.builtin.utility", error));
        invertUtility (strip->getBuiltinSlot (0));
        strip->insertMode[0].store (hardwareMode ? AuxLaneStrip::kInsertHardware : AuxLaneStrip::kInsertPlugin);
        params.outputPair.store (2004);
        Block left {}, right {}, returnL {}, returnR {};
        returnL.fill (0.75f); returnR.fill (-0.5f);
        std::array<Block, 4> physical {};
        const float* inputs[] { returnL.data(), returnR.data() };
        float* outputs[] { physical[0].data(), physical[1].data(), physical[2].data(), physical[3].data() };
        const auto block = [&] (int numOutputs = 4)
        {
            left.fill (0.25f); right.fill (0.125f);
            for (auto& output : physical) output.fill (0.0f);
            strip->processStereoBlock (left.data(), right.data(), kFrames, inputs, 2, outputs, numOutputs);
        };
        struct State { const char* name; bool manualMute, liveMute; float db; };
        for (const auto state : {
            State { "open", false, false, 0.0f },
            State { "manual mute", true, true, 0.0f },
            State { "automation mute", false, true, -12.0f },
            State { "return fader down", false, false, -12.0f },
            State { "return fader floor", false, false, -100.0f },
            State { "unmute", false, false, 0.0f } })
        {
            CAPTURE (state.name);
            params.mute.store (state.manualMute); params.liveMute.store (state.liveMute);
            params.returnLevelDb.store (state.db); params.liveReturnLevelDb.store (state.db);
            for (int n = 0; n < kSettleBlocks; ++n) block();
            const float processedL = hardwareMode ? 0.75f : -0.25f;
            const float processedR = hardwareMode ? -0.5f : -0.125f;
            const float gain = state.liveMute || state.db <= -100.0f ? 0.0f : std::pow (10.0f, state.db / 20.0f);
            float cueError = 0.0f, returnError = 0.0f, sendError = 0.0f;
            for (std::size_t n = 0; n < left.size(); ++n)
            {
                cueError = std::max ({ cueError, std::abs (physical[2][n] - processedL), std::abs (physical[3][n] - processedR) });
                returnError = std::max ({ returnError, std::abs (left[n] - processedL * gain), std::abs (right[n] - processedR * gain) });
                if (hardwareMode)
                    sendError = std::max ({ sendError, std::abs (physical[0][n] - 0.25f), std::abs (physical[1][n] - 0.125f) });
            }
            CHECK_THAT (cueError, WithinAbs (0.0f, 1.0e-5f));
            CHECK_THAT (strip->getPreReturnPeak(), WithinAbs (std::max (std::abs (processedL), std::abs (processedR)), 1.0e-5f));
            CHECK_THAT (returnError, WithinAbs (0.0f, 1.0e-5f));
            CHECK_THAT (sendError, WithinAbs (0.0f, 1.0e-5f));
            if (state.liveMute)
            {
                CHECK_THAT (params.meterPostL.load(), WithinAbs (-100.0f, 1.0e-5f));
                CHECK_THAT (params.meterPostR.load(), WithinAbs (-100.0f, 1.0e-5f));
            }
        }

        for (const bool validCue : { false, true })
        {
            CAPTURE (validCue);
            params.outputPair.store (2004);
            params.liveMute.store (false); params.liveReturnLevelDb.store (0.0f);
            for (int n = 0; n < kSettleBlocks; ++n) block();
            params.liveMute.store (true); params.liveReturnLevelDb.store (-12.0f);
            if (! validCue) params.outputPair.store (-1);
            for (int n = 0; n < kSettleBlocks; ++n) block();
            if (! validCue)
            {
                CHECK_THAT (strip->getPreReturnPeak(), WithinAbs (0.0f, 0.0f));
                for (const auto& output : physical)
                    for (const float sample : output) REQUIRE_THAT (sample, WithinAbs (0.0f, 1.0e-7f));
            }
            params.liveMute.store (false);
            block();
            const float firstGain = 1.0f + (std::pow (10.0f, -12.0f / 20.0f) - 1.0f) / 960.0f;
            CHECK_THAT (left.front(), WithinAbs ((hardwareMode ? 0.75f : -0.25f) * firstGain, 1.0e-5f));
        }

        params.outputPair.store (2004);
        params.liveMute.store (true);
        hardware.pingPending.store (true); hardware.pingResult.store (531);
        for (int n = 0; n < kSettleBlocks; ++n) block (2);
        CHECK_THAT (strip->getPreReturnPeak(), WithinAbs (0.0f, 0.0f));
        for (const auto& output : physical)
            for (const float sample : output) REQUIRE_THAT (sample, WithinAbs (0.0f, 1.0e-7f));
        CHECK (hardware.pingPending.load());
        CHECK (hardware.pingResult.load() == 531);
    }
}

// With no audio device open nothing ever prepares the strips, so every insert
// load - plug-in, built-in or soundfont - has to refuse rather than build a host
// at a sample rate of zero.
TEST_CASE ("an insert never prepared by a device refuses plug-in and soundfont loads",
           "[channel-strip][aux-lane][device]")
{
    const juce::File missing = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                   .getChildFile ("dusk-unprepared-insert-never-opened");
    std::string error;
    const auto refused = [&error] (bool loaded, const char* expected)
    {
        CHECK_FALSE (loaded);
        CHECK (error == expected);
        error.clear();
    };

    ChannelStripParams params;
    auto strip = std::make_unique<ChannelStrip>();
    strip->bind (params);
    refused (strip->loadBuiltin ("dusk.builtin.utility", error), "channel strip not prepared");
   #if DUSKSTUDIO_HAS_NATIVE_CLAP
    refused (strip->loadNativeClap (missing, error), "channel strip not prepared");
   #endif
   #if DUSKSTUDIO_HAS_NATIVE_LV2
    refused (strip->loadNativeLv2 (missing, error), "channel strip not prepared");
   #endif
   #if DUSKSTUDIO_HAS_NATIVE_VST3
    refused (strip->loadNativeVst3 (missing, error), "channel strip not prepared");
   #endif
   #if DUSKSTUDIO_HAS_NATIVE_AU
    refused (strip->loadNativeAu ("aufx dusk Dusk", error), "channel strip not prepared");
   #endif
   #if DUSKSTUDIO_HAS_MULTISAMPLE
    refused (strip->loadNativeMultisample (missing, error), "channel strip not prepared");
    refused ((bool) strip->primeNativeMultisample (missing, error), "channel strip not prepared");
   #endif
    CHECK_FALSE (strip->isBuiltinLoaded());
    CHECK_FALSE (strip->isNativeClapLoaded());
    CHECK_FALSE (strip->isNativeLv2Loaded());
    CHECK_FALSE (strip->isNativeVst3Loaded());
    CHECK_FALSE (strip->isNativeAuLoaded());
    CHECK_FALSE (strip->isNativeMultisampleLoaded());

    AuxLaneParams auxParams;
    auto aux = std::make_unique<AuxLaneStrip>();
    aux->bind (auxParams);
    refused (aux->loadBuiltin (0, "dusk.builtin.utility", error), "aux lane not prepared");
   #if DUSKSTUDIO_HAS_NATIVE_CLAP
    refused (aux->loadNativeClap (0, missing, error), "aux lane not prepared");
   #endif
   #if DUSKSTUDIO_HAS_NATIVE_LV2
    refused (aux->loadNativeLv2 (0, missing, error), "aux lane not prepared");
   #endif
   #if DUSKSTUDIO_HAS_NATIVE_VST3
    refused (aux->loadNativeVst3 (0, missing, error), "aux lane not prepared");
   #endif
   #if DUSKSTUDIO_HAS_NATIVE_AU
    refused (aux->loadNativeAu (0, "aufx dusk Dusk", error), "aux lane not prepared");
   #endif
    CHECK_FALSE (aux->isBuiltinLoaded (0));

    // The refusal is the missing device, not the unit: once a device prepares
    // the inserts the same loads go through.
    strip->prepare (48000.0, kFrames);
    aux->prepare (48000.0, kFrames);
    CHECK (strip->loadBuiltin ("dusk.builtin.utility", error));
    CHECK (aux->loadBuiltin (0, "dusk.builtin.utility", error));
}
