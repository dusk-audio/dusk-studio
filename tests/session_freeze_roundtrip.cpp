#include <catch2/catch_test_macros.hpp>

#include "session/Session.h"
#include "session/SessionSerializer.h"

#include <juce_core/juce_core.h>

namespace
{
juce::File makeTempSessionDir()
{
    auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                  .getChildFile ("dusk-studio-freeze-roundtrip-"
                                    + juce::String (juce::Random::getSystemRandom().nextInt()));
    // Fail at the source if setup can't create the dirs, not later in the serializer.
    REQUIRE (dir.createDirectory().wasOk());
    REQUIRE (dir.getChildFile ("audio").createDirectory().wasOk());
    return dir;
}
} // namespace

// Freeze persistence contract: a frozen MIDI track must reload frozen, with
// frozenRegion rebuilt (file + length + channels) so PlaybackEngine can open
// the baked WAV. The loader also has to DROP a frozen flag whose WAV is gone,
// so a moved / cleaned session falls back to the live instrument instead of
// playing silence. Mirrors the engine-side AudioEngine::freezeTrack writes
// without needing an audio device.
TEST_CASE ("SessionSerializer round-trips track freeze state", "[session][serializer][freeze]")
{
    using duskstudio::Session;
    using duskstudio::Track;
    using duskstudio::SessionSerializer;

    const auto dir    = makeTempSessionDir();
    const auto target = dir.getChildFile ("session.json");
    const auto wav    = dir.getChildFile ("audio").getChildFile ("freeze_track01.wav");

    SECTION ("frozen track with a present WAV reloads frozen + region intact")
    {
        wav.replaceWithText ("not really audio, but existsAsFile() is all the loader checks");

        Session a;
        auto& t0 = a.track (0);
        t0.name = "Synth";
        t0.mode.store ((int) Track::Mode::Midi, std::memory_order_relaxed);
        t0.frozen.store (true, std::memory_order_relaxed);
        t0.frozenAudioPath = wav.getFullPathName();
        t0.frozenRegion.file            = wav;
        t0.frozenRegion.lengthInSamples = 480000;
        t0.frozenRegion.numChannels     = 2;

        REQUIRE (SessionSerializer::save (a, target));

        Session b;
        REQUIRE (SessionSerializer::load (b, target));

        auto& r = b.track (0);
        REQUIRE (r.frozen.load (std::memory_order_relaxed));
        REQUIRE (juce::File (r.frozenAudioPath) == wav);
        REQUIRE (r.frozenRegion.file == wav);
        REQUIRE (r.frozenRegion.lengthInSamples == 480000);
        REQUIRE (r.frozenRegion.numChannels == 2);
        REQUIRE (r.frozenRegion.timelineStart == 0);
    }

    SECTION ("frozen flag is dropped when the baked WAV is missing")
    {
        // Path recorded, file never created (simulates a moved / cleaned session).
        Session a;
        auto& t0 = a.track (0);
        t0.mode.store ((int) Track::Mode::Midi, std::memory_order_relaxed);
        t0.frozen.store (true, std::memory_order_relaxed);
        t0.frozenAudioPath = wav.getFullPathName();
        t0.frozenRegion.file            = wav;
        t0.frozenRegion.lengthInSamples = 480000;
        t0.frozenRegion.numChannels     = 2;

        REQUIRE (SessionSerializer::save (a, target));
        REQUIRE_FALSE (wav.existsAsFile());

        Session b;
        REQUIRE (SessionSerializer::load (b, target));
        REQUIRE_FALSE (b.track (0).frozen.load (std::memory_order_relaxed));
    }

    dir.deleteRecursively();
}
