#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/builtin/NativeBuiltinSlot.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

// The Delay unit wraps the donor tape-echo core. Its output is
// dry*in + echo*taps + reverb*spring, so Echo and Reverb at zero is an exact
// passthrough even though the tape keeps running underneath.

using namespace duskstudio::builtin;
using Catch::Matchers::WithinAbs;

namespace
{
constexpr double kSampleRate = 48000.0;
constexpr int    kBlock      = 256;

int paramIndex (const NativeBuiltinSlot& slot, const char* id)
{
    for (int i = 0; i < slot.paramCount(); ++i)
        if (std::string (slot.paramInfo (i)->id) == id) return i;
    return -1;
}

void loadDelay (NativeBuiltinSlot& slot)
{
    std::string error;
    REQUIRE (slot.loadUnit ("dusk.builtin.delay", kSampleRate, kBlock, error));
    REQUIRE (error.empty());
}
} // namespace

TEST_CASE ("delay unit is registered and loads", "[builtin][delay]")
{
    const auto* unit = findUnit ("dusk.builtin.delay");
    REQUIRE (unit != nullptr);
    REQUIRE_FALSE (unit->isInstrument);

    NativeBuiltinSlot slot;
    loadDelay (slot);
    REQUIRE (slot.displayName() == "Tape Echo");
    // The preamp's 4x oversampling group delay is compensated inside the tape
    // delay, so the unit adds none of its own.
    REQUIRE (slot.getLatencySamples() == 0);
}

TEST_CASE ("delay unit turns silence into silence", "[builtin][delay]")
{
    NativeBuiltinSlot slot;
    loadDelay (slot);
    slot.setParamValue (paramIndex (slot, "echo"), 1.0f);
    slot.setParamValue (paramIndex (slot, "reverb"), 1.0f);

    std::vector<float> l ((size_t) kBlock, 0.0f), r ((size_t) kBlock, 0.0f);
    for (int b = 0; b < 64; ++b)
    {
        std::fill (l.begin(), l.end(), 0.0f);
        std::fill (r.begin(), r.end(), 0.0f);
        slot.processStereo (l.data(), r.data(), l.data(), r.data(), kBlock);
    }

    for (int i = 0; i < kBlock; ++i)
    {
        REQUIRE_THAT (l[(size_t) i], WithinAbs (0.0, 1e-7));
        REQUIRE_THAT (r[(size_t) i], WithinAbs (0.0, 1e-7));
    }
}

TEST_CASE ("delay unit at its defaults is unity gain", "[builtin][delay]")
{
    NativeBuiltinSlot slot;
    loadDelay (slot);

    std::vector<float> l ((size_t) kBlock), r ((size_t) kBlock);
    auto fill = [&]
    {
        for (int i = 0; i < kBlock; ++i)
        {
            l[(size_t) i] = std::sin (0.041f * (float) i) * 0.7f;
            r[(size_t) i] = std::cos (0.023f * (float) i) * 0.4f;
        }
    };

    for (int b = 0; b < 8; ++b) { fill(); slot.processStereo (l.data(), r.data(), l.data(), r.data(), kBlock); }

    fill();
    const auto expectedL = l;
    const auto expectedR = r;
    slot.processStereo (l.data(), r.data(), l.data(), r.data(), kBlock);

    for (int i = 0; i < kBlock; ++i)
    {
        REQUIRE_THAT (l[(size_t) i], WithinAbs (expectedL[(size_t) i], 1e-6));
        REQUIRE_THAT (r[(size_t) i], WithinAbs (expectedR[(size_t) i], 1e-6));
    }
}

TEST_CASE ("delay unit repeats a burst after the input stops", "[builtin][delay]")
{
    NativeBuiltinSlot slot;
    loadDelay (slot);
    slot.setParamValue (paramIndex (slot, "echo"), 1.0f);
    slot.setParamValue (paramIndex (slot, "intensity"), 0.5f);
    slot.setParamValue (paramIndex (slot, "dry"), 0.0f);

    std::vector<float> l ((size_t) kBlock), r ((size_t) kBlock);

    // The shortest head-1 time is 69 ms, so the repeat lands well past the
    // burst: drive one block, then listen through silence for it.
    for (int b = 0; b < 4; ++b)
    {
        for (int i = 0; i < kBlock; ++i)
        {
            const float v = std::sin (0.2f * (float) (b * kBlock + i)) * 0.8f;
            l[(size_t) i] = v;
            r[(size_t) i] = v;
        }
        slot.processStereo (l.data(), r.data(), l.data(), r.data(), kBlock);
    }

    float echoPeak = 0.0f;
    for (int b = 0; b < 96; ++b)
    {
        std::fill (l.begin(), l.end(), 0.0f);
        std::fill (r.begin(), r.end(), 0.0f);
        slot.processStereo (l.data(), r.data(), l.data(), r.data(), kBlock);
        for (int i = 0; i < kBlock; ++i)
            echoPeak = std::max (echoPeak, std::abs (l[(size_t) i]));
    }
    REQUIRE (echoPeak > 1.0e-3f);
}

TEST_CASE ("delay unit state round-trips", "[builtin][delay]")
{
    NativeBuiltinSlot saver;
    loadDelay (saver);
    const int echoIdx      = paramIndex (saver, "echo");
    const int modeIdx      = paramIndex (saver, "mode");
    const int intensityIdx = paramIndex (saver, "intensity");
    saver.setParamValue (echoIdx, 0.65f);
    saver.setParamValue (modeIdx, 7.0f);
    saver.setParamValue (intensityIdx, 0.72f);

    std::vector<std::uint8_t> blob;
    REQUIRE (saver.saveState (blob));

    NativeBuiltinSlot loader;
    loadDelay (loader);
    REQUIRE (loader.loadState (blob));
    REQUIRE_THAT (loader.getParamValue (echoIdx), WithinAbs (0.65, 1e-6));
    REQUIRE_THAT (loader.getParamValue (modeIdx), WithinAbs (7.0, 1e-6));
    REQUIRE_THAT (loader.getParamValue (intensityIdx), WithinAbs (0.72, 1e-6));

    NativeBuiltinSlot reverb;
    std::string error;
    REQUIRE (reverb.loadUnit ("dusk.builtin.reverb", kSampleRate, kBlock, error));
    REQUIRE_FALSE (reverb.loadState (blob));
}
