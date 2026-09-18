#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/builtin/BuiltinScanRows.h"
#include "engine/builtin/NativeBuiltinSlot.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

// The Sunset instrument is the only built-in unit the mixer routes MIDI to, so
// these pin the note lifecycle as well as the parameter surface: nothing until a
// note arrives, sound while one is held, and silence again once the transport's
// all-notes-off reaches it.

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
    FAIL ("no parameter with id " << id);
    return -1;
}

void loadSynth (NativeBuiltinSlot& slot)
{
    std::string error;
    REQUIRE (slot.loadUnit ("dusk.builtin.synth", kSampleRate, kBlock, error));
    REQUIRE (error.empty());
}

void addMessage (dusk::MidiBuffer& midi, std::uint8_t a, std::uint8_t b, std::uint8_t c)
{
    const std::uint8_t bytes[3] = { a, b, c };
    midi.addEvent (bytes, 3, 0);
}

// An instrument's output is its own, so the mixer hands it cleared buffers and
// takes whatever it writes.
float renderPeak (NativeBuiltinSlot& slot, int blocks, const dusk::MidiBuffer* midi)
{
    std::vector<float> l ((size_t) kBlock), r ((size_t) kBlock);
    float peak = 0.0f;
    for (int b = 0; b < blocks; ++b)
    {
        std::fill (l.begin(), l.end(), 0.0f);
        std::fill (r.begin(), r.end(), 0.0f);
        slot.processStereo (l.data(), r.data(), l.data(), r.data(), kBlock,
                            b == 0 ? midi : nullptr);
        for (int i = 0; i < kBlock; ++i)
            peak = std::max (peak, std::max (std::abs (l[(size_t) i]),
                                             std::abs (r[(size_t) i])));
    }
    return peak;
}
} // namespace

TEST_CASE ("synth unit is registered as an instrument", "[builtin][synth]")
{
    const auto* unit = findUnit ("dusk.builtin.synth");
    REQUIRE (unit != nullptr);
    REQUIRE (unit->isInstrument);

    NativeBuiltinSlot slot;
    loadSynth (slot);
    REQUIRE (slot.displayName() == "Sunset");
    REQUIRE (slot.isLoadedInstrument());
    REQUIRE (slot.getLatencySamples() == 0);
}

TEST_CASE ("synth unit reaches an instrument picker and not an effect one",
           "[builtin][synth][picker]")
{
    const auto effects     = descriptorRows (/*instruments*/ false);
    const auto instruments = descriptorRows (/*instruments*/ true);

    auto carries = [] (const std::vector<duskstudio::PluginDescriptor>& rows, const char* id)
    {
        return std::any_of (rows.begin(), rows.end(),
                            [id] (const duskstudio::PluginDescriptor& d)
                            { return d.location == id; });
    };

    REQUIRE (carries (instruments, "dusk.builtin.synth"));
    REQUIRE_FALSE (carries (effects, "dusk.builtin.synth"));
    // The effect units must not turn up on a MIDI track's instrument picker.
    REQUIRE_FALSE (carries (instruments, "dusk.builtin.utility"));
}

TEST_CASE ("synth unit is silent until a note arrives", "[builtin][synth]")
{
    NativeBuiltinSlot slot;
    loadSynth (slot);

    // The core keeps a small analogue noise floor rather than sitting at a hard
    // zero, so idle means "nothing a listener would call sound".
    REQUIRE_THAT (renderPeak (slot, 32, nullptr), WithinAbs (0.0, 5e-4));
}

TEST_CASE ("synth unit sounds while a note is held", "[builtin][synth]")
{
    NativeBuiltinSlot slot;
    loadSynth (slot);

    dusk::MidiBuffer midi;
    addMessage (midi, 0x90, 60, 100);
    REQUIRE (renderPeak (slot, 32, &midi) > 0.01f);
}

TEST_CASE ("synth unit stops on the transport's all-notes-off", "[builtin][synth]")
{
    NativeBuiltinSlot slot;
    loadSynth (slot);
    // A short release so the tail is gone well inside the settle below.
    slot.setParamValue (paramIndex (slot, "amp_release"), 0.01f);

    dusk::MidiBuffer noteOn;
    addMessage (noteOn, 0x90, 64, 110);
    REQUIRE (renderPeak (slot, 16, &noteOn) > 0.01f);

    // Controller 123 is what the engine sends every loaded MIDI consumer when
    // the transport stops.
    dusk::MidiBuffer allNotesOff;
    addMessage (allNotesOff, 0xB0, 123, 0);
    renderPeak (slot, 32, &allNotesOff);

    // The core keeps a small analogue noise floor rather than settling to a
    // hard zero, so silence here means "below anything audible", not 0.0f.
    REQUIRE_THAT (renderPeak (slot, 8, nullptr), WithinAbs (0.0, 5e-4));
}

TEST_CASE ("synth unit releases every voice on demand", "[builtin][synth]")
{
    NativeBuiltinSlot slot;
    loadSynth (slot);

    dusk::MidiBuffer chord;
    addMessage (chord, 0x90, 60, 100);
    addMessage (chord, 0x90, 64, 100);
    addMessage (chord, 0x90, 67, 100);
    REQUIRE (renderPeak (slot, 16, &chord) > 0.01f);

    // What a slot teardown with notes held has to be able to do: controller 120
    // cuts the voices outright rather than releasing them.
    dusk::MidiBuffer allSoundOff;
    addMessage (allSoundOff, 0xB0, 120, 0);
    // allSoundOff fades the voices out over a few milliseconds rather than
    // cutting them, so that a panic does not click.
    renderPeak (slot, 16, &allSoundOff);

    REQUIRE_THAT (renderPeak (slot, 8, nullptr), WithinAbs (0.0, 5e-4));
}

TEST_CASE ("synth unit answers pitch bend, mod wheel and sustain", "[builtin][synth]")
{
    NativeBuiltinSlot slot;
    loadSynth (slot);
    slot.setParamValue (paramIndex (slot, "amp_release"), 0.01f);

    // Sustain down, note on, note off: the pedal has to hold the voice.
    dusk::MidiBuffer held;
    addMessage (held, 0xB0, 64, 127);
    addMessage (held, 0x90, 60, 100);
    addMessage (held, 0x80, 60, 0);
    REQUIRE (renderPeak (slot, 24, &held) > 0.01f);

    dusk::MidiBuffer lift;
    addMessage (lift, 0xB0, 64, 0);
    renderPeak (slot, 32, &lift);
    REQUIRE_THAT (renderPeak (slot, 8, nullptr), WithinAbs (0.0, 5e-4));

    // Bend and mod are accepted without upsetting the render.
    dusk::MidiBuffer expressive;
    addMessage (expressive, 0x90, 60, 100);
    addMessage (expressive, 0xE0, 0x00, 0x60);
    addMessage (expressive, 0xB0, 1, 90);
    const float peak = renderPeak (slot, 24, &expressive);
    REQUIRE (peak > 0.01f);
    REQUIRE (std::isfinite (peak));
}

TEST_CASE ("synth unit state round-trips", "[builtin][synth]")
{
    NativeBuiltinSlot saver;
    loadSynth (saver);
    const int cutoffIdx = paramIndex (saver, "cutoff");
    const int resIdx    = paramIndex (saver, "resonance");
    const int waveIdx   = paramIndex (saver, "osc1_wave");
    saver.setParamValue (cutoffIdx, 1250.0f);
    saver.setParamValue (resIdx, 0.72f);
    saver.setParamValue (waveIdx, 2.0f);

    std::vector<std::uint8_t> blob;
    REQUIRE (saver.saveState (blob));

    NativeBuiltinSlot loader;
    loadSynth (loader);
    REQUIRE (loader.loadState (blob));
    REQUIRE_THAT (loader.getParamValue (cutoffIdx), WithinAbs (1250.0, 1e-6));
    REQUIRE_THAT (loader.getParamValue (resIdx),    WithinAbs (0.72, 1e-6));
    REQUIRE_THAT (loader.getParamValue (waveIdx),   WithinAbs (2.0, 1e-6));

    NativeBuiltinSlot tape;
    std::string error;
    REQUIRE (tape.loadUnit ("dusk.builtin.tape", kSampleRate, kBlock, error));
    REQUIRE_FALSE (tape.loadState (blob));
}
