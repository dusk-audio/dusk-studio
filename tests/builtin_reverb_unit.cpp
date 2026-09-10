#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/builtin/NativeBuiltinSlot.h"
#include "foundation/Base64.h"
#include "session/Session.h"
#include "session/SessionSerializer.h"

#include <juce_core/juce_core.h>

#include <cmath>
#include <memory>
#include <string>
#include <vector>

// The Reverb unit wraps the donor DuskVerb engine, which renders fully wet, so
// the dry/wet blend is the wrapper's own. These pin the contract the mixer
// relies on: transparent at its defaults, silent on silence, and a state blob
// that survives the trip through the session.

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

// The slot owns atomics and unique_ptrs, so it is neither copyable nor
// movable: each case declares its own and loads it in place.
void loadReverb (NativeBuiltinSlot& slot)
{
    std::string error;
    REQUIRE (slot.loadUnit ("dusk.builtin.reverb", kSampleRate, kBlock, error));
    REQUIRE (error.empty());
}
} // namespace

TEST_CASE ("reverb unit is registered and loads", "[builtin][reverb]")
{
    const auto* unit = findUnit ("dusk.builtin.reverb");
    REQUIRE (unit != nullptr);
    REQUIRE_FALSE (unit->isInstrument);

    NativeBuiltinSlot slot;
    loadReverb (slot);
    REQUIRE (slot.displayName() == "Reverb");
    // The engine is causal and reports no lookahead, so nothing to compensate.
    REQUIRE (slot.getLatencySamples() == 0);
}

TEST_CASE ("reverb unit turns silence into silence", "[builtin][reverb]")
{
    NativeBuiltinSlot slot;
    loadReverb (slot);
    slot.setParamValue (paramIndex (slot, "mix"), 1.0f);

    std::vector<float> l ((size_t) kBlock, 0.0f), r ((size_t) kBlock, 0.0f);
    // Long enough for the wet path to have settled at full mix.
    for (int b = 0; b < 64; ++b)
    {
        std::fill (l.begin(), l.end(), 0.0f);
        std::fill (r.begin(), r.end(), 0.0f);
        slot.processStereo (l.data(), r.data(), l.data(), r.data(), kBlock);
    }

    for (int i = 0; i < kBlock; ++i)
    {
        REQUIRE_THAT (l[(size_t) i], WithinAbs (0.0, 1e-9));
        REQUIRE_THAT (r[(size_t) i], WithinAbs (0.0, 1e-9));
    }
}

TEST_CASE ("reverb unit at its defaults is unity gain", "[builtin][reverb]")
{
    NativeBuiltinSlot slot;
    loadReverb (slot);
    REQUIRE_THAT (slot.getParamValue (paramIndex (slot, "mix")), WithinAbs (0.0, 1e-9));

    std::vector<float> l ((size_t) kBlock), r ((size_t) kBlock);
    auto fill = [&]
    {
        for (int i = 0; i < kBlock; ++i)
        {
            l[(size_t) i] = std::sin (0.031f * (float) i);
            r[(size_t) i] = std::cos (0.017f * (float) i) * 0.6f;
        }
    };

    // Warm the tank and the mix smoother, then measure a fresh block.
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

TEST_CASE ("reverb unit produces a tail past the input", "[builtin][reverb]")
{
    NativeBuiltinSlot slot;
    loadReverb (slot);
    slot.setParamValue (paramIndex (slot, "mix"), 1.0f);
    slot.setParamValue (paramIndex (slot, "decay"), 4.0f);

    std::vector<float> l ((size_t) kBlock), r ((size_t) kBlock);

    // One loud block in, then silence: the wet path must keep ringing.
    std::fill (l.begin(), l.end(), 0.5f);
    std::fill (r.begin(), r.end(), 0.5f);
    slot.processStereo (l.data(), r.data(), l.data(), r.data(), kBlock);

    float tailPeak = 0.0f;
    for (int b = 0; b < 16; ++b)
    {
        std::fill (l.begin(), l.end(), 0.0f);
        std::fill (r.begin(), r.end(), 0.0f);
        slot.processStereo (l.data(), r.data(), l.data(), r.data(), kBlock);
        for (int i = 0; i < kBlock; ++i)
            tailPeak = std::max (tailPeak, std::abs (l[(size_t) i]));
    }
    REQUIRE (tailPeak > 1.0e-4f);
}

TEST_CASE ("reverb unit state round-trips", "[builtin][reverb]")
{
    NativeBuiltinSlot saver;
    loadReverb (saver);
    const int mixIdx   = paramIndex (saver, "mix");
    const int decayIdx = paramIndex (saver, "decay");
    const int algoIdx  = paramIndex (saver, "algorithm");
    saver.setParamValue (mixIdx, 0.42f);
    saver.setParamValue (decayIdx, 7.5f);
    saver.setParamValue (algoIdx, 5.0f);

    std::vector<std::uint8_t> blob;
    REQUIRE (saver.saveState (blob));
    REQUIRE_FALSE (blob.empty());

    NativeBuiltinSlot loader;
    loadReverb (loader);
    REQUIRE (loader.loadState (blob));
    REQUIRE_THAT (loader.getParamValue (mixIdx), WithinAbs (0.42, 1e-6));
    REQUIRE_THAT (loader.getParamValue (decayIdx), WithinAbs (7.5, 1e-6));
    REQUIRE_THAT (loader.getParamValue (algoIdx), WithinAbs (5.0, 1e-6));

    // A Utility blob must not be accepted by the reverb, and vice versa.
    NativeBuiltinSlot utility;
    std::string error;
    REQUIRE (utility.loadUnit ("dusk.builtin.utility", kSampleRate, kBlock, error));
    std::vector<std::uint8_t> utilityBlob;
    REQUIRE (utility.saveState (utilityBlob));
    REQUIRE_FALSE (loader.loadState (utilityBlob));
    REQUIRE_FALSE (utility.loadState (blob));
}

TEST_CASE ("a reverb insert survives a session save and reload", "[builtin][reverb][session]")
{
    using duskstudio::Session;
    using duskstudio::SessionSerializer;

    NativeBuiltinSlot saver;
    loadReverb (saver);
    const int decayIdx = paramIndex (saver, "decay");
    const int mixIdx   = paramIndex (saver, "mix");
    saver.setParamValue (decayIdx, 11.25f);
    saver.setParamValue (mixIdx, 0.6f);

    std::vector<std::uint8_t> blob;
    REQUIRE (saver.saveState (blob));

    auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                  .getChildFile ("dusk-studio-builtin-reverb-"
                                 + juce::String (juce::Random::getSystemRandom().nextInt()));
    dir.createDirectory();
    const auto target = dir.getChildFile ("session.json");

    auto savedSession = std::make_unique<Session>();
    savedSession->track (2).builtinUnitId = "dusk.builtin.reverb";
    savedSession->track (2).builtinStateBase64 =
        dusk::base64::encode (blob.data(), blob.size());
    REQUIRE (SessionSerializer::save (*savedSession, target));

    auto loadedSession = std::make_unique<Session>();
    REQUIRE (SessionSerializer::load (*loadedSession, target));
    REQUIRE (loadedSession->track (2).builtinUnitId == "dusk.builtin.reverb");

    // What the engine's restore path does with the reloaded pair.
    const auto restored = dusk::base64::decode (
        loadedSession->track (2).builtinStateBase64.data(),
        loadedSession->track (2).builtinStateBase64.size());
    NativeBuiltinSlot loader;
    loadReverb (loader);
    REQUIRE (loader.loadState (restored));
    REQUIRE_THAT (loader.getParamValue (decayIdx), WithinAbs (11.25, 1e-6));
    REQUIRE_THAT (loader.getParamValue (mixIdx), WithinAbs (0.6, 1e-6));

    dir.deleteRecursively();
}
