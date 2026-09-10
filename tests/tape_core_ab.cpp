#include <catch2/catch_test_macros.hpp>

#include "dsp/MasterTape.h"
#include "session/Session.h"
#include <core/TapeMachineDSP.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

// Contract test for Dusk's framework-free MasterTape adapter against the
// authoritative DAF TapeMachineDSP. The donor's old JUCE processor is no
// longer part of the application or this test: MasterTape is intentionally a
// thin parameter/lifecycle seam around the current core.

namespace
{
constexpr double kSampleRate = 48000.0;
constexpr int kBlock = 256;
constexpr int kBlocks = 16;
constexpr float kExactTolerance = 1.0e-12f;

struct Rng
{
    std::uint32_t state;

    float next() noexcept
    {
        state = state * 1664525u + 1013904223u;
        return static_cast<float> ((state >> 8) & 0xFFFFFF) /
                   static_cast<float> (0x1000000) * 2.0f - 1.0f;
    }
};

void makeSignal (std::vector<float>& left, std::vector<float>& right)
{
    const int total = kBlocks * kBlock;
    left.resize (static_cast<size_t> (total));
    right.resize (static_cast<size_t> (total));

    Rng rng { 0x1234567u };
    constexpr double twoPi = 6.28318530717958647692;
    double p1 = 0.0, p2 = 0.0, p3 = 0.0;
    const int clickPeriod = static_cast<int> (kSampleRate * 0.09);
    const int clickLength = static_cast<int> (kSampleRate * 0.0008);
    const float segmentAmplitude[5] = { 0.12f, 0.5f, 0.25f, 0.7f, 0.05f };

    for (int i = 0; i < total; ++i)
    {
        const double time = static_cast<double> (i) / kSampleRate;
        const float amplitude = segmentAmplitude[static_cast<int> (time / 0.011) % 5];
        const float signalLeft = 0.6f * static_cast<float> (std::sin (p1))
                               + 0.3f * static_cast<float> (std::sin (p2))
                               + 0.2f * static_cast<float> (std::sin (p3));
        const float signalRight = 0.55f * static_cast<float> (std::sin (p1 + 0.7))
                                + 0.32f * static_cast<float> (std::sin (p2 * 1.01))
                                + 0.18f * static_cast<float> (std::sin (p3));
        p1 += twoPi * 90.0 / kSampleRate;
        p2 += twoPi * 610.0 / kSampleRate;
        p3 += twoPi * 2350.0 / kSampleRate;

        float transient = 0.0f;
        if ((i % clickPeriod) < clickLength)
            transient = rng.next() > 0.0f ? 0.55f : -0.55f;

        left[static_cast<size_t> (i)] = amplitude * signalLeft + transient;
        right[static_cast<size_t> (i)] = amplitude * signalRight + transient * 0.8f;
    }
}

struct Settings
{
    int machine = 0;
    int speed = 1;
    int type = 0;
    int signalPath = 0;
    int eqStandard = 0;
    int calibration = 0;
    int headWidth = 1;
    float inputGainDb = 0.0f;
    float bias = 50.0f;
    float highpassHz = 20.0f;
    float lowpassHz = 20000.0f;
    float noiseAmount = 0.0f;
    float wow = 7.0f;
    float flutter = 3.0f;
    float outputGainDb = 0.0f;
    bool autoCal = true;
    bool noiseEnabled = false;
    bool autoComp = true;
    bool crosstalk = true;
    bool wowFlutterEnabled = true;
    bool transformer = true;
    float reproLfDb = 0.0f;
    float reproLmfDb = 0.0f;
    float reproHmfDb = 0.0f;
    float reproHfDb = 0.0f;
    float levelHmfTrimDb = 0.0f;
    float levelHfTrimDb = 0.0f;
    float lpQ = 0.707f;
    float progHmfTrimDb = 0.0f;
    float progHfTrimDb = 0.0f;
    float reproSubBellDb = 0.0f;
    float progLfTrimDb = 0.0f;
};

void applyToParams (duskstudio::TapeParams& params, const Settings& settings)
{
    constexpr auto order = std::memory_order_relaxed;
    params.machine.store (settings.machine, order);
    params.speed.store (settings.speed, order);
    params.type.store (settings.type, order);
    params.signalPath.store (settings.signalPath, order);
    params.eqStandard.store (settings.eqStandard, order);
    params.calibration.store (settings.calibration, order);
    params.headWidth.store (settings.headWidth, order);
    params.inputGainDb.store (settings.inputGainDb, order);
    params.bias.store (settings.bias, order);
    params.highpassHz.store (settings.highpassHz, order);
    params.lowpassHz.store (settings.lowpassHz, order);
    params.noiseAmount.store (settings.noiseAmount, order);
    params.noiseEnabled.store (settings.noiseEnabled, order);
    params.wow.store (settings.wow, order);
    params.flutter.store (settings.flutter, order);
    params.outputGainDb.store (settings.outputGainDb, order);
    params.autoCal.store (settings.autoCal, order);
    params.autoComp.store (settings.autoComp, order);
    params.crosstalk.store (settings.crosstalk, order);
    params.wowFlutterEnabled.store (settings.wowFlutterEnabled, order);
    params.transformer.store (settings.transformer, order);
    params.reproLfDb.store (settings.reproLfDb, order);
    params.reproLmfDb.store (settings.reproLmfDb, order);
    params.reproHmfDb.store (settings.reproHmfDb, order);
    params.reproHfDb.store (settings.reproHfDb, order);
    params.levelHmfTrimDb.store (settings.levelHmfTrimDb, order);
    params.levelHfTrimDb.store (settings.levelHfTrimDb, order);
    params.lpQ.store (settings.lpQ, order);
    params.progHmfTrimDb.store (settings.progHmfTrimDb, order);
    params.progHfTrimDb.store (settings.progHfTrimDb, order);
    params.reproSubBellDb.store (settings.reproSubBellDb, order);
    params.progLfTrimDb.store (settings.progLfTrimDb, order);
}

void applyToCore (duskaudio::TapeMachineDSP& core, const Settings& settings)
{
    core.setTapeMachine (settings.machine);
    core.setTapeSpeed (settings.speed);
    core.setTapeType (settings.type);
    core.setSignalPath (settings.signalPath);
    core.setEqStandard (settings.eqStandard);
    core.setCalibration (settings.calibration);
    core.setHeadWidth (settings.headWidth);
    core.setInputGainDb (settings.inputGainDb);
    core.setBias (settings.bias);
    core.setHighpassHz (settings.highpassHz);
    core.setLowpassHz (settings.lowpassHz);
    core.setNoiseAmount (settings.noiseAmount);
    core.setNoiseEnabled (settings.noiseEnabled);
    core.setWow (settings.wow);
    core.setFlutter (settings.flutter);
    core.setOutputGainDb (settings.outputGainDb);
    core.setAutoCal (settings.autoCal);
    core.setAutoComp (settings.autoComp);
    core.setCrosstalk (settings.crosstalk);
    core.setWowFlutterEnabled (settings.wowFlutterEnabled);
    core.setTransformer (settings.transformer);
    core.setReproLf (settings.reproLfDb);
    core.setReproLmf (settings.reproLmfDb);
    core.setReproHmf (settings.reproHmfDb);
    core.setReproHf (settings.reproHfDb);
    core.setLevelHmfTrim (settings.levelHmfTrimDb);
    core.setLevelHfTrim (settings.levelHfTrimDb);
    core.setLpQ (settings.lpQ);
    core.setProgHmfTrim (settings.progHmfTrimDb);
    core.setProgHfTrim (settings.progHfTrimDb);
    core.setReproSubBell (settings.reproSubBellDb);
    core.setProgLfTrim (settings.progLfTrimDb);
}

struct Render
{
    std::vector<float> left;
    std::vector<float> right;
    int latency = 0;
};

Render renderAdapter (const Settings& settings,
                      const std::vector<float>& inputLeft,
                      const std::vector<float>& inputRight)
{
    duskstudio::MasterTape tape;
    tape.prepare (kSampleRate, kBlock);

    duskstudio::TapeParams params;
    applyToParams (params, settings);
    tape.pushParameters (params);

    Render output { inputLeft, inputRight, tape.latencySamples() };
    for (int offset = 0; offset < static_cast<int> (output.left.size()); offset += kBlock)
    {
        const int count = std::min (kBlock, static_cast<int> (output.left.size()) - offset);
        tape.processInPlace (output.left.data() + offset, output.right.data() + offset, count);
    }
    return output;
}

Render renderCore (const Settings& settings, int storedOversamplingChoice,
                   const std::vector<float>& inputLeft,
                   const std::vector<float>& inputRight)
{
    duskaudio::TapeMachineDSP core;
    core.setOversampling (storedOversamplingChoice);
    core.prepare (kSampleRate, kBlock);
    core.reset();
    applyToCore (core, settings);

    Render output { inputLeft, inputRight, core.latencySamples() };
    for (int offset = 0; offset < static_cast<int> (output.left.size()); offset += kBlock)
    {
        const int count = std::min (kBlock, static_cast<int> (output.left.size()) - offset);
        float* channels[2] = { output.left.data() + offset, output.right.data() + offset };
        core.processBlock (channels, channels, 2, count);
    }
    return output;
}

float peakDifference (const Render& first, const Render& second)
{
    REQUIRE (first.left.size() == second.left.size());
    REQUIRE (first.right.size() == second.right.size());

    float peak = 0.0f;
    for (size_t i = 0; i < first.left.size(); ++i)
    {
        peak = std::max (peak, std::abs (first.left[i] - second.left[i]));
        peak = std::max (peak, std::abs (first.right[i] - second.right[i]));
    }
    return peak;
}
} // namespace

TEST_CASE ("MasterTape matches the current DAF TapeMachine core", "[tape][ab][regression][issue-383]")
{
    std::vector<float> inputLeft, inputRight;
    makeSignal (inputLeft, inputRight);

    Settings settings;
    SECTION ("default Dusk tape state") {}
    SECTION ("hot drive") { settings.inputGainDb = 9.0f; }
    SECTION ("manual output gain")
    {
        settings.autoComp = false;
        settings.outputGainDb = -4.0f;
    }
    SECTION ("Classic 102 at 30 IPS with CCIR EQ")
    {
        settings.machine = 1;
        settings.speed = 2;
        settings.eqStandard = 1;
        settings.type = 2;
    }
    SECTION ("Sync path with manual bias and filters")
    {
        settings.signalPath = 1;
        settings.autoCal = false;
        settings.bias = 72.0f;
        settings.highpassHz = 80.0f;
        settings.lowpassHz = 12000.0f;
    }
    SECTION ("deterministic modulation and noise")
    {
        settings.noiseAmount = 12.0f;
        settings.noiseEnabled = true;
        settings.wow = 18.0f;
        settings.flutter = 9.0f;
    }
    SECTION ("current TM2 American and factory-calibration surface")
    {
        settings.machine = 1;
        settings.speed = 3;
        settings.type = 1;
        settings.headWidth = 2;
        settings.crosstalk = false;
        settings.wowFlutterEnabled = false;
        settings.transformer = false;
        settings.reproLfDb = 2.5f;
        settings.reproLmfDb = -1.5f;
        settings.reproHmfDb = 3.0f;
        settings.reproHfDb = -2.0f;
        settings.levelHmfTrimDb = 4.0f;
        settings.levelHfTrimDb = -3.0f;
        settings.lpQ = 1.35f;
        settings.progHmfTrimDb = 2.0f;
        settings.progHfTrimDb = -1.0f;
        settings.reproSubBellDb = 1.75f;
        settings.progLfTrimDb = 3.25f;
    }

    const Render adapter = renderAdapter (settings, inputLeft, inputRight);
    const Render core = renderCore (settings, 1, inputLeft, inputRight);

    const float difference = peakDifference (adapter, core);
    REQUIRE (adapter.latency == core.latency);
    INFO ("peak adapter/core difference: " << difference);
    REQUIRE (difference <= kExactTolerance);
}

TEST_CASE ("TapeMachine stays on its tuned 2x path for legacy oversampling choices", "[tape][ab]")
{
    std::vector<float> inputLeft, inputRight;
    makeSignal (inputLeft, inputRight);
    const Settings settings;
    const Render reference = renderCore (settings, 1, inputLeft, inputRight);

    // The current donor intentionally ignores its retained 1x/2x/4x state and
    // always runs the factory-preset-calibrated 2x path.
    REQUIRE (reference.latency == 56);
    for (int storedChoice = 0; storedChoice <= 2; ++storedChoice)
    {
        const Render core = renderCore (settings, storedChoice, inputLeft, inputRight);
        INFO ("stored donor choice " << storedChoice);
        REQUIRE (core.latency == reference.latency);
        REQUIRE (peakDifference (core, reference) <= kExactTolerance);
    }

    const Render adapter = renderAdapter (settings, inputLeft, inputRight);
    REQUIRE (adapter.latency == reference.latency);
    REQUIRE (peakDifference (adapter, reference) <= kExactTolerance);
}

TEST_CASE ("MasterTape passes silence through as silence", "[tape]")
{
    const std::vector<float> silence (static_cast<size_t> (kBlocks * kBlock), 0.0f);
    Settings settings;
    settings.inputGainDb = 9.0f;

    const Render output = renderAdapter (settings, silence, silence);
    REQUIRE (std::all_of (output.left.begin(), output.left.end(),
                          [] (float value) { return std::abs (value) <= kExactTolerance; }));
    REQUIRE (std::all_of (output.right.begin(), output.right.end(),
                          [] (float value) { return std::abs (value) <= kExactTolerance; }));
}

TEST_CASE ("MasterTape exposes the donor's zero-latency Thru path", "[tape]")
{
    std::vector<float> inputLeft, inputRight;
    makeSignal (inputLeft, inputRight);
    Settings settings;
    settings.signalPath = 3;
    settings.inputGainDb = 9.0f;

    const Render output = renderAdapter (settings, inputLeft, inputRight);
    const Render input { inputLeft, inputRight, 0 };
    REQUIRE (output.latency == 0);
    REQUIRE (peakDifference (output, input) <= kExactTolerance);
}

TEST_CASE ("MasterTape forwards the donor output meters", "[tape]")
{
    std::vector<float> left, right;
    makeSignal (left, right);

    duskstudio::MasterTape tape;
    tape.prepare (kSampleRate, kBlock);
    duskstudio::TapeParams params;
    tape.pushParameters (params);
    for (int offset = 0; offset < static_cast<int> (left.size()); offset += kBlock)
        tape.processInPlace (left.data() + offset, right.data() + offset, kBlock);

    const auto vu = tape.getVu();
    REQUIRE (std::isfinite (vu.outL));
    REQUIRE (std::isfinite (vu.outR));
    REQUIRE (vu.outL > 0.0f);
    REQUIRE (vu.outR > 0.0f);
}
