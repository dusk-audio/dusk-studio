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

// ---------------------------------------------------------------------------
// Sample-accurate event timing. The core's note and controller calls take no
// offset, so the unit has to split the block at each event; these pin that the
// split happens and that the result does not depend on the buffer size.

namespace
{
constexpr int kTimingBlock = 1024;

void loadSynthAt (NativeBuiltinSlot& slot, int maxBlock)
{
    std::string error;
    REQUIRE (slot.loadUnit ("dusk.builtin.synth", kSampleRate, maxBlock, error));
    REQUIRE (error.empty());
}

void addMessageAt (dusk::MidiBuffer& midi, std::uint8_t a, std::uint8_t b, std::uint8_t c,
                   int samplePosition)
{
    const std::uint8_t bytes[3] = { a, b, c };
    midi.addEvent (bytes, 3, samplePosition);
}

void renderBlock (NativeBuiltinSlot& slot, std::vector<float>& l, std::vector<float>& r,
                  const dusk::MidiBuffer* midi)
{
    std::fill (l.begin(), l.end(), 0.0f);
    std::fill (r.begin(), r.end(), 0.0f);
    slot.processStereo (l.data(), r.data(), l.data(), r.data(), (int) l.size(), midi);
}

float peakOver (const std::vector<float>& x, int begin, int end)
{
    float peak = 0.0f;
    for (int i = begin; i < end; ++i) peak = std::max (peak, std::abs (x[(size_t) i]));
    return peak;
}

float maxDiff (const std::vector<float>& a, const std::vector<float>& b, int begin, int end)
{
    float d = 0.0f;
    for (int i = begin; i < end; ++i)
        d = std::max (d, std::abs (a[(size_t) i] - b[(size_t) i]));
    return d;
}

struct TimedEvent { int frame; std::uint8_t a, b, c; };

// Renders `totalFrames` through fixed-size blocks, placing each event in the
// block that contains it at its offset within that block.
void renderStream (NativeBuiltinSlot& slot, const std::vector<TimedEvent>& events,
                   int totalFrames, int blockFrames, std::vector<float>& outL,
                   std::vector<float>& outR)
{
    outL.assign ((size_t) totalFrames, 0.0f);
    outR.assign ((size_t) totalFrames, 0.0f);
    std::vector<float> l ((size_t) blockFrames), r ((size_t) blockFrames);
    dusk::MidiBuffer midi;

    for (int start = 0; start < totalFrames; start += blockFrames)
    {
        const int n = std::min (blockFrames, totalFrames - start);
        midi.clear();
        for (const auto& e : events)
            if (e.frame >= start && e.frame < start + n)
                addMessageAt (midi, e.a, e.b, e.c, e.frame - start);

        std::fill (l.begin(), l.begin() + n, 0.0f);
        std::fill (r.begin(), r.begin() + n, 0.0f);
        slot.processStereo (l.data(), r.data(), l.data(), r.data(), n, &midi);
        std::copy (l.begin(), l.begin() + n, outL.begin() + start);
        std::copy (r.begin(), r.begin() + n, outR.begin() + start);
    }
}

// Four blocks of a held middle C, so every slot in a comparison reaches the
// measured block with identical envelope and filter state.
void primeHeldNote (NativeBuiltinSlot& slot)
{
    loadSynthAt (slot, kTimingBlock);
    slot.setParamValue (paramIndex (slot, "amp_attack"), 0.001f);

    std::vector<float> l ((size_t) kTimingBlock), r ((size_t) kTimingBlock);
    dusk::MidiBuffer on;
    addMessageAt (on, 0x90, 60, 100, 0);
    renderBlock (slot, l, r, &on);
    for (int i = 0; i < 3; ++i) renderBlock (slot, l, r, nullptr);
}
} // namespace

TEST_CASE ("synth unit starts a note at its sample offset", "[builtin][synth][timing]")
{
    NativeBuiltinSlot slot;
    loadSynthAt (slot, kTimingBlock);
    slot.setParamValue (paramIndex (slot, "amp_attack"), 0.001f);

    dusk::MidiBuffer midi;
    addMessageAt (midi, 0x90, 60, 100, 768);

    std::vector<float> l ((size_t) kTimingBlock), r ((size_t) kTimingBlock);
    renderBlock (slot, l, r, &midi);

    // Nothing before the offset but the core's own noise floor...
    REQUIRE_THAT (peakOver (l, 0, 768), WithinAbs (0.0, 5e-4));
    REQUIRE_THAT (peakOver (r, 0, 768), WithinAbs (0.0, 5e-4));
    // ...and the note from it, with 1 ms of attack to reach full level.
    REQUIRE (peakOver (l, 768, kTimingBlock) > 0.01f);
}

TEST_CASE ("synth unit renders a note that opens and closes inside one block",
           "[builtin][synth][timing]")
{
    NativeBuiltinSlot slot;
    loadSynthAt (slot, kTimingBlock);
    slot.setParamValue (paramIndex (slot, "amp_attack"), 0.001f);
    slot.setParamValue (paramIndex (slot, "amp_release"), 0.01f);

    dusk::MidiBuffer midi;
    addMessageAt (midi, 0x90, 60, 100, 768);
    addMessageAt (midi, 0x80, 60, 0, 1000);

    std::vector<float> l ((size_t) kTimingBlock), r ((size_t) kTimingBlock);
    renderBlock (slot, l, r, &midi);

    // Applying both events ahead of the render releases the voice before a
    // sample of it exists and the note is lost outright.
    REQUIRE (peakOver (l, 768, 1000) > 0.01f);

    for (int i = 0; i < 8; ++i) renderBlock (slot, l, r, nullptr);
    REQUIRE_THAT (peakOver (l, 0, kTimingBlock), WithinAbs (0.0, 5e-4));
}

TEST_CASE ("synth unit applies a controller from its sample offset",
           "[builtin][synth][timing]")
{
    NativeBuiltinSlot plain, atStart, midBlock;
    primeHeldNote (plain);
    primeHeldNote (atStart);
    primeHeldNote (midBlock);

    std::vector<float> plainL ((size_t) kTimingBlock), plainR ((size_t) kTimingBlock);
    std::vector<float> startL ((size_t) kTimingBlock), startR ((size_t) kTimingBlock);
    std::vector<float> midL   ((size_t) kTimingBlock), midR   ((size_t) kTimingBlock);

    // A pitch bend of about +1.5 semitones at the default 2-semitone range.
    dusk::MidiBuffer bendAtStart, bendMidBlock;
    addMessageAt (bendAtStart,  0xE0, 0x00, 0x60, 0);
    addMessageAt (bendMidBlock, 0xE0, 0x00, 0x60, 512);

    renderBlock (plain,    plainL, plainR, nullptr);
    renderBlock (atStart,  startL, startR, &bendAtStart);
    renderBlock (midBlock, midL,   midR,   &bendMidBlock);

    // The bend is audible wherever it has been applied...
    REQUIRE (maxDiff (startL, plainL, 0, 512) > 1e-3f);
    REQUIRE (maxDiff (midL,   plainL, 512, kTimingBlock) > 1e-3f);
    // ...and the half of the block before its offset is untouched by it.
    REQUIRE_THAT (maxDiff (midL, plainL, 0, 512), WithinAbs (0.0, 1e-7));
    REQUIRE_THAT (maxDiff (midR, plainR, 0, 512), WithinAbs (0.0, 1e-7));
}

TEST_CASE ("synth unit renders the same audio at any block size",
           "[builtin][synth][timing]")
{
    const std::vector<TimedEvent> events
    {
        {  100, 0x90, 60, 100 },   // note on
        {  900, 0xB0, 1,   90 },   // mod wheel
        { 1500, 0xE0, 0x00, 0x60 },// pitch bend
        { 2600, 0x80, 60,   0 },   // note off
        { 3000, 0x90, 67, 110 },   // a second note, mid-block
    };
    constexpr int kTotal = 4096;

    NativeBuiltinSlot coarse, fine;
    loadSynthAt (coarse, kTimingBlock);
    loadSynthAt (fine,   kTimingBlock);
    for (auto* slot : { &coarse, &fine })
    {
        slot->setParamValue (paramIndex (*slot, "amp_attack"), 0.001f);
        slot->setParamValue (paramIndex (*slot, "amp_release"), 0.01f);
    }

    std::vector<float> coarseL, coarseR, fineL, fineR;
    renderStream (coarse, events, kTotal, kTimingBlock, coarseL, coarseR);
    renderStream (fine,   events, kTotal, 64,           fineL,   fineR);

    // Something has to be there to compare.
    REQUIRE (peakOver (coarseL, 0, kTotal) > 0.01f);
    // Every event lands on the same sample either way, and the core's state
    // machine advances per host sample, so the two paths are the same render.
    REQUIRE_THAT (maxDiff (coarseL, fineL, 0, kTotal), WithinAbs (0.0, 1e-7));
    REQUIRE_THAT (maxDiff (coarseR, fineR, 0, kTotal), WithinAbs (0.0, 1e-7));
}
