#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/builtin/NativeBuiltinSlot.h"
#include "foundation/TransportPosition.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

// Tape Echo 2, Dusk's own DAF plug-in, as the built-in tape echo: its DSP runs in
// process behind a DafUnitInstance. These pin what a session relies on: the
// parameter surface at the plug-in's indices and symbols, silence in to nothing
// above its noise floor out, an echo landing at the motor time the plug-in states,
// tempo sync to the transport the slot hands it, bypass, latency, and the
// version-2 session blob.

using namespace duskstudio::builtin;
using Catch::Matchers::WithinAbs;

namespace
{
constexpr double kSampleRate = 48000.0;
constexpr int    kBlock      = 256;
constexpr const char* kUnitId = "dusk.builtin.delay";

// Head 1's motor time at the fastest repeat rate (TapeEchoDSP::kMinDelayMs).
constexpr double kFastestHead1Ms = 69.33;

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

// One head, no feedback, no spring, no wow, new tape and wet only: what comes
// back is a single repeat of the input.
void singleCleanRepeat (NativeBuiltinSlot& slot)
{
    set (slot, "mode", 1.0f);
    set (slot, "intensity", 0.0f);
    set (slot, "echo_volume", 1.0f);
    set (slot, "reverb_volume", 0.0f);
    set (slot, "wow_flutter", 0.0f);
    set (slot, "tape_age", 0.0f);
    set (slot, "mix", 1.0f);
}

// Silence long enough for the motor and every smoother to reach their targets.
void settle (NativeBuiltinSlot& slot, const dusk::TransportPosition* transport = nullptr)
{
    std::vector<float> l ((size_t) kBlock), r ((size_t) kBlock);
    for (int b = 0; b < (int) (2.0 * kSampleRate) / kBlock; ++b)
    {
        std::fill (l.begin(), l.end(), 0.0f);
        std::fill (r.begin(), r.end(), 0.0f);
        slot.processStereo (l.data(), r.data(), l.data(), r.data(), kBlock, nullptr, transport);
    }
}

// Plays a 10 ms Hann-windowed 1 kHz burst and returns how long after the burst
// the energy between fromMs and toMs arrives, energy centroid to energy centroid.
double echoArrivalMs (NativeBuiltinSlot& slot, double fromMs, double toMs,
                      const dusk::TransportPosition* transport = nullptr)
{
    const int burst = (int) (0.010 * kSampleRate);
    const int total = (int) (toMs * 0.001 * kSampleRate) + kBlock;
    std::vector<float> input ((size_t) total, 0.0f), output ((size_t) total, 0.0f);
    for (int i = 0; i < burst; ++i)
    {
        const double window = 0.5 - 0.5 * std::cos (2.0 * M_PI * i / (burst - 1));
        input[(size_t) i] = (float) (0.5 * window * std::sin (2.0 * M_PI * 1000.0 * i / kSampleRate));
    }

    std::vector<float> l ((size_t) kBlock), r ((size_t) kBlock);
    for (int start = 0; start < total; start += kBlock)
    {
        const int n = std::min (kBlock, total - start);
        std::copy (input.begin() + start, input.begin() + start + n, l.begin());
        std::copy (input.begin() + start, input.begin() + start + n, r.begin());
        slot.processStereo (l.data(), r.data(), l.data(), r.data(), n, nullptr, transport);
        std::copy (l.begin(), l.begin() + n, output.begin() + start);
    }

    auto centroid = [] (const std::vector<float>& x, int from, int to)
    {
        double weighted = 0.0, energy = 0.0;
        for (int i = from; i < to; ++i)
        {
            const double e = (double) x[(size_t) i] * (double) x[(size_t) i];
            weighted += e * i;
            energy += e;
        }
        REQUIRE (energy > 1.0e-9);
        return weighted / energy;
    };

    const int from = (int) (fromMs * 0.001 * kSampleRate);
    const int to   = (int) (toMs * 0.001 * kSampleRate);
    return (centroid (output, from, to) - centroid (input, 0, burst)) * 1000.0 / kSampleRate;
}
} // namespace

TEST_CASE ("Tape Echo 2 is the built-in tape echo", "[builtin][tape-echo-2]")
{
    const auto* unit = findUnit (kUnitId);
    REQUIRE (unit != nullptr);
    REQUIRE (std::string (unit->name) == "Tape Echo 2");
    REQUIRE_FALSE (unit->isInstrument);
    REQUIRE (unit->createPlugin != nullptr);

    NativeBuiltinSlot slot;
    load (slot);
    REQUIRE (slot.displayName() == "Tape Echo 2");
    REQUIRE (slot.getLatencySamples() == 0);
}

TEST_CASE ("Tape Echo 2's parameters sit at the plug-in's indices and symbols",
           "[builtin][tape-echo-2]")
{
    NativeBuiltinSlot slot;
    load (slot);

    // The plug-in's own table, in its order: indices are what a MIDI binding saves.
    const char* const symbols[] = {
        "mode", "repeat_rate", "intensity", "echo_volume", "reverb_volume", "bass",
        "treble", "input_volume", "wow_flutter", "dry_level", "tempo_sync",
        "sync_division", "tape_age", "daf_bypass", "out_level", "output_volume",
        "echo_pan", "reverb_pan", "input_send", "peak_level", "mix", "echo_rate_note",
    };
    const int count = (int) (sizeof (symbols) / sizeof (symbols[0]));
    REQUIRE (slot.paramCount() == count);
    for (int i = 0; i < count; ++i)
    {
        INFO ("parameter " << i);
        REQUIRE (std::string (slot.paramInfo (i)->id) == symbols[i]);
    }

    // The meters and the parameters kept only for old sessions are hidden.
    for (const char* id : { "dry_level", "sync_division", "out_level", "peak_level" })
        REQUIRE (slot.paramInfo (paramIndex (slot, id))->hidden);
    for (const char* id : { "mode", "mix", "echo_rate_note", "daf_bypass", "tape_age" })
        REQUIRE_FALSE (slot.paramInfo (paramIndex (slot, id))->hidden);

    const auto* mode = slot.paramInfo (paramIndex (slot, "mode"));
    REQUIRE (mode->kind == ParamKind::Choice);
    REQUIRE (mode->choiceCount == 12);
    REQUIRE (slot.paramInfo (paramIndex (slot, "tempo_sync"))->kind == ParamKind::Toggle);
    REQUIRE (slot.paramInfo (paramIndex (slot, "daf_bypass"))->kind == ParamKind::Toggle);

    REQUIRE_THAT (get (slot, "echo_volume"), WithinAbs (0.5, 1e-6));
    REQUIRE_THAT (get (slot, "mix"), WithinAbs (0.5, 1e-6));
    REQUIRE_THAT (get (slot, "echo_rate_note"), WithinAbs (5.0, 1e-6));
}

TEST_CASE ("Tape Echo 2 turns silence into its tape's noise floor", "[builtin][tape-echo-2]")
{
    NativeBuiltinSlot slot;
    load (slot);
    set (slot, "echo_volume", 1.0f);
    set (slot, "reverb_volume", 1.0f);
    set (slot, "intensity", 0.6f);
    set (slot, "mode", 11.0f);
    set (slot, "tape_age", 0.0f);

    std::vector<float> l ((size_t) kBlock), r ((size_t) kBlock);
    float peak = 0.0f;
    for (int b = 0; b < 200; ++b)
    {
        std::fill (l.begin(), l.end(), 0.0f);
        std::fill (r.begin(), r.end(), 0.0f);
        slot.processStereo (l.data(), r.data(), l.data(), r.data(), kBlock);
        for (int i = 0; i < kBlock; ++i)
            peak = std::max ({ peak, std::abs (l[(size_t) i]), std::abs (r[(size_t) i]) });
    }
    // The plug-in models the transport's hum and hiss, about -113 dBFS with new
    // tape, so silence comes back as that floor and nothing louder.
    REQUIRE (peak < 1.0e-5f);
}

TEST_CASE ("Tape Echo 2 repeats at the motor time it states", "[builtin][tape-echo-2]")
{
    NativeBuiltinSlot slot;
    load (slot);
    singleCleanRepeat (slot);
    set (slot, "repeat_rate", 1.0f);
    settle (slot);

    // No hidden latency either: the repeat lands at the motor time itself.
    REQUIRE_THAT (echoArrivalMs (slot, 30.0, 120.0), WithinAbs (kFastestHead1Ms, 1.5));
}

TEST_CASE ("Tape Echo 2 syncs its repeat to the transport the slot hands it",
           "[builtin][tape-echo-2]")
{
    NativeBuiltinSlot slot;
    load (slot);
    singleCleanRepeat (slot);
    set (slot, "tempo_sync", 1.0f);
    // Detent 5 on head 1 is a sixteenth note.
    set (slot, "echo_rate_note", 5.0f);

    SECTION ("with no transport it holds 120 BPM")
    {
        settle (slot);
        REQUIRE_THAT (echoArrivalMs (slot, 40.0, 220.0), WithinAbs (125.0, 2.0));
    }

    SECTION ("with a transport it follows the tempo")
    {
        dusk::TransportPosition transport;
        transport.bpm = 100.0;
        settle (slot, &transport);
        REQUIRE_THAT (echoArrivalMs (slot, 40.0, 220.0, &transport), WithinAbs (150.0, 2.0));

        transport.bpm = 160.0;
        settle (slot, &transport);
        REQUIRE_THAT (echoArrivalMs (slot, 40.0, 220.0, &transport), WithinAbs (93.75, 2.0));
    }
}

TEST_CASE ("Tape Echo 2's bypass passes the input through untouched", "[builtin][tape-echo-2]")
{
    NativeBuiltinSlot slot;
    load (slot);
    set (slot, "echo_volume", 1.0f);
    set (slot, "intensity", 0.5f);

    std::vector<float> l ((size_t) kBlock), r ((size_t) kBlock);
    long n = 0;
    auto tone = [&]
    {
        for (int i = 0; i < kBlock; ++i, ++n)
        {
            l[(size_t) i] = 0.6f * (float) std::sin (0.031 * (double) n);
            r[(size_t) i] = 0.4f * (float) std::cos (0.017 * (double) n);
        }
    };

    auto expectPassthrough = [&]
    {
        tone();
        const auto inL = l;
        const auto inR = r;
        slot.processStereo (l.data(), r.data(), l.data(), r.data(), kBlock);
        for (int i = 0; i < kBlock; ++i)
        {
            REQUIRE_THAT (l[(size_t) i], WithinAbs (inL[(size_t) i], 1e-6));
            REQUIRE_THAT (r[(size_t) i], WithinAbs (inR[(size_t) i], 1e-6));
        }
    };

    SECTION ("the plug-in's own bypass")
    {
        set (slot, "daf_bypass", 1.0f);
        // The plug-in fades its power down rather than cutting it, so give the
        // fade a second to reach nothing.
        for (int b = 0; b < (int) kSampleRate / kBlock; ++b)
        {
            tone();
            slot.processStereo (l.data(), r.data(), l.data(), r.data(), kBlock);
        }
        expectPassthrough();
    }

    SECTION ("the slot's bypass, which also reports no latency")
    {
        slot.setBypassed (true);
        expectPassthrough();
        REQUIRE (slot.getLatencySamples() == 0);
    }
}

TEST_CASE ("Tape Echo 2 saves version-2 state keyed by the plug-in's symbols",
           "[builtin][tape-echo-2]")
{
    NativeBuiltinSlot saver;
    load (saver);
    set (saver, "mode", 7.0f);
    set (saver, "intensity", 0.72f);
    set (saver, "echo_volume", 0.65f);
    set (saver, "tempo_sync", 1.0f);
    set (saver, "echo_rate_note", 9.0f);
    set (saver, "tape_age", 0.8f);   // the plug-in's New / Used / Old detents
    REQUIRE_THAT (get (saver, "tape_age"), WithinAbs (1.0, 1e-6));

    std::vector<std::uint8_t> blob;
    REQUIRE (saver.saveState (blob));
    const std::string text (blob.begin(), blob.end());
    REQUIRE (text.find ("\"version\":2") != std::string::npos);
    REQUIRE (text.find ("\"echo_volume\"") != std::string::npos);
    REQUIRE (text.find ("\"out_level\"") == std::string::npos);

    NativeBuiltinSlot loader;
    load (loader);
    REQUIRE (loader.loadState (blob));
    for (int i = 0; i < saver.paramCount(); ++i)
    {
        INFO ("parameter " << saver.paramInfo (i)->id);
        if (std::string (saver.paramInfo (i)->id) == "out_level"
            || std::string (saver.paramInfo (i)->id) == "peak_level")
            continue;
        REQUIRE_THAT (loader.getParamValue (i), WithinAbs (saver.getParamValue (i), 1e-6));
    }
}

TEST_CASE ("Tape Echo 2 opens a version-1 tape echo blob at its defaults",
           "[builtin][tape-echo-2]")
{
    NativeBuiltinSlot slot;
    load (slot);
    set (slot, "mode", 4.0f);
    set (slot, "mix", 0.9f);

    const std::string v1 = R"({"id":"dusk.builtin.delay","version":1,)"
                           R"("params":{"echo":0.65,"dry":0.0,"mode":7,"intensity":0.72}})";
    REQUIRE (slot.loadState ({ v1.begin(), v1.end() }));
    for (int i = 0; i < slot.paramCount(); ++i)
    {
        const auto* info = slot.paramInfo (i);
        INFO ("parameter " << info->id);
        if (std::string (info->id) == "out_level" || std::string (info->id) == "peak_level")
            continue;
        REQUIRE_THAT (slot.getParamValue (i), WithinAbs (info->defaultValue, 1e-6));
    }

    NativeBuiltinSlot reverb;
    std::string error;
    REQUIRE (reverb.loadUnit ("dusk.builtin.reverb", kSampleRate, kBlock, error));
    std::vector<std::uint8_t> reverbBlob;
    REQUIRE (reverb.saveState (reverbBlob));
    REQUIRE_FALSE (slot.loadState (reverbBlob));
}
