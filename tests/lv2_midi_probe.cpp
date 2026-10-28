#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/lv2/NativeLv2Slot.h"
#include "foundation/MidiBuffer.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>

// The MIDI an LV2 plug-in hears rides an atom sequence of a fixed size. A
// block that overfills it must still end every note it ends.

using Catch::Matchers::WithinAbs;

namespace
{
constexpr int kBlock = 64;
constexpr const char* kProbeUri = "urn:duskstudio:test:midi-probe";

void addMidiAt (dusk::MidiBuffer& buffer, std::uint8_t status, std::uint8_t d1,
                std::uint8_t d2, int samplePosition)
{
    const std::array<std::uint8_t, 3> bytes { status, d1, d2 };
    buffer.addEvent (bytes.data(), (int) bytes.size(), samplePosition);
}

struct Lv2Probe
{
    duskstudio::lv2::NativeLv2Slot slot;
    std::array<float, kBlock> inL {}, inR {}, outL {}, outR {};

    bool load()
    {
        std::string error;
        const bool loaded = slot.load (std::filesystem::u8path (DUSKSTUDIO_MIDI_PROBE_LV2_FIXTURE_PATH),
                                       48000.0, kBlock, error, kProbeUri);
        INFO ("load error: " << error);
        return loaded;
    }

    // The probe plays 0.1 per held voice.
    float voicesAfter (const dusk::MidiBuffer& midi)
    {
        outL.fill (0.0f);
        outR.fill (0.0f);
        slot.processStereo (inL.data(), inR.data(), outL.data(), outR.data(), kBlock, &midi);
        return outL[kBlock - 1] / 0.1f;
    }
};
} // namespace

TEST_CASE ("an LV2 block bigger than the event sequence still ends its notes", "[lv2][midi]")
{
    Lv2Probe probe;
    REQUIRE (probe.load());
    REQUIRE (probe.slot.isLoadedInstrument());
    constexpr int kFlood = 12000;

    SECTION ("a controller flood between a note's start and its end")
    {
        dusk::MidiBuffer block;
        addMidiAt (block, 0x90, 60, 100, 0);
        for (int i = 0; i < kFlood; ++i)
            addMidiAt (block, 0xB0, 1, (std::uint8_t) (i & 0x7F), 1 + i * (kBlock - 2) / kFlood);
        addMidiAt (block, 0x80, 60, 0, kBlock - 1);
        REQUIRE_THAT (probe.voicesAfter (block), WithinAbs (0.0, 1.0e-4));

        dusk::MidiBuffer fits;
        addMidiAt (fits, 0x90, 64, 100, 0);
        REQUIRE_THAT (probe.voicesAfter (fits), WithinAbs (1.0, 1.0e-4));
    }

    SECTION ("more note-offs than the sequence holds")
    {
        dusk::MidiBuffer start;
        addMidiAt (start, 0x90, 60, 100, 0);
        REQUIRE_THAT (probe.voicesAfter (start), WithinAbs (1.0, 1.0e-4));

        dusk::MidiBuffer block;
        for (int i = 0; i < kFlood; ++i)
            addMidiAt (block, 0x80, (std::uint8_t) (61 + i % 60), 0, i * (kBlock - 1) / kFlood);
        addMidiAt (block, 0x80, 60, 0, kBlock - 1);
        REQUIRE_THAT (probe.voicesAfter (block), WithinAbs (0.0, 1.0e-4));
    }
}

TEST_CASE ("LV2 supplies denominator-based transport to every declared time input",
           "[lv2][transport][midi]")
{
    Lv2Probe probe;
    REQUIRE (probe.load());
    bool reporting = false;
    for (int index = 0; index < probe.slot.paramCount(); ++index)
        if (probe.slot.paramInfo (index)->name == "Report Transport")
        {
            probe.slot.setParamValue (probe.slot.paramInfo (index)->id, 1.0f);
            reporting = true;
        }
    REQUIRE (reporting);
    const auto process = [&] (const dusk::TransportPosition* position, const dusk::MidiBuffer* midi = nullptr)
    {
        probe.slot.processStereo (probe.inL.data(), probe.inR.data(),
                                  probe.outL.data(), probe.outR.data(), kBlock, midi, position);
    };
    const auto expectReport = [&] (std::int64_t frame, float speed, float beat, float bar,
                                   float barBeat, float unit, float beatsPerBar, float tempo)
    {
        const std::array<float, 12> expected {
            1.0f, 0.0f, (float) frame, speed, beat, bar, barBeat, unit,
            beatsPerBar, tempo, 48000.0f, 1.0f
        };
        for (std::size_t sample = 0; sample < expected.size(); ++sample)
        {
            INFO ("transport report sample " << sample);
            CHECK_THAT (probe.outL[sample], WithinAbs (expected[sample], 1.0e-5f));
            CHECK_THAT (probe.outR[sample], WithinAbs (expected[sample], 1.0e-5f));
        }
    };
    dusk::TransportPosition position;
    position.bpm = 90.0;
    position.ppqPosition = 7.25;
    position.timeInSamples = 123456;
    position.timeSignatureNumerator = 6;
    position.timeSignatureDenominator = 8;
    position.isRecording = true;
    dusk::MidiBuffer start;
    addMidiAt (start, 0x90, 60, 100, 0);
    process (&position, &start);
    CHECK_THAT (probe.outL.back(), WithinAbs (0.1f, 1.0e-6f));
    CHECK_THAT (probe.outR.back(), WithinAbs (0.1f, 1.0e-6f));
    dusk::MidiBuffer flood;
    constexpr int kFlood = 12000;
    for (int index = 0; index < kFlood; ++index)
        addMidiAt (flood, 0xB0, 1, (std::uint8_t) (index & 0x7F),
                   1 + index * (kBlock - 2) / kFlood);
    addMidiAt (flood, 0x80, 60, 0, kBlock - 1);
    process (&position, &flood);
    expectReport (123456, 1.0f, 14.5f, 2.0f, 2.5f, 8.0f, 6.0f, 180.0f);
    CHECK_THAT (probe.outL.back(), WithinAbs (0.0f, 1.0e-6f));
    CHECK_THAT (probe.outR.back(), WithinAbs (0.0f, 1.0e-6f));

    position.isRecording = false;
    position.bpm = 123.0;
    position.ppqPosition = -0.75;
    position.timeInSamples = -12000;
    process (&position);
    expectReport (-12000, 0.0f, -1.5f, -1.0f, 4.5f, 8.0f, 6.0f, 246.0f);

    process (nullptr);
    CHECK_THAT (probe.outL[0], WithinAbs (0.0f, 1.0e-6f));
    CHECK_THAT (probe.outR[0], WithinAbs (0.0f, 1.0e-6f));
    position.isPlaying = true;
    position.ppqPosition = 8.5;
    position.timeInSamples = 96000;
    position.timeSignatureNumerator = 7;
    position.timeSignatureDenominator = 4;
    process (&position);
    expectReport (96000, 1.0f, 8.5f, 1.0f, 1.5f, 4.0f, 7.0f, 123.0f);
}
