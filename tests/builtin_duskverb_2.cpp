#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/builtin/NativeBuiltinSlot.h"
#include "foundation/Json.h"

#include <DuskVerbParamTable.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

using namespace duskstudio::builtin;
using Catch::Matchers::WithinAbs;

namespace
{
constexpr double kSampleRate = 48000.0;
constexpr int kBlock = 128;
constexpr const char* kUnitId = "dusk.builtin.reverb";

int paramIndex (const NativeBuiltinSlot& slot, const char* id)
{
    for (int i = 0; i < slot.paramCount(); ++i)
        if (std::string (slot.paramInfo (i)->id) == id) return i;
    FAIL ("no parameter with id " << id);
    return -1;
}

void set (NativeBuiltinSlot& slot, const char* id, float value)
{
    slot.setParamValue (paramIndex (slot, id), value);
}

float get (const NativeBuiltinSlot& slot, const char* id)
{
    return slot.getParamValue (paramIndex (slot, id));
}

void load (NativeBuiltinSlot& slot)
{
    std::string error;
    REQUIRE (slot.loadUnit (kUnitId, kSampleRate, kBlock, error));
    REQUIRE (error.empty());
}

void processSilence (NativeBuiltinSlot& slot)
{
    std::vector<float> l ((size_t) kBlock), r ((size_t) kBlock);
    slot.processStereo (l.data(), r.data(), l.data(), r.data(), kBlock);
}

dusk::json::Json parseBlob (const std::vector<std::uint8_t>& blob)
{
    return dusk::json::Json::parse (std::string (blob.begin(), blob.end()), nullptr,
                                    /*allow_exceptions*/ false);
}
} // namespace

TEST_CASE ("The reverb built-in is DuskVerb 2 under its existing id",
           "[builtin][DuskVerb][reverb]")
{
    const auto* unit = findUnit (kUnitId);
    REQUIRE (unit != nullptr);
    REQUIRE (std::string (unit->id) == kUnitId);
    REQUIRE (std::string (unit->name) == "DuskVerb 2");
    REQUIRE (std::string (unit->category) == "Fx|Reverb");
    REQUIRE_FALSE (unit->isInstrument);
    REQUIRE (unit->createPlugin != nullptr);

    NativeBuiltinSlot slot;
    load (slot);
    REQUIRE (slot.displayName() == "DuskVerb 2");
}

TEST_CASE ("DuskVerb 2 declares its 92 parameters with their declared defaults and ranges",
           "[builtin][DuskVerb][reverb]")
{
    NativeBuiltinSlot slot;
    load (slot);

    REQUIRE (duskverb::kNumParams == 92);
    REQUIRE (slot.paramCount() == duskverb::kNumParams);
    for (int i = 0; i < duskverb::kNumParams; ++i)
    {
        const auto& declared = duskverb::paramDesc (i);
        const auto* exposed = slot.paramInfo (i);
        const char* declaredSymbol = i == duskverb::Bypass ? "daf_bypass" : declared.id;
        INFO ("parameter " << i << " (" << declared.id << ")");
        REQUIRE (exposed != nullptr);
        REQUIRE (std::string (exposed->id) == declaredSymbol);
        REQUIRE_THAT (exposed->minValue, WithinAbs (duskverb::hostMin (declared), 1e-6));
        REQUIRE_THAT (exposed->maxValue, WithinAbs (duskverb::hostMax (declared), 1e-6));
        REQUIRE_THAT (exposed->defaultValue,
                      WithinAbs (duskverb::hostDefault (declared), 1e-6));
        REQUIRE_THAT (slot.getParamValue (i),
                      WithinAbs (duskverb::hostDefault (declared), 1e-6));
    }

    const auto& mix = duskverb::paramDesc (duskverb::Mix);
    REQUIRE_THAT (mix.def, WithinAbs (0.35, 1e-6));
    REQUIRE_THAT (get (slot, "mix"), WithinAbs (duskverb::hostDefault (mix), 1e-6));
}

TEST_CASE ("DuskVerb 2 is stereo in, stereo out, with zero latency",
           "[builtin][DuskVerb][reverb]")
{
    NativeBuiltinSlot slot;
    load (slot);

    const auto* instance = slot.getInstance();
    REQUIRE (instance != nullptr);
    const auto& layout = instance->portLayout();
    REQUIRE (layout.mainInIndex >= 0);
    REQUIRE (layout.mainOutIndex >= 0);
    REQUIRE (layout.inputs[(size_t) layout.mainInIndex].channelCount == 2);
    REQUIRE (layout.outputs[(size_t) layout.mainOutIndex].channelCount == 2);
    REQUIRE (slot.getLatencySamples() == 0);
}

TEST_CASE ("Silence in stays silence out", "[builtin][DuskVerb][reverb]")
{
    NativeBuiltinSlot slot;
    load (slot);

    std::vector<float> l ((size_t) kBlock), r ((size_t) kBlock);
    float peak = 0.0f;
    bool finite = true;
    for (int block = 0; block < 32; ++block)
    {
        std::fill (l.begin(), l.end(), 0.0f);
        std::fill (r.begin(), r.end(), 0.0f);
        slot.processStereo (l.data(), r.data(), l.data(), r.data(), kBlock);
        for (int i = 0; i < kBlock; ++i)
        {
            finite = finite && std::isfinite (l[(size_t) i]) && std::isfinite (r[(size_t) i]);
            peak = std::max ({ peak, std::abs (l[(size_t) i]), std::abs (r[(size_t) i]) });
        }
    }

    REQUIRE (finite);
    REQUIRE_THAT (peak, WithinAbs (0.0, 1e-12));
}

TEST_CASE ("An impulse produces a finite, decaying stereo tail",
           "[builtin][DuskVerb][reverb]")
{
    NativeBuiltinSlot slot;
    load (slot);
    set (slot, "mix", 1.0f);

    constexpr int totalFrames = (int) (1.5 * kSampleRate);
    std::vector<float> l ((size_t) kBlock), r ((size_t) kBlock);
    double earlyEnergyL = 0.0, earlyEnergyR = 0.0;
    double lateEnergyL = 0.0, lateEnergyR = 0.0;
    double stereoDifferenceEnergy = 0.0;
    bool finite = true;

    for (int start = 0; start < totalFrames; start += kBlock)
    {
        const int frames = std::min (kBlock, totalFrames - start);
        std::fill (l.begin(), l.end(), 0.0f);
        std::fill (r.begin(), r.end(), 0.0f);
        if (start == 0) l[0] = 0.5f;
        slot.processStereo (l.data(), r.data(), l.data(), r.data(), frames);

        for (int i = 0; i < frames; ++i)
        {
            const int frame = start + i;
            const double left = l[(size_t) i];
            const double right = r[(size_t) i];
            finite = finite && std::isfinite (left) && std::isfinite (right);
            if (frame >= (int) (0.05 * kSampleRate) && frame < (int) (0.35 * kSampleRate))
            {
                earlyEnergyL += left * left;
                earlyEnergyR += right * right;
                const double difference = left - right;
                stereoDifferenceEnergy += difference * difference;
            }
            if (frame >= (int) (1.15 * kSampleRate) && frame < (int) (1.45 * kSampleRate))
            {
                lateEnergyL += left * left;
                lateEnergyR += right * right;
            }
        }
    }

    REQUIRE (finite);
    REQUIRE (earlyEnergyL > 1.0e-8);
    REQUIRE (earlyEnergyR > 1.0e-8);
    REQUIRE (stereoDifferenceEnergy > 1.0e-8);
    REQUIRE (lateEnergyL < earlyEnergyL);
    REQUIRE (lateEnergyR < earlyEnergyR);
}

TEST_CASE ("Full state round-trips through a saved session blob",
           "[builtin][DuskVerb][reverb][session]")
{
    NativeBuiltinSlot saver;
    load (saver);
    set (saver, "algorithm", 5.0f);
    set (saver, "mix", 0.68f);
    set (saver, "width", 1.35f);
    processSilence (saver);

    std::vector<std::uint8_t> savedBlob;
    REQUIRE (saver.saveState (savedBlob));
    const auto saved = parseBlob (savedBlob);
    REQUIRE (saved.is_object());
    REQUIRE (dusk::json::getInt (saved, "version", 0) == 3);
    const auto& savedState = dusk::json::child (saved, "state");
    REQUIRE (savedState.is_object());
    const auto savedParameters = dusk::json::getString (savedState, "parameters");
    REQUIRE_FALSE (savedParameters.empty());
    REQUIRE (savedParameters.find ("mix=") != std::string::npos);

    NativeBuiltinSlot loader;
    load (loader);
    REQUIRE (loader.loadState (savedBlob));
    for (int i = 0; i < saver.paramCount(); ++i)
    {
        INFO ("parameter " << saver.paramInfo (i)->id);
        REQUIRE_THAT (loader.getParamValue (i), WithinAbs (saver.getParamValue (i), 1e-6));
    }

    std::vector<std::uint8_t> restoredBlob;
    REQUIRE (loader.saveState (restoredBlob));
    const auto restored = parseBlob (restoredBlob);
    REQUIRE (dusk::json::getString (dusk::json::child (restored, "state"), "parameters")
             == savedParameters);
}

TEST_CASE ("A session saved with the old Reverb unit loads as DuskVerb 2 defaults",
           "[builtin][DuskVerb][reverb][session]")
{
    const std::string oldReverbBlob =
        R"({"id":"dusk.builtin.reverb","params":{"algorithm":5.0,"damping":0.7,)"
        R"("decay":7.5,"hi_cut":12000.0,"lo_cut":20.0,"mix":0.42,)"
        R"("predelay":20.0,"size":0.5,"width":1.0},"version":1})";

    NativeBuiltinSlot slot;
    load (slot);
    set (slot, "algorithm", 2.0f);
    set (slot, "mix", 0.91f);
    REQUIRE (slot.loadState ({ oldReverbBlob.begin(), oldReverbBlob.end() }));

    for (int i = 0; i < slot.paramCount(); ++i)
    {
        const auto* info = slot.paramInfo (i);
        INFO ("parameter " << info->id);
        REQUIRE_THAT (slot.getParamValue (i), WithinAbs (info->defaultValue, 1e-6));
    }

    std::vector<std::uint8_t> migratedBlob;
    REQUIRE (slot.saveState (migratedBlob));
    const auto migrated = parseBlob (migratedBlob);
    REQUIRE (dusk::json::getInt (migrated, "version", 0) == 3);
    REQUIRE_FALSE (dusk::json::getString (
        dusk::json::child (migrated, "state"), "parameters").empty());
}
