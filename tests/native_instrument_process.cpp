// Natively-hosted instruments: load a real synth through each format's slot,
// feed it a note, and require audible output (and silence without notes).
// Gated on DUSKSTUDIO_TEST_INSTRUMENT_{CLAP,VST3,LV2}=/path/to/bundle so CI
// without synths stays green.

#include <catch2/catch_test_macros.hpp>

#include "support/DuskMidiTestBridge.h"

#if DUSKSTUDIO_HAS_NATIVE_CLAP
 #include "engine/clap/NativeClapSlot.h"
#endif
#if DUSKSTUDIO_HAS_NATIVE_LV2
 #include "engine/lv2/NativeLv2Slot.h"
#endif
#if DUSKSTUDIO_HAS_NATIVE_VST3
 #include "engine/vst3/NativeVst3Slot.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace
{
constexpr int kBlock = 512;

// Drive `blocks` blocks; a note-on lands in the first block, a note-off midway.
// Returns the output peak across the run.
template <typename Slot>
float driveNote (Slot& slot, int blocks)
{
    std::vector<float> L ((size_t) kBlock), R ((size_t) kBlock);
    float peak = 0.0f;
    for (int b = 0; b < blocks; ++b)
    {
        juce::MidiBuffer midi;
        // Several keys so drum-mapped instruments (kick/snare/hat) trigger too,
        // re-struck periodically — sample-library instruments can still be
        // streaming their kit when the first note lands.
        const int keys[] = { 36, 38, 42, 60 };
        if (b % 16 == 0)
            for (const int k : keys)
                for (const int ch : { 1, 10 })   // 10: GM drum-channel convention
                    midi.addEvent (juce::MidiMessage::noteOn (ch, k, (juce::uint8) 100), 0);
        if (b % 16 == 12)
            for (const int k : keys)
                for (const int ch : { 1, 10 })
                    midi.addEvent (juce::MidiMessage::noteOff (ch, k), 0);
        const dusk::MidiBuffer dmidi = duskstudio::test::toDusk (midi);
        std::fill (L.begin(), L.end(), 0.0f);
        std::fill (R.begin(), R.end(), 0.0f);
        slot.processStereo (L.data(), R.data(), L.data(), R.data(), kBlock, &dmidi);
        for (int i = 0; i < kBlock; ++i)
        {
            REQUIRE (std::isfinite (L[(size_t) i]));
            REQUIRE (std::isfinite (R[(size_t) i]));
            peak = std::max ({ peak, std::abs (L[(size_t) i]), std::abs (R[(size_t) i]) });
        }
    }
    return peak;
}

template <typename Slot>
float driveSilence (Slot& slot, int blocks)
{
    std::vector<float> L ((size_t) kBlock), R ((size_t) kBlock);
    float peak = 0.0f;
    for (int b = 0; b < blocks; ++b)
    {
        std::fill (L.begin(), L.end(), 0.0f);
        std::fill (R.begin(), R.end(), 0.0f);
        slot.processStereo (L.data(), R.data(), L.data(), R.data(), kBlock, nullptr);
        for (int i = 0; i < kBlock; ++i)
            peak = std::max ({ peak, std::abs (L[(size_t) i]), std::abs (R[(size_t) i]) });
    }
    return peak;
}
} // namespace

#if DUSKSTUDIO_HAS_NATIVE_CLAP
TEST_CASE ("Native CLAP instrument produces audio from notes", "[clap][instrument]")
{
    const char* path = std::getenv ("DUSKSTUDIO_TEST_INSTRUMENT_CLAP");
    if (path == nullptr || *path == '\0')
    { SUCCEED ("DUSKSTUDIO_TEST_INSTRUMENT_CLAP not set — skipping"); return; }

    duskstudio::clap::NativeClapSlot slot;
    std::string err;
    REQUIRE (slot.load (std::filesystem::u8path (path), 48000.0, kBlock, err));
    REQUIRE (slot.isLoadedInstrument());

    driveSilence (slot, 188);   // ~2 s warmup: sample libraries stream their kits
    REQUIRE (driveSilence (slot, 8) < 1.0e-2f);
    REQUIRE (driveNote (slot, 64) > 1.0e-3f);
}
#endif

#if DUSKSTUDIO_HAS_NATIVE_VST3
TEST_CASE ("Native VST3 instrument produces audio from notes", "[vst3][instrument]")
{
    const char* path = std::getenv ("DUSKSTUDIO_TEST_INSTRUMENT_VST3");
    if (path == nullptr || *path == '\0')
    { SUCCEED ("DUSKSTUDIO_TEST_INSTRUMENT_VST3 not set — skipping"); return; }

    // The default pick is effects-only; instruments load by explicit class id.
    duskstudio::vst3::Vst3Bundle bundle;
    std::string err;
    REQUIRE (bundle.load (path, err));
    std::string classId;
    for (const auto& d : bundle.plugins())
        if (d.isInstrument) { classId = d.id; break; }
    if (classId.empty())
    { SUCCEED ("module advertises no instrument class — skipping"); return; }

    duskstudio::vst3::NativeVst3Slot slot;
    REQUIRE (slot.load (std::filesystem::u8path (path), 48000.0, kBlock, err, classId));
    // No isLoadedInstrument assert: instrument-flagged hybrids with an audio
    // input bus exist (drum-replacement tools). Notes making sound is the test.

    driveSilence (slot, 188);   // ~2 s warmup: sample libraries stream their kits
    REQUIRE (driveSilence (slot, 8) < 1.0e-2f);
    REQUIRE (driveNote (slot, 64) > 1.0e-3f);

    // The CC bridge cached the plugin's IMidiMapping. Zero is legitimate (drum
    // instruments map no controllers), so only synth-style plugins that map
    // anything prove the bridge — Diva maps the usual suspects.
    INFO ("IMidiMapping assignments: " << slot.getInstance()->midiCcMappingCount());
}
#endif

#if DUSKSTUDIO_HAS_NATIVE_LV2
TEST_CASE ("Native LV2 instrument produces audio from notes", "[lv2][instrument]")
{
    const char* path = std::getenv ("DUSKSTUDIO_TEST_INSTRUMENT_LV2");
    if (path == nullptr || *path == '\0')
    { SUCCEED ("DUSKSTUDIO_TEST_INSTRUMENT_LV2 not set — skipping"); return; }

    duskstudio::lv2::NativeLv2Slot slot;
    std::string err;
    REQUIRE (slot.load (std::filesystem::u8path (path), 48000.0, kBlock, err));
    // No isLoadedInstrument assert: JUCE-built LV2 instruments expose an audio
    // input bus, so the layout legitimately isn't input-less. The strip's MIDI
    // branch only needs isLoaded — what matters is that notes make sound.

    driveSilence (slot, 188);   // ~2 s warmup: sample libraries stream their kits
    REQUIRE (driveSilence (slot, 8) < 1.0e-2f);
    REQUIRE (driveNote (slot, 64) > 1.0e-3f);
}
#endif
