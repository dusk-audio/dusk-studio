#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "dsp/AuxLaneStrip.h"
#include "dsp/ChannelStrip.h"

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
