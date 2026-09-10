#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/builtin/NativeBuiltinSlot.h"

#include <cmath>
#include <string>
#include <vector>

// The built-in insert rung: registry lookup, the Utility unit's DSP contract,
// the opaque state blob a session carries, and the slot that presents all of it
// to the mixer as a native plug-in.

using namespace duskstudio::builtin;
using Catch::Matchers::WithinAbs;

namespace
{
constexpr double kSampleRate = 48000.0;
constexpr int    kBlock      = 128;

int paramIndex (const NativeBuiltinSlot& slot, const char* id)
{
    for (int i = 0; i < slot.paramCount(); ++i)
        if (std::string (slot.paramInfo (i)->id) == id) return i;
    return -1;
}

// Several blocks so every smoother has reached its target before we measure.
void runBlocks (NativeBuiltinSlot& slot, std::vector<float>& l, std::vector<float>& r,
                int blocks)
{
    for (int b = 0; b < blocks; ++b)
        slot.processStereo (l.data(), r.data(), l.data(), r.data(), (int) l.size());
}
} // namespace

TEST_CASE ("built-in registry resolves units by id")
{
    REQUIRE_FALSE (registry().empty());

    const auto* utility = findUnit ("dusk.builtin.utility");
    REQUIRE (utility != nullptr);
    REQUIRE (std::string (utility->name) == "Utility");
    REQUIRE_FALSE (utility->isInstrument);

    REQUIRE (findUnit ("dusk.builtin.nope") == nullptr);
    REQUIRE (createUnit ("dusk.builtin.nope") == nullptr);
    REQUIRE (createUnit ("dusk.builtin.utility") != nullptr);

    // Every registered unit must be constructible, or the picker offers a row
    // that cannot load.
    for (const auto& unit : registry())
        REQUIRE (createUnit (unit.id) != nullptr);
}

TEST_CASE ("built-in slot loads a unit without touching disk")
{
    NativeBuiltinSlot slot;
    std::string error;
    REQUIRE (slot.loadUnit ("dusk.builtin.utility", kSampleRate, kBlock, error));
    REQUIRE (error.empty());
    REQUIRE (slot.isLoaded());
    REQUIRE (slot.getPluginId() == "dusk.builtin.utility");
    REQUIRE (slot.displayName() == "Utility");
    REQUIRE_FALSE (slot.isLoadedInstrument());
    REQUIRE (slot.getLatencySamples() == 0);

    SECTION ("an unknown id fails and leaves the slot empty")
    {
        std::string err;
        REQUIRE_FALSE (slot.loadUnit ("dusk.builtin.nope", kSampleRate, kBlock, err));
        REQUIRE_FALSE (err.empty());
        REQUIRE_FALSE (slot.isLoaded());
    }

    SECTION ("unload clears the slot")
    {
        slot.unload();
        REQUIRE_FALSE (slot.isLoaded());
        REQUIRE (slot.getPluginId().empty());
    }
}

TEST_CASE ("utility unit passes silence through as silence")
{
    NativeBuiltinSlot slot;
    std::string error;
    REQUIRE (slot.loadUnit ("dusk.builtin.utility", kSampleRate, kBlock, error));

    std::vector<float> l ((size_t) kBlock, 0.0f), r ((size_t) kBlock, 0.0f);
    runBlocks (slot, l, r, 8);

    for (int i = 0; i < kBlock; ++i)
    {
        REQUIRE (l[(size_t) i] == 0.0f);
        REQUIRE (r[(size_t) i] == 0.0f);
    }
}

TEST_CASE ("utility unit at its defaults is unity gain")
{
    NativeBuiltinSlot slot;
    std::string error;
    REQUIRE (slot.loadUnit ("dusk.builtin.utility", kSampleRate, kBlock, error));

    std::vector<float> l ((size_t) kBlock), r ((size_t) kBlock);
    auto fill = [&]
    {
        for (int i = 0; i < kBlock; ++i)
        {
            l[(size_t) i] = std::sin (0.05f * (float) i);
            r[(size_t) i] = std::cos (0.11f * (float) i) * 0.5f;
        }
    };

    // Warm the smoothers on throwaway blocks, then measure a fresh one.
    fill();
    runBlocks (slot, l, r, 8);

    fill();
    const auto expectedL = l;
    const auto expectedR = r;
    runBlocks (slot, l, r, 1);

    for (int i = 0; i < kBlock; ++i)
    {
        REQUIRE_THAT (l[(size_t) i], WithinAbs (expectedL[(size_t) i], 1e-6));
        REQUIRE_THAT (r[(size_t) i], WithinAbs (expectedR[(size_t) i], 1e-6));
    }
}

TEST_CASE ("utility unit applies gain, polarity, width and mono sum")
{
    NativeBuiltinSlot slot;
    std::string error;
    REQUIRE (slot.loadUnit ("dusk.builtin.utility", kSampleRate, kBlock, error));

    const int gainIdx  = paramIndex (slot, "gain_db");
    const int polIdx   = paramIndex (slot, "polarity");
    const int widthIdx = paramIndex (slot, "width");
    const int monoIdx  = paramIndex (slot, "mono");
    REQUIRE (gainIdx >= 0);
    REQUIRE (polIdx >= 0);
    REQUIRE (widthIdx >= 0);
    REQUIRE (monoIdx >= 0);

    std::vector<float> l ((size_t) kBlock), r ((size_t) kBlock);
    auto fillConstant = [&] (float left, float right)
    {
        std::fill (l.begin(), l.end(), left);
        std::fill (r.begin(), r.end(), right);
    };

    SECTION ("-6 dB halves the signal")
    {
        slot.setParamValue (gainIdx, -6.0206f);
        for (int b = 0; b < 40; ++b) { fillConstant (1.0f, 1.0f); runBlocks (slot, l, r, 1); }
        REQUIRE_THAT (l.back(), WithinAbs (0.5, 1e-4));
        REQUIRE_THAT (r.back(), WithinAbs (0.5, 1e-4));
    }

    SECTION ("polarity inverts both channels")
    {
        slot.setParamValue (polIdx, 1.0f);
        for (int b = 0; b < 40; ++b) { fillConstant (0.25f, -0.75f); runBlocks (slot, l, r, 1); }
        REQUIRE_THAT (l.back(), WithinAbs (-0.25, 1e-4));
        REQUIRE_THAT (r.back(), WithinAbs (0.75, 1e-4));
    }

    SECTION ("zero width collapses to the mid signal")
    {
        slot.setParamValue (widthIdx, 0.0f);
        for (int b = 0; b < 40; ++b) { fillConstant (1.0f, 0.0f); runBlocks (slot, l, r, 1); }
        REQUIRE_THAT (l.back(), WithinAbs (0.5, 1e-4));
        REQUIRE_THAT (r.back(), WithinAbs (0.5, 1e-4));
    }

    SECTION ("mono sum averages the two channels")
    {
        slot.setParamValue (monoIdx, 1.0f);
        for (int b = 0; b < 40; ++b) { fillConstant (0.8f, 0.2f); runBlocks (slot, l, r, 1); }
        REQUIRE_THAT (l.back(), WithinAbs (0.5, 1e-4));
        REQUIRE_THAT (r.back(), WithinAbs (0.5, 1e-4));
    }

    SECTION ("parameters clamp to their declared range")
    {
        slot.setParamValue (widthIdx, 9999.0f);
        REQUIRE_THAT (slot.getParamValue (widthIdx),
                      WithinAbs (slot.paramInfo (widthIdx)->maxValue, 1e-6));
        slot.setParamValue (gainIdx, -9999.0f);
        REQUIRE_THAT (slot.getParamValue (gainIdx),
                      WithinAbs (slot.paramInfo (gainIdx)->minValue, 1e-6));
    }
}

TEST_CASE ("built-in unit state round-trips through the slot")
{
    NativeBuiltinSlot saver;
    std::string error;
    REQUIRE (saver.loadUnit ("dusk.builtin.utility", kSampleRate, kBlock, error));

    const int gainIdx  = paramIndex (saver, "gain_db");
    const int widthIdx = paramIndex (saver, "width");
    saver.setParamValue (gainIdx, -3.5f);
    saver.setParamValue (widthIdx, 150.0f);

    std::vector<std::uint8_t> blob;
    REQUIRE (saver.saveState (blob));
    REQUIRE_FALSE (blob.empty());

    NativeBuiltinSlot loader;
    REQUIRE (loader.loadUnit ("dusk.builtin.utility", kSampleRate, kBlock, error));
    REQUIRE (loader.loadState (blob));
    REQUIRE_THAT (loader.getParamValue (gainIdx), WithinAbs (-3.5, 1e-6));
    REQUIRE_THAT (loader.getParamValue (widthIdx), WithinAbs (150.0, 1e-6));

    SECTION ("a blob from another unit is refused rather than partly applied")
    {
        std::string text (blob.begin(), blob.end());
        const auto at = text.find ("dusk.builtin.utility");
        REQUIRE (at != std::string::npos);
        text.replace (at, std::string ("dusk.builtin.utility").size(), "dusk.builtin.someth");
        std::vector<std::uint8_t> foreign (text.begin(), text.end());

        NativeBuiltinSlot other;
        REQUIRE (other.loadUnit ("dusk.builtin.utility", kSampleRate, kBlock, error));
        REQUIRE_FALSE (other.loadState (foreign));
        REQUIRE_THAT (other.getParamValue (gainIdx), WithinAbs (0.0, 1e-6));
    }

    SECTION ("garbage is refused")
    {
        NativeBuiltinSlot other;
        REQUIRE (other.loadUnit ("dusk.builtin.utility", kSampleRate, kBlock, error));
        REQUIRE_FALSE (other.loadState ({ 0x01, 0x02, 0x03 }));
    }
}

TEST_CASE ("a bypassed built-in slot passes dry audio and reports no latency")
{
    NativeBuiltinSlot slot;
    std::string error;
    REQUIRE (slot.loadUnit ("dusk.builtin.utility", kSampleRate, kBlock, error));
    slot.setParamValue (paramIndex (slot, "gain_db"), -60.0f);
    slot.setBypassed (true);

    std::vector<float> l ((size_t) kBlock, 0.4f), r ((size_t) kBlock, -0.2f);
    runBlocks (slot, l, r, 8);

    REQUIRE_THAT (l.back(), WithinAbs (0.4, 1e-6));
    REQUIRE_THAT (r.back(), WithinAbs (-0.2, 1e-6));
    REQUIRE (slot.getLatencySamples() == 0);
}
