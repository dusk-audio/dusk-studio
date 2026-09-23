#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/builtin/NativeBuiltinSlot.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

// The built-in tape unit is Tape Machine 2, Dusk's own DAF plug-in, run in
// process: the same plug-in the master tape runs. A tape is not transparent by
// design, so the unity assertion goes through the plug-in's own Thru path, which
// is the setting that means "off the tape". The unit took over the id of the
// knob tape unit it replaced, so a session saved with that unit restores into it.

using namespace duskstudio::builtin;
using Catch::Matchers::WithinAbs;

namespace
{
constexpr double kSampleRate = 48000.0;
constexpr int    kBlock      = 256;
constexpr int    kThruPath   = 3;
constexpr double kPi         = 3.14159265358979323846;   // M_PI is non-standard
constexpr const char* kUnitId = "dusk.builtin.tape";

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

void loadTape (NativeBuiltinSlot& slot)
{
    std::string error;
    REQUIRE (slot.loadUnit (kUnitId, kSampleRate, kBlock, error));
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

void runSilence (NativeBuiltinSlot& slot, int blocks)
{
    std::vector<float> l ((size_t) kBlock), r ((size_t) kBlock);
    for (int b = 0; b < blocks; ++b)
    {
        std::fill (l.begin(), l.end(), 0.0f);
        std::fill (r.begin(), r.end(), 0.0f);
        slot.processStereo (l.data(), r.data(), l.data(), r.data(), kBlock);
    }
}

std::vector<std::uint8_t> bytes (const std::string& text)
{
    return { text.begin(), text.end() };
}
} // namespace

TEST_CASE ("tape unit is registered and loads", "[builtin][tape]")
{
    const auto* unit = findUnit (kUnitId);
    REQUIRE (unit != nullptr);
    REQUIRE_FALSE (unit->isInstrument);
    REQUIRE (unit->createPlugin != nullptr);

    NativeBuiltinSlot slot;
    loadTape (slot);
    REQUIRE (slot.displayName() == "Tape Machine 2");
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
    set (slot, "signalPath", (float) kThruPath);

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
    runSilence (slot, 1);
    REQUIRE (slot.getLatencySamples() > 0);

    // Thru is a sample-exact passthrough that never enters those filters, so
    // reporting the same figure would have PDC shift every other track to match
    // a delay this insert is not adding. The plug-in restates its latency after
    // the block that carries the change.
    set (slot, "signalPath", (float) kThruPath);
    runSilence (slot, 1);
    REQUIRE (slot.getLatencySamples() == 0);

    slot.setBypassed (true);
    REQUIRE (slot.getLatencySamples() == 0);
}

TEST_CASE ("tape unit colours the signal on its normal path", "[builtin][tape]")
{
    NativeBuiltinSlot slot;
    loadTape (slot);
    set (slot, "inputGain", 8.0f);

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
    set (saver, "inputGain", 4.5f);
    set (saver, "tapeSpeed", 3.0f);
    set (saver, "bias", 62.0f);
    set (saver, "reproHF", -2.5f);

    std::vector<std::uint8_t> blob;
    REQUIRE (saver.saveState (blob));

    NativeBuiltinSlot loader;
    loadTape (loader);
    REQUIRE (loader.loadState (blob));
    REQUIRE_THAT (get (loader, "inputGain"), WithinAbs (4.5, 1e-6));
    REQUIRE_THAT (get (loader, "tapeSpeed"), WithinAbs (3.0, 1e-6));
    REQUIRE_THAT (get (loader, "bias"),      WithinAbs (62.0, 1e-6));
    REQUIRE_THAT (get (loader, "reproHF"),   WithinAbs (-2.5, 1e-6));

    NativeBuiltinSlot delay;
    std::string error;
    REQUIRE (delay.loadUnit ("dusk.builtin.delay", kSampleRate, kBlock, error));
    REQUIRE_FALSE (delay.loadState (blob));
}

TEST_CASE ("tape unit restores a session saved with the knob tape unit", "[builtin][tape]")
{
    // The knob unit's blob: its own control ids, version 1.
    const auto legacy = bytes (
        R"({"id":"dusk.builtin.tape","version":1,"params":{)"
        R"("machine":1,"speed":2,"type":3,"signal_path":1,"eq_standard":1,)"
        R"("input":4.5,"bias":62,"calibration":2,"output":-3,"hpf":80,"lpf":12000,)"
        R"("wow":0,"flutter":1.5,"noise":12,"auto_cal":0,"auto_comp":0}})");

    NativeBuiltinSlot slot;
    loadTape (slot);
    set (slot, "headWidth", 0.0f);
    REQUIRE (slot.loadState (legacy));

    REQUIRE_THAT (get (slot, "tapeMachine"),   WithinAbs (1.0, 1e-6));
    REQUIRE_THAT (get (slot, "tapeSpeed"),     WithinAbs (2.0, 1e-6));
    REQUIRE_THAT (get (slot, "tapeType"),      WithinAbs (3.0, 1e-6));
    REQUIRE_THAT (get (slot, "signalPath"),    WithinAbs (1.0, 1e-6));
    REQUIRE_THAT (get (slot, "eqStandard"),    WithinAbs (1.0, 1e-6));
    REQUIRE_THAT (get (slot, "inputGain"),     WithinAbs (4.5, 1e-6));
    REQUIRE_THAT (get (slot, "bias"),          WithinAbs (62.0, 1e-6));
    REQUIRE_THAT (get (slot, "calibration"),   WithinAbs (2.0, 1e-6));
    REQUIRE_THAT (get (slot, "outputGain"),    WithinAbs (-3.0, 1e-6));
    REQUIRE_THAT (get (slot, "highpassFreq"),  WithinAbs (80.0, 1e-4));
    REQUIRE_THAT (get (slot, "lowpassFreq"),   WithinAbs (12000.0, 1e-3));
    REQUIRE_THAT (get (slot, "wowAmount"),     WithinAbs (0.0, 1e-6));
    REQUIRE_THAT (get (slot, "flutterAmount"), WithinAbs (1.5, 1e-6));
    REQUIRE_THAT (get (slot, "noiseAmount"),   WithinAbs (12.0, 1e-6));
    REQUIRE_THAT (get (slot, "autoCal"),       WithinAbs (0.0, 1e-6));
    REQUIRE_THAT (get (slot, "autoComp"),      WithinAbs (0.0, 1e-6));

    // A control the knob unit never had restores as the plug-in's default.
    REQUIRE_THAT (get (slot, "headWidth"), WithinAbs (1.0, 1e-6));
}
