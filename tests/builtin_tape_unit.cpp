#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/builtin/NativeBuiltinSlot.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

// The Tape unit wraps the Tape Machine 2 core the master bus also runs. A tape
// is not transparent by design, so the unity assertion goes through the core's
// own Thru path, which is the setting that means "off the tape".

using namespace duskstudio::builtin;
using Catch::Matchers::WithinAbs;

namespace
{
constexpr double kSampleRate = 48000.0;
constexpr int    kBlock      = 256;
constexpr int    kThruPath   = 3;
constexpr double kPi         = 3.14159265358979323846;   // M_PI is non-standard

int paramIndex (const NativeBuiltinSlot& slot, const char* id)
{
    for (int i = 0; i < slot.paramCount(); ++i)
        if (std::string (slot.paramInfo (i)->id) == id) return i;
    FAIL ("no parameter with id " << id);
    return -1;
}

void loadTape (NativeBuiltinSlot& slot)
{
    std::string error;
    REQUIRE (slot.loadUnit ("dusk.builtin.tape", kSampleRate, kBlock, error));
    REQUIRE (error.empty());
}

void fillTone (std::vector<float>& l, std::vector<float>& r, int startSample)
{
    for (size_t i = 0; i < l.size(); ++i)
    {
        const double t = (double) (startSample + (int) i) / kSampleRate;
        const float v = (float) (0.4 * std::sin (2.0 * kPi * 440.0 * t));
        l[i] = v;
        r[i] = v * 0.9f;
    }
}
} // namespace

TEST_CASE ("tape unit is registered and loads", "[builtin][tape]")
{
    const auto* unit = findUnit ("dusk.builtin.tape");
    REQUIRE (unit != nullptr);
    REQUIRE_FALSE (unit->isInstrument);

    NativeBuiltinSlot slot;
    loadTape (slot);
    REQUIRE (slot.displayName() == "Tape");
}

TEST_CASE ("tape unit turns silence into silence", "[builtin][tape]")
{
    NativeBuiltinSlot slot;
    loadTape (slot);

    std::vector<float> l ((size_t) kBlock, 0.0f), r ((size_t) kBlock, 0.0f);
    for (int b = 0; b < 64; ++b)
    {
        std::fill (l.begin(), l.end(), 0.0f);
        std::fill (r.begin(), r.end(), 0.0f);
        slot.processStereo (l.data(), r.data(), l.data(), r.data(), kBlock);
    }

    for (int i = 0; i < kBlock; ++i)
    {
        REQUIRE (std::isfinite (l[(size_t) i]));
        REQUIRE_THAT (l[(size_t) i], WithinAbs (0.0, 1e-7));
        REQUIRE_THAT (r[(size_t) i], WithinAbs (0.0, 1e-7));
    }
}

TEST_CASE ("tape unit on the Thru path is unity gain", "[builtin][tape]")
{
    NativeBuiltinSlot slot;
    loadTape (slot);
    slot.setParamValue (paramIndex (slot, "signal_path"), (float) kThruPath);

    std::vector<float> l ((size_t) kBlock), r ((size_t) kBlock);
    for (int b = 0; b < 20; ++b)
    {
        fillTone (l, r, b * kBlock);
        slot.processStereo (l.data(), r.data(), l.data(), r.data(), kBlock);
    }

    fillTone (l, r, 20 * kBlock);
    const auto expectedL = l;
    const auto expectedR = r;
    slot.processStereo (l.data(), r.data(), l.data(), r.data(), kBlock);

    for (int i = 0; i < kBlock; ++i)
    {
        REQUIRE_THAT (l[(size_t) i], WithinAbs (expectedL[(size_t) i], 1e-6));
        REQUIRE_THAT (r[(size_t) i], WithinAbs (expectedR[(size_t) i], 1e-6));
    }
}

TEST_CASE ("tape unit reports latency only on a path that takes it", "[builtin][tape]")
{
    NativeBuiltinSlot slot;
    loadTape (slot);

    // Repro: the core's filter round trip, which plugin delay compensation has
    // to cover.
    REQUIRE (slot.getLatencySamples() == 56);

    // Thru is a sample-exact passthrough that never enters those filters, so
    // reporting the same figure would have PDC shift every other track to match
    // a delay this insert is not adding.
    slot.setParamValue (paramIndex (slot, "signal_path"), (float) kThruPath);
    REQUIRE (slot.getLatencySamples() == 0);

    slot.setBypassed (true);
    REQUIRE (slot.getLatencySamples() == 0);
}

TEST_CASE ("tape unit colours the signal on its normal path", "[builtin][tape]")
{
    NativeBuiltinSlot slot;
    loadTape (slot);
    slot.setParamValue (paramIndex (slot, "input"), 8.0f);

    std::vector<float> l ((size_t) kBlock), r ((size_t) kBlock);
    for (int b = 0; b < 20; ++b)
    {
        fillTone (l, r, b * kBlock);
        slot.processStereo (l.data(), r.data(), l.data(), r.data(), kBlock);
    }

    fillTone (l, r, 20 * kBlock);
    const auto dry = l;
    slot.processStereo (l.data(), r.data(), l.data(), r.data(), kBlock);

    float worst = 0.0f;
    for (int i = 0; i < kBlock; ++i)
        worst = std::max (worst, std::abs (l[(size_t) i] - dry[(size_t) i]));
    REQUIRE (worst > 0.01f);
}

TEST_CASE ("tape unit state round-trips", "[builtin][tape]")
{
    NativeBuiltinSlot saver;
    loadTape (saver);
    const int inputIdx = paramIndex (saver, "input");
    const int speedIdx = paramIndex (saver, "speed");
    const int biasIdx  = paramIndex (saver, "bias");
    saver.setParamValue (inputIdx, 4.5f);
    saver.setParamValue (speedIdx, 2.0f);
    saver.setParamValue (biasIdx, 62.0f);

    std::vector<std::uint8_t> blob;
    REQUIRE (saver.saveState (blob));

    NativeBuiltinSlot loader;
    loadTape (loader);
    REQUIRE (loader.loadState (blob));
    REQUIRE_THAT (loader.getParamValue (inputIdx), WithinAbs (4.5, 1e-6));
    REQUIRE_THAT (loader.getParamValue (speedIdx), WithinAbs (2.0, 1e-6));
    REQUIRE_THAT (loader.getParamValue (biasIdx),  WithinAbs (62.0, 1e-6));

    NativeBuiltinSlot delay;
    std::string error;
    REQUIRE (delay.loadUnit ("dusk.builtin.delay", kSampleRate, kBlock, error));
    REQUIRE_FALSE (delay.loadState (blob));
}
